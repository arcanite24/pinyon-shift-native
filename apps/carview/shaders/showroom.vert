#version 450
// carview showroom: car-space vertices, per-draw model matrix, impact-zone
// cosmetic damage deformation, and the tyre's radial fit (FORMAT-CARS).

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_frame;  // unit quaternion (x, y, z, w), snorm
layout(location = 2) in vec2 in_uv0;    // unorm, before the sub-section's transform
layout(location = 3) in vec2 in_uv1;
layout(location = 4) in vec4 in_light;  // baked lighting bytes, unorm

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
  vec4 paint_two_tone;
  vec4 paint_finish;
  vec4 damage_dirt;
  vec4 damage_zones;  // x front, y rear, z left, w right (0..1)
} frame;

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 uv0;     // U offset, U scale, V offset, V scale
  vec4 uv1;
  vec4 tint;
  vec4 params;  // x material class, y flags
} draw;

struct CarInstanceGpu {
  mat4 model;
  vec4 stripe;
  vec4 lamp_rgb[30];
  vec4 pad;
};
layout(std430, set = 0, binding = 6) readonly buffer CarInstances { CarInstanceGpu car_instances[]; };

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out float v_occlusion;
layout(location = 5) flat out uint v_instance;

const uint kFlagInstanced = 8192u;

vec3 rotate(vec4 q, vec3 v) {
  return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
  vec3 p = in_position;
  vec4 q = normalize(in_frame);
  vec3 n = rotate(q, vec3(1.0, 0.0, 0.0));
  uint flags = uint(draw.params.y + 0.5);
  v_instance = uint(gl_InstanceIndex);
  mat4 model_mat = (flags & kFlagInstanced) != 0u ? (car_instances[v_instance].model * draw.model) : draw.model;
  vec4 world = model_mat * vec4(p, 1.0);
  mat3 normal_matrix = transpose(inverse(mat3(model_mat)));
  vec3 wn = normalize(normal_matrix * n);

  // Cosmetic impact-zone mesh dent deformation on body/paint/chrome/carbon/glass
  // (model space: -Z is front, +Z is rear, -X is left, +X is right).
  uint klass = uint(draw.params.x + 0.5);
  if (klass != 6u && klass != 7u && klass != 8u && klass != 11u) {
    float z_front = smoothstep(-0.75, -1.95, world.z) * frame.damage_zones.x;
    float z_rear = smoothstep(0.75, 1.95, world.z) * frame.damage_zones.y;
    float x_left = smoothstep(-0.55, -0.95, world.x) * frame.damage_zones.z;
    float x_right = smoothstep(0.55, 0.95, world.x) * frame.damage_zones.w;
    float dent = clamp(max(max(z_front, z_rear), max(x_left, x_right)), 0.0, 1.0);
    if (dent > 0.005) {
      float crumple = 0.65 + 0.35 * sin(world.x * 17.3 + world.z * 13.7 + world.y * 19.1);
      vec3 push_in = -wn * (dent * 0.068 * crumple);
      world.xyz += push_in;
      // Perturb normal slightly so dented sheet metal catches specular highlights realistically.
      vec3 ripple = vec3(sin(world.z * 21.0 + world.y * 14.0), cos(world.x * 19.0 + world.z * 16.0),
                         sin(world.x * 23.0 - world.y * 15.0));
      wn = normalize(wn + ripple * (dent * 0.28));
    }
  }

  v_position = world.xyz;
  v_normal = wn;
  v_uv0 = vec2(draw.uv0.x + draw.uv0.y * in_uv0.x, draw.uv0.z + draw.uv0.w * in_uv0.y);
  v_uv1 = vec2(draw.uv1.x + draw.uv1.y * in_uv1.x, draw.uv1.z + draw.uv1.w * in_uv1.y);
  // The first lighting byte behaves as occlusion: about 100 open to the sky.
  v_occlusion = clamp(in_light.x * 255.0 / 100.0, 0.0, 1.0);
  // Glass and lamp covers (class 2) are drawn in the game's transparent
  // pass with their own matrix (AUTOSHOW_REFERENCE, up to about 1 px off).
  gl_Position = (klass == 2u ? frame.transparent_view_projection : frame.view_projection) * world;
}
