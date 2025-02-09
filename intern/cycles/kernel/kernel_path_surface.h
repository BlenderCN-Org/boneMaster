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

CCL_NAMESPACE_BEGIN

#if defined(__BRANCHED_PATH__) || defined(__SUBSURFACE__) || defined(__SHADOW_TRICKS__) || \
    defined(__BAKING__)
/* branched path tracing: connect path directly to position on one or more lights and add it to L
 */
ccl_device_noinline_cpu void kernel_branched_path_surface_connect_light(
    KernelGlobals *kg,
    ShaderData *sd,
    ShaderData *emission_sd,
    ccl_addr_space PathState *state,
    float3 throughput,
    float num_samples_adjust,
    PathRadiance *L,
    int sample_all_lights)
{
#  ifdef __EMISSION__
  /* sample illumination from lights to find path contribution */
  BsdfEval L_light ccl_optional_struct_init;

  int num_lights = 0;
  if (kernel_data.integrator.use_direct_light) {
    if (sample_all_lights) {
      num_lights = kernel_data.integrator.num_all_lights;
      if (kernel_data.integrator.pdf_triangles != 0.0f) {
        num_lights += 1;
      }
    }
    else {
      num_lights = 1;
    }
  }

  for (int i = 0; i < num_lights; i++) {
    /* sample one light at random */
    int num_samples = 1;
    int num_all_lights = 1;
    uint lamp_rng_hash = state->rng_hash;
    bool double_pdf = false;
    bool is_mesh_light = false;
    bool is_lamp = false;

    if (sample_all_lights) {
      /* lamp sampling */
      is_lamp = i < kernel_data.integrator.num_all_lights;
      if (is_lamp) {
        if (UNLIKELY(light_select_reached_max_bounces(kg, i, state->bounce))) {
          continue;
        }
        num_samples = ceil_to_int(num_samples_adjust * light_select_num_samples(kg, i));
        num_all_lights = kernel_data.integrator.num_all_lights;
        //lamp_rng_hash = cmj_hash(state->rng_hash, i);
        lamp_rng_hash = path_rng_hash(state->rng_hash, i);
        double_pdf = kernel_data.integrator.pdf_triangles != 0.0f;
      }
      /* mesh light sampling */
      else {
        num_samples = ceil_to_int(num_samples_adjust * kernel_data.integrator.mesh_light_samples);
        double_pdf = kernel_data.integrator.num_all_lights != 0;
        is_mesh_light = true;
      }
    }

    float num_samples_inv = num_samples_adjust / (num_samples * num_all_lights);

    for (int j = 0; j < num_samples; j++) {
      Ray light_ray ccl_optional_struct_init;
      light_ray.t = 0.0f; /* reset ray */
#    ifdef __OBJECT_MOTION__
      light_ray.time = sd->time;
#    endif
      bool has_emission = false;

      if (kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL)) {
        float light_u, light_v;
        path_branched_rng_2D(
            kg, lamp_rng_hash, state, j, num_samples, PRNG_LIGHT_U, &light_u, &light_v);
        float terminate = path_branched_rng_light_termination(
            kg, lamp_rng_hash, state, j, num_samples);

        /* only sample triangle lights */
        if (is_mesh_light && double_pdf) {
          light_u = 0.5f * light_u;
        }

        LightSample ls ccl_optional_struct_init;
        const int lamp = is_lamp ? i : -1;
        if (light_sample(kg, lamp, light_u, light_v, sd->time, sd->P, state->bounce, &ls)) {
          /* The sampling probability returned by lamp_light_sample assumes that all lights were
           * sampled. However, this code only samples lamps, so if the scene also had mesh lights,
           * the real probability is twice as high. */
          if (double_pdf) {
            ls.pdf *= 2.0f;
          }

          has_emission = direct_emission(
              kg, sd, emission_sd, &ls, state, &light_ray, &L_light, &is_lamp, terminate);
        }
        /* trace shadow ray */
        float3 shadow;

        const bool blocked = shadow_blocked(kg, sd, emission_sd, state, &light_ray, &shadow);

        if (has_emission) {

          
          if (!blocked) {
            /* accumulate */
            path_radiance_accum_light(
                kg, L, state, throughput * num_samples_inv, &L_light, shadow, num_samples_inv, ls.lamp, is_lamp);
          }
          else {
            path_radiance_accum_total_light(L, state, throughput * num_samples_inv, &L_light);
          }
        }        
      }


    }
  }
