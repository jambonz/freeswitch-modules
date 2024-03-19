#ifndef VECTOR_MATH_H
#define VECTOR_MATH_H

#include <stddef.h>
#include <stdint.h>

#define SMAX 32767
#define SMIN (-32768)
#define normalize_to_16bit_basic(n) if (n > SMAX) n = SMAX; else if (n < SMIN) n = SMIN;

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__AVX2__)
#include <immintrin.h>
#pragma message("Using AVX2 implementation for vector_add.")
static inline void vector_add(int16_t* a, int16_t* b, size_t len) {
    size_t i = 0;
    for (; i + 15 < len; i += 16) {
        __m256i va = _mm256_loadu_si256((const __m256i*)(a + i));
        __m256i vb = _mm256_loadu_si256((const __m256i*)(b + i));
        __m256i vc = _mm256_add_epi16(va, vb);
        _mm256_storeu_si256((__m256i*)(a + i), vc);
    }
    for (; i < len; ++i) {
        a[i] += b[i];
    }
}
static void vector_normalize(int16_t* a, size_t len) {
  __m256i max_val = _mm256_set1_epi16(SMAX);
  __m256i min_val = _mm256_set1_epi16(SMIN);

  size_t i = 0;
  for (; i + 15 < len; i += 16) {
      __m256i values = _mm256_loadu_si256((__m256i*)(a + i));
      __m256i gt_max = _mm256_cmpgt_epi16(values, max_val);
      __m256i lt_min = _mm256_cmpgt_epi16(min_val, values);
      values = _mm256_blendv_epi8(values, max_val, gt_max);
      values = _mm256_blendv_epi8(values, min_val, lt_min);
      _mm256_storeu_si256((__m256i*)(a + i), values);
  }

  // Process remaining elements
  for (; i < len; ++i) {
      if (a[i] > SMAX) a[i] = SMAX;
      else if (a[i] < SMIN) a[i] = SMIN;
  }
}
#elif defined(__SSE2__)
#include <emmintrin.h>
#pragma message("Using SSE2 implementation for vector_add.")
static inline void vector_add(int16_t* a, int16_t* b, size_t len) {
  size_t i = 0;
  for (; i + 7 < len; i += 8) {
    __m128i va = _mm_loadu_si128((const __m128i*)(a + i));
    __m128i vb = _mm_loadu_si128((const __m128i*)(b + i));
    __m128i vc = _mm_add_epi16(va, vb);
    _mm_storeu_si128((__m128i*)(a + i), vc);
  }
  for (; i < len; ++i) {
    a[i] += b[i];
  }
}
static void vector_normalize(int16_t* a, size_t len) {
  __m128i max_val = _mm_set1_epi16(SMAX);
  __m128i min_val = _mm_set1_epi16(SMIN);

  size_t i = 0;
  for (; i + 7 < len; i += 8) {
    __m128i values = _mm_loadu_si128((__m128i*)(a + i));
    __m128i gt_max = _mm_cmpgt_epi16(values, max_val);
    __m128i lt_min = _mm_cmpgt_epi16(min_val, values);
    __m128i max_masked = _mm_and_si128(gt_max, max_val);
    __m128i min_masked = _mm_and_si128(lt_min, min_val);
    __m128i other_masked = _mm_andnot_si128(_mm_or_si128(gt_max, lt_min), values);
    values = _mm_or_si128(_mm_or_si128(max_masked, min_masked), other_masked);
    _mm_storeu_si128((__m128i*)(a + i), values);
  }

  // Process remaining elements
  for (; i < len; ++i) {
      if (a[i] > SMAX) a[i] = SMAX;
      else if (a[i] < SMIN) a[i] = SMIN;
  }
}
#else
#pragma message("Using basic loop implementation for vector_add.")
static inline void vector_add(int16_t* a, int16_t* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        a[i] += b[i];
    }
}
static inline void vector_normalize(int16_t* a, size_t len) {
  for (size_t i = 0; i < len; i++) {
    normalize_to_16bit_basic(a[i]);
  }
}
#endif

#ifdef __cplusplus
}
#endif

#endif
