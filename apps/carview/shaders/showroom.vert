#version 450
// carview showroom: car-space vertices, per-draw model matrix, and the
// tyre's radial fit (FORMAT-CARS, Wheels: rule for M1).

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_frame;  // unit quaternion (x, y, z, w), snorm
layout(location = 2) in vec2 in_uv0;    // unorm, before the sub-section's transform
layout(location = 3) in vec2 in_uv1;
layout(location = 4) in vec4 in_light;  // baked lighting bytes, unorm

layout(set = 0, binding = 0) uniform Frame {
  mat4 view_projection;
  vec4 eye;
  vec4 key_light;   // xyz towards the light, w intensity
  vec4 fill_light;
  vec4 options;     // x exposure, y environment intensity, z livery overlay
  vec4 tint_light;
  vec4 reflect_tint;
  mat4 transparent_view_projection;  // glass and lamp covers
  vec4 stripe;  // the factory stripe colour (linear rgb)
} frame;

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 uv0;     // U offset, U scale, V offset, V scale
  vec4 uv1;
  vec4 tint;
  vec4 params;  // x material class, y flags
} draw;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out float v_occlusion;

vec3 rotate(vec4 q, vec3 v) {
  return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
  vec3 p = in_position;
  vec4 q = normalize(in_frame);
  vec3 n = rotate(q, vec3(1.0, 0.0, 0.0));
  vec4 world = draw.model * vec4(p, 1.0);
  mat3 normal_matrix = transpose(inverse(mat3(draw.model)));
  v_position = world.xyz;
  v_normal = normalize(normal_matrix * n);
  v_uv0 = vec2(draw.uv0.x + draw.uv0.y * in_uv0.x, draw.uv0.z + draw.uv0.w * in_uv0.y);
  v_uv1 = vec2(draw.uv1.x + draw.uv1.y * in_uv1.x, draw.uv1.z + draw.uv1.w * in_uv1.y);
  // The first lighting byte behaves as occlusion: about 100 open to the sky.
  v_occlusion = clamp(in_light.x * 255.0 / 100.0, 0.0, 1.0);
  // Glass and lamp covers (class 2) are drawn in the game's transparent
  // pass with their own matrix (AUTOSHOW_REFERENCE, up to about 1 px off).
  gl_Position = (uint(draw.params.x + 0.5) == 2u ? frame.transparent_view_projection : frame.view_projection) * world;
}