#  endif
}

/* branched path tracing: bounce off or through surface to with new direction stored in ray */
ccl_device bool kernel_branched_path_surface_bounce(KernelGlobals *kg,
                                                    ShaderData *sd,
                                                    const ShaderClosure *sc,
                                                    int sample,
                                                    int num_samples,
                                                    ccl_addr_space float3 *throughput,
                                                    ccl_addr_space PathState *state,
                                                    PathRadianceState *L_state,
                                                    ccl_addr_space Ray *ray,
                                                    float sum_sample_weight)
{
  /* sample BSDF */
  float bsdf_pdf;
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float3 bsdf_omega_in ccl_optional_struct_init;
  differential3 bsdf_domega_in ccl_optional_struct_init;
  float bsdf_u, bsdf_v;
  path_branched_rng_2D(
      kg, state->rng_hash, state, sample, num_samples, PRNG_BSDF_U, &bsdf_u, &bsdf_v);
  int label;

  label = shader_bsdf_sample_closure(
      kg, sd, sc, bsdf_u, bsdf_v, &bsdf_eval, &bsdf_omega_in, &bsdf_domega_in, &bsdf_pdf);

  if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval))
    return false;

  /* modify throughput */
  path_radiance_bsdf_bounce(kg, L_state, throughput, &bsdf_eval, bsdf_pdf, state->bounce, label);

#  ifdef __DENOISING_FEATURES__
  state->denoising_feature_weight *= sc->sample_weight / (sum_sample_weight * num_samples);
#  endif

  /* modify path state */
  path_state_next(kg, state, label);

  /* setup ray */
  ray->P = ray_offset(sd->P, (label & LABEL_TRANSMIT) ? -sd->Ng : sd->Ng);
  ray->D = normalize(bsdf_omega_in);
  ray->t = FLT_MAX;
#  ifdef __RAY_DIFFERENTIALS__
  ray->dP = sd->dP;
  ray->dD = bsdf_domega_in;
#  endif
#  ifdef __OBJECT_MOTION__
  ray->time = sd->time;
#  endif

#  ifdef __VOLUME__
  /* enter/exit volume */
  if (label & LABEL_TRANSMIT)
    kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
#  endif

  /* branch RNG state */
  path_state_branch(state, sample, num_samples);

  /* set MIS state */
  state->min_ray_pdf = fminf(bsdf_pdf, FLT_MAX);
  state->ray_pdf = bsdf_pdf;
#  ifdef __LAMP_MIS__
  state->ray_t = 0.0f;
#  endif

  return true;
}

#endif

/* path tracing: connect path directly to position on a light and add it to L */
ccl_device_inline void kernel_path_surface_connect_light(KernelGlobals *kg,
                                                         ShaderData *sd,
                                                         ShaderData *emission_sd,
                                                         float3 throughput,
                                                         ccl_addr_space PathState *state,
                                                         PathRadiance *L)
{
  PROFILING_INIT(kg, PROFILING_CONNECT_LIGHT);

#ifdef __EMISSION__
#  ifdef __SHADOW_TRICKS__
  int all = (state->flag & PATH_RAY_SHADOW_CATCHER);
  kernel_branched_path_surface_connect_light(kg, sd, emission_sd, state, throughput, 1.0f, L, all);
#  else
  /* sample illumination from lights to find path contribution */
  Ray light_ray ccl_optional_struct_init;
  BsdfEval L_light ccl_optional_struct_init;
  bool is_lamp = false;
  bool has_emission = false;

  light_ray.t = 0.0f;
#    ifdef __OBJECT_MOTION__
  light_ray.time = sd->time;
#    endif

  if (kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL)) {
    float light_u, light_v;
    path_state_rng_2D(kg, state, PRNG_LIGHT_U, &light_u, &light_v);

    LightSample ls ccl_optional_struct_init;
    if (light_sample(kg, -1, light_u, light_v, sd->time, sd->P, state->bounce, &ls)) {
      float terminate = path_state_rng_light_termination(kg, state);
      has_emission = direct_emission(
          kg, sd, emission_sd, &ls, state, &light_ray, &L_light, &is_lamp, terminate);
    }
    /* trace shadow ray */
    float3 shadow;

    const bool blocked = shadow_blocked(kg, sd, emission_sd, state, &light_ray, &shadow);

    if (has_emission) {
      if (!blocked) {
        /* accumulate */
        path_radiance_accum_light(kg, L, state, throughput, &L_light, shadow, 1.0f, ls.lamp, is_lamp);
      }
      else {
        path_radiance_accum_total_light(L, state, throughput, &L_light);
      }
    }    
  }


