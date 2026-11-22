#version 450
// carview starfield point sprites from realtimesky.zip/Stars.bin (SPEC-SKY-AND-TOD §3.3).
layout(constant_id = 1) const bool kReverseZ = false;

layout(location = 0) in vec4 in_star;  // xyz unit direction (celestial sphere), w brightness [0..1]

layout(push_constant) uniform StarPush {
  mat4 view_proj_rot;  // view_projection * world_rotation (translation zeroed, camera at origin)
  vec4 params;         // x star_fade, y sidereal_angle_rad, z point_size, w intensity
} pc;

layout(location = 0) out float v_alpha;

void main() {
  // Gentle sidereal rotation about the celestial Y axis.
  float c = cos(pc.params.y), s = sin(pc.params.y);
  vec3 dir = vec3(in_star.x * c - in_star.z * s, in_star.y, in_star.x * s + in_star.z * c);
  // Hide stars below the horizon.
  if (dir.y < 0.0 || pc.params.x < 0.002) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 1.0;
    v_alpha = 0.0;
    return;
  }
  float t = clamp((in_star.w - 0.05) / 0.95, 0.0, 1.0);
  float horizon = smoothstep(0.0, 0.14, dir.y);
  v_alpha = pc.params.x * pow(t, 1.8) * horizon * pc.params.w;
  vec4 clip = pc.view_proj_rot * vec4(dir, 0.0);
  if (clip.w <= 1e-4) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 1.0;
    v_alpha = 0.0;
    return;
  }
  clip.z = kReverseZ ? 0.0 : clip.w * 0.9999999;
  gl_Position = clip;
  gl_PointSize = mix(1.2, pc.params.z, t);
}
