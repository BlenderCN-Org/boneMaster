#ifndef VOLUMETRICS
vec3 tint_from_color(vec3 color)
{
  float lum = dot(color, vec3(0.3, 0.6, 0.1)); /* luminance approx. */
  return (lum > 0) ? color / lum : vec3(1.0);  /* normalize lum. to isolate hue+sat */
}

void convert_metallic_to_specular_tinted(vec3 basecol,
                                         vec3 basecol_tint,
                                         float metallic,
                                         float specular_fac,
                                         float specular_tint,
                                         out vec3 diffuse,
                                         out vec3 f0)
{
  vec3 tmp_col = mix(vec3(1.0), basecol_tint, specular_tint);
  f0 = mix((0.08 * specular_fac) * tmp_col, basecol, metallic);
  diffuse = basecol * (1.0 - metallic);
}

vec3 principled_sheen(float NV, vec3 basecol_tint, float sheen_tint)
{
  float f = 1.0 - NV;
  /* Temporary fix for T59784. Normal map seems to contain NaNs for tangent space normal maps,
   * therefore we need to clamp value. */
  f = clamp(f, 0.0, 1.0);
  /* Empirical approximation (manual curve fitting). Can be refined. */
  float sheen = f * f * f * 0.077 + f * 0.01 + 0.00026;
  return sheen * mix(vec3(1.0), basecol_tint, sheen_tint);
}

void node_bsdf_principled(vec4 base_color,
                          float subsurface,
                          vec3 subsurface_radius,
                          vec4 subsurface_color,
                          float metallic,
                          float specular,
                          float specular_tint,
                          float roughness,
                          float anisotropic,
                          float anisotropic_rotation,
                          float sheen,
                          float sheen_tint,
                          float clearcoat,
                          float clearcoat_roughness,
                          float ior,
                          float transmission,
                          float transmission_roughness,
                          vec4 emission,
                          float alpha,
                          vec3 N,
                          vec3 CN,
                          vec3 T,
                          vec3 I,
                          float ssr_id,
                          float sss_id,
                          vec3 sss_scale,
                          out Closure result)
{
  N = normalize(N);
  ior = max(ior, 1e-5);
  metallic = saturate(metallic);
  transmission = saturate(transmission);
  float dielectric = 1.0 - metallic;
  transmission *= dielectric;
  sheen *= dielectric;
  subsurface_color *= dielectric;

  vec3 diffuse, f0, out_diff, out_spec, out_trans, out_refr, ssr_spec;
  vec3 ctint = tint_from_color(base_color.rgb);
  convert_metallic_to_specular_tinted(
      base_color.rgb, ctint, metallic, specular, specular_tint, diffuse, f0);

  float NV = dot(N, cameraVec);
  vec3 out_sheen = sheen * principled_sheen(NV, ctint, sheen_tint);

  /* Far from being accurate, but 2 glossy evaluation is too expensive.
   * Most noticeable difference is at grazing angles since the bsdf lut
   * f0 color interpolation is done on top of this interpolation. */
  vec3 f0_glass = mix(vec3(1.0), base_color.rgb, specular_tint);
  float fresnel = F_eta(ior, NV);
  vec3 spec_col = F_color_blend(ior, fresnel, f0_glass) * fresnel;
  f0 = mix(f0, spec_col, transmission);

  vec3 f90 = mix(vec3(1.0), f0, (1.0 - specular) * metallic);

  vec3 mixed_ss_base_color = mix(diffuse, subsurface_color.rgb, subsurface);

  float sss_scalef = avg(sss_scale) * subsurface;
  eevee_closure_principled(N,
                           mixed_ss_base_color,
                           f0,
                           f90,
                           int(ssr_id),
                           roughness,
                           CN,
                           clearcoat * 0.25,
                           clearcoat_roughness,
                           1.0,
                           sss_scalef,
                           ior,
                           out_diff,
                           out_trans,
                           out_spec,
                           out_refr,
                           ssr_spec);

  vec3 refr_color = base_color.rgb;
  refr_color *= (refractionDepth > 0.0) ? refr_color :
                                          vec3(1.0); /* Simulate 2 transmission event */
  out_refr *= refr_color * (1.0 - fresnel) * transmission;

  result = CLOSURE_DEFAULT;
  result.radiance = out_spec + out_refr;
  result.radiance += out_diff * out_sheen; /* Coarse approx. */

  closure_load_ssr_data(ssr_spec * alpha, roughness, N, viewCameraVec, int(ssr_id), result);

  vec3 sss_radiance = (out_diff + out_trans) * alpha;
#  ifndef USE_SSS
  result.radiance += sss_radiance * mixed_ss_base_color * (1.0 - transmission);
#  else
#    ifdef USE_SSS_ALBEDO
  vec3 sss_albedo = mixed_ss_base_color;
#    else
  sss_radiance *= mixed_ss_base_color;
#    endif
  sss_radiance *= (1.0 - transmission);
  closure_load_sss_data(sss_scalef,
                        sss_radiance,
#    ifdef USE_SSS_ALBEDO
                        sss_albedo,
#    endif
                        int(sss_id),
                        result);
#  endif /* USE_SSS */

  result.radiance += emission.rgb;
  result.radiance *= alpha;
  result.transmittance = vec3(1.0 - alpha);
}

