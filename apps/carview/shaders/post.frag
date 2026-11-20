#version 450
// carview: the drive's post chain (SHOWROOM_LIGHTING, Post chain; the
// constants are the track's TrackSettings.xml). The scene arrives as linear
// HDR, clipped at 4.0 as the game's resolve clips it. Modes:
//   0 bright pass: the exposed scene above the bloom cutoff, four bilinear
//     taps into a quarter-size target (ours);
//   1 blur: a Gaussian along mode.yz, five bilinear taps (ours);
//   2 composite: exposure, the bloom added, the Hable filmic curve over
//     its white point, desaturation towards the luminance, sRGB encoding.
// The order of exposure, curve and desaturation is open in the spec
// (ours: as listed).

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D bloom;

layout(push_constant) uniform Post {
  vec4 mode;    // x mode, yz texel step of src
  vec4 abc;     // Hable A, B, C, white
  vec4 def;     // Hable D, E, F, exposure
  vec4 desat;   // luminance weights rgb, amount s
  vec4 bloom_p; // cutoff, scale, on
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

vec3 scene(vec2 uv) { return min(texture(src, uv).rgb, vec3(4.0)) * p.def.w; }

void main() {
  int mode = int(p.mode.x + 0.5);
  if (mode == 0) {
    vec2 t = p.mode.yz;
    vec3 c = 0.25 * (scene(v_uv + vec2(-t.x, -t.y)) + scene(v_uv + vec2(t.x, -t.y)) +
                     scene(v_uv + vec2(-t.x, t.y)) + scene(v_uv + vec2(t.x, t.y)));
    out_color = vec4(max(c - vec3(p.bloom_p.x), vec3(0.0)), 1.0);
  } else if (mode == 1) {
    // Gaussian, sigma about 2 texels, as five bilinear taps.
    const float w[3] = float[](0.2270270, 0.3162162, 0.0702703);
    const float o[3] = float[](0.0, 1.3846154, 3.2307692);
    vec3 c = texture(src, v_uv).rgb * w[0];
    for (int i = 1; i < 3; ++i) {
      c += texture(src, v_uv + p.mode.yz * o[i]).rgb * w[i];
      c += texture(src, v_uv - p.mode.yz * o[i]).rgb * w[i];
    }
    out_color = vec4(c, 1.0);
  } else {
    vec3 c = scene(v_uv);
    if (p.bloom_p.z > 0.5) c += texture(bloom, v_uv).rgb * p.bloom_p.y;
    vec3 t = hable(c) / hable(vec3(p.abc.w));
    t = clamp(t, vec3(0.0), vec3(1.0));
    float l = dot(t, p.desat.rgb);
    t = mix(vec3(l), t, p.desat.w);
    out_color = vec4(encode_srgb(t), 1.0);
  }
}
