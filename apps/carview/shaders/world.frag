#version 450
// carview: Colorado's first-pass shading (WORLD-MATERIALS, "Minimal
// recipe"): slot 0's texture (sRGB when its format word says gamma) lit by
// the frame's sun and two-colour sky. Not the game's shading: no layer
// blends, AO, lightmaps or fog yet. Cut-outs (props, trees) write their
// diffuse alpha for alpha to coverage; decals and the lake blend with the
// texture's alpha times the vertex colour's (ours: which alpha the game
// uses is open). Output is encoded to sRGB here (the targets are UNORM).

layout(set = 0, binding = 0) uniform Frame {
  mat4 view_projection;
  vec4 eye;
  vec4 key_light;
  vec4 fill_light;
  vec4 options;
  vec4 tint_light;
  vec4 reflect_tint;
  mat4 transparent_view_projection;
  vec4 stripe;
} frame;

layout(set = 1, binding = 0) uniform sampler2D albedo;

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 tint;
  vec4 params;
} draw;

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in vec4 v_colour;

layout(location = 0) out vec4 out_color;

const uint kOpaque = 0u, kCoverage = 1u, kAlpha = 2u;
const uint kFlagTextured = 1u;

// The frame's lighting constants (daytime free-roam dump).
const vec3 kSunColour = vec3(0.9695, 0.8358, 0.584);
const vec3 kSkyColour = vec3(0.1687, 0.2229, 0.2746);
const vec3 kGroundColour = vec3(0.1788, 0.1729, 0.1024);

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
  uint blend = uint(draw.params.x + 0.5);
  uint flags = uint(draw.params.y + 0.5);
  vec4 t = (flags & kFlagTextured) != 0u ? texture(albedo, v_uv0) : vec4(draw.tint.rgb, 1.0);

  vec3 up = normalize(draw.up.xyz);
  // The authored normal (not turned to the eye: terrain and leaf cards above
  // the eye would go dark); leaf cards without one take the up axis.
  vec3 n = dot(v_normal, v_normal) > 1e-6 ? normalize(v_normal) : up;
  vec3 sun = normalize(draw.sun.xyz);
  vec3 light = kSunColour * max(dot(n, sun), 0.0) + mix(kGroundColour, kSkyColour, 0.5 + 0.5 * dot(n, up));
  vec3 colour = t.rgb * light * draw.up.w;

  float alpha = 1.0;
  if (blend == kCoverage) {
    // Alpha to coverage, sharpened to the texel edge.
    alpha = clamp((t.a - 0.5) / max(fwidth(t.a), 1e-4) + 0.5, 0.0, 1.0);
  } else if (blend == kAlpha) {
    alpha = t.a * v_colour.a;
  }
  vec3 encoded = encode_srgb(colour);
  // Blended draws are premultiplied (ONE, ONE_MINUS_SRC_ALPHA).
  out_color = blend == kAlpha ? vec4(encoded * alpha, alpha) : vec4(encoded, alpha);
}
