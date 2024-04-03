#include "vector_math.h"
#include <assert.h>
#include <string.h>
#include <cstdlib>

#define GRANULAR_VOLUME_MAX (50)
#define SMAX 32767
#define SMIN (-32768)
#define normalize_to_16bit_basic(n) if (n > SMAX) n = SMAX; else if (n < SMIN) n = SMIN;
#define normalize_volume_granular(x) if (x > GRANULAR_VOLUME_MAX) x = GRANULAR_VOLUME_MAX; if (x < -GRANULAR_VOLUME_MAX) x = -GRANULAR_VOLUME_MAX;

#ifdef __cplusplus
extern "C" {
#endif

#if defined(USE_AVX2)
#include <immintrin.h>
#pragma message("Using AVX2 SIMD.")
void vector_add(int16_t* a, int16_t* b, size_t len) {
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
void vector_normalize(int16_t* a, size_t len) {
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
typedef union {
    int16_t* data;
    __m256i* fp_avx2;
} vector_data_t;

void vector_change_sln_volume_granular(int16_t* data, uint32_t samples, int32_t vol) {
  float newrate = 0;
  static const float pos[GRANULAR_VOLUME_MAX] = {
		1.122018,   1.258925,   1.412538,   1.584893,   1.778279,   1.995262,   2.238721,   2.511887,   2.818383,   3.162278,
		3.548134,   3.981072,   4.466835,   5.011872,   5.623413,   6.309574,   7.079458,   7.943282,   8.912509,  10.000000,
		11.220183,  12.589254,  14.125375,  15.848933,  17.782795,  19.952621,  22.387213,  25.118862,  28.183832,  31.622776,
		35.481335,  39.810719,  44.668358,  50.118729,  56.234131,  63.095726,  70.794586,  79.432816,  89.125107, 100.000000,
		112.201836, 125.892517, 141.253784, 158.489334, 177.827942, 199.526215, 223.872070, 251.188705, 281.838318, 316.227753
  };
  static const float neg[GRANULAR_VOLUME_MAX] = {
		0.891251, 0.794328, 0.707946, 0.630957, 0.562341, 0.501187, 0.446684, 0.398107, 0.354813, 0.316228,
		0.281838, 0.251189, 0.223872, 0.199526, 0.177828, 0.158489, 0.141254, 0.125893, 0.112202, 0.100000,
		0.089125, 0.079433, 0.070795, 0.063096, 0.056234, 0.050119, 0.044668, 0.039811, 0.035481, 0.031623,
		0.028184, 0.025119, 0.022387, 0.019953, 0.017783, 0.015849, 0.014125, 0.012589, 0.011220, 0.010000,
		0.008913, 0.007943, 0.007079, 0.006310, 0.005623, 0.005012, 0.004467, 0.003981, 0.003548, 0.000000  // NOTE mapped -50 dB ratio to total silence instead of 0.003162
  };
  const float* chart;
  uint32_t i = abs(vol) - 1;

  if (vol == 0) return;
  normalize_volume_granular(vol);

  chart = vol > 0 ? pos : neg;

  newrate = chart[i];

  if (newrate) {
    __m256 scale_factor_reg = _mm256_set1_ps(newrate);
    uint32_t processed_samples = samples - (samples % 8); // Ensure we process only multiples of 8
    for (uint32_t i = 0; i < processed_samples; i += 8) {
      __m128i data_ = _mm_loadu_si128((__m128i*)(data + i));
      __m256i data_32 = _mm256_cvtepi16_epi32(data_);

      __m256 data_float = _mm256_cvtepi32_ps(data_32);
      __m256 result = _mm256_mul_ps(data_float, scale_factor_reg);

      __m256i result_32 = _mm256_cvtps_epi32(result);

      // Handle saturation
      __m256i min_val = _mm256_set1_epi32(SMIN);
      __m256i max_val = _mm256_set1_epi32(SMAX);
      result_32 = _mm256_min_epi32(result_32, max_val);
      result_32 = _mm256_max_epi32(result_32, min_val);

      __m128i result_16 = _mm_packs_epi32(_mm256_castsi256_si128(result_32), _mm256_extractf128_si256(result_32, 1));

      _mm_storeu_si128((__m128i*)(data + i), result_16);
    }

    // Process any remaining samples
    for (uint32_t i = processed_samples; i < samples; i++) {
      int32_t tmp = (int32_t)(data[i] * newrate);
      tmp = tmp > SMAX ? SMAX : (tmp < SMIN ? SMIN : tmp);
      data[i] = (int16_t)tmp;
    }
  }
}
#elif defined(USE_SSE2)
#include <emmintrin.h>
#pragma message("Using SSE2 SIMD.")
void vector_add(int16_t* a, int16_t* b, size_t len) {
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
void vector_normalize(int16_t* a, size_t len) {
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

typedef union {
    int16_t* data;
    __m128i* fp_sse2;
} vector_data_t;

#else
#pragma message("Building without vector math support")
void vector_add(int16_t* a, int16_t* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        a[i] += b[i];
    }
}
void vector_normalize(int16_t* a, size_t len) {
  for (size_t i = 0; i < len; i++) {
    normalize_to_16bit_basic(a[i]);
  }
}
void vector_change_sln_volume_granular(int16_t* data, uint32_t samples, int32_t vol) {
  float newrate = 0;
  static const float pos[GRANULAR_VOLUME_MAX] = {
		1.122018,   1.258925,   1.412538,   1.584893,   1.778279,   1.995262,   2.238721,   2.511887,   2.818383,   3.162278,
		3.548134,   3.981072,   4.466835,   5.011872,   5.623413,   6.309574,   7.079458,   7.943282,   8.912509,  10.000000,
		11.220183,  12.589254,  14.125375,  15.848933,  17.782795,  19.952621,  22.387213,  25.118862,  28.183832,  31.622776,
		35.481335,  39.810719,  44.668358,  50.118729,  56.234131,  63.095726,  70.794586,  79.432816,  89.125107, 100.000000,
		112.201836, 125.892517, 141.253784, 158.489334, 177.827942, 199.526215, 223.872070, 251.188705, 281.838318, 316.227753
  };
  static const float neg[GRANULAR_VOLUME_MAX] = {
		0.891251, 0.794328, 0.707946, 0.630957, 0.562341, 0.501187, 0.446684, 0.398107, 0.354813, 0.316228,
		0.281838, 0.251189, 0.223872, 0.199526, 0.177828, 0.158489, 0.141254, 0.125893, 0.112202, 0.100000,
		0.089125, 0.079433, 0.070795, 0.063096, 0.056234, 0.050119, 0.044668, 0.039811, 0.035481, 0.031623,
		0.028184, 0.025119, 0.022387, 0.019953, 0.017783, 0.015849, 0.014125, 0.012589, 0.011220, 0.010000,
		0.008913, 0.007943, 0.007079, 0.006310, 0.005623, 0.005012, 0.004467, 0.003981, 0.003548, 0.000000  // NOTE mapped -50 dB ratio to total silence instead of 0.003162
  };
  const float* chart;
  uint32_t i;

  if (vol == 0) return;

  normalize_volume_granular(vol);

  chart = vol > 0 ? pos : neg;

  i = abs(vol) - 1;
  assert(i < GRANULAR_VOLUME_MAX);
  newrate = chart[i];
  if (newrate) {
    int32_t tmp;
    uint32_t x;
    int16_t *fp = data;

    for (x = 0; x < samples; x++) {
      tmp = (int32_t) (fp[x] * newrate);
      normalize_to_16bit_basic(tmp);
      fp[x] = (int16_t) tmp;
    }
  }
}

#endif

#ifdef __cplusplus
}
#endif