void node_bsdf_principled_dielectric(vec4 base_color,
                                     float subsurface,
                                     vec3 subsurface_radius,
                                     vec4 subsurface_color,
                                     float metallic,
                                     float specular,
                                     float specular_tint,
                                     float roughness,
                                     float anisotropic,
                                     float anisotropic_rotation,
                                     float sheen,
                                     float sheen_tint,
                                     float clearcoat,
                                     float clearcoat_roughness,
                                     float ior,
                                     float transmission,
                                     float transmission_roughness,
                                     vec4 emission,
                                     float alpha,
                                     vec3 N,
                                     vec3 CN,
                                     vec3 T,
                                     vec3 I,
                                     float ssr_id,
                                     float sss_id,
                                     vec3 sss_scale,
                                     out Closure result)
{
  N = normalize(N);
  metallic = saturate(metallic);
  float dielectric = 1.0 - metallic;

  vec3 diffuse, f0, out_diff, out_spec, ssr_spec;
  vec3 ctint = tint_from_color(base_color.rgb);
  convert_metallic_to_specular_tinted(
      base_color.rgb, ctint, metallic, specular, specular_tint, diffuse, f0);

  float NV = dot(N, cameraVec);
  vec3 out_sheen = sheen * principled_sheen(NV, ctint, sheen_tint);

  eevee_closure_default(
      N, diffuse, f0, vec3(1.0), int(ssr_id), roughness, 1.0, out_diff, out_spec, ssr_spec);

  result = CLOSURE_DEFAULT;
  result.radiance = out_spec + out_diff * (diffuse + out_sheen);
  closure_load_ssr_data(ssr_spec * alpha, roughness, N, viewCameraVec, int(ssr_id), result);
  result.radiance += emission.rgb;
  result.radiance *= alpha;
  result.transmittance = vec3(1.0 - alpha);
}

void node_bsdf_principled_metallic(vec4 base_color,
                                   float subsurface,
                                   vec3 subsurface_radius,
                                   vec4 subsurface_color,
                                   float metallic,
                                   float specular,
                                   float specular_tint,
                                   float roughness,
                                   float anisotropic,
                                   float anisotropic_rotation,
                                   float sheen,
                                   float sheen_tint,
                                   float clearcoat,
                                   float clearcoat_roughness,
                                   float ior,
                                   float transmission,
                                   float transmission_roughness,
                                   vec4 emission,
                                   float alpha,
                                   vec3 N,
                                   vec3 CN,
                                   vec3 T,
                                   vec3 I,
                                   float ssr_id,
                                   float sss_id,
                                   vec3 sss_scale,
                                   out Closure result)
{
  N = normalize(N);
  vec3 out_spec, ssr_spec;

  vec3 f90 = mix(vec3(1.0), base_color.rgb, (1.0 - specular) * metallic);

  eevee_closure_glossy(N, base_color.rgb, f90, int(ssr_id), roughness, 1.0, out_spec, ssr_spec);

  result = CLOSURE_DEFAULT;
  result.radiance = out_spec;
  closure_load_ssr_data(ssr_spec * alpha, roughness, N, viewCameraVec, int(ssr_id), result);
  result.radiance += emission.rgb;
  result.radiance *= alpha;
  result.transmittance = vec3(1.0 - alpha);
}

void node_bsdf_principled_clearcoat(vec4 base_color,
                                    float subsurface,
                                    vec3 subsurface_radius,
                                    vec4 subsurface_color,
                                    float metallic,
                                    float specular,
                                    float specular_tint,
                                    float roughness,
                                    float anisotropic,
                                    float anisotropic_rotation,
                                    float sheen,
                                    float sheen_tint,
                                    float clearcoat,
                                    float clearcoat_roughness,
                                    float ior,
                                    float transmission,
                                    float transmission_roughness,
                                    vec4 emission,
                                    float alpha,
                                    vec3 N,
                                    vec3 CN,
                                    vec3 T,
                                    vec3 I,
                                    float ssr_id,
                                    float sss_id,
                                    vec3 sss_scale,
                                    out Closure result)
{
  vec3 out_spec, ssr_spec;
  N = normalize(N);

  vec3 f90 = mix(vec3(1.0), base_color.rgb, (1.0 - specular) * metallic);

  eevee_closure_clearcoat(N,
                          base_color.rgb,
                          f90,
                          int(ssr_id),
                          roughness,
                          CN,
                          clearcoat * 0.25,
                          clearcoat_roughness,
                          1.0,
                          out_spec,
                          ssr_spec);

  result = CLOSURE_DEFAULT;
  result.radiance = out_spec;
  closure_load_ssr_data(ssr_spec * alpha, roughness, N, viewCameraVec, int(ssr_id), result);
  result.radiance += emission.rgb;
  result.radiance *= alpha;
  result.transmittance = vec3(1.0 - alpha);
}

