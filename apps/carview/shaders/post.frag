#version 450
// carview: the drive's HDR post-processing chain (SPEC-SKY-AND-TOD §5, SPEC-VFX-AND-PARTICLES)
// + Phase A SOTA upgrades:
//   - XeGTAO multi-bounce + Bent Normal directional occlusion (ao_tex RGBA8)
//   - Screen-Space Contact Shadows (sscs_tex R8)
//   - Physically-based Jimenez / Dual-Kawase 6-mip bloom (aux)
//   - McGuire Tile-Max / Neighbor-Max depth-aware velocity motion blur (motion_tex, mblur_neighbor_tex)
//   - Selectable SOTA Filmic Tonemapper suite: FH1 Hable (default), AgX, GT Filmic, Khronos PBR Neutral
//
// Modes (p.mode.x):
//   0 bright pass (legacy fallback; primary bloom uses bloom_down.frag / bloom_up.frag)
//   1 blur (legacy fallback)
//   2 composite: HDR scene + DoF + McGuire/radial motion blur + XeGTAO + SSCS + god rays +
//                lens flare + Jimenez bloom -> exposure -> selected filmic tonemapper ->
//                N^3 3D LUT -> paired vignettes -> contrast/brightness + desat -> display transfer
//   3 lum_16: 16x16 centre-weighted Rec.709 luminance reduction from HDR scene
//   4 lum_adapt: 1x1 temporal adaptation from 16x16 lum grid + previous 1x1 lum
//   5 upsample_add (legacy fallback)

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D aux;
layout(set = 0, binding = 2) uniform sampler3D lut3d;
layout(set = 0, binding = 3) uniform sampler2D lum_adapt;
layout(set = 0, binding = 4) uniform sampler2D depth_tex;
layout(set = 0, binding = 6) uniform sampler2D ao_tex;
layout(set = 0, binding = 7) uniform sampler2D sscs_tex;
layout(set = 0, binding = 8) uniform sampler2D motion_tex;
layout(set = 0, binding = 9) uniform sampler2D mblur_neighbor_tex;

layout(set = 0, binding = 5) uniform Optical {
  vec4 flags_near;    // x flags (1=mblur, 2=dof, 4=god_rays, 8=lens_flare, 16=reverse_z, 32=paired_vig), y near, z contrast, w brightness
  vec4 mblur;         // xy center uv, z strength [0..1], w car_protect_dist_m
  vec4 dof;           // x focal_distance_m, y focal_range_m, z max_blur [0..1], w sepia [0..1]
  vec4 sun;           // xy sun_screen_uv, z sun_visibility [0..1], w god_ray_strength
  vec4 sun_color;     // rgb sun_scatter_color, w lens_flare_strength
  vec4 vig_top_col;   // rgb color, w opacity
  vec4 vig_top_geom;  // xy center, zw radius
  vec4 vig_top_rot;   // x cos(rot), y sin(rot), z feather, w unused
  vec4 vig_bot_col;   // rgb color, w opacity
  vec4 vig_bot_geom;  // xy center, zw radius
  vec4 vig_bot_rot;   // x cos(rot), y sin(rot), z feather, w unused
  vec4 texel_size;    // xy 1/w, 1/h, z time_sec, w frame_jitter [0..1]
  vec4 modern_p;      // x ao_strength, y lut_size, z lut_strength, w out_alpha
  vec4 sota_p;        // x tonemapper (0=Hable, 1=AgX, 2=GT, 3=PBRNeutral), y sscs_strength, z mblur_taps (0/9/13), w gtao_bent_strength
} opt;

layout(push_constant) uniform Post {
  vec4 mode;     // x mode, yz texel step, w mode-param (adapt_alpha / upsample_weight / vignette_amount)
  vec4 abc;      // Hable A, B, C, white
  vec4 def;      // Hable D, E, F, exposure (filmic_exposure+bias when auto-exp, else linear multiplier)
  vec4 desat;    // luminance weights rgb, amount s
  vec4 bloom_p;  // x cutoff, y scale, z flags (1=bloom, 2=lut, 4=auto_exp), w lower_grad
  vec4 balance;  // rgb post-mod / balance, w upper_grad
  vec4 exp_keys; // Key_ExtraDark, Key_Dark, Key_Light, Key_ExtraLight
  vec4 exp_ev;   // Exp_ExtraDark, Exp_Dark, Exp_Light, Exp_ExtraLight
} p;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 hable(vec3 x) {
  float A = p.abc.x, B = p.abc.y, C = p.abc.z, D = p.def.x, E = p.def.y, F = p.def.z;
  return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}

