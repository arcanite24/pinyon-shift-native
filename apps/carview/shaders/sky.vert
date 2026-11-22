#version 450
// carview real-time sky fullscreen triangle (SPEC-SKY-AND-TOD §3).
layout(constant_id = 1) const bool kReverseZ = false;

layout(location = 0) out vec2 v_ndc;
layout(location = 1) out vec2 v_uv;

void main() {
  const vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  vec2 p = pos[gl_VertexIndex];
  v_ndc = p;
  v_uv = p * 0.5 + 0.5;
  gl_Position = vec4(p, kReverseZ ? 0.0 : 1.0, 1.0);
}
