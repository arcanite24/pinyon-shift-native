#version 450
// carview: jitter-aware temporal accumulation & spatial-temporal upscaler (TAA-U fallback / preview).
// Uses 3x3 YCoCg variance clipping, closest-depth velocity dilation, reactive-mask
// weighting, and chroma/disocclusion rejection to prevent ghosting on high-contrast
// moving overlays such as the 3D driving-line chevrons.

layout(set = 0, binding = 0) uniform sampler2D curr_color;
layout(set = 0, binding = 1) uniform sampler2D history_color;
layout(set = 0, binding = 2) uniform sampler2D motion_tex;

layout(push_constant) uniform TemporalPush {
  vec4 render_size;  // xy = render W,H, zw = 1/render W,H
  vec4 output_size;  // xy = output W,H, zw = 1/output W,H
  vec4 params;       // xy = jitter_uv, z = reset (1 = discard history), w = blend_max (0.9)
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 rgb_to_ycocg(vec3 c) {
  float y = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
  float co = 0.5 * c.r - 0.5 * c.b;
  float cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
  return vec3(y, co, cg);
}

vec3 ycocg_to_rgb(vec3 ycc) {
  float t = ycc.x - ycc.z;
  return clamp(vec3(t + ycc.y, ycc.x + ycc.z, t - ycc.y), vec3(0.0), vec3(1.0));
}

// Ray-to-AABB clip toward box centre in YCoCg space (preserves hue without corner clamping).
vec3 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec3 hist) {
  vec3 center = 0.5 * (aabb_max + aabb_min);
  vec3 extents = max(0.5 * (aabb_max - aabb_min), vec3(1e-4));
  vec3 offset = hist - center;
  vec3 unit = abs(offset / extents);
  float max_unit = max(unit.x, max(unit.y, unit.z));
  if (max_unit > 1.0) {
    return center + offset / max_unit;
  }
  return hist;
}

vec3 sample_catmull_rom(sampler2D tex, vec2 uv, vec2 tex_size, vec2 inv_size) {
  vec2 pos = uv * tex_size;
  vec2 tc = floor(pos - 0.5) + 0.5;
  vec2 f = pos - tc;
  vec2 f2 = f * f;
  vec2 f3 = f2 * f;
  vec2 w0 = f2 - 0.5 * (f3 + f);
  vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
  vec2 w3 = 0.5 * (f3 - f2);
  vec2 w2 = 1.0 - w0 - w1 - w3;
  vec2 s0 = w0 + w1;
  vec2 s1 = w2 + w3;
  vec2 t0 = (tc - 1.0 + w1 / max(s0, vec2(1e-5))) * inv_size;
  vec2 t1 = (tc + 1.0 + w3 / max(s1, vec2(1e-5))) * inv_size;
  return max(texture(tex, vec2(t0.x, t0.y)).rgb * s0.x * s0.y +
             texture(tex, vec2(t1.x, t0.y)).rgb * s1.x * s0.y +
             texture(tex, vec2(t0.x, t1.y)).rgb * s0.x * s1.y +
             texture(tex, vec2(t1.x, t1.y)).rgb * s1.x * s1.y, vec3(0.0));
}

