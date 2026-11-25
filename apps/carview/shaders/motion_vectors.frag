#version 450
// carview: per-pixel static world + per-object dynamic vehicle motion vectors
// + Reactive Mask preparation.
// Output format: RGBA16F:
//   .rg = screen UV velocity (curr_uv - prev_uv - jitter_diff) in [-0.25..0.25]
//   .b  = reactive mask [0..1] (depth-discontinuity silhouettes, high-velocity edges)
//   .a  = linear depth (metres)
//
// Pass encoding in pc.params.w (fits strictly within 80-byte push constants):
//   pc.params.w > 0.0: Pass 0 fullscreen static world reprojection (m = prev_vp_world * inv(curr_vp))
//   pc.params.w < 0.0: Dynamic vehicle OBB overlay pass (player car or up to 8 nearest AI/traffic cars):
//     m[0..2] = rows 0, 1, 3 of prev_vp * prev_model * inv(curr_model) * inv(curr_vp) (with jitter folded)
//     m[3].xyz = view-space OBB center, m[3].w = half2(qx, qy)
//     params.x = half2(qz, hx), params.y = half2(hy, hz), params.z = half2(inv_proj00, inv_proj11)
//     -params.w = near_plane + (rev_z ? 1000 : 0) + (is_player ? 2000 : 0)

layout(set = 0, binding = 0) uniform sampler2D depth_tex;

layout(push_constant) uniform MotionPush {
  mat4 prev_vp_inv_curr_vp;
  vec4 params;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_motion;

float linear_depth_m(float z, float near_p, bool rev_z) {
  float np = max(near_p, 0.05);
  if (rev_z) {
    return z <= 1e-5 ? 1e5 : np / z;
  }
  float far_p = 16000.0;
  return (np * far_p) / max(far_p - z * (far_p - np), 1e-4);
}

float compute_edge_reactive(vec2 uv, float lin_d, float near_p, bool rev_z) {
  vec2 texel = 1.0 / max(vec2(textureSize(depth_tex, 0)), vec2(1.0));
  float dl = linear_depth_m(texture(depth_tex, clamp(uv - vec2(texel.x, 0.0), vec2(0.001), vec2(0.999))).r, near_p, rev_z);
  float dr = linear_depth_m(texture(depth_tex, clamp(uv + vec2(texel.x, 0.0), vec2(0.001), vec2(0.999))).r, near_p, rev_z);
  float dt = linear_depth_m(texture(depth_tex, clamp(uv - vec2(0.0, texel.y), vec2(0.001), vec2(0.999))).r, near_p, rev_z);
  float db = linear_depth_m(texture(depth_tex, clamp(uv + vec2(0.0, texel.y), vec2(0.001), vec2(0.999))).r, near_p, rev_z);
  float max_dd = max(max(abs(dl - lin_d), abs(dr - lin_d)), max(abs(dt - lin_d), abs(db - lin_d)));
  float d_safe = max(lin_d, 0.05);
  return smoothstep(d_safe * 0.06, d_safe * 0.25, max_dd);
}

void main() {
  if (pc.params.w < 0.0) {
    // Dynamic vehicle OBB pass: test reconstructed view-space point against the vehicle's
    // exact 3D OBB and overwrite velocity only for pixels belonging to this moving vehicle.
    float enc = -pc.params.w;
    bool is_player = enc >= 2000.0;
    if (is_player) enc -= 2000.0;
    bool rev_z = enc >= 1000.0;
    if (rev_z) enc -= 1000.0;
    float near_p = max(enc, 0.05);

    float z = texture(depth_tex, v_uv).r;
    float lin_d = linear_depth_m(z, near_p, rev_z);
    if (lin_d > 5000.0) discard;

    vec3 obb_center = pc.prev_vp_inv_curr_vp[3].xyz;
    vec2 qxy = unpackHalf2x16(floatBitsToUint(pc.prev_vp_inv_curr_vp[3].w));
    vec2 qz_hx = unpackHalf2x16(floatBitsToUint(pc.params.x));
    vec2 hy_hz = unpackHalf2x16(floatBitsToUint(pc.params.y));
    vec2 inv_proj = unpackHalf2x16(floatBitsToUint(pc.params.z));

    vec3 qxyz = vec3(qxy.x, qxy.y, qz_hx.x);
    float qw = sqrt(max(1.0 - dot(qxyz, qxyz), 0.0));
    vec3 half_ext = max(vec3(qz_hx.y, hy_hz.x, hy_hz.y), vec3(0.05));

    vec2 ndc = v_uv * 2.0 - 1.0;
    vec3 view_pos = vec3(ndc.x * inv_proj.x * lin_d, ndc.y * inv_proj.y * lin_d, -lin_d);
    vec3 rel = view_pos - obb_center;
    vec3 t = 2.0 * cross(-qxyz, rel);
    vec3 local_p = rel + qw * t + cross(-qxyz, t);
    if (any(greaterThan(abs(local_p), half_ext))) discard;

    vec4 clip = vec4(ndc, z, 1.0);
    float px = dot(pc.prev_vp_inv_curr_vp[0], clip);
    float py = dot(pc.prev_vp_inv_curr_vp[1], clip);
    float pw = dot(pc.prev_vp_inv_curr_vp[2], clip);
    vec2 vel = vec2(0.0);
    if (abs(pw) >= 1e-6) {
      vec2 prev_uv = (vec2(px, py) / pw) * 0.5 + 0.5;
      vel = v_uv - prev_uv;
      float vlen = length(vel);
      if (vlen > 0.25) vel *= (0.25 / vlen);
    }

    float edge_reactive = compute_edge_reactive(v_uv, lin_d, near_p, rev_z);
    float speed_reactive = clamp(length(vel) * (is_player ? 18.0 : 14.0), 0.0, 1.0) * (is_player ? 0.35 : 0.45);
    float reactive = clamp(max(edge_reactive, speed_reactive), 0.0, 1.0);
    out_motion = vec4(vel, reactive, lin_d);
    return;
  }

  // Pass 0: Fullscreen static-world camera reprojection.
  bool rev_z = pc.params.z > 0.5;
  float near_p = max(pc.params.w, 0.05);
  float z = texture(depth_tex, v_uv).r;
  float lin_d = linear_depth_m(z, near_p, rev_z);
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

  float edge_reactive = compute_edge_reactive(v_uv, lin_d, near_p, rev_z);
  float speed_reactive = clamp(length(vel) * 18.0, 0.0, 1.0) * 0.35;
  float reactive = clamp(max(edge_reactive, speed_reactive), 0.0, 1.0);

  out_motion = vec4(vel, reactive, lin_d);
}
