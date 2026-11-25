#version 450
// carview starfield instanced quads from realtimesky.zip/Stars.bin (SPEC-SKY-AND-TOD §3.3).
// Draws 6 vertices (2 triangles) per star instance — avoids POINT_LIST / gl_PointSize for DX12 & Metal parity.
layout(constant_id = 1) const bool kReverseZ = false;

layout(location = 0) in vec4 in_star;  // per-instance: xyz unit direction (celestial sphere), w brightness [0..1]

layout(push_constant) uniform StarPush {
  mat4 view_proj_rot;  // view_projection * world_rotation (translation zeroed, camera at origin)
  vec4 params;         // x star_fade, y sidereal_angle_rad, z point_size_px, w intensity
  vec4 viewport_inv;   // xy = 1/width, 1/height; zw = unused
} pc;

layout(location = 0) out float v_alpha;
layout(location = 1) out vec2 v_uv;

void main() {
  const vec2 corners[6] = vec2[6](
      vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
      vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
  vec2 corner = corners[gl_VertexIndex % 6];
  v_uv = corner * 0.5 + 0.5;

  // Gentle sidereal rotation about the celestial Y axis.
  float c = cos(pc.params.y), s = sin(pc.params.y);
  vec3 dir = vec3(in_star.x * c - in_star.z * s, in_star.y, in_star.x * s + in_star.z * c);
  // Hide stars below the horizon.
  if (dir.y < 0.0 || pc.params.x < 0.002) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_alpha = 0.0;
    return;
  }
  float t = clamp((in_star.w - 0.05) / 0.95, 0.0, 1.0);
  float horizon = smoothstep(0.0, 0.14, dir.y);
  v_alpha = pc.params.x * pow(t, 1.8) * horizon * pc.params.w;
  vec4 clip = pc.view_proj_rot * vec4(dir, 0.0);
  if (clip.w <= 1e-4) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_alpha = 0.0;
    return;
  }
  // Scale star quad size by viewport_height / 720.0 so night stars retain consistent visual weight at 1440p/4K.
  float res_scale = clamp((1.0 / max(pc.viewport_inv.y, 1e-5)) / 720.0, 0.75, 6.0);
  float size_px = mix(1.2, pc.params.z, t) * res_scale;
  vec2 ndc = clip.xy / clip.w;
  // Half-extent in NDC is (size_px * 0.5) * (2.0 / viewport) = size_px * viewport_inv.
  vec2 offset_ndc = corner * (size_px * pc.viewport_inv.xy);
  gl_Position = vec4(ndc + offset_ndc, kReverseZ ? 0.0 : 1.0, 1.0);
}