// Troy Sobotka AgX (hue-preserving highlight desaturation, calibrated to 18% mid-grey).
vec3 tonemap_agx(vec3 color) {
  color = max(color, vec3(0.0));
  const mat3 kAgxInset = mat3(
      0.842479062253094,  0.0423282422610123, 0.0423756549057051,
      0.0784335999999992, 0.878468636469772,  0.0784336,
      0.0792237451477643, 0.0791661274605434, 0.879142973793104);
  const mat3 kAgxOutset = mat3(
       1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
      -0.0980208811401368, 1.15190312990417,   -0.0980434501171241,
      -0.0990297440797205,-0.0989611768448433,  1.15107367264116);
  vec3 v = kAgxInset * color;
  const float kMinEv = -12.47393;
  const float kMaxEv = 4.026069;
  v = clamp(log2(max(v, vec3(1e-10))), vec3(kMinEv), vec3(kMaxEv));
  v = (v - kMinEv) / (kMaxEv - kMinEv);
  vec3 v2 = v * v;
  vec3 v4 = v2 * v2;
  v = +15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
  vec3 out_srgb = clamp(kAgxOutset * v, vec3(0.0), vec3(1.0));
  return pow(out_srgb, vec3(2.2));
}

// Gran Turismo / Uchimura Filmic (Hajime Uchimura, Polyphony Digital CEDEC 2017).
float gt_filmic_channel(float x) {
  const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33, b = 0.0;
  const float l0 = ((P - m) * l) / a;
  const float S0 = m + l0;
  const float S1 = m + a * l0;
  const float C2 = (a * P) / (P - S1);
  const float CP = -C2 / P;
  x = max(x, 0.0);
  float w0 = 1.0 - smoothstep(0.0, m, x);
  float w2 = step(m + l0, x);
  float w1 = 1.0 - w0 - w2;
  float T = m * pow(x / m, c) + b;
  float S = P - (P - S1) * exp(CP * (x - S0));
  float L = m + a * (x - m);
  return clamp(T * w0 + L * w1 + S * w2, 0.0, 1.0);
}

vec3 tonemap_gt_filmic(vec3 color) {
  return vec3(gt_filmic_channel(color.r), gt_filmic_channel(color.g), gt_filmic_channel(color.b));
}

// Khronos PBR Neutral Tone Mapper (2024 spec, exact hue fidelity for paint & liveries).
vec3 tonemap_pbr_neutral(vec3 color) {
  color = max(color, vec3(0.0));
  const float startCompression = 0.8 - 0.04;
  const float desaturation = 0.15;
  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;
  float peak = max(color.r, max(color.g, color.b));
  if (peak < startCompression) return clamp(color, vec3(0.0), vec3(1.0));
  const float d = 1.0 - startCompression;
  float newPeak = 1.0 - d * d / (peak + d - startCompression);
  color *= newPeak / max(peak, 1e-6);
  float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
  return clamp(mix(color, vec3(newPeak), g), vec3(0.0), vec3(1.0));
}

// Jimenez et al. GTAO multi-bounce diffuse albedo approximation (restores warm colored crevice bounce).
vec3 gtao_multi_bounce(float visibility, vec3 albedo_approx) {
  vec3 a =  2.0404 * albedo_approx - 0.3324;
  vec3 b = -4.7951 * albedo_approx + 0.6417;
  vec3 c =  2.7552 * albedo_approx + 0.6903;
  float v = clamp(visibility, 0.0, 1.0);
  return max(vec3(v), ((v * a + b) * v + c) * v);
}

