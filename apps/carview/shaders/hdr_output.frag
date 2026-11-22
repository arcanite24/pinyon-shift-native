#version 450
// carview: HDR10 PQ / scRGB output, highlight expansion, SDR|HDR compare split-screen,
// and 3-step HDR Calibration test patterns (matching horizon1-recomp's hdr_output.ps.glsl).

layout(set = 0, binding = 0) uniform sampler2D stand_in;

layout(push_constant) uniform HdrPush {
  vec4 p0;    // x = paper_white_scale (nits/80), y = highlight_scale, z = knee, w = knee_range_inv
  vec4 p1;    // x = gamma (0=sRGB), y = max_channel_weight, z = compare_x (<0 off), w = pattern_value (nits/80)
  uvec4 p2;   // x = output_mode (0=SDR preview, 1=scRGB, 2=HDR10 PQ), y = pattern (0..3), z = width, w = height
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 output_color;

const uint kOutputSdrPreview = 0u;
const uint kOutputScRgb = 1u;
const uint kOutputPq = 2u;

const uint kPatternNone = 0u;
const uint kPatternBlack = 1u;
const uint kPatternPaperWhite = 2u;
const uint kPatternPeak = 3u;

float max3(vec3 v) { return max(v.r, max(v.g, v.b)); }

vec3 srgb_to_linear(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 encode_srgb(vec3 linear) {
  linear = clamp(linear, 0.0, 1.0);
  return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
             greaterThan(linear, vec3(0.0031308)));
}

float sdr_level(float code) {
  float linear = pc.p1.x > 0.0
      ? pow(code, pc.p1.x)
      : (code <= 0.04045 ? code / 12.92 : pow((code + 0.055) / 1.055, 2.4));
  return linear * pc.p0.x;
}

bool in_rect(vec2 uv, vec2 center, vec2 half_size) {
  return all(lessThan(abs(uv - center), half_size));
}

const float kPatternTop = 0.62;
const float kPatternBottom = 0.88;

vec3 pattern_scrgb(vec2 pixel, vec2 size) {
  vec2 uv = pixel / size;
  if (uv.y < kPatternTop || uv.y > kPatternBottom) return vec3(0.0);
  uv.y = (uv.y - kPatternTop) / (kPatternBottom - kPatternTop);
  vec2 square = vec2(size.y * (kPatternBottom - kPatternTop) / size.x, 1.0);
  if (pc.p2.y == kPatternBlack) {
    for (int i = 0; i < 6; ++i) {
      if (in_rect(uv, vec2((float(i) + 0.5) / 6.0, 0.5), square * 0.3)) {
        return vec3(sdr_level(float(i + 1) / 100.0));
      }
    }
    return vec3(0.0);
  }
  if (pc.p2.y == kPatternPaperWhite) {
    if (in_rect(uv, vec2(0.2, 0.5), square * 0.35)) {
      return vec3(sdr_level(1.0));
    }
    if (uv.y > 0.3 && uv.y < 0.7 && uv.x > 0.4 && uv.x < 0.95) {
      return vec3(sdr_level(floor((uv.x - 0.4) / 0.55 * 11.0) / 10.0));
    }
    return vec3(sdr_level(0.5));
  }
  if (pc.p2.y == kPatternPeak) {
    if (in_rect(uv, vec2(0.5, 0.5), square * 0.3)) {
      return vec3(pc.p1.w);
    }
    return vec3(10000.0 / 80.0);
  }
  return vec3(0.0);
}

vec4 encode_output(vec3 scrgb) {
  if (pc.p2.x == kOutputScRgb) {
    return vec4(scrgb, 1.0);
  }
  if (pc.p2.x == kOutputPq) {
    const mat3 kBt709ToBt2020 = mat3(
        0.6274040, 0.0690970, 0.0163916,
        0.3292820, 0.9195400, 0.0880132,
        0.0433136, 0.0113612, 0.8955950);
    vec3 y = clamp(kBt709ToBt2020 * scrgb * (80.0 / 10000.0), 0.0, 1.0);
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    vec3 p = pow(y, vec3(m1));
    return vec4(pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2)), 1.0);
  }
  // SDR preview fallback (e.g. headless PNG verification of compare split or calibration):
  // keep [0..1] paper-white range exact and soft-compress extended highlights > 1.0 cleanly.
  vec3 norm = max(scrgb / max(pc.p0.x, 1e-4), vec3(0.0));
  vec3 mapped = norm / (vec3(1.0) + max(norm - vec3(1.0), vec3(0.0)) * 0.35);
  return vec4(encode_srgb(mapped), 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  vec2 size = vec2(max(pc.p2.z, 1u), max(pc.p2.w, 1u));
  vec2 frag = vec2(pixel) + 0.5;
  vec4 sdr = texelFetch(stand_in, pixel, 0);
  float ui = clamp(sdr.a, 0.0, 1.0);
  vec3 linear = srgb_to_linear(sdr.rgb);

  if (pc.p1.z >= 0.0 && frag.x < pc.p1.z) {
    vec3 reference = linear * pc.p0.x;
    if (frag.x > pc.p1.z - 3.0) reference = vec3(pc.p0.x);
    output_color = encode_output(reference);
    return;
  }

  if (pc.p1.x > 0.0) {
    linear = pow(clamp(sdr.rgb, 0.0, 1.0), vec3(pc.p1.x));
  }
  float luminance = dot(linear, vec3(0.2126, 0.7152, 0.0722));
  float brightness = mix(luminance, max3(linear), pc.p1.y);
  float t = clamp((brightness - pc.p0.z) * pc.p0.w, 0.0, 1.0);
  float boost = 1.0 + pc.p0.y * t * t * (1.0 - ui);
  vec3 color = linear * (pc.p0.x * boost);
  if (pc.p2.y != kPatternNone) {
    color = mix(pattern_scrgb(frag, size), linear * pc.p0.x, ui);
  }
  output_color = encode_output(color);
}
