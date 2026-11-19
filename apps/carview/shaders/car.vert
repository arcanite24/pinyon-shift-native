#version 450
// carview: one pipeline for every car part.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec2 in_extra;  // x: baked occlusion (0-1), y: material kind

layout(push_constant) uniform Constants {
  mat4 view_projection;
  vec4 light_direction;  // xyz: towards the light
  vec4 paint;            // rgb: paint colour
  vec4 eye;              // xyz: camera position
} pc;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec2 v_extra;

void main() {
  v_position = in_position;
  v_normal = in_normal;
  v_uv = in_uv;
  v_extra = in_extra;
  gl_Position = pc.view_projection * vec4(in_position, 1.0);
}
