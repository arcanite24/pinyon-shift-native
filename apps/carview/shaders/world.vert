#version 450
// carview: Colorado's render models with their own textures
// (WORLD-MATERIALS, "Minimal recipe"). Positions relative to the scenery's
// origin in the game frame; the push constant's model matrix takes them to
// the frame's space.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_normal;  // snorm, game frame; zero when the layout has none
layout(location = 2) in vec2 in_uv0;     // surface UV, dequantised
layout(location = 3) in vec2 in_uv1;     // lightmap UV
layout(location = 4) in vec4 in_colour;  // vertex colour, unorm RGBA

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

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 sun;     // xyz towards the sun in the frame's space
  vec4 up;      // xyz up in the frame's space, w exposure
  vec4 tint;    // flat colour of an untextured material
  vec4 params;  // x blend mode, y flags
} draw;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec4 v_colour;

void main() {
  vec4 world = draw.model * vec4(in_position, 1.0);
  mat3 normal_matrix = transpose(inverse(mat3(draw.model)));
  v_position = world.xyz;
  v_normal = dot(in_normal.xyz, in_normal.xyz) > 0.01 ? normal_matrix * in_normal.xyz : vec3(0.0);
  v_uv0 = in_uv0;
  v_uv1 = in_uv1;
  v_colour = in_colour;
  gl_Position = frame.view_projection * world;
}
