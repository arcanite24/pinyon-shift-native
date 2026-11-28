#version 460
// 64-bit Visibility Buffer vertex shader.
// Reads per-instance draw_id from instanced vertex attribute location 1
// (portable across Vulkan, DirectX 12 StartInstanceLocation, and Metal).

struct MeshletInstance {
  mat4 model;
  mat4 prev_model;
  uint first_meshlet;
  uint meshlet_count;
  uint material_class;
  uint material_id;
};

struct VisibleDraw {
  uint instance_id;
  uint meshlet_id;
  uint material_class;
  uint material_id;
};

layout(set = 0, binding = 0) uniform FrameParams {
  mat4 view_proj;
  mat4 prev_view_proj;
} u;

layout(set = 0, binding = 1) readonly buffer InstanceBuf {
  MeshletInstance instances[];
};

layout(set = 0, binding = 2) readonly buffer VisibleDrawBuf {
  VisibleDraw visible_draws[];
};

layout(location = 0) in vec3 in_pos;
layout(location = 1) in uint in_draw_id;

layout(location = 0) flat out uvec2 v_vis_meta;

void main() {
  uint draw_id = in_draw_id;
  VisibleDraw vd = visible_draws[draw_id];
  MeshletInstance inst = instances[vd.instance_id];

  vec4 clip = u.view_proj * (inst.model * vec4(in_pos, 1.0));
  clip.y = -clip.y;  // Align Vulkan clip Y-down with DX12/Metal Y-up viewport
  gl_Position = clip;
  // Encode draw_id + 1 so word0 == 0 is unambiguously empty sky/background.
  v_vis_meta = uvec2(draw_id + 1u, (vd.material_class << 16u) | (vd.meshlet_id & 0xFFFFu));
}
