#version 450
// Under-car contact / ambient-occlusion drop shadow quad in drive mode.

layout(set = 0, binding = 0) uniform Frame {
  mat4 view_projection;
} frame;

layout(push_constant) uniform ShadowQuad {
  mat4 model;
  vec4 params;  // x strength [0..1], y min_floor [0..1], z corner_radius, w unused
} q;

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_params;

void main() {
  const vec2 corners[6] = vec2[6](
      vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
      vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));
  vec2 c = corners[gl_VertexIndex];
  v_uv = c;
  v_params = q.params;
  vec4 world = q.model * vec4(c.x, 0.0, c.y, 1.0);
  gl_Position = frame.view_projection * world;
}