void main() {
  // apply_ndc_jitter() shifts NDC by +2*j/render_size, moving UV by +pc.params.xy.
  // Sample the jittered current frame at v_uv + pc.params.xy to align with unjittered v_uv.
  vec2 src_uv = clamp(v_uv + pc.params.xy, vec2(0.0), vec2(1.0));
  vec2 d = pc.render_size.zw;

  vec3 c0 = texture(curr_color, src_uv).rgb;
  if (pc.params.z > 0.5) {
    out_color = vec4(c0, 0.0);
    return;
  }

  // 3x3 YCoCg neighborhood moments + cross/box bounds (Salvi variance clipping).
  vec3 c0_ycc = rgb_to_ycocg(c0);
  vec3 m1 = vec3(0.0);
  vec3 m2 = vec3(0.0);
  vec3 cmin9 = c0_ycc;
  vec3 cmax9 = c0_ycc;
  vec3 cmin5 = c0_ycc;
  vec3 cmax5 = c0_ycc;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      vec3 s_rgb = (x == 0 && y == 0)
                       ? c0
                       : texture(curr_color, clamp(src_uv + vec2(float(x), float(y)) * d, vec2(0.0), vec2(1.0))).rgb;
      vec3 s_ycc = rgb_to_ycocg(s_rgb);
      m1 += s_ycc;
      m2 += s_ycc * s_ycc;
      cmin9 = min(cmin9, s_ycc);
      cmax9 = max(cmax9, s_ycc);
      if (x == 0 || y == 0) {
        cmin5 = min(cmin5, s_ycc);
        cmax5 = max(cmax5, s_ycc);
      }
    }
  }
  vec3 mu = m1 * (1.0 / 9.0);
  vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(0.0)));
  vec3 nmin = 0.5 * (cmin5 + cmin9);
  vec3 nmax = 0.5 * (cmax5 + cmax9);

  // Closest-depth 3x3 motion dilation + max reactive mask (prevents silhouette & overlay trails).
  vec2 best_vel = vec2(0.0);
  float best_depth = 1e9;
  float max_reactive = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      vec4 mv = texture(motion_tex, clamp(v_uv + vec2(float(x), float(y)) * d, vec2(0.0), vec2(1.0)));
      max_reactive = max(max_reactive, mv.b);
      if (mv.a < best_depth) {
        best_depth = mv.a;
        best_vel = mv.rg;
      }
    }
  }

  vec2 hist_uv = v_uv - best_vel;
  if (any(lessThan(hist_uv, vec2(0.0))) || any(greaterThan(hist_uv, vec2(1.0)))) {
    out_color = vec4(c0, 0.0);
    return;
  }

  vec3 raw_hist = sample_catmull_rom(history_color, hist_uv, pc.output_size.xy, pc.output_size.zw);
  vec3 hist_ycc = rgb_to_ycocg(raw_hist);

  // Tighten variance box on reactive edges and high-chroma overlays (e.g. 3D driving-line chevrons).
  float chroma_sat = max(length(c0_ycc.yz), length(hist_ycc.yz));
  float gamma = mix(1.05, 0.72, clamp(max(max_reactive, smoothstep(0.08, 0.30, chroma_sat)), 0.0, 1.0));
  vec3 vmin = max(nmin, mu - gamma * sigma);
  vec3 vmax = min(nmax, mu + gamma * sigma);
  vec3 clipped_ycc = clip_aabb(vmin, vmax, hist_ycc);

  // Disocclusion & chroma/luma mismatch rejection:
  // Transparent world overlays (3D racing-line chevrons) and scrolling road features whose
  // previous-frame color disagrees with the current neighborhood must reject stale history
  // immediately instead of smearing ghost trails across the asphalt.
  float clip_dist = length(hist_ycc - clipped_ycc);
  float diff_chroma = length(hist_ycc.yz - c0_ycc.yz);
  float rel_luma = abs(hist_ycc.x - c0_ycc.x) / max(c0_ycc.x + 0.12, 0.12);
  float box_reject = smoothstep(0.008, 0.065, clip_dist);
  float chroma_reject = smoothstep(0.025, 0.11, diff_chroma);
  float luma_reject = smoothstep(0.07, 0.28, rel_luma);
  float reject = clamp(max(max(box_reject, chroma_reject), max(luma_reject * 0.85, max_reactive)), 0.0, 1.0);

  vec3 hist_rgb = ycocg_to_rgb(clipped_ycc);
  float motion_px = length(best_vel * pc.output_size.xy);
  float base_alpha = mix(pc.params.w, 0.35, clamp(motion_px * 0.05, 0.0, 1.0));
  float keep = 1.0 - reject;
  float alpha = base_alpha * keep * keep;
  out_color = vec4(mix(c0, hist_rgb, alpha), 0.0);
}
