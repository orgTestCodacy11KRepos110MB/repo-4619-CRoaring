/*
 * utilasm.h
 *
 */

#ifndef INCLUDE_UTILASM_H_
#define INCLUDE_UTILASM_H_

#define ASMBITMANIPOPTIMIZATION// optimization flag


#define ASM_SHIFT_RIGHT(srcReg, bitsReg, destReg)       \
    __asm volatile ("shrx %1, %2, %0" :                 \
                    "=r" (destReg): /* write */    \
                    "r" (bitsReg),  /* read only */     \
                    "r" (srcReg)    /* read only */     \
                    )

#define ASM_INPLACESHIFT_RIGHT(srcReg, bitsReg)       \
    __asm volatile ("shrx %1, %0, %0" :                 \
                    "+r" (srcReg): /* read/write */    \
                    "r" (bitsReg)  /* read only */     \
                    )

#define ASM_SHIFT_LEFT(srcReg, bitsReg, destReg)       \
    __asm volatile ("shlx %1, %2, %0" :                 \
                    "=r" (destReg): /* write */    \
                    "r" (bitsReg),  /* read only */     \
                    "r" (srcReg)    /* read only */     \
                    )
// set bit at position testBit within testByte to 1 and
// copy cmovDst to cmovSrc if that bit was previously clear
#define ASM_SET_BIT_INC_WAS_CLEAR(testByte, testBit, count)     \
    __asm volatile ("bts %2, %0\n"                              \
                    "sbb $-1, %1\n" :                           \
                    "+r" (testByte), /* read/write */           \
                    "+r" (count) :   /* read/write */           \
                    "r" (testBit)    /* read only */            \
                    )

#define ASM_CLEAR_BIT_DEC_WAS_SET(testByte, testBit, count)     \
    __asm volatile ("btr %2, %0\n"                              \
                    "sbb $0, %1\n" :                            \
                    "+r" (testByte), /* read/write */           \
                    "+r" (count) :   /* read/write */           \
                    "r" (testBit)    /* read only */            \
                    )

#define ASM_BT64(testByte, testBit, count)                         \
__asm volatile("bt %2,%1\n"                                  \
		"sbb %0,%0":                                 \
        "=r" (count) :   /* write */           \
         "r" (testByte), /* read only */           \
		"r" (testBit)    /* read only */            \
        )



#endif /* INCLUDE_UTILASM_H_ */
