#version 450
// carview: camera reprojection motion vectors (screen UV velocity in [-1..1]).

layout(set = 0, binding = 0) uniform sampler2D depth_tex;

layout(push_constant) uniform MotionPush {
  mat4 prev_vp_inv_curr_vp;  // prev_vp * inverse(curr_vp)
  vec4 params;               // x = jitter_dx, y = jitter_dy, z = reverse_z, w = unused
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec2 out_velocity;

void main() {
  float z = texture(depth_tex, v_uv).r;
  vec2 ndc = v_uv * 2.0 - 1.0;
  vec4 clip = vec4(ndc, z, 1.0);
  vec4 prev_clip = pc.prev_vp_inv_curr_vp * clip;
  if (abs(prev_clip.w) < 1e-6) {
    out_velocity = vec2(0.0);
    return;
  }
  vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
  // Return velocity = (curr_uv - prev_uv) minus jitter difference.
  out_velocity = (v_uv - prev_uv) - pc.params.xy;
}
