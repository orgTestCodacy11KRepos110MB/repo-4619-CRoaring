/*
 * bitset.c
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitset.h"
#include "util.h"
#include "utilasm.h"


extern int bitset_container_cardinality(bitset_container_t *bitset);
extern bool bitset_container_nonzero_cardinality(bitset_container_t *bitset);
extern void bitset_container_set(bitset_container_t *bitset, uint16_t pos);
extern void bitset_container_unset(bitset_container_t *bitset, uint16_t pos);
extern bool bitset_container_get(const bitset_container_t *bitset, uint16_t pos);



/* Create a new bitset. Return NULL in case of failure. */
bitset_container_t *bitset_container_create() {
    bitset_container_t *bitset = calloc(1, sizeof(bitset_container_t));

    if (!bitset) {
        return NULL;
    }

    if (posix_memalign((void *)&bitset->array, sizeof(__m256i),
                       sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS)) {
        free(bitset);
        return NULL;
    }

    memset(bitset->array, 0, sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    bitset->cardinality = 0;
    return bitset;
}

/* Copy one container into another. We assume that they are distinct. */
void bitset_container_copy(bitset_container_t *source, bitset_container_t *dest) {
	dest->cardinality = source->cardinality;
	memcpy(dest->array,source->array,sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
}


/* Free memory. */
void bitset_container_free(bitset_container_t *bitset) {
    free(bitset->array);
    bitset->array = NULL;
    free(bitset);
}

/* duplicate container. */
bitset_container_t *bitset_container_clone( bitset_container_t *src) {
    bitset_container_t *bitset = calloc(1, sizeof(bitset_container_t));

    if (!bitset) {
        return NULL;
    }

    if (posix_memalign((void *)&bitset->array, sizeof(__m256i),
                       sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS)) {
        free(bitset);
        return NULL;
    }
    bitset->cardinality = src->cardinality;
    memcpy(bitset->array, src->array,  sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    return bitset;
}


void bitset_container_set_range(bitset_container_t *bitset, uint32_t begin, uint32_t end) {
	bitset_set_range(bitset->array, begin,end);
	bitset->cardinality = bitset_container_compute_cardinality(bitset);// could be smarter
}

//#define USEPOPCNT // when this is disabled bitset_container_compute_cardinality uses AVX to compute hamming weight

#ifdef USEPOPCNT

/* Get the number of bits set (force computation) */
int bitset_container_compute_cardinality(const bitset_container_t *bitset) {
    const uint64_t *array = bitset->array;
    int32_t sum = 0;
    for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 4) {
        sum += _mm_popcnt_u64(array[i]);
        sum += _mm_popcnt_u64(array[i + 1]);
        sum += _mm_popcnt_u64(array[i + 2]);
        sum += _mm_popcnt_u64(array[i + 3]);
    }
    return sum;
}

#else

/* Get the number of bits set (force computation) */
int bitset_container_compute_cardinality(const bitset_container_t *bitset) {
    const uint64_t *array = bitset->array;
    // these are precomputed hamming weights (weight(0), weight(1)...)
    const __m256i shuf = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                          0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const __m256i  mask = _mm256_set1_epi8(0x0f); // low 4 bits of each byte
    __m256i total = _mm256_setzero_si256();
    __m256i zero = _mm256_setzero_si256();
    const int inner = 4;// length of the inner loop, could go up to 8 safely
    const int outer = BITSET_CONTAINER_SIZE_IN_WORDS*sizeof(uint64_t)/(sizeof(__m256i)*inner); // length of outer loop
    for(int  k = 0; k < outer ; k++) {
        __m256i innertotal = _mm256_setzero_si256();
        for(int i = 0; i < inner; ++i) {
            __m256i ymm1 = _mm256_lddqu_si256((const __m256i *)array + k*inner + i);
            __m256i ymm2 = _mm256_srli_epi32(ymm1,4); // shift right, shiftingin zeroes
            ymm1 = _mm256_and_si256(ymm1,mask); // contains even 4 bits
            ymm2 = _mm256_and_si256(ymm2,mask); // contains odd 4 bits
            ymm1 = _mm256_shuffle_epi8(shuf,ymm1);// use table look-up to sum the 4 bits
            ymm2 = _mm256_shuffle_epi8(shuf,ymm2);
            innertotal = _mm256_add_epi8(innertotal,ymm1);// inner total values in each byte are bounded by 8 * inner
            innertotal = _mm256_add_epi8(innertotal,ymm2);// inner total values in each byte are bounded by 8 * inner
        }
        innertotal = _mm256_sad_epu8(zero,innertotal);// produces 4 64-bit counters (having values in [0,8 * inner * 4])
        total= _mm256_add_epi64(total,innertotal); // add the 4 64-bit counters to previous counter
    }
    return _mm256_extract_epi64(total,0)+_mm256_extract_epi64(total,1)+_mm256_extract_epi64(total,2)+_mm256_extract_epi64(total,3);
}
#endif


#ifdef USEAVX


#ifndef USEPOPCNT
#define BITSET_CONTAINER_FN_REPEAT 8
#define WORDS_IN_AVX2_REG sizeof(__m256i) / sizeof(uint64_t)
#define LOOP_SIZE BITSET_CONTAINER_SIZE_IN_WORDS / (WORDS_IN_AVX2_REG * BITSET_CONTAINER_FN_REPEAT)


/* Computes a binary operation (eg union) on bitset1 and bitset2 and write the
   result to bitsetout */
// clang-format off
#define BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic)            \
int bitset_container_##opname##_nocard(const bitset_container_t *src_1, \
                                       const bitset_container_t *src_2, \
                                       bitset_container_t *dst) {       \
    const uint8_t *array_1 = (const uint8_t *)src_1->array;             \
    const uint8_t *array_2 = (const uint8_t *)src_2->array;             \
    /* not using the blocking optimization for some reason*/            \
    uint8_t *out = (uint8_t*)dst->array;                                \
    const int innerloop = 8;                                            \
    for (size_t i = 0;                                                  \
        i < BITSET_CONTAINER_SIZE_IN_WORDS / (WORDS_IN_AVX2_REG);       \
                                                         i+=innerloop) {\
        __m256i A1, A2, AO;                                             \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1));                  \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2));                  \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)out, AO);                        \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1 + 32));             \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2 + 32));             \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)(out+32), AO);                   \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1 + 64));             \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2 + 64));             \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)(out+64), AO);                   \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1 + 96));             \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2 + 96));             \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)(out+96), AO);                   \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1 + 128));            \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2 + 128));            \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)(out+128), AO);                  \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1 + 160));            \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2 + 160));            \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)(out+160), AO);                  \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1 + 192));            \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2 + 192));            \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)(out+192), AO);                  \
        A1 = _mm256_lddqu_si256((__m256i *)(array_1 + 224));            \
        A2 = _mm256_lddqu_si256((__m256i *)(array_2 + 224));            \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm256_storeu_si256((__m256i *)(out+224), AO);                  \
        out+=256;                                                       \
        array_1 += 256;                                                 \
        array_2 += 256;                                                 \
    }                                                                   \
    dst->cardinality = -1;                                              \
    return dst->cardinality;                                            \
}                                                                       \
/* next, a version that updates cardinality*/                           \
int bitset_container_##opname(const bitset_container_t *src_1,          \
                              const bitset_container_t *src_2,          \
                              bitset_container_t *dst) {                \
    const uint64_t *array_1 = src_1->array;                             \
    const uint64_t *array_2 = src_2->array;                             \
    uint64_t *out = dst->array;                                         \
    const __m256i shuf =                                                \
       _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, \
                        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);\
    const __m256i  mask = _mm256_set1_epi8(0x0f);                       \
    __m256i total = _mm256_setzero_si256();                             \
    __m256i zero = _mm256_setzero_si256();                              \
    for (size_t idx = 0; idx < 256; idx += 4) {                         \
        __m256i A1, A2, ymm1, ymm2;                                     \
       __m256i innertotal = _mm256_setzero_si256();                     \
        A1 = _mm256_lddqu_si256((__m256i *)array_1 + idx + 0);          \
        A2 = _mm256_lddqu_si256((__m256i *)array_2 + idx + 0);          \
        ymm1 = avx_intrinsic(A2, A1);                                   \
        _mm256_storeu_si256((__m256i *)out + idx + 0, ymm1);            \
        ymm2 = _mm256_srli_epi32(ymm1,4);                               \
        ymm1 = _mm256_and_si256(ymm1,mask);                             \
        ymm2 = _mm256_and_si256(ymm2,mask);                             \
        ymm1 = _mm256_shuffle_epi8(shuf,ymm1);                          \
        ymm2 = _mm256_shuffle_epi8(shuf,ymm2);                          \
        innertotal = _mm256_add_epi8(innertotal,ymm1);                  \
        innertotal = _mm256_add_epi8(innertotal,ymm2);                  \
        A1 = _mm256_lddqu_si256((__m256i *)array_1 + idx + 1);          \
        A2 = _mm256_lddqu_si256((__m256i *)array_2 + idx + 1);          \
        ymm1 = avx_intrinsic(A2, A1);                                   \
        _mm256_storeu_si256((__m256i *)out + idx + 1, ymm1);            \
        ymm2 = _mm256_srli_epi32(ymm1,4);                               \
        ymm1 = _mm256_and_si256(ymm1,mask);                             \
        ymm2 = _mm256_and_si256(ymm2,mask);                             \
        ymm1 = _mm256_shuffle_epi8(shuf,ymm1);                          \
        ymm2 = _mm256_shuffle_epi8(shuf,ymm2);                          \
        innertotal = _mm256_add_epi8(innertotal,ymm1);                  \
        innertotal = _mm256_add_epi8(innertotal,ymm2);                  \
        A1 = _mm256_lddqu_si256((__m256i *)array_1 + idx + 2);          \
        A2 = _mm256_lddqu_si256((__m256i *)array_2 + idx + 2);          \
        ymm1 = avx_intrinsic(A2, A1);                                   \
        _mm256_storeu_si256((__m256i *)out + idx + 2, ymm1);            \
        ymm2 = _mm256_srli_epi32(ymm1,4);                               \
        ymm1 = _mm256_and_si256(ymm1,mask);                             \
        ymm2 = _mm256_and_si256(ymm2,mask);                             \
        ymm1 = _mm256_shuffle_epi8(shuf,ymm1);                          \
        ymm2 = _mm256_shuffle_epi8(shuf,ymm2);                          \
        innertotal = _mm256_add_epi8(innertotal,ymm1);                  \
        innertotal = _mm256_add_epi8(innertotal,ymm2);                  \
        A1 = _mm256_lddqu_si256((__m256i *)array_1 + idx + 3);          \
        A2 = _mm256_lddqu_si256((__m256i *)array_2 + idx + 3);          \
        ymm1 = avx_intrinsic(A2, A1);                                   \
        _mm256_storeu_si256((__m256i *)out + idx + 3, ymm1);            \
        ymm2 = _mm256_srli_epi32(ymm1,4);                               \
        ymm1 = _mm256_and_si256(ymm1,mask);                             \
        ymm2 = _mm256_and_si256(ymm2,mask);                             \
        ymm1 = _mm256_shuffle_epi8(shuf,ymm1);                          \
        ymm2 = _mm256_shuffle_epi8(shuf,ymm2);                          \
        innertotal = _mm256_add_epi8(innertotal,ymm1);                  \
        innertotal = _mm256_add_epi8(innertotal,ymm2);                  \
        innertotal = _mm256_sad_epu8(zero,innertotal);                  \
        total= _mm256_add_epi64(total,innertotal);                      \
    }                                                                   \
    dst->cardinality = _mm256_extract_epi64(total,0) +                  \
        _mm256_extract_epi64(total,1) +                                 \
        _mm256_extract_epi64(total,2) +                                 \
        _mm256_extract_epi64(total,3);                                  \
    return dst->cardinality;                                            \
}



