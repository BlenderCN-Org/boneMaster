/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kernel/kernel_jitter.h"
#include "util/util_hash.h"

CCL_NAMESPACE_BEGIN

/* Pseudo random numbers, uncomment this for debugging correlations. Only run
 * this single threaded on a CPU for repeatable results. */
//#define __DEBUG_CORRELATION__

/* High Dimensional Sobol.
 *
 * Multidimensional sobol with generator matrices. Dimension 0 and 1 are equal
 * to classic Van der Corput and Sobol sequences. */

#ifdef __SOBOL__

/* Skip initial numbers that for some dimensions have clear patterns that
 * don't cover the entire sample space. Ideally we would have a better
 * progressive pattern that doesn't suffer from this problem, because even
 * with this offset some dimensions are quite poor.
 */
#  define SOBOL_SKIP 64

/* The MSB of RNG stores whether dithering should be used.
 * If it is set, RNG[0:14] stores the x pixel coordinate and RNG[15:29] the y coordinate, RNG[30] is unused.
 * If it isn't set, RNG[0:15] and RNG[16:30] are directly used for scrambling (note that the second one only has 15 bits).
 *
 * This distinction is needed because some parts of the code hash the RNG to get multiple decorrelated samples (mainly branched path tracing).
 * That operation isn't well defined for the dithered scrambling, so the code falls back to the regular scrambling (see path_rng_hash).
*/
#define DITHER_MASK 0x80000000
#define DITHER_COORD_MASK 0x7fff
#define DITHER_Y_SHIFT 15

ccl_device uint sobol_dimension(KernelGlobals *kg, int index, int dimension)
{
  uint result = 0;
  uint i = index + SOBOL_SKIP;
  for (int j = 0, x; (x = find_first_set(i)); i >>= x) {
    j += x;
    //result ^= kernel_tex_fetch(__sobol_directions, 32 * dimension + j - 1);
    result ^= kernel_tex_fetch(__sample_pattern_lut, 32 * dimension + j);
  }
  return result;
}

#endif /* __SOBOL__ */

#define NUM_PJ_SAMPLES 64 * 64
#define NUM_PJ_PATTERNS 48

ccl_device_forceinline float path_rng_1D(
    KernelGlobals *kg, uint rng_hash, int sample, int num_samples, int dimension)
{
#ifdef __DEBUG_CORRELATION__
  return (float)drand48();
#endif
  if (kernel_data.integrator.sampling_pattern == SAMPLING_PATTERN_PMJ) {
    /* Fallback to random */
    /*
    if (sample > NUM_PJ_SAMPLES) {
      int p = rng_hash + dimension;
      return cmj_randfloat(sample, p);
    }
    uint tmp_rng = cmj_hash_simple(dimension, rng_hash);
    int index = ((dimension % NUM_PJ_PATTERNS) * NUM_PJ_SAMPLES + sample) * 2;
    return __uint_as_float(kernel_tex_fetch(__sample_pattern_lut, index) ^
                           (tmp_rng & 0x007fffff)) -
           1.0f;
    */
      float r;
      /* Fallback to random */
      if(sample > NUM_PJ_SAMPLES) {
        r = cmj_randfloat(sample, dimension);
      }
      int index = ((dimension % NUM_PJ_PATTERNS) * NUM_PJ_SAMPLES + sample) * 2 + (dimension & 1);
      r = __uint_as_float(kernel_tex_fetch(__sample_pattern_lut, index)) - 1.0f;
      /* Cranly-Patterson rotation using rng seed */
      float shift;

      if(kernel_data.integrator.dither_size > 0) {
        int size = kernel_data.integrator.dither_size;
        /* Extract the pixel coordinates from rng and wrap them into the dither matrix. */
        int x = (rng_hash & DITHER_COORD_MASK) % size;
        int y = ((rng_hash >> DITHER_Y_SHIFT) & DITHER_COORD_MASK) % size;
        float2 shifts = kernel_tex_fetch(__sobol_dither, y*size + x);
        shift = (dimension & 1)? shifts.y: shifts.x;
      }
      else {
        /* Hash rng with dimension to solve correlation issues.
         * See T38710, T50116.
         */
        uint tmp_rng = cmj_hash_simple(dimension, rng_hash);
        shift = tmp_rng * (1.0f/(float)0xFFFFFFFF);
      }

      shift *= kernel_data.integrator.scrambling_distance;

      return r + shift - floorf(r + shift);
  }
#ifdef __CMJ__
#  ifdef __SOBOL__
  if (kernel_data.integrator.sampling_pattern == SAMPLING_PATTERN_CMJ)
#  endif
  {
    /* Correlated multi-jitter. */
    int p = rng_hash + dimension;
    return cmj_sample_1D(sample, num_samples, p);
  }
#endif

#ifdef __SOBOL__
  /* Sobol sequence value using direction vectors. */
  uint result = sobol_dimension(kg, sample, dimension);
  float r = (float)result * (1.0f / (float)0xFFFFFFFF);

  /* Cranly-Patterson rotation using rng seed */
  float shift;

  /* Hash rng with dimension to solve correlation issues.
   * See T38710, T50116.
   */
  //uint tmp_rng = cmj_hash_simple(dimension, rng_hash);
  //shift = tmp_rng * (1.0f / (float)0xFFFFFFFF);
  //shift = tmp_rng * (kernel_data.integrator.scrambling_distance / (float)0xFFFFFFFF);
	if(kernel_data.integrator.dither_size > 0) {
		int size = kernel_data.integrator.dither_size;
		/* Extract the pixel coordinates from rng and wrap them into the dither matrix. */
		int x = (rng_hash & DITHER_COORD_MASK) % size;
		int y = ((rng_hash >> DITHER_Y_SHIFT) & DITHER_COORD_MASK) % size;
		float2 shifts = kernel_tex_fetch(__sobol_dither, y*size + x);
		shift = (dimension & 1)? shifts.y: shifts.x;
	}
	else {
		/* Hash rng with dimension to solve correlation issues.
		* See T38710, T50116.
		*/
		uint tmp_rng = cmj_hash_simple(dimension, rng_hash);
		shift = tmp_rng * (1.0f/(float)0xFFFFFFFF);
	}
  shift *= kernel_data.integrator.scrambling_distance;
  return r + shift - floorf(r + shift);
#endif
}

