#version 450
// carview: Spatial Output Scaling pass.
// mode 0: Bilinear resample
// mode 1: AMD FidelityFX CAS (Contrast Adaptive Sharpening)
// mode 2: AMD FidelityFX FSR 1.0 EASU (Edge-Adaptive Spatial Upsampling)
// mode 3: AMD FidelityFX FSR 1.0 RCAS (Robust Contrast Adaptive Sharpening)

layout(set = 0, binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform UpscalePush {
  vec4 src_size;   // xy = src W,H, zw = 1/W, 1/H
  vec4 dst_size;   // xy = dst W,H, zw = 1/W, 1/H
  vec4 params;     // x = mode (0..3), y = sharpening [0..1], z = out_alpha, w = unused
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float luma_g(vec3 c) { return c.g * 0.5 + (c.r * 0.25 + c.b * 0.25); }

vec3 cas_filter(vec2 uv, float sharp) {
  vec2 d = pc.src_size.zw;
  vec3 a = texture(src_tex, uv + vec2(-d.x, -d.y)).rgb;
  vec3 b = texture(src_tex, uv + vec2(0.0,  -d.y)).rgb;
  vec3 c = texture(src_tex, uv + vec2(d.x,  -d.y)).rgb;
  vec3 d0 = texture(src_tex, uv + vec2(-d.x, 0.0)).rgb;
  vec3 e = texture(src_tex, uv).rgb;
  vec3 f = texture(src_tex, uv + vec2(d.x,  0.0)).rgb;
  vec3 g = texture(src_tex, uv + vec2(-d.x, d.y)).rgb;
  vec3 h = texture(src_tex, uv + vec2(0.0,  d.y)).rgb;
  vec3 i = texture(src_tex, uv + vec2(d.x,  d.y)).rgb;

  vec3 mn = min(min(min(d0, e), min(f, b)), h);
  vec3 mn2 = min(mn, min(min(a, c), min(g, i)));
  mn += mn2;
  vec3 mx = max(max(max(d0, e), max(f, b)), h);
  vec3 mx2 = max(mx, max(max(a, c), max(g, i)));
  mx += mx2;

  vec3 rcpM = 1.0 / max(mx, vec3(1e-5));
  vec3 amp = clamp(min(mn, 2.0 - mx) * rcpM, 0.0, 1.0);
  amp = sqrt(amp);
  float peak = -1.0 / mix(8.0, 5.0, clamp(sharp, 0.0, 1.0));
  vec3 w = amp * peak;
  vec3 rcpW = 1.0 / (1.0 + 4.0 * w);
  return clamp(((b + d0 + f + h) * w + e) * rcpW, 0.0, 1.0);
}

void easu_set(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
              float lA, float lB, float lC, float lD, float lE) {
  float w = 0.0;
  if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
  if (biT) w = pp.x * (1.0 - pp.y);
  if (biU) w = (1.0 - pp.x) * pp.y;
  if (biV) w = pp.x * pp.y;
  float dc = lD - lC;
  float cb = lC - lB;
  float lenX = max(abs(dc), abs(cb));
  lenX = lenX > 1e-5 ? 1.0 / lenX : 0.0;
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
  lenX *= lenX;
  len += lenX * w;

  float ec = lE - lC;
  float ca = lC - lA;
  float lenY = max(abs(ec), abs(ca));
  lenY = lenY > 1e-5 ? 1.0 / lenY : 0.0;
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
  lenY *= lenY;
  len += lenY * w;
}

void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
  vec2 v;
  v.x = off.x * dir.x + off.y * dir.y;
  v.y = off.x * (-dir.y) + off.y * dir.x;
  v *= len2;
  float d2 = min(dot(v, v), clp);
  float wB = 0.4 * d2 - 1.0;
  float wA = lob * d2 - 1.0;
  wB *= wB;
  wA *= wA;
  wB = 1.5625 * wB - 0.5625;
  float w = wB * wA;
  aC += c * w;
  aW += w;
}

vec3 fsr1_easu(vec2 dst_uv) {
  vec2 ip = dst_uv * pc.src_size.xy - 0.5;
  vec2 fp = floor(ip);
  vec2 pp = ip - fp;
  vec2 p0 = (fp + 0.5) * pc.src_size.zw;
  vec2 d = pc.src_size.zw;

  vec3 b = texture(src_tex, p0 + vec2(0.0, -d.y)).rgb;
  vec3 c = texture(src_tex, p0 + vec2(d.x, -d.y)).rgb;
  vec3 e = texture(src_tex, p0 + vec2(-d.x, 0.0)).rgb;
  vec3 f = texture(src_tex, p0).rgb;
  vec3 g = texture(src_tex, p0 + vec2(d.x, 0.0)).rgb;
  vec3 h = texture(src_tex, p0 + vec2(2.0 * d.x, 0.0)).rgb;
  vec3 i = texture(src_tex, p0 + vec2(-d.x, d.y)).rgb;
  vec3 j = texture(src_tex, p0 + vec2(0.0, d.y)).rgb;
  vec3 k = texture(src_tex, p0 + vec2(d.x, d.y)).rgb;
  vec3 l = texture(src_tex, p0 + vec2(2.0 * d.x, d.y)).rgb;
  vec3 n = texture(src_tex, p0 + vec2(0.0, 2.0 * d.y)).rgb;
  vec3 o = texture(src_tex, p0 + vec2(d.x, 2.0 * d.y)).rgb;

  float lb = luma_g(b), lc = luma_g(c), le = luma_g(e), lf = luma_g(f);
  float lg = luma_g(g), lh = luma_g(h), li = luma_g(i), lj = luma_g(j);
  float lk = luma_g(k), ll = luma_g(l), ln = luma_g(n), lo = luma_g(o);

  vec2 dir = vec2(0.0);
  float len = 0.0;
  easu_set(dir, len, pp, true,  false, false, false, lb, le, lf, lg, lj);
  easu_set(dir, len, pp, false, true,  false, false, lc, lf, lg, lh, lk);
  easu_set(dir, len, pp, false, false, true,  false, lf, li, lj, lk, ln);
  easu_set(dir, len, pp, false, false, false, true,  lg, lj, lk, ll, lo);

  float dirR = dot(dir, dir);
  if (dirR < 1.0 / 32768.0) dir = vec2(1.0, 0.0);
  else dir *= inversesqrt(dirR);

  len *= 0.5;
  len *= len;
  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  float lob = 0.5 + ((0.25 - 0.04) - 0.5) * len;
  float clp = 1.0 / lob;

  vec3 min4 = min(min(f, g), min(j, k));
  vec3 max4 = max(max(f, g), max(j, k));

  vec3 aC = vec3(0.0);
  float aW = 0.0;
  easu_tap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
  easu_tap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
  easu_tap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
  easu_tap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
  easu_tap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
  easu_tap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
  easu_tap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
  easu_tap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
  easu_tap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
  easu_tap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
  easu_tap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);
  easu_tap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);

  return clamp(aC / max(aW, 1e-5), min4, max4);
}