#else //USEPOPCNT
#define BITSET_CONTAINER_FN_REPEAT 8
#define WORDS_IN_AVX2_REG sizeof(__m256i) / sizeof(uint64_t)
#define LOOP_SIZE BITSET_CONTAINER_SIZE_IN_WORDS / (WORDS_IN_AVX2_REG * BITSET_CONTAINER_FN_REPEAT)


/* Computes a binary operation (eg union) on bitset1 and bitset2 and write the
   result to bitsetout */
// clang-format off
#define BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic)            \
int bitset_container_##opname##_nocard(const bitset_container_t *src_1, \
                                       const bitset_container_t *src_2, \
                                       bitset_container_t *dst) {       \
    const uint64_t *array_1 = src_1->array;                             \
    const uint64_t *array_2 = src_2->array;                             \
    /* not using the blocking optimization for some reason*/            \
    uint64_t *out = dst->array;                                         \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS / 4; i++) {   \
        __m256i A1 = _mm256_lddqu_si256((__m256i *)array_1 + i);        \
        __m256i A2 = _mm256_lddqu_si256((__m256i *)array_2 + i);        \
        /* swapped order to get andnot to work*/                        \
        __m256i AO = avx_intrinsic(A2, A1);                             \
        _mm256_storeu_si256((__m256i *)out + i, AO);                    \
    }                                                                   \
    dst->cardinality = -1;                                              \
    return dst->cardinality;                                            \
}                                                                       \
/* next, a version that updates cardinality*/                           \
int bitset_container_##opname(const bitset_container_t *src_1,          \
                              const bitset_container_t *src_2,          \
                              bitset_container_t *dst) {                \
    const uint64_t *array_1 = src_1->array;                             \
    const uint64_t *array_2 = src_2->array;                             \
    uint64_t *out = dst->array;                                         \
    int32_t sum = 0;                                                    \
    for (size_t i = 0; i < LOOP_SIZE; i++) {                            \
        for (size_t j = 0; j < BITSET_CONTAINER_FN_REPEAT; ++j) {                           \
            const int idx = (i * BITSET_CONTAINER_FN_REPEAT) + j;                           \
            __m256i A1 = _mm256_lddqu_si256((__m256i *)array_1 + idx);  \
            __m256i A2 = _mm256_lddqu_si256((__m256i *)array_2 + idx);  \
            __m256i AO = avx_intrinsic(A2, A1);                         \
            _mm256_storeu_si256((__m256i *)out + idx, AO);              \
        }                                                               \
        for (size_t j = 0; j < BITSET_CONTAINER_FN_REPEAT; ++j) {                           \
            const int idx = (i * BITSET_CONTAINER_FN_REPEAT * WORDS_IN_AVX2_REG);           \
            sum += _mm_popcnt_u64(out[idx + j * 4]);                    \
            sum += _mm_popcnt_u64(out[idx + j * 4 + 1]);                \
            sum += _mm_popcnt_u64(out[idx + j * 4 + 2]);                \
            sum += _mm_popcnt_u64(out[idx + j * 4 + 3]);                \
        }                                                               \
    }                                                                   \
    dst->cardinality = sum;                                             \
    return dst->cardinality;                                            \
}

