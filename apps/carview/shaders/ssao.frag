#version 450
precision highp int;
precision highp float;
// carview: Intel XeGTAO / Ground-Truth Ambient Occlusion (Jimenez et al.) + Bent Normals
// + edge-preserving depth/normal-aware bilateral spatial denoiser.
// Output format: RGBA8_UNORM (.r = GTAO visibility [0..1], .gba = view-space bent normal * 0.5 + 0.5).
// Mode 0: XeGTAO horizon-based visibility integral + bent normal from resolved depth.
// Mode 1: Horizontal depth/normal-aware bilateral spatial denoise.
// Mode 2: Vertical depth/normal-aware bilateral spatial denoise.

layout(set = 0, binding = 0) uniform sampler2D src_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;

layout(push_constant) uniform GtaoPush {
  vec4 proj_params;  // x = proj00, y = proj11, z = near_plane, w = far_plane
  vec4 ao_params;    // x = radius_m, y = slices (2..3), z = steps_per_side (2..4), w = mode (0/1/2)
  vec4 texel_step;   // xy = inv_target_size, z = reverse_z (0 or 1), w = frame_jitter [0..1]
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_gtao;

const float kPi = 3.14159265359;
const float kHalfPi = 1.57079632679;

float linear_depth(vec2 uv) {
  float z = texture(depth_tex, clamp(uv, vec2(0.0005), vec2(0.9995))).r;
  float near_p = max(pc.proj_params.z, 0.05);
  if (pc.texel_step.z > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = max(pc.proj_params.w, near_p + 1.0);
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

vec3 view_pos_from_depth(vec2 uv, float d) {
  vec2 ndc = uv * 2.0 - 1.0;
  float p00 = abs(pc.proj_params.x) > 1e-4 ? pc.proj_params.x : 1.0;
  float p11 = abs(pc.proj_params.y) > 1e-4 ? pc.proj_params.y : -1.0;
  return vec3(ndc.x * d / p00, ndc.y * d / p11, -d);
}

// Minimum-depth-difference view-space normal reconstruction (eliminates silhouette edge halos).
vec3 reconstruct_view_normal(vec2 uv, vec3 P, float center_d) {
  vec2 du = vec2(pc.texel_step.x, 0.0);
  vec2 dv = vec2(0.0, pc.texel_step.y);

  float dl = linear_depth(uv - du);
  float dr = linear_depth(uv + du);
  float dt = linear_depth(uv - dv);
  float db = linear_depth(uv + dv);

  vec3 Pl = view_pos_from_depth(uv - du, dl);
  vec3 Pr = view_pos_from_depth(uv + du, dr);
  vec3 Pt = view_pos_from_depth(uv - dv, dt);
  vec3 Pb = view_pos_from_depth(uv + dv, db);

  vec3 dpx = (abs(dl - center_d) < abs(dr - center_d)) ? (P - Pl) : (Pr - P);
  vec3 dpy = (abs(dt - center_d) < abs(db - center_d)) ? (P - Pt) : (Pb - P);

  vec3 N = cross(dpy, dpx);
  float nlen = length(N);
  if (nlen < 1e-6) return vec3(0.0, 0.0, 1.0);
  N /= nlen;
  // Ensure reconstructed normal faces the camera in view space.
  if (dot(N, normalize(-P)) < 0.0) N = -N;
  return N;
}

// Interleaved Gradient Noise (Jimenez) for smooth high-frequency spatio-temporal dither.
float ign(vec2 px) {
  return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

mediump float fast_acos(mediump float x) {
  mediump float ax = abs(clamp(x, -1.0, 1.0));
  mediump float res = -0.156583 * ax + kHalfPi;
  res *= sqrt(max(1.0 - ax, 0.0));
  return x >= 0.0 ? res : (kPi - res);
}

vec4 compute_xegtao(vec2 uv) {
  float center_d = linear_depth(uv);
  if (center_d > 320.0) return vec4(1.0, 0.5, 0.5, 1.0);

  vec3 P = view_pos_from_depth(uv, center_d);
  vec3 V = normalize(-P);
  vec3 N = reconstruct_view_normal(uv, P, center_d);

  float radius = max(pc.ao_params.x, 0.30);
  float proj_scale = abs(pc.proj_params.y) * 0.5;
  float screen_r = clamp((radius * proj_scale) / max(center_d, 0.35), 0.004, 0.11);
  // Subtle screen-radius pixel floor so close-up car wheel wells stay crisp.
  float min_uv_r = max(pc.texel_step.x, pc.texel_step.y) * 2.5;
  screen_r = max(screen_r, min_uv_r);

  int num_slices = clamp(int(pc.ao_params.y + 0.5), 1, 4);
  int steps_per_side = clamp(int(pc.ao_params.z + 0.5), 2, 5);
  float aspect = abs(pc.proj_params.y / (abs(pc.proj_params.x) > 1e-4 ? pc.proj_params.x : 1.0));

  vec2 px = gl_FragCoord.xy;
  float noise_slice = fract(ign(px) + pc.texel_step.w * 0.61803399);
  float noise_step = fract(ign(px + vec2(37.0, 17.0)) + pc.texel_step.w * 0.38196601);

  float inv_radius = 1.0 / radius;
  float falloff_range = 0.615 * radius;
  float falloff_from = radius * (1.0 - 0.615);
  float falloff_mul = -1.0 / max(falloff_range, 1e-4);
  float falloff_add = falloff_from / max(falloff_range, 1e-4) + 1.0;

  // Offset view position slightly toward camera to avoid self-occlusion on sloped asphalt.
  vec3 P_biased = P + V * clamp(center_d * 0.0012, 0.002, 0.04);

  float vis_sum = 0.0;
  vec3 bent_sum = vec3(0.0);
  float weight_sum = 0.0;

  for (int s = 0; s < num_slices; ++s) {
    float slice_t = (float(s) + noise_slice) / float(num_slices);
    float phi = slice_t * kPi;
    float cos_phi = cos(phi);
    float sin_phi = sin(phi);
    vec2 omega = vec2(cos_phi, sin_phi * aspect);

    // Slice plane basis in view space (view_dir = V, ortho_dir perpendicular to V in slice plane).
    vec3 dir_v = vec3(cos_phi, sin_phi * sign(pc.proj_params.y), 0.0);
    vec3 ortho_dir = dir_v - dot(dir_v, V) * V;
    float ortho_len = length(ortho_dir);
    if (ortho_len < 1e-5) continue;
    ortho_dir /= ortho_len;

    vec3 axis_v = normalize(cross(ortho_dir, V));
    vec3 proj_n = N - axis_v * dot(N, axis_v);
    float proj_n_len = length(proj_n);
    if (proj_n_len < 1e-5) continue;

    mediump float cos_n = clamp(dot(proj_n / proj_n_len, V), -1.0, 1.0);
    // Angle of projected surface normal from V toward +ortho_dir (matches +omega screen step).
    mediump float sgn = sign(dot(proj_n, ortho_dir));
    mediump float n_angle = sgn * fast_acos(cos_n);
    mediump float sin_n = sin(n_angle);

    // Intel XeGTAO horizon baseline: cos(n +/- pi/2) == -/+ sin(n) (saves 2 trig SFU calls/slice).
    mediump float low_h_cos1 = -sin_n;
    mediump float low_h_cos2 = sin_n;
    mediump float max_h_cos1 = low_h_cos1;
    mediump float max_h_cos2 = low_h_cos2;

    for (int st = 0; st < steps_per_side; ++st) {
      // Power-distributed step spacing (XeGTAO: dense micro-detail near center, wide reach far).
      mediump float step_base = (float(st) + noise_step) / float(steps_per_side);
      mediump float step_frac = step_base * step_base * 0.85 + step_base * 0.15;
      vec2 step_uv = omega * (screen_r * step_frac);

      // + side (toward +ortho_dir)
      vec2 uv1 = uv + step_uv;
      if (uv1.x > 0.001 && uv1.x < 0.999 && uv1.y > 0.001 && uv1.y < 0.999) {
        float d1 = linear_depth(uv1);
        if (d1 < 350.0) {
          vec3 H1 = view_pos_from_depth(uv1, d1) - P_biased;
          float h1_len = length(H1);
          mediump float h1_cos = dot(H1, V) / max(h1_len, 1e-5);
          // Thin-object thickness & distance falloff (Intel XeGTAO).
          mediump float weight = clamp(h1_len * falloff_mul + falloff_add, 0.0, 1.0);
          mediump float h1_weighted = mix(low_h_cos1, h1_cos, weight);
          max_h_cos1 = max(max_h_cos1, h1_weighted);
        }
      }

      // - side (toward -ortho_dir)
      vec2 uv2 = uv - step_uv;
      if (uv2.x > 0.001 && uv2.x < 0.999 && uv2.y > 0.001 && uv2.y < 0.999) {
        float d2 = linear_depth(uv2);
        if (d2 < 350.0) {
          vec3 H2 = view_pos_from_depth(uv2, d2) - P_biased;
          float h2_len = length(H2);
          mediump float h2_cos = dot(H2, V) / max(h2_len, 1e-5);
          mediump float weight = clamp(h2_len * falloff_mul + falloff_add, 0.0, 1.0);
          mediump float h2_weighted = mix(low_h_cos2, h2_cos, weight);
          max_h_cos2 = max(max_h_cos2, h2_weighted);
        }
      }
    }

    mediump float h1 = n_angle + max(-fast_acos(max_h_cos2) - n_angle, -kHalfPi);
    mediump float h2 = n_angle + min(fast_acos(max_h_cos1) - n_angle, kHalfPi);

    // Exact Jimenez cosine-weighted hemisphere visibility integral, normalized by
    // the exact unoccluded slice integral (cos_n + n_angle * sin_n) so flat surfaces
    // at any oblique viewing angle evaluate cleanly to 1.0 with zero dark halos.
    mediump float i1 = -cos(2.0 * h1 - n_angle) + cos_n + 2.0 * h1 * sin_n;
    mediump float i2 = -cos(2.0 * h2 - n_angle) + cos_n + 2.0 * h2 * sin_n;
    mediump float unocc_norm = max(cos_n + n_angle * sin_n, 0.05);
    mediump float slice_vis = clamp(0.25 * (i1 + i2) / unocc_norm, 0.0, 1.0);

    vis_sum += proj_n_len * slice_vis;
    weight_sum += proj_n_len;

    // Exact Jimenez bent normal slice integral in FP16.
    mediump float t0 = (6.0 * sin(h1 - n_angle) - sin(3.0 * h1 - n_angle) +
                        6.0 * sin(h2 - n_angle) - sin(3.0 * h2 - n_angle) +
                        16.0 * sin_n - 3.0 * (sin(h1 + n_angle) + sin(h2 + n_angle))) / 12.0;
    mediump float t1 = (-cos(3.0 * h1 - n_angle) - cos(3.0 * h2 - n_angle) +
                        8.0 * cos_n - 3.0 * (cos(h1 + n_angle) + cos(h2 + n_angle))) / 12.0;
    bent_sum += proj_n_len * (ortho_dir * t0 + V * t1);
  }

  float vis = weight_sum > 1e-4 ? clamp(vis_sum / weight_sum, 0.0, 1.0) : 1.0;
  // Mild perceptual contrast curve matching Intel XeGTAO defaults without dark halo banding.
  vis = clamp(pow(vis, 1.12), 0.08, 1.0);

  vec3 bent_n = length(bent_sum) > 1e-4 ? normalize(bent_sum) : N;
  // Fade gracefully on distant mountains so far horizons keep full clarity.
  float dist_fade = 1.0 - smoothstep(110.0, 260.0, center_d);
  vis = mix(1.0, vis, dist_fade);
  bent_n = normalize(mix(N, bent_n, dist_fade));

  return vec4(vis, bent_n * 0.5 + 0.5);
}

// 7-tap depth- and bent-normal-aware edge-preserving spatial bilateral denoiser (XeGTAO style).
mediump vec4 bilateral_denoise(vec2 uv, vec2 dir) {
  mediump vec4 center = texture(src_tex, uv);
  float center_d = linear_depth(uv);
  if (center_d > 320.0) return vec4(1.0, 0.5, 0.5, 1.0);

  mediump vec3 center_bn = normalize(center.gba * 2.0 - 1.0);
  const mediump float kSpatial[4] = float[](0.280, 0.215, 0.110, 0.035);
  float depth_inv = 1.0 / max(center_d * 0.065, 0.09);

  mediump vec4 acc = center * kSpatial[0];
  mediump float wsum = kSpatial[0];

  for (int side = -1; side <= 1; side += 2) {
    for (int i = 1; i <= 3; ++i) {
      vec2 suv = clamp(uv + dir * (float(side * i)), vec2(0.0005), vec2(0.9995));
      float sd = linear_depth(suv);
      mediump vec4 sample_v = texture(src_tex, suv);
      mediump float dz = abs(sd - center_d) * depth_inv;
      // Sharp edge-stopping weight on depth discontinuities + bent-normal consistency.
      mediump float w_depth = exp(-dz * dz * 1.8);
      mediump vec3 s_bn = normalize(sample_v.gba * 2.0 - 1.0);
      mediump float w_norm = clamp(dot(center_bn, s_bn) * 0.5 + 0.5, 0.15, 1.0);
      mediump float w = kSpatial[i] * w_depth * w_norm;
      acc += sample_v * w;
      wsum += w;
    }
  }
  mediump vec4 out_v = acc / max(wsum, 1e-4);
  mediump vec3 bn = out_v.gba * 2.0 - 1.0;
  mediump float blen = length(bn);
  bn = blen > 1e-4 ? (bn / blen) : center_bn;
  return vec4(clamp(out_v.r, 0.0, 1.0), bn * 0.5 + 0.5);
}

void main() {
  int mode = int(pc.ao_params.w + 0.5);
  if (mode == 0) {
    out_gtao = compute_xegtao(v_uv);
  } else if (mode == 1) {
    out_gtao = bilateral_denoise(v_uv, vec2(pc.texel_step.x, 0.0));
  } else {
    out_gtao = bilateral_denoise(v_uv, vec2(0.0, pc.texel_step.y));
  }
}