ccl_device_forceinline void path_rng_2D(KernelGlobals *kg,
                                        uint rng_hash,
                                        int sample,
                                        int num_samples,
                                        int dimension,
                                        float *fx,
                                        float *fy)
{
#ifdef __DEBUG_CORRELATION__
  *fx = (float)drand48();
  *fy = (float)drand48();
  return;
#endif
  if (kernel_data.integrator.sampling_pattern == SAMPLING_PATTERN_PMJ) {
    /*
    if (sample > NUM_PJ_SAMPLES) {
      int p = rng_hash + dimension;
      *fx = cmj_randfloat(sample, p);
      *fy = cmj_randfloat(sample, p + 1);
    }
    uint tmp_rng = cmj_hash_simple(dimension, rng_hash);
    int index = ((dimension % NUM_PJ_PATTERNS) * NUM_PJ_SAMPLES + sample) * 2;
    *fx = __uint_as_float(kernel_tex_fetch(__sample_pattern_lut, index) ^ (tmp_rng & 0x007fffff)) -
          1.0f;
    tmp_rng = cmj_hash_simple(dimension + 1, rng_hash);
    *fy = __uint_as_float(kernel_tex_fetch(__sample_pattern_lut, index + 1) ^
                          (tmp_rng & 0x007fffff)) -
          1.0f;
    return;
    */
     *fx = path_rng_1D(kg, rng_hash, sample, num_samples, dimension);
     *fy = path_rng_1D(kg, rng_hash, sample, num_samples, dimension + 1);
     return;
  }
#ifdef __CMJ__
#  ifdef __SOBOL__
  if (kernel_data.integrator.sampling_pattern == SAMPLING_PATTERN_CMJ)
#  endif
  {
    /* Correlated multi-jitter. */
    int p = rng_hash + dimension;
    cmj_sample_2D(sample, num_samples, p, fx, fy);
    return;
  }
#endif

#ifdef __SOBOL__
  /* Sobol. */
  *fx = path_rng_1D(kg, rng_hash, sample, num_samples, dimension);
  *fy = path_rng_1D(kg, rng_hash, sample, num_samples, dimension + 1);
#endif
}

ccl_device_inline void path_rng_init(KernelGlobals *kg,
                                     int sample,
                                     int num_samples,
                                     uint *rng_hash,
                                     int x,
                                     int y,
                                     float *fx,
                                     float *fy)
{
  /* load state */
	if(kernel_data.integrator.dither_size > 0) {
		*rng_hash = ((y & DITHER_COORD_MASK) << DITHER_Y_SHIFT) | (x & DITHER_COORD_MASK) | DITHER_MASK;
	}
	else {
    *rng_hash = hash_uint2(x, y);
    *rng_hash ^= kernel_data.integrator.seed;
	}

#ifdef __DEBUG_CORRELATION__
  srand48(*rng_hash + sample);
#endif

  if (sample == 0) {
    *fx = 0.5f;
    *fy = 0.5f;
  }
  else {
    path_rng_2D(kg, *rng_hash, sample, num_samples, PRNG_FILTER_U, fx, fy);
  }
}

/* Linear Congruential Generator */

ccl_device uint lcg_step_uint(uint *rng)
{
  /* implicit mod 2^32 */
  *rng = (1103515245 * (*rng) + 12345);
  return *rng;
}

ccl_device float lcg_step_float(uint *rng)
{
  /* implicit mod 2^32 */
  *rng = (1103515245 * (*rng) + 12345);
  return (float)*rng * (1.0f / (float)0xFFFFFFFF);
}

ccl_device uint lcg_init(uint seed)
{
  uint rng = seed;
  lcg_step_uint(&rng);
  return rng;
}

/* Path Tracing Utility Functions
 *
 * For each random number in each step of the path we must have a unique
 * dimension to avoid using the same sequence twice.
 *
 * For branches in the path we must be careful not to reuse the same number
 * in a sequence and offset accordingly.
 */