#endif //USEPOPCNT

#else /* not USEAVX  */

#define BITSET_CONTAINER_FN(opname, opsymbol, avxintrinsic)               \
int bitset_container_##opname(const bitset_container_t *src_1,            \
                              const bitset_container_t *src_2,            \
                              bitset_container_t *dst) {                  \
    const uint64_t *array_1 = src_1->array;                               \
    const uint64_t *array_2 = src_2->array;                               \
    uint64_t *out = dst->array;                                           \
    int32_t sum = 0;                                                      \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 2) {      \
        const uint64_t word_1 = (array_1[i])opsymbol(array_2[i]),         \
                       word_2 = (array_1[i + 1])opsymbol(array_2[i + 1]); \
        out[i] = word_1;                                                  \
        out[i + 1] = word_2;                                              \
        sum += _mm_popcnt_u64(word_1);                                    \
        sum += _mm_popcnt_u64(word_2);                                    \
    }                                                                     \
    dst->cardinality = sum;                                               \
    return dst->cardinality;                                              \
}                                                                         \
int bitset_container_##opname##_nocard(const bitset_container_t *src_1,   \
                                       const bitset_container_t *src_2,   \
                                       bitset_container_t *dst) {         \
    const uint64_t *array_1 = src_1->array, *array_2 = src_2->array;      \
    uint64_t *out = dst->array;                                           \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i++) {         \
        out[i] = (array_1[i])opsymbol(array_2[i]);                        \
    }                                                                     \
    dst->cardinality = -1;                                                \
    return dst->cardinality;                                              \
}
#endif

