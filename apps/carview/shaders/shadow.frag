#version 450
// carview: the sun shadow pass's cut-outs (props, tree sprites): depth
// only, texels under one half alpha left out (ours, as world.frag's alpha
// test). Opaque casters draw with no fragment shader.

layout(set = 1, binding = 0) uniform sampler2D albedo;

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 tint;    // textured: the tilings in xy
  vec4 params;  // y flags
} draw;

layout(location = 2) in vec2 v_uv0;

const uint kFlagTextured = 1u;

void main() {
  uint flags = uint(draw.params.y + 0.5);
  if ((flags & kFlagTextured) != 0u && texture(albedo, v_uv0 * draw.tint.xy).a < 0.5) discard;
}
