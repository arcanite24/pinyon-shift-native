#version 450
// carview: the drive's HDR post-processing chain (SPEC-SKY-AND-TOD §5).
// Modes (p.mode.x):
//   0 bright pass: exposed HDR scene above bloom cutoff with lower/upper gradient curve
//   1 blur: separable Gaussian blur along p.mode.yz (5 bilinear taps)
//   2 composite: linear scene + bloom -> exposure (4-key adaptive EV or fixed) ->
//                Hable filmic curve -> 16^3 3D LUT -> vignette -> desat/post-mod -> sRGB
//   3 lum_16: 16x16 centre-weighted Rec.709 luminance reduction from HDR scene
//   4 lum_adapt: 1x1 temporal adaptation from 16x16 lum grid + previous 1x1 lum
//   5 upsample_add: bilinear upsample of aux * p.mode.w added to src (bloom pyramid)

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D aux;
layout(set = 0, binding = 2) uniform sampler3D lut3d;
layout(set = 0, binding = 3) uniform sampler2D lum_adapt;

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

vec3 raw_scene(vec2 uv) { return min(max(texture(src, uv).rgb, vec3(0.0)), vec3(4.0)); }

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

  // Mode 2: Final composite (SPEC-SKY-AND-TOD §5.5 exact order).
  uint flags = uint(p.bloom_p.z + 0.5);
  float exp_scale = eval_auto_exposure();
  vec3 c_lin = raw_scene(v_uv) * exp_scale;
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

  // Subtle elliptical vignette scaled by TOD VignetteAmount.
  float vig_amt = clamp(p.mode.w, 0.0, 1.0);
  if (vig_amt > 0.001) {
    vec2 q = (v_uv - vec2(0.5)) * 2.0;
    float r2 = dot(q, q);
    float vig = clamp(1.0 - 0.14 * vig_amt * pow(r2 * 0.5, 1.6), 0.72, 1.0);
    t *= vig;
  }

  // Desaturation + post-mod / colour balance.
  float l = dot(t, p.desat.rgb);
  t = mix(vec3(l), t, p.desat.w);
  t = clamp(t * p.balance.rgb, vec3(0.0), vec3(1.0));
  out_color = vec4(encode_srgb(t), 1.0);
}
