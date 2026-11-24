#version 450
// carview batched/instanced soft particle vertex shader (SPEC-VFX-AND-PARTICLES).
// Draws 6 vertices per particle instance (billboarded or velocity-aligned quad).

layout(set = 0, binding = 0) uniform Frame {
  mat4 view_projection;
  vec4 eye;
  vec4 key_light;
  vec4 fill_light;
  vec4 options;
  vec4 tint_light;
  vec4 reflect_tint;
  mat4 transparent_view_projection;
  vec4 stripe;
  mat4 shadow_matrix;
  vec4 shadow;
  vec4 shadow_bias;
  vec4 world_fade;
  vec4 drive;
  vec4 sun_color;
  vec4 sky_color;
  vec4 ground_color;
  vec4 fog_color;
  vec4 fog_scatter;
  vec4 fog_params;
  vec4 car_light_top;
  vec4 car_light_bot;
  mat4 shadow_matrix_far;
  vec4 shadow_far_bias;
  vec4 env_basis_x;
  vec4 env_basis_y;
  vec4 env_basis_z;
} frame;

layout(push_constant) uniform ParticlePush {
  vec4 viewport_near;  // xy = 1/width, 1/height; z = near_plane (m); w = cockpit_clip_dist (m)
  vec4 sun_dir;        // xyz towards sun in render space, w = exposure
  vec4 up_dir;         // xyz up in render space, w = has_scene_depth (0 or 1)
} push_c;

// Per-instance attributes (96 bytes, divisor = 1):
layout(location = 0) in vec4 in_pos_sx;    // xyz pos (render space), w size_x (m)
layout(location = 1) in vec4 in_vel_sy;    // xyz vel (render space), w size_y (m)
layout(location = 2) in vec4 in_uv_rect;   // xy atlas min UV, zw atlas UV span
layout(location = 3) in vec4 in_color;     // rgba
layout(location = 4) in vec4 in_light;     // light_influence (ambient, sun, sky, shadow)
layout(location = 5) in vec4 in_extra;     // x spin_rad, y soft_depth_m, z headlight_power, w flags

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_color;
layout(location = 3) out vec4 v_light;
layout(location = 4) out vec4 v_extra;  // x soft_depth_m, y headlight_power, z flags, w eye_dist
layout(location = 5) out vec2 v_local_uv;

void main() {
  const vec2 corners[6] = vec2[6](
      vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
      vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
  vec2 c = corners[gl_VertexIndex % 6];
  v_local_uv = c + 0.5;
  v_uv = in_uv_rect.xy + v_local_uv * in_uv_rect.zw;

  vec3 pos = in_pos_sx.xyz;
  vec3 to_eye = frame.eye.xyz - pos;
  float dist = length(to_eye);
  if (dist < 0.08 || in_color.a <= 0.002) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_color = vec4(0.0);
    v_world_pos = pos;
    v_light = vec4(0.0);
    v_extra = vec4(0.0);
    v_local_uv = vec2(0.0);
    return;
  }

  vec3 view_dir = to_eye / dist;
  uint flags = uint(in_extra.w + 0.5);
  bool vel_aligned = (flags & 1u) != 0u;
  float max_scr = clamp(float((flags >> 8u) & 0xFFu) * 0.01, 0.22, 0.95);
  // Cap world quad size so close particles never blow out fill rate while allowing lush smoke/dust plumes.
  float max_world = max(dist * max_scr * 2.4, 0.25);
  float sx = min(max(in_pos_sx.w, 0.01), max_world);
  float sy = min(max(in_vel_sy.w, 0.01), max_world);

  vec3 right, up;
  if (vel_aligned && dot(in_vel_sy.xyz, in_vel_sy.xyz) > 0.04) {
    vec3 vdir = normalize(in_vel_sy.xyz);
    vec3 side = cross(view_dir, vdir);
    float sl = length(side);
    if (sl > 1e-3) {
      right = side / sl;
      up = vdir;
    } else {
      vec3 ref = abs(view_dir.y) < 0.96 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
      right = normalize(cross(ref, view_dir));
      up = cross(view_dir, right);
    }
  } else {
    vec3 ref = abs(view_dir.y) < 0.96 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 base_r = normalize(cross(ref, view_dir));
    vec3 base_u = cross(view_dir, base_r);
    float cs = cos(in_extra.x);
    float sn = sin(in_extra.x);
    right = base_r * cs - base_u * sn;
    up = base_r * sn + base_u * cs;
  }

  vec3 world_p = pos + right * (c.x * sx) + up * (c.y * sy);
  v_world_pos = world_p;
  v_color = in_color;
  v_light = in_light;
  v_extra = vec4(in_extra.y, in_extra.z, float(flags & 0xFFu), dist);
  gl_Position = frame.view_projection * vec4(world_p, 1.0);
}
