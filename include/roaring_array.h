#ifndef INCLUDE_ROARING_ARRAY_H
#define INCLUDE_ROARING_ARRAY_H

#include <stdint.h>
#include "containers.h"

#define MAX_CONTAINERS 65536

/**
 * Roaring arrays are array-based key-value pairs having containers as values
 * and 16-bit integer keys. A roaring bitmap  might be implemented as such.
 */


// parallel arrays.  Element sizes quite different.
// Alternative is array
// of structs.  Which would have better
// cache performance through binary searches?

typedef struct roaring_array_s {
  int32_t size;
  int32_t allocation_size;
  uint16_t *keys;
  void **containers;
  uint8_t *typecodes;
} roaring_array_t;


/**
 * Create a new roaring array
 */
roaring_array_t *ra_create();


/**
 * Copies this roaring array (caller is responsible for memory management)
 */
roaring_array_t * ra_copy(roaring_array_t *r);

/**
 * Frees the memory used by a roaring array
 */
void ra_free(roaring_array_t *r);

/**
 * Get the index corresponding to a 16-bit key
 */
int32_t ra_get_index( roaring_array_t *ra, uint16_t x);

/**
 * Retrieves the container at index i, filling in the typecode
 */
void *ra_get_container_at_index(roaring_array_t *ra, uint16_t i, uint8_t *typecode);

/**
 * Retrieves the key at index i
 */
uint16_t ra_get_key_at_index(roaring_array_t *ra, uint16_t i);

/**
 * Add a new key-value pair at index i
 */
void ra_insert_new_key_value_at( roaring_array_t *ra, int32_t i, uint16_t key, void *container, uint8_t typecode);

/**
 * Append a new key-value pair
 */
void ra_append( roaring_array_t *ra, uint16_t s, void *c, uint8_t typecode);


/**
 * Append a new key-value pair to ra, cloning a value from sa at index index
 */
void ra_append_copy(roaring_array_t *ra, roaring_array_t *sa, uint16_t index) ;



/**
 * Append new key-value pairs to ra, cloning  values from sa at indexes [start_index, uint16_t end_index)
 */
void ra_append_copy_range(roaring_array_t *ra, roaring_array_t *sa,
                       uint16_t start_index, uint16_t end_index) ;



/**
 * Set the container at the corresponding index using the specified typecode.
 */
void ra_set_container_at_index(roaring_array_t *ra, int32_t i, void *c, uint8_t typecode);

#endif
