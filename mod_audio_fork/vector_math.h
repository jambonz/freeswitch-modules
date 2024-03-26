#ifndef VECTOR_MATH_H
#define VECTOR_MATH_H

#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

void vector_add(int16_t* a, int16_t* b, size_t len);
void vector_normalize(int16_t* a, size_t len);
void vector_change_sln_volume_granular(int16_t* data, uint32_t samples, int32_t vol);

#ifdef __cplusplus
}
#endif

#endif
