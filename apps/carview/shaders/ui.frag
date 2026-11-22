#version 450
// carview's UI layer (ui.h): each shape's coverage from its signed distance
// (the CPU twin is ui.cpp's rasterize). Output premultiplied, sRGB-encoded.
layout(location = 0) in vec2 pix;
layout(location = 1) flat in vec4 rect;
layout(location = 2) flat in vec4 p;
layout(location = 3) flat in vec4 q;
layout(location = 4) flat in vec4 r;
layout(location = 5) flat in vec4 m;
layout(location = 6) flat in uvec4 cw;

layout(std430, set = 0, binding = 0) readonly buffer Font {
  vec4 segments[];
};
layout(set = 0, binding = 1) uniform sampler2D atlas;

layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;

float seg_dist(vec2 x, vec4 s) {
  vec2 ba = s.zw - s.xy, pa = x - s.xy;
  float l = dot(ba, ba);
  float t = l > 0.0 ? clamp(dot(pa, ba) / l, 0.0, 1.0) : 0.0;
  return length(pa - t * ba);
}

float round_rect(vec2 x, vec4 r, float radius, float inset) {
  vec2 c = 0.5 * (r.xy + r.zw);
  vec2 h = 0.5 * (r.zw - r.xy) - inset;
  float rr = min(radius, min(h.x, h.y));
  vec2 d = abs(x - c) - h + rr;
  return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - rr;
}

void main() {
  vec4 c0 = unpackUnorm4x8(cw.x).bgra;  // 0xAARRGGBB in memory as B, G, R, A
  vec4 c1 = unpackUnorm4x8(cw.y).bgra;
  vec4 color = c0;
  float cov = 0.0;
  uint kind = cw.z;
  if (kind == 0u) {
    float d = round_rect(pix, rect, p.x, p.z);
    if (p.y > 0.0) d = abs(d + 0.5 * p.y) - 0.5 * p.y;
    color = mix(c0, c1, clamp((pix.y - rect.y) / max(1.0, rect.w - rect.y), 0.0, 1.0));
    if (q.x > 0.0) {
      float t = clamp((q.x - d) / (2.0 * q.x), 0.0, 1.0);
      cov = t * t * (3.0 - 2.0 * t);
    } else {
      cov = clamp(0.5 - d, 0.0, 1.0);
    }
  } else if (kind == 1u) {
    cov = clamp(0.5 - (seg_dist(pix, p) - q.x), 0.0, 1.0);
  } else if (kind == 2u) {
    vec2 d = pix - p.xy;
    float a = atan(d.x, -d.y);
    float rel = mod(a - q.x, 2.0 * kPi);
    float dist;
    if (rel <= q.y - q.x) {
      dist = abs(length(d) - p.z) - p.w;
    } else if (q.z > 0.0) {
      // Square ends: the distance past the nearer end's radial edge.
      float past = min(rel - (q.y - q.x), 2.0 * kPi - rel) * length(d);
      dist = max(abs(length(d) - p.z) - p.w, past);
    } else {
      vec2 e0 = p.xy + p.z * vec2(sin(q.x), -cos(q.x));
      vec2 e1 = p.xy + p.z * vec2(sin(q.y), -cos(q.y));
      dist = min(length(pix - e0), length(pix - e1)) - p.w;
    }
    cov = clamp(0.5 - dist, 0.0, 1.0);
  } else if (kind == 3u) {
    vec2 u = (pix - p.xy) / p.z;
    u.x -= q.z * (10.0 - u.y);
    float best = 1e9;
    uint first = uint(q.x), count = uint(q.y);
    for (uint i = first; i < first + count; ++i) best = min(best, seg_dist(u, segments[i]));
    cov = clamp(0.5 - (best - p.w) * p.z, 0.0, 1.0);
  } else if (kind == 4u && p.y > 0.0) {
    // Rotated: centre the rect's, half size p.yz, p.w radians clockwise.
    vec2 d = pix - 0.5 * (rect.xy + rect.zw);
    float cs = cos(p.w), sn = sin(p.w);
    vec2 lp = vec2(cs * d.x + sn * d.y, -sn * d.x + cs * d.y);
    vec2 t = lp / (2.0 * p.yz) + 0.5;
    color = texture(atlas, mix(q.xy, q.zw, clamp(t, 0.0, 1.0))) * c0;
    vec2 e = abs(lp) - p.yz;
    cov = clamp(0.5 - max(e.x, e.y), 0.0, 1.0);
  } else if (kind == 4u) {
    vec2 t = (pix - rect.xy) / max(rect.zw - rect.xy, vec2(1.0));
    color = texture(atlas, mix(q.xy, q.zw, t)) * c0;
    cov = clamp(0.5 - round_rect(pix, rect, p.x, 0.0), 0.0, 1.0);
  } else if (kind == 5u) {
    // Two-texture masked / crop multiply mode (FH1 UI4 2,455 two-texture materials):
    // color = material_rgba * tex0(uv0_transformed) * tex1(uv1).
    vec2 t;
    if (p.y > 0.0) {
      vec2 d = pix - 0.5 * (rect.xy + rect.zw);
      float cs = cos(p.w), sn = sin(p.w);
      vec2 lp = vec2(cs * d.x + sn * d.y, -sn * d.x + cs * d.y);
      t = lp / (2.0 * p.yz) + 0.5;
      vec2 e = abs(lp) - p.yz;
      cov = clamp(0.5 - max(e.x, e.y), 0.0, 1.0);
    } else {
      t = (pix - rect.xy) / max(rect.zw - rect.xy, vec2(1.0));
      cov = clamp(0.5 - round_rect(pix, rect, p.x, 0.0), 0.0, 1.0);
    }
    vec2 t0 = vec2(t.x * m.x + m.z, t.y * m.y + m.w);
    vec4 s0;
    uint flags = cw.w;
    if ((flags & 2u) != 0u) {
      s0 = texture(atlas, mix(q.xy, q.zw, fract(t0)));
    } else if ((flags & 1u) != 0u) {
      if (t0.x < 0.0 || t0.x > 1.0 || t0.y < 0.0 || t0.y > 1.0) {
        s0 = vec4(0.0);
      } else {
        s0 = texture(atlas, mix(q.xy, q.zw, t0));
      }
    } else {
      s0 = texture(atlas, mix(q.xy, q.zw, clamp(t0, 0.0, 1.0)));
    }
    vec4 s1 = texture(atlas, mix(r.xy, r.zw, clamp(t, 0.0, 1.0)));
    color = c0 * s0 * s1;
  }
  float a = color.a * cov;
  out_color = vec4(color.rgb * a, a);
}