void node_bsdf_principled_subsurface(vec4 base_color,
                                     float subsurface,
                                     vec3 subsurface_radius,
                                     vec4 subsurface_color,
                                     float metallic,
                                     float specular,
                                     float specular_tint,
                                     float roughness,
                                     float anisotropic,
                                     float anisotropic_rotation,
                                     float sheen,
                                     float sheen_tint,
                                     float clearcoat,
                                     float clearcoat_roughness,
                                     float ior,
                                     float transmission,
                                     float transmission_roughness,
                                     vec4 emission,
                                     float alpha,
                                     vec3 N,
                                     vec3 CN,
                                     vec3 T,
                                     vec3 I,
                                     float ssr_id,
                                     float sss_id,
                                     vec3 sss_scale,
                                     out Closure result)
{
  metallic = saturate(metallic);
  N = normalize(N);

  vec3 diffuse, f0, out_diff, out_spec, out_trans, ssr_spec;
  vec3 ctint = tint_from_color(base_color.rgb);
  convert_metallic_to_specular_tinted(
      base_color.rgb, ctint, metallic, specular, specular_tint, diffuse, f0);

  subsurface_color = subsurface_color * (1.0 - metallic);
  vec3 mixed_ss_base_color = mix(diffuse, subsurface_color.rgb, subsurface);
  float sss_scalef = avg(sss_scale) * subsurface;

  float NV = dot(N, cameraVec);
  vec3 out_sheen = sheen * principled_sheen(NV, ctint, sheen_tint);

  vec3 f90 = mix(vec3(1.0), base_color.rgb, (1.0 - specular) * metallic);

  eevee_closure_skin(N,
                     mixed_ss_base_color,
                     f0,
                     f90,
                     int(ssr_id),
                     roughness,
                     1.0,
                     sss_scalef,
                     out_diff,
                     out_trans,
                     out_spec,
                     ssr_spec);

  result = CLOSURE_DEFAULT;
  result.radiance = out_spec;
  closure_load_ssr_data(ssr_spec * alpha, roughness, N, viewCameraVec, int(ssr_id), result);

  vec3 sss_radiance = (out_diff + out_trans) * alpha;
#  ifndef USE_SSS
  result.radiance += sss_radiance * mixed_ss_base_color * (1.0 - transmission);
#  else
#    ifdef USE_SSS_ALBEDO
  vec3 sss_albedo = mixed_ss_base_color;
#    else
  sss_radiance *= mixed_ss_base_color;
#    endif
  sss_radiance *= (1.0 - transmission);
  closure_load_sss_data(sss_scalef,
                        sss_radiance,
#    ifdef USE_SSS_ALBEDO
                        sss_albedo,
#    endif
                        int(sss_id),
                        result);
#  endif /* USE_SSS */

  result.radiance += out_diff * out_sheen;
  result.radiance += emission.rgb;
  result.radiance *= alpha;
  result.transmittance = vec3(1.0 - alpha);
}

void node_bsdf_principled_glass(vec4 base_color,
                                float subsurface,
                                vec3 subsurface_radius,
                                vec4 subsurface_color,
                                float metallic,
                                float specular,
                                float specular_tint,
                                float roughness,
                                float anisotropic,
                                float anisotropic_rotation,
                                float sheen,
                                float sheen_tint,
                                float clearcoat,
                                float clearcoat_roughness,
                                float ior,
                                float transmission,
                                float transmission_roughness,
                                vec4 emission,
                                float alpha,
                                vec3 N,
                                vec3 CN,
                                vec3 T,
                                vec3 I,
                                float ssr_id,
                                float sss_id,
                                vec3 sss_scale,
                                out Closure result)
{
  ior = max(ior, 1e-5);
  N = normalize(N);

  vec3 f0, out_spec, out_refr, ssr_spec;
  f0 = mix(vec3(1.0), base_color.rgb, specular_tint);

  eevee_closure_glass(
      N, vec3(1.0), vec3(1.0), int(ssr_id), roughness, 1.0, ior, out_spec, out_refr, ssr_spec);

  vec3 refr_color = base_color.rgb;
  refr_color *= (refractionDepth > 0.0) ? refr_color :
                                          vec3(1.0); /* Simulate 2 transmission events */
  out_refr *= refr_color;

  float fresnel = F_eta(ior, dot(N, cameraVec));
  vec3 spec_col = F_color_blend(ior, fresnel, f0);
  out_spec *= spec_col;
  ssr_spec *= spec_col * fresnel;

  result = CLOSURE_DEFAULT;
  result.radiance = mix(out_refr, out_spec, fresnel);
  closure_load_ssr_data(ssr_spec * alpha, roughness, N, viewCameraVec, int(ssr_id), result);
  result.radiance += emission.rgb;
  result.radiance *= alpha;
  result.transmittance = vec3(1.0 - alpha);
}
#else
/* Stub principled because it is not compatible with volumetrics. */
#  define node_bsdf_principled
#  define node_bsdf_principled_dielectric
#  define node_bsdf_principled_metallic
#  define node_bsdf_principled_clearcoat
#  define node_bsdf_principled_subsurface
#  define node_bsdf_principled_glass
#endif
