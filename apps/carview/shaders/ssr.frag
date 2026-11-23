#version 450
// carview: Phase B Hi-Z Stochastic Screen-Space Reflections (SSR).
// Marches the Reversed-Z Hi-Z pyramid (falling back to full-res depth when Hi-Z
// is unavailable) with roughness-aware GGX jitter and binary hit refinement.
// Gloss/reflectivity weight is carried in hdr_tex.a (car clearcoat, chrome,
// wet/polished H_ROAD asphalt, and Colorado alpine lakes; sky/terrain/foliage = 0).
//
// Modes (pc.texel_mode.z):
//   0 = Hi-Z stochastic ray march -> RGBA16F (reflected_hdr_rgb, confidence * gloss)
//   1 = 5-tap depth- and gloss-aware bilateral spatial resolve

layout(set = 0, binding = 0) uniform sampler2D hdr_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D hiz_tex;
layout(set = 0, binding = 3) uniform sampler2D ssr_in;

layout(push_constant) uniform SsrPush {
  vec4 proj_params;  // x = proj00, y = proj11, z = near_plane, w = far_plane
  vec4 ssr_params;   // x = max_steps (24/36), y = reverse_z (0/1), z = has_hiz (0/1), w = frame_jitter [0..1]
  vec4 texel_mode;   // x = 1/w, y = 1/h, z = mode (0=trace, 1=resolve), w = strength
  vec4 hiz_params;   // x = hiz_w, y = hiz_h, z = hiz_max_mip, w = unused
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_ssr;

bool is_sky_z(float z) {
  return pc.ssr_params.y > 0.5 ? (z <= 1e-4) : (z >= 0.9999);
}

float linear_depth_from_z(float z) {
  float near_p = max(pc.proj_params.z, 0.05);
  if (pc.ssr_params.y > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = max(pc.proj_params.w, near_p + 1.0);
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

float sample_linear_depth(vec2 uv) {
  float z = texture(depth_tex, clamp(uv, vec2(0.0005), vec2(0.9995))).r;
  return linear_depth_from_z(z);
}

vec3 view_pos_from_depth(vec2 uv, float d) {
  vec2 ndc = uv * 2.0 - 1.0;
  float p00 = abs(pc.proj_params.x) > 1e-4 ? pc.proj_params.x : 1.0;
  float p11 = abs(pc.proj_params.y) > 1e-4 ? pc.proj_params.y : -1.0;
  return vec3(ndc.x * d / p00, ndc.y * d / p11, -d);
}

vec2 project_view_to_uv(vec3 p) {
  float d = max(-p.z, 1e-4);
  vec2 ndc = vec2(p.x * pc.proj_params.x / d, p.y * pc.proj_params.y / d);
  return ndc * 0.5 + 0.5;
}

float ign(vec2 px) {
  return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

// Edge-preserving view-space normal reconstruction (picks closer neighbor in X/Y to avoid silhouette halos).
vec3 reconstruct_view_normal(vec2 uv, vec3 P, float d_c) {
  vec2 tx = pc.texel_mode.xy;
  float d_l = sample_linear_depth(uv - vec2(tx.x, 0.0));
  float d_r = sample_linear_depth(uv + vec2(tx.x, 0.0));
  float d_d = sample_linear_depth(uv - vec2(0.0, tx.y));
  float d_u = sample_linear_depth(uv + vec2(0.0, tx.y));

  vec3 P_l = view_pos_from_depth(uv - vec2(tx.x, 0.0), d_l);
  vec3 P_r = view_pos_from_depth(uv + vec2(tx.x, 0.0), d_r);
  vec3 P_d = view_pos_from_depth(uv - vec2(0.0, tx.y), d_d);
  vec3 P_u = view_pos_from_depth(uv + vec2(0.0, tx.y), d_u);

  vec3 dpdx = (abs(d_r - d_c) < abs(d_l - d_c)) ? (P_r - P) : (P - P_l);
  vec3 dpdy = (abs(d_u - d_c) < abs(d_d - d_c)) ? (P_u - P) : (P - P_d);
  vec3 N = cross(dpdx, dpdy);
  float len2 = dot(N, N);
  if (len2 < 1e-10) return vec3(0.0, 1.0, 0.0);
  N *= inversesqrt(len2);
  // Ensure normal faces the camera in view space (P points away from origin, so dot(N, -P) >= 0).
  if (dot(N, P) > 0.0) N = -N;
  return N;
}

void main() {
  int mode = int(pc.texel_mode.z + 0.5);
  if (mode == 1) {
    // Depth- and gloss-aware 5-tap bilateral resolve to smooth stochastic GGX reflections.
    vec4 center = texture(ssr_in, v_uv);
    float d_c = sample_linear_depth(v_uv);
    float gloss = clamp(texture(hdr_tex, v_uv).a, 0.0, 1.0);
    if (gloss < 0.06 || d_c > 800.0) {
      out_ssr = vec4(0.0);
      return;
    }
    float rough = clamp(1.0 - gloss, 0.04, 0.85);
    vec2 step_uv = pc.texel_mode.xy * mix(0.75, 2.25, rough);
    const vec2 offs[4] = vec2[4](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
    vec4 acc = center * 2.0;
    float wsum = 2.0;
    float z_tol = max(d_c * 0.045, 0.35);
    for (int i = 0; i < 4; ++i) {
      vec2 suv = clamp(v_uv + offs[i] * step_uv, vec2(0.001), vec2(0.999));
      float sd = sample_linear_depth(suv);
      float sg = clamp(texture(hdr_tex, suv).a, 0.0, 1.0);
      float wz = exp(-abs(sd - d_c) / z_tol);
      float wg = exp(-abs(sg - gloss) * 6.0);
      float w = wz * wg;
      acc += texture(ssr_in, suv) * w;
      wsum += w;
    }
    out_ssr = acc / max(wsum, 1e-4);
    return;
  }

  // Pass 0: Hi-Z accelerated stochastic SSR ray march.
  vec4 hdr_c = texture(hdr_tex, v_uv);
  float gloss = clamp(hdr_c.a, 0.0, 1.0);
  if (gloss < 0.08) {
    out_ssr = vec4(0.0);
    return;
  }
  float z_c = texture(depth_tex, v_uv).r;
  if (is_sky_z(z_c)) {
    out_ssr = vec4(0.0);
    return;
  }
  float d_c = linear_depth_from_z(z_c);
  if (d_c > 550.0) {
    out_ssr = vec4(0.0);
    return;
  }

  vec3 P = view_pos_from_depth(v_uv, d_c);
  vec3 V = normalize(-P);
  vec3 N = reconstruct_view_normal(v_uv, P, d_c);
  float nv = clamp(dot(N, V), 0.0, 1.0);
  float rough = clamp(1.0 - gloss, 0.04, 0.85);

  // Stochastic GGX cone perturbation (vanishes on mirror-like lakes & clearcoat, softens wet asphalt).
  float r1 = fract(ign(gl_FragCoord.xy) + pc.ssr_params.w * 0.61803399);
  float r2 = fract(ign(gl_FragCoord.xy + vec2(17.0, 31.0)) + pc.ssr_params.w * 0.38196601);
  vec3 up_ref = abs(N.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  vec3 T = normalize(cross(up_ref, N));
  vec3 B = cross(N, T);
  float cone = rough * rough * 0.18;
  float phi = 6.2831853 * r1;
  vec3 N_jit = normalize(N + (T * cos(phi) + B * sin(phi)) * (cone * sqrt(r2)));
  vec3 R = reflect(-V, N_jit);

  // Fade out rays pointing back toward the viewer (off-screen behind camera -> GGX cubemap fallback).
  float back_fade = smoothstep(0.32, -0.10, R.z);
  if (back_fade <= 0.005) {
    out_ssr = vec4(0.0);
    return;
  }

  // Adaptive march range: crisp local reflections near car/road, long reflections across alpine lakes.
  float max_dist = mix(28.0, 160.0, smoothstep(0.35, 0.90, gloss));
  float near_p = max(pc.proj_params.z, 0.05);
  if (R.z > 1e-4) {
    float safe_t = (-near_p * 1.15 - P.z) / R.z;
    max_dist = min(max_dist, max(safe_t, 0.0));
  }
  if (max_dist < 0.25) {
    out_ssr = vec4(0.0);
    return;
  }

  int max_steps = clamp(int(pc.ssr_params.x + 0.5), 12, 40);
  bool has_hiz = pc.ssr_params.z > 0.5;
  float hiz_max_mip = max(pc.hiz_params.z, 1.0);
  float mip = has_hiz ? 2.0 : 0.0;

  // Bias start along normal + reflection direction to prevent self-intersection.
  vec3 ray_orig = P + N * clamp(0.03 + d_c * 0.0025, 0.03, 0.30);
  float t = clamp(0.10 + d_c * 0.008, 0.10, 1.2) * (0.35 + 0.65 * r1);
  float prev_t = 0.0;
  float base_step = max_dist / float(max_steps);
  bool hit = false;
  vec2 hit_uv = vec2(0.0);
  float hit_dist_frac = 1.0;

  for (int i = 0; i < max_steps; ++i) {
    if (t > max_dist) break;
    vec3 Q = ray_orig + R * t;
    if (Q.z >= -near_p) break;
    vec2 uv = project_view_to_uv(Q);
    if (uv.x <= 0.002 || uv.x >= 0.998 || uv.y <= 0.002 || uv.y >= 0.998) break;

    float ray_d = -Q.z;
    float step_len = base_step * (0.45 + 1.35 * (float(i) / float(max_steps)));

    if (has_hiz && mip > 0.5) {
      float hz = textureLod(hiz_tex, uv, mip).r;
      float hiz_d = linear_depth_from_z(hz);
      // In Reversed-Z Hi-Z stores min depth (= farthest occluder in tile); if ray is still well in front
      // of the tile's nearest possible surface, accelerate cleanly.
      if (ray_d < hiz_d * 0.92 - 0.15) {
        prev_t = t;
        t += step_len * (1.0 + 0.45 * mip);
        mip = min(mip + 1.0, min(hiz_max_mip, 4.0));
        continue;
      }
      mip = max(mip - 1.5, 0.0);
    }

    float sz = texture(depth_tex, uv).r;
    if (!is_sky_z(sz)) {
      float scene_d = linear_depth_from_z(sz);
      float delta = ray_d - scene_d;
      float thickness = clamp(0.32 + step_len * 0.75 + ray_d * 0.025, 0.32, 6.5);
      if (delta > 0.02 && delta < thickness) {
        // 4-step binary refinement for crisp sub-pixel hit precision.
        float lo = prev_t, hi = t;
        for (int b = 0; b < 4; ++b) {
          float mid = 0.5 * (lo + hi);
          vec3 Qm = ray_orig + R * mid;
          vec2 uvm = project_view_to_uv(Qm);
          float sm_d = sample_linear_depth(uvm);
          if (-Qm.z > sm_d) hi = mid;
          else lo = mid;
        }
        vec3 Qf = ray_orig + R * hi;
        hit_uv = project_view_to_uv(Qf);
        float final_z = texture(depth_tex, clamp(hit_uv, vec2(0.001), vec2(0.999))).r;
        float final_d = linear_depth_from_z(final_z);
        if (!is_sky_z(final_z) && abs(-Qf.z - final_d) < thickness * 1.15) {
          hit = true;
          hit_dist_frac = clamp(hi / max_dist, 0.0, 1.0);
          break;
        }
      }
    }
    prev_t = t;
    t += step_len;
  }

  if (!hit) {
    out_ssr = vec4(0.0);
    return;
  }

  vec3 refl_rgb = min(max(texture(hdr_tex, clamp(hit_uv, vec2(0.001), vec2(0.999))).rgb, vec3(0.0)), vec3(32.0));
  vec2 edge = min(hit_uv, vec2(1.0) - hit_uv);
  float edge_fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
  float dist_fade = 1.0 - smoothstep(0.70, 1.0, hit_dist_frac);
  float rough_fade = smoothstep(0.82, 0.14, rough);
  float fresnel = clamp(0.06 + 0.94 * pow(1.0 - nv, 4.0), 0.06, 0.96);
  // Water & chrome stay highly reflective even at steeper viewing angles; car clearcoat obeys Fresnel.
  float angle_weight = mix(fresnel, max(fresnel, 0.55), smoothstep(0.78, 0.95, gloss));
  float conf = clamp(edge_fade * dist_fade * back_fade * rough_fade * angle_weight * gloss * pc.texel_mode.w, 0.0, 0.88);
  out_ssr = vec4(refl_rgb, conf);
}