float eval_auto_exposure() {
  uint flags = uint(p.bloom_p.z + 0.5);
  if ((flags & 4u) == 0u) return p.def.w;
  // Adapted luminance scaled into TrackSettings.xml key domain (~0..65).
  float L = max(texture(lum_adapt, vec2(0.5)).r, 0.0) * 64.0;
  float ev_curve = p.exp_ev.x;
  if (L <= p.exp_keys.x) {
    ev_curve = p.exp_ev.x;
  } else if (L <= p.exp_keys.y) {
    float t = clamp((L - p.exp_keys.x) / max(p.exp_keys.y - p.exp_keys.x, 1e-4), 0.0, 1.0);
    ev_curve = mix(p.exp_ev.x, p.exp_ev.y, t);
  } else if (L <= p.exp_keys.z) {
    float t = clamp((L - p.exp_keys.y) / max(p.exp_keys.z - p.exp_keys.y, 1e-4), 0.0, 1.0);
    ev_curve = mix(p.exp_ev.y, p.exp_ev.z, t);
  } else if (L <= p.exp_keys.w) {
    float t = clamp((L - p.exp_keys.z) / max(p.exp_keys.w - p.exp_keys.z, 1e-4), 0.0, 1.0);
    ev_curve = mix(p.exp_ev.z, p.exp_ev.w, t);
  } else {
    ev_curve = p.exp_ev.w;
  }
  float base_exp = max(p.def.w, 0.05);
  return clamp(base_exp * exp2(clamp(ev_curve, -2.0, 4.0)), 0.05, 8.0);
}

vec3 raw_scene(vec2 uv) { return min(max(texture(src, clamp(uv, vec2(0.001), vec2(0.999))).rgb, vec3(0.0)), vec3(64.0)); }

bool is_sky_depth(float z, bool rev_z) {
  return rev_z ? (z <= 1e-4) : (z >= 0.9999);
}

