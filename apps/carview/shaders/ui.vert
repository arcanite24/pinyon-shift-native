#version 450
// carview's UI layer (ui.h): one instanced quad per shape, in pixels.
layout(location = 0) in vec4 in_rect;
layout(location = 1) in vec4 in_p;
layout(location = 2) in vec4 in_q;
layout(location = 3) in uvec4 in_cw;  // color, color2, kind, flags

layout(push_constant) uniform Push {
  vec2 inv_half;  // scale * 2 / screen size
  vec2 offset;    // origin * 2 / screen size - 1 (0,0 virtual -> -1,-1 when full screen)
} pc;

layout(location = 0) out vec2 pix;
layout(location = 1) flat out vec4 rect;
layout(location = 2) flat out vec4 p;
layout(location = 3) flat out vec4 q;
layout(location = 4) flat out uvec4 cw;

const vec2 kCorner[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));

void main() {
  pix = mix(in_rect.xy, in_rect.zw, kCorner[gl_VertexIndex]);
  rect = in_rect;
  p = in_p;
  q = in_q;
  cw = in_cw;
  gl_Position = vec4(pix * pc.inv_half + pc.offset, 0.0, 1.0);
}
