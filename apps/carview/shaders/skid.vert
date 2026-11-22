#version 450
// carview terrain-conforming skidmark & tyre-rut vertex shader (SPEC-VFX-AND-PARTICLES).

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
  mat4 shadow_matrix;
  vec4 shadow;
  vec4 shadow_bias;
  vec4 world_fade;
  vec4 drive;
  vec4 sun_color;
  vec4 sky_color;
  vec4 ground_color;
  vec4 fog_color;
  vec4 fog_scatter;
  vec4 fog_params;
  vec4 car_light_top;
  vec4 car_light_bot;
  mat4 shadow_matrix_far;
  vec4 shadow_far_bias;
  vec4 env_basis_x;
  vec4 env_basis_y;
  vec4 env_basis_z;
} frame;

layout(push_constant) uniform SkidPush {
  mat4 model;    // scenery-origin -> render space
  vec4 sun_dir;  // xyz towards sun in render space, w = exposure
  vec4 up_dir;   // xyz up in render space, w = is_rut (0 asphalt, 1 dirt/gravel rut)
} push_c;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_color;

void main() {
  vec4 wp = push_c.model * vec4(in_position, 1.0);
  v_world_pos = wp.xyz;
  v_normal = normalize(mat3(push_c.model) * in_normal);
  v_uv = in_uv;
  v_color = in_color;
  gl_Position = frame.view_projection * wp;
}
