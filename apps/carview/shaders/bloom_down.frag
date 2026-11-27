#version 450
precision highp int;
precision mediump float;
// carview: Physically-Based Jimenez / Call of Duty SIGGRAPH 2014 13-tap progressive
// bloom downsample with Karis Average anti-firefly weighting and soft-knee threshold on mip 0.
//
// Descriptor layout matches post_set_layout_ (binding 0 = src, binding 3 = lum_adapt).
// Push constants match Post (128 bytes):
//   mode.x = mip_index (0 = prefilter + Karis average from HDR scene; 1..5 = 13-tap downsample)
//   mode.yz = src_texel_size (1/src_w, 1/src_h)
//   mode.w = soft_knee (default 0.5)

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D aux;
layout(set = 0, binding = 3) uniform sampler2D lum_adapt;

layout(push_constant) uniform Post {
  vec4 mode;     // x mip_index, yz src texel step, w bloom_persist [0..0.35] on mip 0
  vec4 abc;
  vec4 def;      // w exposure
  vec4 desat;
  vec4 bloom_p;  // x cutoff, y scale, z flags (4=auto_exp), w lower_grad
  vec4 balance;  // w upper_grad
  vec4 exp_keys;
  vec4 exp_ev;
} p;

layout(location = 0) in highp vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Must stay bit-identical to eval_auto_exposure() in post.frag so bloom bright-pass
// and final filmic tonemapping share exact exposure math (including negative EVs).
float eval_auto_exposure() {
  uint flags = uint(p.bloom_p.z + 0.5);
  if ((flags & 4u) == 0u) return p.def.w;
  float raw_l = texture(lum_adapt, vec2(0.5)).r;
  if (isnan(raw_l) || isinf(raw_l)) raw_l = 0.35;
  float L = max(raw_l, 0.06) * 110.0;
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
  // Exact SKY_AND_TOD §5.2 EV formula with daylight-to-night HDR calibration offset
  // matching FH1's Colorado sky & asphalt luminance without crushing night headlights.
  float calib_ev = mix(-0.42, -0.74, smoothstep(1.5, 14.0, L));
  float ev_total = clamp(ev_curve + p.def.w + calib_ev, -2.0, 6.0);
  return exp2(ev_total);
}

float karis_weight(vec3 c) {
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  return 1.0 / (1.0 + max(luma, 0.0) * 0.45);
}

vec3 soft_threshold(vec3 c, float exp_scale) {
  c = min(max(c, vec3(0.0)), vec3(64.0)) * exp_scale;
  float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float cutoff = max(p.bloom_p.x, 0.01);
  float knee = cutoff * 0.45;
  float lo_g = max(p.bloom_p.w, 0.0);
  float hi_g = max(p.balance.w, 0.01);
  // Soft-knee quadratic curve honoring FH1 lower/upper bloom gradient while preventing shimmer.
  float rq = clamp(lum - cutoff + knee, 0.0, 2.0 * knee);
  rq = (rq * rq) / max(4.0 * knee, 1e-5);
  float excess = max(rq, lum - cutoff);
  float gain = mix(lo_g, hi_g, smoothstep(cutoff - knee, cutoff + knee, lum));
  return c * (max(excess, 0.0) * gain / max(lum, 1e-4));
}

vec3 sample_src(highp vec2 uv) {
  vec3 c = texture(src, clamp(uv, vec2(0.0005), vec2(0.9995))).rgb;
  c = mix(c, vec3(0.0), isnan(c));
  c = mix(c, vec3(0.0), isinf(c));
  return min(max(c, vec3(0.0)), vec3(64.0));
}

void main() {
  highp vec2 d = p.mode.yz;
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
    vec3 bright = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / wsum;
    float persist = clamp(p.mode.w, 0.0, 0.35);
    if (persist > 0.0005) {
      vec3 prev = texture(aux, clamp(v_uv, vec2(0.0005), vec2(0.9995))).rgb;
      prev = mix(prev, vec3(0.0), isnan(prev));
      prev = mix(prev, vec3(0.0), isinf(prev));
      bright = max(vec3(0.0), bright + min(max(prev, vec3(0.0)), vec3(16.0)) * persist);
    }
    out_color = vec4(bright, 1.0);
    return;
  }

  // Mips 1..5: standard Jimenez energy-preserving 13-tap downsample.
  vec3 res = g * 0.125;
  res += (a + c + k + m) * 0.03125;
  res += (b + f + h + l) * 0.0625;
  res += (d_t + e_t + i_t + j_t) * 0.125;
  out_color = vec4(max(res, vec3(0.0)), 1.0);
}
