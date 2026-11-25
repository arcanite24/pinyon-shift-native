#version 450
// carview: hardware-instanced GPU-driven tree sprite vertex shader (world_gpu.cpp).
// Draws 6 vertices per visible tree instance (indexless quad) and shares
// world_gpu.frag + bindless textures with world_gpu.vert.

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
  vec4 world_fade;  // prop LOD distance, its band; the edge fade's start and end
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
  vec4 env_basis_x;  // xyz game X in render space, w real_time_sec
  vec4 env_basis_y;  // xyz game Y in render space, w wind_speed
  vec4 env_basis_z;  // xyz game Z in render space, w wind_amplitude
} frame;

struct TreeInstanceGpu {
  vec4 base_hw;   // xyz base in scenery frame, w half_width
  vec4 up_h;      // xyz up axis in scenery frame, w height
  vec4 uv_rect;   // u0, v0, u1, v1
  uvec4 col_tex;  // x RGBA8 tint, y bindless tex slot (unused here), z DrawGpu row, w unused
};

layout(std430, set = 2, binding = 1) readonly buffer Trees { TreeInstanceGpu trees[]; };
layout(std430, set = 3, binding = 0) readonly buffer VisibleTrees { uint visible_indices[]; };

layout(push_constant) uniform Push {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 swap_pad;
  vec4 pad;
} push;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec4 v_colour;
layout(location = 5) flat out vec2 v_fade;
layout(location = 6) flat out uint v_draw;
layout(location = 7) out vec2 v_uv2;
layout(location = 8) flat out float v_emissive_thresh;

void main() {
  const uint tid = visible_indices[uint(gl_InstanceIndex)];
  const TreeInstanceGpu t = trees[tid];
  const int kQuad[6] = int[6](0, 1, 2, 0, 2, 3);
  const int c = kQuad[gl_VertexIndex % 6];

  const float ox = (c == 1 || c == 2) ? t.base_hw.w : -t.base_hw.w;
  const float oy = (c >= 2) ? t.up_h.w : 0.0;
  const vec2 uv = vec2((c == 1 || c == 2) ? t.uv_rect.z : t.uv_rect.x,
                       (c >= 2) ? t.uv_rect.y : t.uv_rect.w);

  vec4 world = push.model * vec4(t.base_hw.xyz, 1.0);
  const vec3 u_axis = normalize(mat3(push.model) * t.up_h.xyz);
  const vec3 to_eye = frame.eye.xyz - world.xyz;
  const float eye_dist = length(to_eye);

  // Smooth LOD billboard cross-fade (v_fade.x = fade-in, v_fade.y = far fade-out) using world_fade.
  v_fade = vec2(1.0, 0.0);
  if (frame.world_fade.y > 0.0) {
    const bool is_distant_proxy = (t.uv_rect.x <= 0.001 && t.uv_rect.y <= 0.001 &&
                                   t.uv_rect.z >= 0.999 && t.uv_rect.w >= 0.999);
    float s = 1.0;
    if (is_distant_proxy && frame.world_fade.w > frame.world_fade.z) {
      // Complementary cross-fade matching 3D tree props fading out across [world_fade.z, world_fade.w].
      s = smoothstep(frame.world_fade.z, frame.world_fade.w, eye_dist);
    } else {
      // Smooth near-camera fade-in so roadside native tree sprites never pop or clip sharply.
      const float near_start = 8.0;
      const float near_end = near_start + min(max(frame.world_fade.y, 12.0), 18.0);
      s = smoothstep(near_start, near_end, eye_dist);
    }
    const float far_end = max(frame.world_fade.w * 4.5, 1750.0);
    const float far_start = far_end - max(frame.world_fade.y * 8.0, 180.0);
    const float e = smoothstep(far_start, far_end, eye_dist);
    v_fade = vec2(s, e);
    if (s <= 0.001 || e >= 0.999) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
      v_position = vec3(0.0);
      v_normal = vec3(0.0);
      v_uv0 = vec2(0.0);
      v_uv1 = vec2(0.0);
      v_uv2 = vec2(0.0);
      v_colour = vec4(0.0);
      v_draw = t.col_tex.z;
      v_emissive_thresh = 0.5;
      return;
    }
  }

  vec3 r_axis = cross(u_axis, to_eye);
  const float len = length(r_axis);
  r_axis = len > 1e-4 ? (r_axis / len) : vec3(1.0, 0.0, 0.0);

  // World-space horizontal wind sway on tree canopies (independent of camera rotation).
  const float t_sec = push.pad.x > 0.0 ? push.pad.x : frame.env_basis_x.w;
  const float wspd = push.pad.y > 0.0 ? push.pad.y : frame.env_basis_y.w;
  const float wamp = push.pad.z > 0.0 ? push.pad.z : frame.env_basis_z.w;
  vec3 sway_offset = vec3(0.0);
  if (oy > 0.01 && wamp > 0.001) {
    // Dynamic multi-axis Colorado wind direction (matches world_vert.glsl grass & 3D canopy sway).
    const float w_ang = 0.6435 + 0.38 * sin(t_sec * 0.075 * max(wspd, 0.4)) + 0.16 * cos(t_sec * 0.19);
    const vec2 w_xz = vec2(cos(w_ang), sin(w_ang));
    vec3 wind_world = frame.env_basis_x.xyz * w_xz.x + frame.env_basis_z.xyz * w_xz.y;
    if (dot(wind_world, wind_world) < 1e-4) {
      wind_world = mat3(push.model) * vec3(w_xz.x, 0.0, w_xz.y);
    }
    wind_world -= u_axis * dot(wind_world, u_axis);
    const float wlen = length(wind_world);
    const vec3 wind_dir = wlen > 1e-4 ? (wind_world / wlen) : vec3(1.0, 0.0, 0.0);
    const vec3 wind_side = cross(u_axis, wind_dir);
    // Traveling multi-axis gust wave aligned with dynamic world-space wind XZ.
    const float phase = dot(t.base_hw.xz, w_xz * 0.20) - t_sec * max(wspd, 0.4) * 1.15;
    const float gust = sin(phase) + 0.35 * sin(phase * 2.3 + 1.1);
    const float cross_gust = sin(phase * 1.47 + 2.4) * 0.22;
    const float amp = min(wamp, 0.85) * min(oy * 0.045, 0.42);
    sway_offset = (wind_dir * gust + wind_side * cross_gust) * amp;
  }
  world.xyz += r_axis * ox + u_axis * oy + sway_offset;

  v_position = world.xyz;
  v_normal = normalize(u_axis * 0.65 + normalize(to_eye + vec3(0.0, 0.001, 0.0)) * 0.35);
  v_uv0 = uv;
  v_uv1 = vec2(ox, oy);
  v_colour = unpackUnorm4x8(t.col_tex.x);
  v_draw = t.col_tex.z;
  v_uv2 = vec2(0.0);
  v_emissive_thresh = 0.5;
  gl_Position = frame.view_projection * world;
}
