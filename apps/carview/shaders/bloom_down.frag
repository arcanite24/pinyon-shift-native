#version 450
// carview: Physically-Based Jimenez / Call of Duty SIGGRAPH 2014 13-tap progressive
// bloom downsample with Karis Average anti-firefly weighting and soft-knee threshold on mip 0.
//
// Descriptor layout matches post_set_layout_ (binding 0 = src, binding 3 = lum_adapt).
// Push constants match Post (128 bytes):
//   mode.x = mip_index (0 = prefilter + Karis average from HDR scene; 1..5 = 13-tap downsample)
//   mode.yz = src_texel_size (1/src_w, 1/src_h)
//   mode.w = soft_knee (default 0.5)

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 3) uniform sampler2D lum_adapt;

layout(push_constant) uniform Post {
  vec4 mode;     // x mip_index, yz src texel step, w soft_knee
  vec4 abc;
  vec4 def;      // w exposure
  vec4 desat;
  vec4 bloom_p;  // x cutoff, y scale, z flags (4=auto_exp), w lower_grad
  vec4 balance;  // w upper_grad
  vec4 exp_keys;
  vec4 exp_ev;
} p;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float eval_auto_exposure() {
  uint flags = uint(p.bloom_p.z + 0.5);
  if ((flags & 4u) == 0u) return p.def.w;
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

float karis_weight(vec3 c) {
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  return 1.0 / (1.0 + max(luma, 0.0));
}

vec3 soft_threshold(vec3 c, float exp_scale) {
  c = min(max(c, vec3(0.0)), vec3(64.0)) * exp_scale;
  float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float cutoff = max(p.bloom_p.x, 0.01);
  float knee = cutoff * clamp(p.mode.w > 0.001 ? p.mode.w : 0.45, 0.05, 1.0);
  float lo_g = max(p.bloom_p.w, 0.0);
  float hi_g = max(p.balance.w, 0.01);
  // Soft-knee quadratic curve honoring FH1 lower/upper bloom gradient while preventing shimmer.
  float rq = clamp(lum - cutoff + knee, 0.0, 2.0 * knee);
  rq = (rq * rq) / max(4.0 * knee, 1e-5);
  float excess = max(rq, lum - cutoff);
  float gain = mix(lo_g, hi_g, smoothstep(cutoff - knee, cutoff + knee, lum));
  return c * (max(excess, 0.0) * gain / max(lum, 1e-4));
}

vec3 sample_src(vec2 uv) {
  return min(max(texture(src, clamp(uv, vec2(0.0005), vec2(0.9995))).rgb, vec3(0.0)), vec3(64.0));
}

void main() {
  vec2 d = p.mode.yz;
  int mip = int(p.mode.x + 0.5);

  // Jimenez 13-tap overlapping box/cross filter pattern:
  //   a . b . c
  //   . d . e .
  //   f . g . h
  //   . i . j .
  //   k . l . m
  vec3 a = sample_src(v_uv + vec2(-2.0 * d.x,  2.0 * d.y));
  vec3 b = sample_src(v_uv + vec2( 0.0,        2.0 * d.y));
  vec3 c = sample_src(v_uv + vec2( 2.0 * d.x,  2.0 * d.y));

  vec3 d_t = sample_src(v_uv + vec2(-d.x,  d.y));
  vec3 e_t = sample_src(v_uv + vec2( d.x,  d.y));

  vec3 f = sample_src(v_uv + vec2(-2.0 * d.x,  0.0));
  vec3 g = sample_src(v_uv);
  vec3 h = sample_src(v_uv + vec2( 2.0 * d.x,  0.0));

  vec3 i_t = sample_src(v_uv + vec2(-d.x, -d.y));
  vec3 j_t = sample_src(v_uv + vec2( d.x, -d.y));

  vec3 k = sample_src(v_uv + vec2(-2.0 * d.x, -2.0 * d.y));
  vec3 l = sample_src(v_uv + vec2( 0.0,       -2.0 * d.y));
  vec3 m = sample_src(v_uv + vec2( 2.0 * d.x, -2.0 * d.y));

  if (mip == 0) {
    // Prefilter + Karis average across 5 sub-quads eliminates subpixel specular fireflies.
    float exp_scale = eval_auto_exposure();
    a = soft_threshold(a, exp_scale);
    b = soft_threshold(b, exp_scale);
    c = soft_threshold(c, exp_scale);
    d_t = soft_threshold(d_t, exp_scale);
    e_t = soft_threshold(e_t, exp_scale);
    f = soft_threshold(f, exp_scale);
    g = soft_threshold(g, exp_scale);
    h = soft_threshold(h, exp_scale);
    i_t = soft_threshold(i_t, exp_scale);
    j_t = soft_threshold(j_t, exp_scale);
    k = soft_threshold(k, exp_scale);
    l = soft_threshold(l, exp_scale);
    m = soft_threshold(m, exp_scale);

    vec3 g0 = (d_t + e_t + i_t + j_t) * 0.25;
    vec3 g1 = (a + b + f + g) * 0.25;
    vec3 g2 = (b + c + g + h) * 0.25;
    vec3 g3 = (f + g + k + l) * 0.25;
    vec3 g4 = (g + h + l + m) * 0.25;

    float w0 = karis_weight(g0) * 0.5;
    float w1 = karis_weight(g1) * 0.125;
    float w2 = karis_weight(g2) * 0.125;
    float w3 = karis_weight(g3) * 0.125;
    float w4 = karis_weight(g4) * 0.125;
    float wsum = max(w0 + w1 + w2 + w3 + w4, 1e-5);
    out_color = vec4((g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / wsum, 1.0);
    return;
  }

  // Mips 1..5: standard Jimenez energy-preserving 13-tap downsample.
  vec3 res = g * 0.125;
  res += (a + c + k + m) * 0.03125;
  res += (b + f + h + l) * 0.0625;
  res += (d_t + e_t + i_t + j_t) * 0.125;
  out_color = vec4(max(res, vec3(0.0)), 1.0);
}
