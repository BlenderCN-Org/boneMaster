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

#include "device/device.h"
#include "render/background.h"
#include "render/integrator.h"
#include "render/film.h"
#include "render/jitter.h"
#include "render/light.h"
#include "render/scene.h"
#include "render/shader.h"
#include "render/sobol.h"

#include "util/util_foreach.h"
#include "util/util_logging.h"
#include "util/util_hash.h"

CCL_NAMESPACE_BEGIN

NODE_DEFINE(Integrator)
{
  NodeType *type = NodeType::add("integrator", create);

  SOCKET_INT(min_bounce, "Min Bounce", 0);
  SOCKET_INT(max_bounce, "Max Bounce", 7);

  SOCKET_INT(max_diffuse_bounce, "Max Diffuse Bounce", 7);
  SOCKET_INT(max_glossy_bounce, "Max Glossy Bounce", 7);
  SOCKET_INT(max_transmission_bounce, "Max Transmission Bounce", 7);
  SOCKET_INT(max_volume_bounce, "Max Volume Bounce", 7);

  SOCKET_INT(transparent_min_bounce, "Transparent Min Bounce", 0);
  SOCKET_INT(transparent_max_bounce, "Transparent Max Bounce", 7);

  SOCKET_INT(ao_bounces, "AO Bounces", 0);

  SOCKET_INT(volume_max_steps, "Volume Max Steps", 1024);
  SOCKET_FLOAT(volume_step_size, "Volume Step Size", 0.1f);

  SOCKET_BOOLEAN(caustics_reflective, "Reflective Caustics", true);
  SOCKET_BOOLEAN(caustics_refractive, "Refractive Caustics", true);
  SOCKET_FLOAT(filter_glossy, "Filter Glossy", 0.0f);
  SOCKET_INT(seed, "Seed", 0);
  SOCKET_FLOAT(sample_clamp_direct, "Sample Clamp Direct", 0.0f);
  SOCKET_FLOAT(sample_clamp_indirect, "Sample Clamp Indirect", 0.0f);
  SOCKET_BOOLEAN(motion_blur, "Motion Blur", false);

  SOCKET_INT(aa_samples, "AA Samples", 0);
  SOCKET_INT(diffuse_samples, "Diffuse Samples", 1);
  SOCKET_INT(glossy_samples, "Glossy Samples", 1);
  SOCKET_INT(transmission_samples, "Transmission Samples", 1);
  SOCKET_INT(ao_samples, "AO Samples", 1);
  SOCKET_INT(mesh_light_samples, "Mesh Light Samples", 1);
  SOCKET_INT(subsurface_samples, "Subsurface Samples", 1);
  SOCKET_INT(volume_samples, "Volume Samples", 1);
  SOCKET_INT(start_sample, "Start Sample", 0);

  SOCKET_FLOAT(adaptive_threshold, "Adaptive Threshold", 0.0f);
  SOCKET_INT(adaptive_min_samples, "Adaptive Min Samples", 0);

  SOCKET_BOOLEAN(sample_all_lights_direct, "Sample All Lights Direct", true);
  SOCKET_BOOLEAN(sample_all_lights_indirect, "Sample All Lights Indirect", true);
  SOCKET_FLOAT(light_sampling_threshold, "Light Sampling Threshold", 0.05f);

  SOCKET_UINT(background_lightgroups, "Background Lightgroups", 0);

  static NodeEnum method_enum;
  method_enum.insert("path", PATH);
  method_enum.insert("branched_path", BRANCHED_PATH);
  SOCKET_ENUM(method, "Method", method_enum, PATH);

  static NodeEnum sampling_pattern_enum;
  sampling_pattern_enum.insert("sobol", SAMPLING_PATTERN_SOBOL);
  sampling_pattern_enum.insert("cmj", SAMPLING_PATTERN_CMJ);
  sampling_pattern_enum.insert("pmj", SAMPLING_PATTERN_PMJ);
  SOCKET_ENUM(sampling_pattern, "Sampling Pattern", sampling_pattern_enum, SAMPLING_PATTERN_SOBOL);
  SOCKET_FLOAT(scrambling_distance, "Scrambling Distance", 1.0f);
  SOCKET_BOOLEAN(use_auto_scramble, "Auto Scramble", true);
  SOCKET_BOOLEAN(use_dithered_sampling, "Use Dithered Sampling", false);
  
  return type;
}

