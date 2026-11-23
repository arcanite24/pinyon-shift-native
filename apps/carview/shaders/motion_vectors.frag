#version 450
// carview: camera reprojection motion vectors + Reactive Mask preparation.
// Output format: RGBA16F:
//   .rg = screen UV velocity (curr_uv - prev_uv - jitter_diff) in [-1..1]
//   .b  = reactive mask [0..1] (depth-discontinuity silhouettes, near-car protection, high-velocity edges)
//   .a  = linear depth (metres)

layout(set = 0, binding = 0) uniform sampler2D depth_tex;

layout(push_constant) uniform MotionPush {
  mat4 prev_vp_inv_curr_vp;  // prev_vp * inverse(curr_vp)
  vec4 params;               // x = jitter_dx, y = jitter_dy, z = reverse_z, w = near_plane
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_motion;

float linear_depth_m(float z) {
  float near_p = max(pc.params.w, 0.05);
  if (pc.params.z > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = 4000.0;
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

void main() {
  float z = texture(depth_tex, v_uv).r;
  float lin_d = linear_depth_m(z);
  vec2 ndc = v_uv * 2.0 - 1.0;
  vec4 clip = vec4(ndc, z, 1.0);
  vec4 prev_clip = pc.prev_vp_inv_curr_vp * clip;
  vec2 vel = vec2(0.0);
  if (abs(prev_clip.w) >= 1e-6) {
    vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
    vel = (v_uv - prev_uv) - pc.params.xy;
    // Clamp extreme velocities (e.g. teleport / camera cuts) to prevent blowup.
    float vlen = length(vel);
    if (vlen > 0.25) vel *= (0.25 / vlen);
  }

  // Reactive mask preparation (disocclusion depth edges + high-speed silhouette protection
  // for McGuire motion blur and FSR/TSR/DLSS temporal upscalers).
  vec2 texel = 1.0 / max(vec2(textureSize(depth_tex, 0)), vec2(1.0));
  float dl = linear_depth_m(texture(depth_tex, clamp(v_uv - vec2(texel.x, 0.0), vec2(0.001), vec2(0.999))).r);
  float dr = linear_depth_m(texture(depth_tex, clamp(v_uv + vec2(texel.x, 0.0), vec2(0.001), vec2(0.999))).r);
  float dt = linear_depth_m(texture(depth_tex, clamp(v_uv - vec2(0.0, texel.y), vec2(0.001), vec2(0.999))).r);
  float db = linear_depth_m(texture(depth_tex, clamp(v_uv + vec2(0.0, texel.y), vec2(0.001), vec2(0.999))).r);
  float max_dd = max(max(abs(dl - lin_d), abs(dr - lin_d)), max(abs(dt - lin_d), abs(db - lin_d)));
  float edge_reactive = smoothstep(lin_d * 0.06, lin_d * 0.25, max_dd);
  float speed_reactive = clamp(length(vel) * 18.0, 0.0, 1.0) * 0.35;
  float reactive = clamp(max(edge_reactive, speed_reactive), 0.0, 1.0);

  out_motion = vec4(vel, reactive, lin_d);
}