float linear_depth_m(vec2 uv, bool rev_z) {
  float z = texture(depth_tex, clamp(uv, vec2(0.001), vec2(0.999))).r;
  float near_p = max(opt.flags_near.y, 0.05);
  if (rev_z) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = 4000.0;
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

float ign_noise(vec2 px) {
  return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

// McGuire et al. (2012) depth-aware velocity motion blur helpers.
float soft_depth_compare(float za, float zb) {
  float soft_z = max(0.06 * min(za, zb), 0.25);
  return clamp(1.0 - (za - zb) / soft_z, 0.0, 1.0);
}

float cone_weight(float d, float v) {
  return clamp(1.0 - d / max(v, 1e-5), 0.0, 1.0);
}

float cylinder_weight(float d, float v) {
  return 1.0 - smoothstep(0.95 * v, 1.05 * v, d);
}

float eval_rotated_vignette(vec2 uv, vec4 geom, vec4 rot) {
  vec2 d = uv - geom.xy;
  vec2 r = vec2(rot.x * d.x + rot.y * d.y, -rot.y * d.x + rot.x * d.y);
  vec2 rad = max(geom.zw, vec2(0.05));
  float e = length(r / rad);
  float feather = max(rot.z, 0.05);
  return smoothstep(1.0 - feather, 1.0 + feather * 0.6, e);
}

float hash12(vec2 q) {
  vec3 p3 = fract(vec3(q.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

float procedural_lens_dirt(vec2 uv) {
  vec2 g1 = floor(uv * vec2(18.0, 11.0));
  vec2 f1 = fract(uv * vec2(18.0, 11.0)) - 0.5;
  float r1 = hash12(g1);
  float speck1 = smoothstep(0.32, 0.05, length(f1 - (vec2(hash12(g1 + 7.1), hash12(g1 + 19.3)) - 0.5) * 0.45)) *
                 step(0.74, r1);
  vec2 g2 = floor(uv * vec2(9.0, 6.0));
  vec2 f2 = fract(uv * vec2(9.0, 6.0)) - 0.5;
  float r2 = hash12(g2 + 41.0);
  float smudge = smoothstep(0.45, 0.05, length(f2)) * step(0.78, r2) * 0.65;
  return clamp(speck1 * 0.7 + smudge, 0.0, 1.0);
}

void main() {
  int mode = int(p.mode.x + 0.5);
  if (mode == 3) {
    // 16x16 centre-weighted luminance reduction (4 bilinear taps per cell).
    vec2 t = p.mode.yz;
    vec3 c = 0.25 * (raw_scene(v_uv + vec2(-t.x, -t.y)) + raw_scene(v_uv + vec2(t.x, -t.y)) +
                     raw_scene(v_uv + vec2(-t.x, t.y)) + raw_scene(v_uv + vec2(t.x, t.y)));
    float lum = dot(c, vec3(0.2125, 0.7154, 0.0721));
    float r = length((v_uv - vec2(0.5, 0.55)) * 2.0);
    float w = mix(1.0, 0.2, smoothstep(0.25, 1.15, r));
    out_color = vec4(lum * w, w, 0.0, 1.0);
    return;
  }
  if (mode == 4) {
    // 1x1 temporal adaptation: 8x8 bilinear taps over the 16x16 grid = full 256-cell average.
    float sum_l = 0.0, sum_w = 0.0;
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        vec2 uv = (vec2(float(x), float(y)) + 0.5) / 8.0;
        vec2 s = texture(src, uv).rg;
        sum_l += s.r;
        sum_w += s.g;
      }
    }
    float l_frame = sum_l / max(sum_w, 1e-4);
    float l_prev = texture(aux, vec2(0.5)).r;
    float alpha = clamp(p.mode.w, 0.0, 1.0);
    float l_adapt = mix(l_prev, l_frame, alpha);
    out_color = vec4(l_adapt, l_frame, 0.0, 1.0);
    return;
  }
  if (mode == 0) {
    float exp_scale = eval_auto_exposure();
    vec2 t = p.mode.yz;
    vec3 c = 0.25 * (raw_scene(v_uv + vec2(-t.x, -t.y)) + raw_scene(v_uv + vec2(t.x, -t.y)) +
                     raw_scene(v_uv + vec2(-t.x, t.y)) + raw_scene(v_uv + vec2(t.x, t.y))) * exp_scale;
    float lum = dot(c, vec3(0.2125, 0.7154, 0.0721));
    float cutoff = max(p.bloom_p.x, 0.01);
    float lo_g = max(p.bloom_p.w, 0.0);
    float hi_g = max(p.balance.w, 0.01);
    float mapped = max(lo_g * lum, hi_g * (lum - cutoff) + lo_g * cutoff);
    vec3 bright = max(c * (mapped / max(lum, 1e-4)) - vec3(cutoff), vec3(0.0));
    out_color = vec4(bright, 1.0);
    return;
  }
  if (mode == 1) {
    const float w[3] = float[](0.2270270, 0.3162162, 0.0702703);
    const float o[3] = float[](0.0, 1.3846154, 3.2307692);
    vec3 c = texture(src, v_uv).rgb * w[0];
    for (int i = 1; i < 3; ++i) {
      c += texture(src, v_uv + p.mode.yz * o[i]).rgb * w[i];
      c += texture(src, v_uv - p.mode.yz * o[i]).rgb * w[i];
    }
    out_color = vec4(c, 1.0);
    return;
  }
  if (mode == 5) {
    vec3 cur = texture(src, v_uv).rgb * p.mode.y;
    vec3 prev = texture(aux, v_uv).rgb * p.mode.z;
    out_color = vec4(cur + prev, 1.0);
    return;
  }

  // Mode 2: Final composite.
  uint flags = uint(p.bloom_p.z + 0.5);
  uint oflags = uint(opt.flags_near.x + 0.5);
  bool rev_z = (oflags & 16u) != 0u;
  float exp_scale = eval_auto_exposure();
  vec3 scene_raw = raw_scene(v_uv);

  float pix_dist = ((oflags & 3u) != 0u) ? linear_depth_m(v_uv, rev_z) : 100.0;

  // Depth of Field (Poisson bokeh gather driven by TrackSettings focal parameters).
  if ((oflags & 2u) != 0u && opt.dof.z > 0.005) {
    float focal_d = max(opt.dof.x, 2.0);
    float focal_rng = max(opt.dof.y, 6.0);
    float coc = clamp((abs(pix_dist - focal_d) - 4.0) / focal_rng, 0.0, 1.0) * opt.dof.z;
    if (coc > 0.02) {
      const vec2 poisson[8] = vec2[8](
          vec2(-0.707, -0.707), vec2(0.707, -0.707), vec2(-0.707, 0.707), vec2(0.707, 0.707),
          vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
      vec2 rad = opt.texel_size.xy * (coc * 5.5);
      vec3 acc = scene_raw;
      float wsum = 1.0;
      for (int i = 0; i < 8; ++i) {
        vec2 suv = clamp(v_uv + poisson[i] * rad, vec2(0.001), vec2(0.999));
        float sd = linear_depth_m(suv, rev_z);
        float w = (sd + 1.5 < pix_dist && pix_dist > focal_d) ? 0.15 : 1.0;
        acc += raw_scene(suv) * w;
        wsum += w;
      }
      scene_raw = mix(scene_raw, acc / wsum, coc);
    }
  }

  // Motion Blur: McGuire Tile-Max / Neighbor-Max velocity reconstruction (Tier 1-3)
  // with crisp car-silhouette preservation, or lightweight radial fallback on Tier 0.
  if ((oflags & 1u) != 0u && opt.mblur.z > 0.005) {
    int mcguire_taps = int(opt.sota_p.z + 0.5);
    if (mcguire_taps >= 7) {
      float car_mask = smoothstep(opt.mblur.w * 0.75, opt.mblur.w * 1.40, pix_dist);
      vec2 vn = texture(mblur_neighbor_tex, clamp(v_uv, vec2(0.001), vec2(0.999))).rg;
      // Scale velocity by shutter strength; cap blur span to 24 pixels to stay artifact-free.
      float shutter = clamp(opt.mblur.z * 0.85 + 0.20, 0.15, 1.15);
      vn *= shutter;
      vec2 texel = opt.texel_size.xy;
      float max_uv = length(texel) * 24.0;
      float vn_len = length(vn);
      if (vn_len > max_uv) {
        vn *= (max_uv / vn_len);
        vn_len = max_uv;
      }
      // Only run gather when the tile neighborhood actually has visible motion (> 0.6 px)
      // and we are outside the protected player-car hull.
      if (vn_len > length(texel) * 0.6 && car_mask > 0.02) {
        vec4 mv_c = texture(motion_tex, v_uv);
        vec2 vc = mv_c.rg * shutter * car_mask;
        float vc_len = max(length(vc), length(texel) * 0.5);
        float jitter = fract(ign_noise(gl_FragCoord.xy) + opt.texel_size.w * 0.61803399) - 0.5;
        // Center sample weighted inversely by center velocity (McGuire eq. 1).
        float wsum = 1.0 / max(vc_len, 1e-4);
        vec3 acc = scene_raw * wsum;
        int n_taps = clamp(mcguire_taps, 7, 15);
        for (int i = 0; i < n_taps; ++i) {
          float t = ((float(i) + 0.5 + jitter) / float(n_taps) - 0.5) * 2.0;
          vec2 suv = clamp(v_uv + vn * t, vec2(0.001), vec2(0.999));
          vec4 mv_s = texture(motion_tex, suv);
          float ds = mv_s.a;
          // Reject foreground player-car samples from smearing onto background road/sky.
          float s_car_mask = smoothstep(opt.mblur.w * 0.72, opt.mblur.w * 1.35, ds);
          vec2 vs = mv_s.rg * shutter * s_car_mask;
          float vs_len = length(vs);
          float sample_d = abs(t) * vn_len;
          float f = soft_depth_compare(pix_dist, ds);
          float b = soft_depth_compare(ds, pix_dist);
          float ay = f * cone_weight(sample_d, vs_len) +
                     b * cone_weight(sample_d, vc_len) +
                     cylinder_weight(sample_d, vs_len) * cylinder_weight(sample_d, vc_len) * 2.0;
          ay *= s_car_mask;
          acc += raw_scene(suv) * ay;
          wsum += ay;
        }
        scene_raw = mix(scene_raw, acc / max(wsum, 1e-4), car_mask);
      }
    } else {
      // Lightweight Tier-0 radial speed blur fallback.
      vec2 to_edge = v_uv - opt.mblur.xy;
      float r = length(to_edge);
      float car_mask = smoothstep(opt.mblur.w * 0.75, opt.mblur.w * 1.45, pix_dist);
      float blur_w = smoothstep(0.10, 0.85, r) * opt.mblur.z * car_mask;
      if (blur_w > 0.005) {
        vec2 step_uv = to_edge * (blur_w * 0.032);
        vec3 acc = scene_raw * 0.28;
        float wsum = 0.28;
        for (int k = 1; k <= 5; ++k) {
          float fk = float(k) / 5.0;
          float wt = 0.18 * (1.0 - 0.55 * fk);
          acc += raw_scene(v_uv - step_uv * fk) * wt;
          wsum += wt;
        }
        scene_raw = acc / wsum;
      }
    }
  }

  vec3 c_lin = scene_raw * exp_scale;

  // XeGTAO multi-bounce diffuse occlusion + Bent-Normal directional darkening.
  if (opt.modern_p.x > 0.001) {
    vec4 gtao = texture(ao_tex, clamp(v_uv, vec2(0.001), vec2(0.999)));
    float ao_vis = clamp(gtao.r, 0.0, 1.0);
    // Estimate local diffuse reflectance for Jimenez multi-bounce colored AO restoration.
    vec3 albedo_est = clamp(c_lin / (c_lin + vec3(0.45)), vec3(0.02), vec3(0.92));
    vec3 ao_mb = gtao_multi_bounce(ao_vis, albedo_est);
    // Bent-normal crevice specular/sky occlusion: when bent normal tilts sharply from view Z
    // inside wheel wells / under fenders, add subtle directional grounding.
    vec3 bent_n = normalize(gtao.gba * 2.0 - 1.0);
    float bn_open = clamp(bent_n.z * 0.5 + 0.5, 0.25, 1.0);
    float bn_mod = mix(1.0, mix(bn_open, 1.0, ao_vis), clamp(opt.sota_p.w, 0.0, 1.0) * 0.35);
    // Preserve bright emissives (headlights, brake lights, sun disc) from AO darkening.
    float lum_lin = dot(c_lin, vec3(0.2126, 0.7152, 0.0722));
    float emissive_keep = 1.0 / (1.0 + max(lum_lin - 1.5, 0.0) * 1.4);
    float ao_amt = clamp(opt.modern_p.x, 0.0, 1.0) * emissive_keep;
    c_lin *= mix(vec3(1.0), ao_mb * bn_mod, ao_amt);
  }

  // Screen-Space Contact Shadows (SSCS): micro-scale sun contact grounding on tires, curbs & rocks.
  if (opt.sota_p.y > 0.001) {
    float sscs = clamp(texture(sscs_tex, clamp(v_uv, vec2(0.001), vec2(0.999))).r, 0.0, 1.0);
    float lum_lin = dot(c_lin, vec3(0.2126, 0.7152, 0.0722));
    float emissive_keep = 1.0 / (1.0 + max(lum_lin - 1.4, 0.0) * 1.2);
    c_lin *= mix(1.0, sscs, clamp(opt.sota_p.y, 0.0, 1.0) * emissive_keep);
  }

  // God rays / volumetric sun shafts (radial sky occlusion march toward sun_screen_uv).
  if ((oflags & 4u) != 0u && opt.sun.z > 0.01 && opt.sun.w > 0.005) {
    vec2 sun_uv = opt.sun.xy;
    vec2 delta = (sun_uv - v_uv) * (1.0 / 12.0);
    float step_len = length(delta);
    if (step_len > 0.065) delta *= (0.065 / step_len);
    vec2 sample_uv = v_uv;
    float illum = 0.0;
    float decay = 1.0;
    for (int i = 0; i < 12; ++i) {
      sample_uv += delta;
      if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0) break;
      float sz = texture(depth_tex, sample_uv).r;
      if (is_sky_depth(sz, rev_z)) {
        illum += decay;
      }
      decay *= 0.88;
    }
    illum /= 8.2;
    float sun_dist = length((v_uv - sun_uv) * vec2(1.6, 1.0));
    float radial_falloff = exp(-sun_dist * 2.2);
    vec3 shaft = opt.sun_color.rgb * (illum * radial_falloff * opt.sun.z * opt.sun.w);
    c_lin += shaft;
  }

  // Lens flare (halo + anamorphic streak + chromatic ghosts) & procedural lens dirt.
  if ((oflags & 8u) != 0u && opt.sun.z > 0.01 && opt.sun_color.w > 0.005) {
    vec2 sun_uv = opt.sun.xy;
    float unocc = 0.0;
    vec2 o = opt.texel_size.xy * 6.0;
    unocc += is_sky_depth(texture(depth_tex, clamp(sun_uv, vec2(0.002), vec2(0.998))).r, rev_z) ? 0.36 : 0.0;
    unocc += is_sky_depth(texture(depth_tex, clamp(sun_uv + vec2(o.x, 0.0), vec2(0.002), vec2(0.998))).r, rev_z) ? 0.16 : 0.0;
    unocc += is_sky_depth(texture(depth_tex, clamp(sun_uv - vec2(o.x, 0.0), vec2(0.002), vec2(0.998))).r, rev_z) ? 0.16 : 0.0;
    unocc += is_sky_depth(texture(depth_tex, clamp(sun_uv + vec2(0.0, o.y), vec2(0.002), vec2(0.998))).r, rev_z) ? 0.16 : 0.0;
    unocc += is_sky_depth(texture(depth_tex, clamp(sun_uv - vec2(0.0, o.y), vec2(0.002), vec2(0.998))).r, rev_z) ? 0.16 : 0.0;

    float flare_vis = unocc * opt.sun.z * opt.sun_color.w;
    if (flare_vis > 0.005) {
      vec2 asp = vec2(1.777, 1.0);
      vec2 d_sun = (v_uv - sun_uv) * asp;
      float halo = exp(-length(d_sun) * 5.2) * 0.42;
      float streak = exp(-abs(d_sun.y) * 52.0) * exp(-abs(d_sun.x) * 2.4) * 0.38;
      vec3 flare = opt.sun_color.rgb * (halo + streak);

      vec2 axis = vec2(0.5) - sun_uv;
      const float g_pos[3] = float[](0.55, 1.15, 1.72);
      const float g_rad[3] = float[](0.075, 0.12, 0.055);
      const vec3 g_col[3] = vec3[3](vec3(1.0, 0.72, 0.45), vec3(0.55, 0.85, 1.0), vec3(1.0, 0.58, 0.82));
      for (int i = 0; i < 3; ++i) {
        vec2 gc = vec2(0.5) + axis * g_pos[i];
        float gd = length((v_uv - gc) * asp);
        float ring = smoothstep(g_rad[i], g_rad[i] * 0.25, gd) * 0.16;
        flare += g_col[i] * ring;
      }

      float dirt = procedural_lens_dirt(v_uv) * exp(-length(d_sun) * 1.65) * 0.18;
      flare += opt.sun_color.rgb * dirt;
      c_lin += flare * flare_vis;
    }
  }

  // Physically-based Jimenez / Dual-Kawase bloom composite (energy-conserving blend).
  if ((flags & 1u) != 0u) {
    vec3 bloom_rgb = max(texture(aux, clamp(v_uv, vec2(0.001), vec2(0.999))).rgb, vec3(0.0));
    c_lin += bloom_rgb * p.bloom_p.y;
  }

  // Selectable SOTA Filmic Tonemapper (0 = FH1 Hable default, 1 = AgX, 2 = GT Filmic, 3 = Khronos PBR Neutral).
  // FH1's TrackSettings exposure (~0.31) is calibrated for Hable's ~2.4x mid-tone shoulder gain;
  // matching the exposure pivot keeps AgX / GT Filmic / PBR Neutral luminance-matched at mid-grey.
  int tm_mode = int(opt.sota_p.x + 0.5);
  vec3 t;
  if (tm_mode == 1) {
    t = tonemap_agx(c_lin * 2.5);
  } else if (tm_mode == 2) {
    t = tonemap_gt_filmic(c_lin * 2.3);
  } else if (tm_mode == 3) {
    t = tonemap_pbr_neutral(c_lin * 2.2);
  } else {
    vec3 denom = max(hable(vec3(max(p.abc.w, 0.05))), vec3(1e-4));
    t = clamp(hable(c_lin) / denom, vec3(0.0), vec3(1.0));
  }

  // N^3 3D colour-grading volume LUT (texel-centre addressing (N-1)/N + 0.5/N).
  if ((flags & 2u) != 0u && opt.modern_p.z > 0.001) {
    float n = max(opt.modern_p.y, 2.0);
    vec3 uvw = clamp(t, vec3(0.0), vec3(1.0)) * ((n - 1.0) / n) + vec3(0.5 / n);
    t = mix(t, texture(lut3d, uvw).rgb, clamp(opt.modern_p.z, 0.0, 1.0));
  }

  // Paired rotated elliptical VignetteTop + VignetteBottom (SKY_AND_TOD.md §5.5 step 4):
  // evaluated in sqrt (gamma-2) space with FH1's subtractive sky-tinting formula.
  float vig_scale = clamp(p.mode.w, 0.0, 1.0);
  if ((oflags & 32u) != 0u) {
    vec3 s = sqrt(max(t, vec3(0.0)));
    if (opt.vig_top_col.w > 0.001) {
      float m_top = eval_rotated_vignette(v_uv, opt.vig_top_geom, opt.vig_top_rot);
      float a_top = clamp(m_top * opt.vig_top_col.w * max(vig_scale, 0.35), 0.0, 1.0);
      vec3 target_top = clamp(s - vec3(1.0) + opt.vig_top_col.rgb, vec3(0.0), vec3(1.0));
      s = mix(s, target_top, a_top);
    }
    if (opt.vig_bot_col.w > 0.001) {
      float m_bot = eval_rotated_vignette(v_uv, opt.vig_bot_geom, opt.vig_bot_rot);
      float a_bot = clamp(m_bot * opt.vig_bot_col.w * max(vig_scale, 0.35), 0.0, 1.0);
      vec3 target_bot = clamp(s - vec3(1.0) + opt.vig_bot_col.rgb, vec3(0.0), vec3(1.0));
      s = mix(s, target_bot, a_bot);
    }
    t = s * s;
  } else if (vig_scale > 0.001) {
    vec2 q = (v_uv - vec2(0.5)) * 2.0;
    float r2 = dot(q, q);
    float vig = clamp(1.0 - 0.14 * vig_scale * pow(r2 * 0.5, 1.6), 0.72, 1.0);
    t *= vig;
  }

  // ColorSettings_Templates.xml Contrast + Brightness, then Desaturation + post-mod.
  float contrast = opt.flags_near.z > 0.01 ? opt.flags_near.z : 1.0;
  float bright_off = opt.flags_near.w;
  t = clamp((t - vec3(0.5)) * contrast + vec3(0.5 + bright_off), vec3(0.0), vec3(1.0));

  float l = dot(t, p.desat.rgb);
  t = mix(vec3(l), t, p.desat.w);
  if (opt.dof.w > 0.001) {
    vec3 sep = clamp(vec3(dot(t, vec3(0.393, 0.769, 0.189)),
                          dot(t, vec3(0.349, 0.686, 0.168)),
                          dot(t, vec3(0.272, 0.534, 0.131))), vec3(0.0), vec3(1.0));
    t = mix(t, sep, clamp(opt.dof.w, 0.0, 1.0));
  }
  t = clamp(t * p.balance.rgb, vec3(0.0), vec3(1.0));
  // FH1 display transfer (SKY_AND_TOD.md §5.5): gamma-2.0 sqrt output, preserving modern_p.w alpha.
  out_color = vec4(sqrt(t), opt.modern_p.w);
}