BITSET_CONTAINER_FN(or, |, _mm256_or_si256)
BITSET_CONTAINER_FN(and, &, _mm256_and_si256)
BITSET_CONTAINER_FN(xor, ^, _mm256_xor_si256)
BITSET_CONTAINER_FN(andnot, &~, _mm256_andnot_si256)


#ifdef USEAVX
#define USEAVX2FORDECODING// optimization
#endif

int bitset_container_to_uint32_array( uint32_t *out, const bitset_container_t *cont, uint32_t base) {
#ifdef USEAVX2FORDECODING
	if(cont->cardinality >= 8192)// heuristic
		return (int) bitset_extract_setbits_avx2(cont->array, BITSET_CONTAINER_SIZE_IN_WORDS, out,cont->cardinality,base);
	else
		return (int) bitset_extract_setbits(cont->array, BITSET_CONTAINER_SIZE_IN_WORDS, out,base);
#else
	return (int) bitset_extract_setbits(cont->array, BITSET_CONTAINER_SIZE_IN_WORDS, out,base);
#endif
}

/*
 * Print this container using printf (useful for debugging).
 */
void bitset_container_printf(const bitset_container_t * v) {
	printf("{");
	uint32_t base = 0;
	bool iamfirst = true;// TODO: rework so that this is not necessary yet still readable
	for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i) {
		uint64_t w = v->array[i];
		while (w != 0) {
			uint64_t t = w & -w;
			int r = __builtin_ctzl(w);
			if(iamfirst) {// predicted to be false
				printf("%d",base + r);
				iamfirst = false;
			} else {
				printf(",%d",base + r);
			}
			base += 64;
			w ^= t;
		}
	}
	printf("}");
}


/*
 * Print this container using printf as a comma-separated list of 32-bit integers starting at base.
 */
void bitset_container_printf_as_uint32_array(const bitset_container_t * v, uint32_t base) {
	bool iamfirst = true;// TODO: rework so that this is not necessary yet still readable
	for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i) {
		uint64_t w = v->array[i];
		while (w != 0) {
			uint64_t t = w & -w;
			int r = __builtin_ctzl(w);
			if(iamfirst) {// predicted to be false
				printf("%d", r + base);
				iamfirst = false;
			} else {
				printf(",%d",r + base);
			}
			base += 64;
			w ^= t;
		}
	}
}




// clang-format On