#  endif
#endif
}

/* path tracing: bounce off or through surface to with new direction stored in ray */
ccl_device bool kernel_path_surface_bounce(KernelGlobals *kg,
                                           ShaderData *sd,
                                           ccl_addr_space float3 *throughput,
                                           ccl_addr_space PathState *state,
                                           PathRadianceState *L_state,
                                           ccl_addr_space Ray *ray)
{
  PROFILING_INIT(kg, PROFILING_SURFACE_BOUNCE);

  /* no BSDF? we can stop here */
  if (sd->flag & SD_BSDF) {
    /* sample BSDF */
    float bsdf_pdf;
    BsdfEval bsdf_eval ccl_optional_struct_init;
    float3 bsdf_omega_in ccl_optional_struct_init;
    differential3 bsdf_domega_in ccl_optional_struct_init;
    float bsdf_u, bsdf_v;
    path_state_rng_2D(kg, state, PRNG_BSDF_U, &bsdf_u, &bsdf_v);
    int label;

    label = shader_bsdf_sample(
        kg, sd, bsdf_u, bsdf_v, &bsdf_eval, &bsdf_omega_in, &bsdf_domega_in, &bsdf_pdf);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval))
      return false;

    /* modify throughput */
    path_radiance_bsdf_bounce(kg, L_state, throughput, &bsdf_eval, bsdf_pdf, state->bounce, label);

    /* set labels */
    if (!(label & LABEL_TRANSPARENT)) {
      state->ray_pdf = bsdf_pdf;
#ifdef __LAMP_MIS__
      state->ray_t = 0.0f;
#endif
      state->min_ray_pdf = fminf(bsdf_pdf, state->min_ray_pdf);
    }

    /* update path state */
    path_state_next(kg, state, label);

    /* setup ray */
    ray->P = ray_offset(sd->P, (label & LABEL_TRANSMIT) ? -sd->Ng : sd->Ng);
    ray->D = normalize(bsdf_omega_in);

    if (state->bounce == 0)
      ray->t -= sd->ray_length; /* clipping works through transparent */
    else
      ray->t = FLT_MAX;

#ifdef __RAY_DIFFERENTIALS__
    ray->dP = sd->dP;
    ray->dD = bsdf_domega_in;
#endif

#ifdef __VOLUME__
    /* enter/exit volume */
    if (label & LABEL_TRANSMIT)
      kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
#endif
    return true;
  }
#ifdef __VOLUME__
  else if (sd->flag & SD_HAS_ONLY_VOLUME) {
    if (!path_state_volume_next(kg, state)) {
      return false;
    }

    if (state->bounce == 0)
      ray->t -= sd->ray_length; /* clipping works through transparent */
    else
      ray->t = FLT_MAX;

    /* setup ray position, direction stays unchanged */
    ray->P = ray_offset(sd->P, -sd->Ng);
#  ifdef __RAY_DIFFERENTIALS__
    ray->dP = sd->dP;
#  endif

    /* enter/exit volume */
    kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
    return true;
  }
#endif
  else {
    /* no bsdf or volume? */
    return false;
  }
}

CCL_NAMESPACE_END
