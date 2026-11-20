#version 450
// carview reference mode: the car-coverage mask. Every car draw writes 1.
// The inputs match showroom.vert's outputs (unused here).

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in float v_occlusion;

layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(1.0);
}