ccl_device_inline float path_state_rng_1D(KernelGlobals *kg,
                                          const ccl_addr_space PathState *state,
                                          int dimension)
{
  return path_rng_1D(
      kg, state->rng_hash, state->sample, state->num_samples, state->rng_offset + dimension);
}

ccl_device_inline void path_state_rng_2D(
    KernelGlobals *kg, const ccl_addr_space PathState *state, int dimension, float *fx, float *fy)
{
  path_rng_2D(kg,
              state->rng_hash,
              state->sample,
              state->num_samples,
              state->rng_offset + dimension,
              fx,
              fy);
}

ccl_device_inline float path_state_rng_1D_hash(KernelGlobals *kg,
                                               const ccl_addr_space PathState *state,
                                               uint hash)
{
	/* Use a hash instead of dimension, this is not great but avoids adding
	 * more dimensions to each bounce which reduces quality of dimensions we
	 * are already using. */
	return path_rng_1D(kg,
	                   cmj_hash_simple(state->rng_hash, hash),
	                   state->sample,
                     state->num_samples,
	                   state->rng_offset);
}

ccl_device_inline float path_branched_rng_1D(KernelGlobals *kg,
                                             uint rng_hash,
                                             const ccl_addr_space PathState *state,
                                             int branch,
                                             int num_branches,
                                             int dimension)
{
  return path_rng_1D(kg,
                     rng_hash,
                     state->sample * num_branches + branch,
                     state->num_samples * num_branches,
                     state->rng_offset + dimension);
}

ccl_device_inline void path_branched_rng_2D(KernelGlobals *kg,
                                            uint rng_hash,
                                            const ccl_addr_space PathState *state,
                                            int branch,
                                            int num_branches,
                                            int dimension,
                                            float *fx,
                                            float *fy)
{
  path_rng_2D(kg,
              rng_hash,
              state->sample * num_branches + branch,
              state->num_samples * num_branches,
              state->rng_offset + dimension,
              fx,
              fy);
}

/* Utility functions to get light termination value,
 * since it might not be needed in many cases.
 */
ccl_device_inline float path_state_rng_light_termination(KernelGlobals *kg,
                                                         const ccl_addr_space PathState *state)
{
  if (kernel_data.integrator.light_inv_rr_threshold > 0.0f) {
    return path_state_rng_1D(kg, state, PRNG_LIGHT_TERMINATE);
  }
  return 0.0f;
}

ccl_device_inline float path_branched_rng_light_termination(KernelGlobals *kg,
                                                            uint rng_hash,
                                                            const ccl_addr_space PathState *state,
                                                            int branch,
                                                            int num_branches)
{
  if (kernel_data.integrator.light_inv_rr_threshold > 0.0f) {
    return path_branched_rng_1D(kg, rng_hash, state, branch, num_branches, PRNG_LIGHT_TERMINATE);
  }
  return 0.0f;
}

ccl_device_inline uint lcg_state_init(PathState *state, uint scramble)
{
	return lcg_init(state->rng_hash + state->rng_offset + state->sample*scramble);
}

ccl_device_inline uint lcg_state_init_addrspace(ccl_addr_space PathState *state, uint scramble)
{
	return lcg_init(state->rng_hash + state->rng_offset + state->sample*scramble);
}

ccl_device float lcg_step_float_addrspace(ccl_addr_space uint *rng)
{
  /* Implicit mod 2^32 */
  *rng = (1103515245 * (*rng) + 12345);
  return (float)*rng * (1.0f / (float)0xFFFFFFFF);
}

ccl_device_inline bool sample_is_even(int pattern, int sample)
{
  if (pattern == SAMPLING_PATTERN_PMJ) {
    /* See Section 10.2.1, "Progressive Multi-Jittered Sample Sequences", Christensen et al.
     * We can use this to get divide sample sequence into two classes for easier variance estimation.
     * There must be a more elegant way of writing this? */
    return (bool)(sample & 2) ^ (bool)(sample & 8) ^ (bool)(sample & 0x20) ^
           (bool)(sample & 0x80) ^ (bool)(sample & 0x200) ^ (bool)(sample & 0x800) ^
           (bool)(sample & 0x2000) ^ (bool)(sample & 0x8000) ^ (bool)(sample & 0x20000) ^
           (bool)(sample & 0x80000) ^ (bool)(sample & 0x200000) ^ (bool)(sample & 0x800000) ^
           (bool)(sample & 0x2000000) ^ (bool)(sample & 0x8000000) ^ (bool)(sample & 0x20000000) ^
           (bool)(sample & 0x80000000);
  }
  else {
    /* TODO: Are there reliable ways of dividing CMJ and Sobol into two classes? */
    return sample & 0x1;
  }
}


ccl_device_inline uint path_rng_hash(uint rng_hash, int i)
{
	/* Fall back to the regular scrambling after hashing. */
	return cmj_hash(rng_hash, i) & (~DITHER_MASK);
}

CCL_NAMESPACE_END
