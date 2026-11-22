#version 450
// carview additive lamp & streetlamp glow sprites (CAR_LAMPS.md §4.3, SPEC-SKY-AND-TOD §6.3).
// Instanced camera-facing quads (6 vertices per sprite, 1 GlowSprite instance per sprite).

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
  vec4 shadow_bias;  // y: cockpit_clip_dist (> 0 in cockpit camera to prevent glow bleed)
} frame;

// Per-instance attributes (64 bytes, divisor = 1):
layout(location = 0) in vec4 in_pos_radius;  // xyz position (car/render space), w radius (metres)
layout(location = 1) in vec4 in_dir_inner;   // xyz unit emission axis (0,0,0 = omnidirectional), w cos(inner_cone)
layout(location = 2) in vec4 in_color_outer; // rgb linear HDR colour * intensity, w cos(outer_cone)
layout(location = 3) in vec4 in_extra;       // x tex_index (0..6), y view_bias_m, z max_dist_m, w fade

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out int v_tex;

void main() {
  const vec2 corners[6] = vec2[6](
      vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
      vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
  vec2 c = corners[gl_VertexIndex % 6];
  v_uv = c * 0.5 + 0.5;
  v_tex = clamp(int(in_extra.x + 0.5), 0, 6);

  vec3 pos = in_pos_radius.xyz;
  float radius = in_pos_radius.w;
  vec3 to_eye = frame.eye.xyz - pos;
  float dist = length(to_eye);
  if (dist < 0.05 || radius <= 0.001 || in_extra.w <= 0.001) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_color = vec4(0.0);
    return;
  }
  vec3 view_dir = to_eye / dist;

  // Angular cone falloff for directional car lamps (omnidirectional when dir == 0).
  float cone = 1.0;
  float dir_len2 = dot(in_dir_inner.xyz, in_dir_inner.xyz);
  if (dir_len2 > 0.01) {
    vec3 lamp_dir = in_dir_inner.xyz * inversesqrt(dir_len2);
    float cos_ang = dot(view_dir, lamp_dir);
    cone = smoothstep(in_color_outer.w, in_dir_inner.w, cos_ang);
  }
  float max_d = max(in_extra.z, 10.0);
  float dist_fade = 1.0 - smoothstep(max_d * 0.65, max_d, dist);
  float cockpit_clip = frame.shadow_bias.y;
  float cockpit_fade = cockpit_clip > 0.05 ? smoothstep(cockpit_clip * 0.65, cockpit_clip, dist) : 1.0;
  float alpha = cone * dist_fade * cockpit_fade * in_extra.w;
  if (alpha <= 0.002) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_color = vec4(0.0);
    return;
  }

  // Slight bias toward the camera along view_dir to prevent Z-fighting with lamp lenses.
  vec3 center = pos + view_dir * min(in_extra.y, dist * 0.4);
  vec3 up_ref = abs(view_dir.y) < 0.98 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  vec3 right = normalize(cross(up_ref, view_dir));
  vec3 up = cross(view_dir, right);
  // Subtle perspective size boost at distance so distant headlights/streetlamps stay legible.
  float r_eff = radius * (1.0 + clamp(dist * 0.004, 0.0, 1.8));
  vec3 world_p = center + (right * c.x + up * c.y) * r_eff;

  v_color = vec4(in_color_outer.rgb, alpha);
  gl_Position = frame.view_projection * vec4(world_p, 1.0);
}
