#version 450
// carview: hardware-instanced GPU-driven tree sprite vertex shader (world_gpu.cpp).
// Draws 6 vertices per visible tree instance (indexless quad) and shares
// world_gpu.frag + bindless textures with world_gpu.vert.

layout(set = 0, binding = 0) uniform Frame {
  mat4 view_projection;
  vec4 eye;
} frame;

struct TreeInstanceGpu {
  vec4 base_hw;   // xyz base in scenery frame, w half_width
  vec4 up_h;      // xyz up axis in scenery frame, w height
  vec4 uv_rect;   // u0, v0, u1, v1
  uvec4 col_tex;  // x RGBA8 tint, y bindless tex slot (unused here), z DrawGpu row, w unused
};

layout(std430, set = 2, binding = 1) readonly buffer Trees { TreeInstanceGpu trees[]; };
layout(std430, set = 3, binding = 0) readonly buffer VisibleTrees { uint visible_indices[]; };

layout(push_constant) uniform Push {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 swap_pad;
  vec4 pad;
} push;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec4 v_colour;
layout(location = 5) flat out vec2 v_fade;
layout(location = 6) flat out uint v_draw;
layout(location = 7) out vec2 v_uv2;
layout(location = 8) flat out float v_emissive_thresh;

void main() {
  const uint tid = visible_indices[uint(gl_InstanceIndex)];
  const TreeInstanceGpu t = trees[tid];
  const int kQuad[6] = int[6](0, 1, 2, 0, 2, 3);
  const int c = kQuad[gl_VertexIndex % 6];

  const float ox = (c == 1 || c == 2) ? t.base_hw.w : -t.base_hw.w;
  const float oy = (c >= 2) ? t.up_h.w : 0.0;
  const vec2 uv = vec2((c == 1 || c == 2) ? t.uv_rect.z : t.uv_rect.x,
                       (c >= 2) ? t.uv_rect.y : t.uv_rect.w);

  vec4 world = push.model * vec4(t.base_hw.xyz, 1.0);
  const vec3 u_axis = normalize(mat3(push.model) * t.up_h.xyz);
  const vec3 to_eye = frame.eye.xyz - world.xyz;
  vec3 r_axis = cross(u_axis, to_eye);
  const float len = length(r_axis);
  r_axis = len > 1e-4 ? (r_axis / len) : vec3(1.0, 0.0, 0.0);
  // Subtle wind sway on tree canopies (push.pad: x=time_sec, y=wind_speed, z=wind_amplitude).
  float sway = 0.0;
  if (oy > 0.01 && push.pad.z > 0.001) {
    float phase = dot(t.base_hw.xz, vec2(0.19, 0.23)) + push.pad.x * max(push.pad.y, 0.4) * 1.15;
    float gust = sin(phase) + 0.35 * sin(phase * 2.3 + 1.1);
    sway = gust * min(push.pad.z, 0.85) * min(oy * 0.045, 0.42);
  }
  world.xyz += r_axis * (ox + sway) + u_axis * oy;

  v_position = world.xyz;
  v_normal = normalize(u_axis * 0.65 + normalize(to_eye + vec3(0.0, 0.001, 0.0)) * 0.35);
  v_uv0 = uv;
  v_uv1 = vec2(ox, oy);
  v_colour = unpackUnorm4x8(t.col_tex.x);
  v_fade = vec2(0.0);
  v_draw = t.col_tex.z;
  v_uv2 = vec2(0.0);
  v_emissive_thresh = 0.5;
  gl_Position = frame.view_projection * world;
}