vec3 fsr1_rcas(vec2 uv, float sharp) {
  vec2 d = pc.src_size.zw;
  vec3 b = texture(src_tex, uv + vec2(0.0, -d.y)).rgb;
  vec3 d0 = texture(src_tex, uv + vec2(-d.x, 0.0)).rgb;
  vec3 e = texture(src_tex, uv).rgb;
  vec3 f = texture(src_tex, uv + vec2(d.x, 0.0)).rgb;
  vec3 h = texture(src_tex, uv + vec2(0.0, d.y)).rgb;

  float bL = luma_g(b), dL = luma_g(d0), eL = luma_g(e), fL = luma_g(f), hL = luma_g(h);
  float nz = 0.25 * (bL + dL + fL + hL) - eL;
  float mxL = max(max(bL, dL), max(fL, max(hL, eL)));
  float mnL = min(min(bL, dL), min(fL, min(hL, eL)));
  nz = clamp(abs(nz) / max(mxL - mnL, 1e-5), 0.0, 1.0);
  nz = -0.5 * nz + 1.0;

  vec3 mn4 = min(min(b, d0), min(f, h));
  vec3 mx4 = max(max(b, d0), max(f, h));
  vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1e-5));
  vec3 hitMax = (1.0 - mx4) / max(4.0 * mn4 - 4.0, vec3(-1e-5));
  vec3 lobeRGB = max(-hitMin, hitMax);
  float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0));
  float atten = exp2(-clamp(1.0 - sharp, 0.0, 1.0) * 2.0);
  float w = lobe * nz * atten;
  return clamp(((b + d0 + f + h) * w + e) / (4.0 * w + 1.0), 0.0, 1.0);
}

void main() {
  int mode = int(pc.params.x + 0.5);
  vec3 col;
  if (mode == 1) col = cas_filter(v_uv, pc.params.y);
  else if (mode == 2) col = fsr1_easu(v_uv);
  else if (mode == 3) col = fsr1_rcas(v_uv, pc.params.y);
  else col = texture(src_tex, v_uv).rgb;
  out_color = vec4(col, pc.params.z);
}
