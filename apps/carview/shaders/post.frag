#version 450
// carview: the drive's HDR post-processing chain (SPEC-SKY-AND-TOD §5, SPEC-VFX-AND-PARTICLES).
// Modes (p.mode.x):
//   0 bright pass: exposed HDR scene above bloom cutoff with lower/upper gradient curve
//   1 blur: separable Gaussian blur along p.mode.yz (5 bilinear taps)
//   2 composite: HDR scene + Depth of Field + radial motion blur + god rays / sun shafts +
//                lens flare & lens dirt + bloom -> exposure (4-key adaptive EV or fixed) ->
//                Hable filmic curve -> 16^3 3D LUT -> paired rotated elliptical vignettes ->
//                contrast/brightness + desat/post-mod -> sRGB
//   3 lum_16: 16x16 centre-weighted Rec.709 luminance reduction from HDR scene
//   4 lum_adapt: 1x1 temporal adaptation from 16x16 lum grid + previous 1x1 lum
//   5 upsample_add: bilinear upsample of aux * p.mode.w added to src (bloom pyramid)

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D aux;
layout(set = 0, binding = 2) uniform sampler3D lut3d;
layout(set = 0, binding = 3) uniform sampler2D lum_adapt;
layout(set = 0, binding = 4) uniform sampler2D depth_tex;

layout(set = 0, binding = 5) uniform Optical {
  vec4 flags_near;    // x flags (1=mblur, 2=dof, 4=god_rays, 8=lens_flare, 16=reverse_z, 32=paired_vig), y near, z contrast, w brightness
  vec4 mblur;         // xy center uv, z strength [0..1], w car_protect_dist_m
  vec4 dof;           // x focal_distance_m, y focal_range_m, z max_blur [0..1], w unused
  vec4 sun;           // xy sun_screen_uv, z sun_visibility [0..1], w god_ray_strength
  vec4 sun_color;     // rgb sun_scatter_color, w lens_flare_strength
  vec4 vig_top_col;   // rgb color, w opacity
  vec4 vig_top_geom;  // xy center, zw radius
  vec4 vig_top_rot;   // x cos(rot), y sin(rot), z feather, w unused
  vec4 vig_bot_col;   // rgb color, w opacity
  vec4 vig_bot_geom;  // xy center, zw radius
  vec4 vig_bot_rot;   // x cos(rot), y sin(rot), z feather, w unused
  vec4 texel_size;    // xy 1/w, 1/h, z time_sec, w unused
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

vec3 encode_srgb(vec3 c) {
  c = clamp(c, vec3(0.0), vec3(1.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
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
  float ev_total = clamp(ev_curve + p.def.w, -2.0, 6.0);
  return exp2(ev_total);
}

vec3 raw_scene(vec2 uv) { return min(max(texture(src, clamp(uv, vec2(0.001), vec2(0.999))).rgb, vec3(0.0)), vec3(4.0)); }

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
    // Smooth radial centre weight emphasizing the road ahead (SPEC-SKY-AND-TOD §5.2).
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
    // Bright pass on exposed linear HDR scene (SPEC-SKY-AND-TOD §5.3).
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
    // Separable Gaussian blur (sigma ~2 texels, 5 bilinear taps).
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
    // Multi-mip bloom upsample combine: src * weight_cur + aux * weight_prev.
    vec3 cur = texture(src, v_uv).rgb * p.mode.y;
    vec3 prev = texture(aux, v_uv).rgb * p.mode.z;
    out_color = vec4(cur + prev, 1.0);
    return;
  }

  // Mode 2: Final composite (SPEC-SKY-AND-TOD §5.5 + SPEC-VFX-AND-PARTICLES optical post-FX).
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
        // Reject sharp foreground car pixels leaking onto distant background blur.
        float w = (sd + 1.5 < pix_dist && pix_dist > focal_d) ? 0.15 : 1.0;
        acc += raw_scene(suv) * w;
        wsum += w;
      }
      scene_raw = mix(scene_raw, acc / wsum, coc);
    }
  }

  // Speed / camera radial motion blur (keeps player car crisp via depth mask).
  if ((oflags & 1u) != 0u && opt.mblur.z > 0.005) {
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

  vec3 c_lin = scene_raw * exp_scale;

  // God rays / volumetric sun shafts (radial sky occlusion march toward sun_screen_uv).
  if ((oflags & 4u) != 0u && opt.sun.z > 0.01 && opt.sun.w > 0.005) {
    vec2 sun_uv = opt.sun.xy;
    vec2 delta = (sun_uv - v_uv) * (1.0 / 12.0);
    // Clamp max march step so rays stay coherent when sun is near screen edge.
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
    // 5-tap depth occlusion check around the sun disk so mountains/buildings block lens flare.
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
      // Soft warm sun halo + crisp horizontal anamorphic streak.
      float halo = exp(-length(d_sun) * 5.2) * 0.42;
      float streak = exp(-abs(d_sun.y) * 52.0) * exp(-abs(d_sun.x) * 2.4) * 0.38;
      vec3 flare = opt.sun_color.rgb * (halo + streak);

      // Three chromatic lens ghosts reflected through screen centre (0.5, 0.5).
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

      // Subtle lens dirt illuminated around the sun.
      float dirt = procedural_lens_dirt(v_uv) * exp(-length(d_sun) * 1.65) * 0.18;
      flare += opt.sun_color.rgb * dirt;
      c_lin += flare * flare_vis;
    }
  }

  if ((flags & 1u) != 0u) {
    c_lin += texture(aux, v_uv).rgb * p.bloom_p.y;
  }
  vec3 denom = max(hable(vec3(max(p.abc.w, 0.05))), vec3(1e-4));
  vec3 t = clamp(hable(c_lin) / denom, vec3(0.0), vec3(1.0));

  // 16x16x16 3D colour-grading volume LUT (texel-centre addressing 15/16 + 1/32).
  if ((flags & 2u) != 0u) {
    vec3 uvw = t * (15.0 / 16.0) + vec3(1.0 / 32.0);
    t = texture(lut3d, uvw).rgb;
  }

  // Paired rotated elliptical VignetteTop + VignetteBottom (Vignette_Templates.xml), or legacy vignette.
  float vig_scale = clamp(p.mode.w, 0.0, 1.0);
  if ((oflags & 32u) != 0u) {
    if (opt.vig_top_col.w > 0.001) {
      float m_top = eval_rotated_vignette(v_uv, opt.vig_top_geom, opt.vig_top_rot);
      float a_top = clamp(m_top * opt.vig_top_col.w * max(vig_scale, 0.35) * 0.45, 0.0, 0.55);
      t = mix(t, t * opt.vig_top_col.rgb, a_top);
    }
    if (opt.vig_bot_col.w > 0.001) {
      float m_bot = eval_rotated_vignette(v_uv, opt.vig_bot_geom, opt.vig_bot_rot);
      float a_bot = clamp(m_bot * opt.vig_bot_col.w * max(vig_scale, 0.35) * 0.45, 0.0, 0.55);
      t = mix(t, t * opt.vig_bot_col.rgb, a_bot);
    }
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
  t = clamp(t * p.balance.rgb, vec3(0.0), vec3(1.0));
  out_color = vec4(encode_srgb(t), 1.0);
}