Integrator::Integrator() : Node(node_type)
{
  need_update = true;
}

Integrator::~Integrator()
{
}

void Integrator::device_update(Device *device, DeviceScene *dscene, Scene *scene)
{
  if (!need_update)
    return;

  device_free(device, dscene);

  KernelIntegrator *kintegrator = &dscene->data.integrator;

  /* integrator parameters */
  kintegrator->min_bounce = min_bounce + 1;
  kintegrator->max_bounce = max_bounce + 1;

  kintegrator->max_diffuse_bounce = max_diffuse_bounce + 1;
  kintegrator->max_glossy_bounce = max_glossy_bounce + 1;
  kintegrator->max_transmission_bounce = max_transmission_bounce + 1;
  kintegrator->max_volume_bounce = max_volume_bounce + 1;

  kintegrator->transparent_min_bounce = transparent_min_bounce + 1;
  kintegrator->transparent_max_bounce = transparent_max_bounce + 1;

  if (ao_bounces == 0) {
    kintegrator->ao_bounces = INT_MAX;
  }
  else {
    kintegrator->ao_bounces = ao_bounces - 1;
  }

  /* Transparent Shadows
   * We only need to enable transparent shadows, if we actually have
   * transparent shaders in the scene. Otherwise we can disable it
   * to improve performance a bit. */
  kintegrator->transparent_shadows = false;
  foreach (Shader *shader, scene->shaders) {
    /* keep this in sync with SD_HAS_TRANSPARENT_SHADOW in shader.cpp */
    if ((shader->has_surface_transparent && shader->use_transparent_shadow) ||
        shader->has_volume) {
      kintegrator->transparent_shadows = true;
      break;
    }
  }

  kintegrator->volume_max_steps = volume_max_steps;
  kintegrator->volume_step_size = volume_step_size;

  kintegrator->caustics_reflective = caustics_reflective;
  kintegrator->caustics_refractive = caustics_refractive;
  kintegrator->filter_glossy = (filter_glossy == 0.0f) ? FLT_MAX : 1.0f / filter_glossy;

  kintegrator->seed = hash_uint2(seed, 0);

  kintegrator->use_ambient_occlusion = ((Pass::contains(scene->film->passes, PASS_AO)) ||
                                        dscene->data.background.ao_factor != 0.0f);

  kintegrator->sample_clamp_direct = (sample_clamp_direct == 0.0f) ? FLT_MAX :
                                                                     sample_clamp_direct * 3.0f;
  kintegrator->sample_clamp_indirect = (sample_clamp_indirect == 0.0f) ?
                                           FLT_MAX :
                                           sample_clamp_indirect * 3.0f;

  kintegrator->branched = (method == BRANCHED_PATH);
  kintegrator->volume_decoupled = device->info.has_volume_decoupled;
  kintegrator->diffuse_samples = diffuse_samples;
  kintegrator->glossy_samples = glossy_samples;
  kintegrator->transmission_samples = transmission_samples;
  kintegrator->ao_samples = ao_samples;
  kintegrator->mesh_light_samples = mesh_light_samples;
  kintegrator->subsurface_samples = subsurface_samples;
  kintegrator->volume_samples = volume_samples;
  kintegrator->start_sample = start_sample;

  if (method == BRANCHED_PATH) {
    kintegrator->sample_all_lights_direct = sample_all_lights_direct;
    kintegrator->sample_all_lights_indirect = sample_all_lights_indirect;
  }
  else {
    kintegrator->sample_all_lights_direct = false;
    kintegrator->sample_all_lights_indirect = false;
  }

  kintegrator->sampling_pattern = sampling_pattern;
  if(use_auto_scramble){
    kintegrator->scrambling_distance = scrambling_distance;
  }
  
  kintegrator->aa_samples = aa_samples;
	//kintegrator->aa_samples = aa_samples;

  if (aa_samples > 0 && adaptive_min_samples == 0) {
    kintegrator->adaptive_min_samples = max(4, (int)sqrtf(aa_samples));
    VLOG(1) << "Cycles adaptive sampling: automatic min samples = "
            << kintegrator->adaptive_min_samples;
  }
  else {
    kintegrator->adaptive_min_samples = max(4, adaptive_min_samples);
  }
  if (aa_samples > 0 && adaptive_threshold == 0.0f) {
    kintegrator->adaptive_threshold = max(0.001f, 1.0f / (float)aa_samples);
    VLOG(1) << "Cycles adaptive sampling: automatic threshold = "
            << kintegrator->adaptive_threshold;
  }
  else {
    kintegrator->adaptive_threshold = adaptive_threshold;
  }

  if (light_sampling_threshold > 0.0f) {
    kintegrator->light_inv_rr_threshold = 1.0f / light_sampling_threshold;
  }
  else {
    kintegrator->light_inv_rr_threshold = 0.0f;
  }

  kintegrator->background_lightgroups = background_lightgroups;

  /* sobol directions table */
  int max_samples = 1;

  if (method == BRANCHED_PATH) {
    foreach (Light *light, scene->lights)
      max_samples = max(max_samples, light->samples);

    max_samples = max(max_samples,
                      max(diffuse_samples, max(glossy_samples, transmission_samples)));
    max_samples = max(max_samples, max(ao_samples, max(mesh_light_samples, subsurface_samples)));
    max_samples = max(max_samples, volume_samples);
  }

  uint total_bounces = max_bounce + transparent_max_bounce + 3 + VOLUME_BOUNDS_MAX +
                       max(BSSRDF_MAX_HITS, BSSRDF_MAX_BOUNCES);

  max_samples *= total_bounces;

  int dimensions = PRNG_BASE_NUM + max_samples * PRNG_BOUNCE_NUM;
  dimensions = min(dimensions, SOBOL_MAX_DIMENSIONS);

  //uint *directions = dscene->sobol_directions.alloc(SOBOL_BITS * dimensions);
  if (sampling_pattern == SAMPLING_PATTERN_SOBOL) {
    uint *directions = dscene->sample_pattern_lut.alloc(SOBOL_BITS * dimensions);

    sobol_generate_direction_vectors((uint(*)[SOBOL_BITS])directions, dimensions);

    //dscene->sobol_directions.copy_to_device();
    dscene->sample_pattern_lut.copy_to_device();
  }
  else {
    constexpr int sequence_size = 64 * 64;
    constexpr int num_sequences = 48;
    float2 *directions = (float2 *)dscene->sample_pattern_lut.alloc(sequence_size * num_sequences *
                                                                    2);
    TaskPool pool;
    for (int j = 0; j < num_sequences; ++j) {
      float2 *sequence = directions + j * sequence_size;
      pool.push(
          function_bind(&progressive_multi_jitter_02_generate_2D, sequence, sequence_size, j));
    }
    pool.wait_work();
    dscene->sample_pattern_lut.copy_to_device();
  }  

  /* Sobol dithering table */
	if(use_dithered_sampling) {
		int dither_size = sobol_dither_matrix_size();
		float2 *dither_matrix = dscene->sobol_dither.resize(dither_size*dither_size);

		sobol_generate_dither_matrix(dither_matrix);

		dscene->sobol_dither.copy_to_device(); //device->tex_alloc("__sobol_dither", 

		kintegrator->dither_size = dither_size;
	}
	else {
		kintegrator->dither_size = 0;
	}

  /* Clamping. */
  bool use_sample_clamp = (sample_clamp_direct != 0.0f || sample_clamp_indirect != 0.0f);
  if (use_sample_clamp != scene->film->use_sample_clamp) {
    scene->film->use_sample_clamp = use_sample_clamp;
    scene->film->tag_update(scene);
  }

  need_update = false;
}

void Integrator::device_free(Device *, DeviceScene *dscene)
{
	//dscene->sobol_directions.free();
  dscene->sample_pattern_lut.free();
	dscene->sobol_dither.free();
}

bool Integrator::modified(const Integrator &integrator)
{
  return !Node::equals(integrator);
}

void Integrator::tag_update(Scene *scene)
{
  foreach (Shader *shader, scene->shaders) {
    if (shader->has_integrator_dependency) {
      scene->shader_manager->need_update = true;
      break;
    }
  }
  need_update = true;
}

CCL_NAMESPACE_END
