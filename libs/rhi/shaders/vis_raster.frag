#version 460
// 64-bit Visibility Buffer fragment shader (writes R32G32_UINT):
//   word0 = ((draw_id + 1) << 8) | (gl_PrimitiveID & 0xFF)
//   word1 = (material_class << 16) | (meshlet_id & 0xFFFF)

layout(location = 0) flat in uvec2 v_vis_meta;
layout(location = 0) out uvec2 out_vis;

void main() {
  out_vis = uvec2((v_vis_meta.x << 8u) | (uint(gl_PrimitiveID) & 0xFFu), v_vis_meta.y);
}
