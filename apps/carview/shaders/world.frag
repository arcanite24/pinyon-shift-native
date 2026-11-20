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
layout(set = 1, binding = 1) uniform sampler2D layer_b;    // Blend_B
layout(set = 1, binding = 2) uniform sampler2D splat_map;  // Splat

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
const uint kFlagTextured = 1u, kFlagSky = 2u, kFlagVertexBlend = 4u, kFlagSplat = 8u, kFlagVertexFade = 16u,
           kFlagBillboard = 32u;

// The sky gradient (ours, linear): pale haze at the horizon to blue above.
const vec3 kSkyHorizon = vec3(0.477, 0.604, 0.708);
const vec3 kSkyZenith = vec3(0.089, 0.235, 0.571);

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
  if ((flags & kFlagSky) != 0u) {
    float h = dot(normalize(v_position), normalize(draw.up.xyz));
    vec3 sky = mix(kSkyHorizon, kSkyZenith, pow(clamp(h, 0.0, 1.0), 0.6));
    out_color = vec4(encode_srgb(sky * (h < 0.0 ? 0.85 : 1.0)), 1.0);
    return;
  }
  // Terrain layers (WORLD-MATERIALS, Terrain shading): tint holds the
  // tilings, Blend_A's in xy and Blend_B's in zw; the weight is the vertex
  // colour's R (H_VBLND, baked from c4) or Splat's R (H_BLND2/3, uv1
  // pre-tiled). Blend_C is not drawn (its weight is not settled).
  vec4 t;
  if ((flags & kFlagTextured) != 0u) {
    t = texture(albedo, v_uv0 * draw.tint.xy);
    if ((flags & (kFlagVertexBlend | kFlagSplat)) != 0u) {
      vec4 b = texture(layer_b, v_uv0 * draw.tint.zw);
      float w = (flags & kFlagSplat) != 0u ? texture(splat_map, v_uv1).r : v_colour.r;
      t = mix(t, b, w);
    }
  } else {
    t = vec4(draw.tint.rgb, 1.0);
  }
  // Tree sprites: the atlas colour times the record's tint, 2 x RGB.
  if ((flags & kFlagBillboard) != 0u) t.rgb *= 2.0 * v_colour.rgb;

  vec3 up = normalize(draw.up.xyz);
  // The authored normal (not turned to the eye: terrain and leaf cards above
  // the eye would go dark); leaf cards without one take the up axis.
  vec3 n = dot(v_normal, v_normal) > 1e-6 ? normalize(v_normal) : up;
  vec3 sun = normalize(draw.sun.xyz);
  vec3 light = kSunColour * max(dot(n, sun), 0.0) + mix(kGroundColour, kSkyColour, 0.5 + 0.5 * dot(n, up));
  vec3 colour = t.rgb * light * draw.up.w * (draw.params.w > 0.0 ? draw.params.w : 1.0);

  float alpha = 1.0;
  if (blend == kCoverage) {
    // Alpha to coverage, sharpened to the texel edge.
    alpha = clamp((t.a - 0.5) / max(fwidth(t.a), 1e-4) + 0.5, 0.0, 1.0);
  } else if (blend == kAlpha) {
    // H_VBLND_DECAL: the diffuse alpha times the vertex fade (baked into R);
    // road decals: times the vertex colour's alpha (the likely source).
    alpha = t.a * ((flags & kFlagVertexFade) != 0u ? v_colour.r : v_colour.a);
  }
  vec3 encoded = encode_srgb(colour);
  // Blended draws are premultiplied (ONE, ONE_MINUS_SRC_ALPHA).
  out_color = blend == kAlpha ? vec4(encoded * alpha, alpha) : vec4(encoded, alpha);
}
