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
  vec4 car_sun_color; // rgb sun scaled by IBLDirectScaleCar, w ibl_direct_car (<0 fallback)
  mat4 shadow_matrix_mid;
  vec4 shadow_mid_bias;
  vec4 phase_b_params;
  vec4 ambient_color;
  vec4 cockpit_mfd;
  vec4 cockpit_mfd_ext;
  vec4 damage_corners;
  vec4 paint_shader_a;
  vec4 paint_shader_b;
  vec4 paint_shader_c;
  vec4 paint_shader_d;
  vec4 region_two_tone[3];
  vec4 region_finish[3];
  vec4 rim_shader;
  vec4 rim_tint_dirt;
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
layout(location = 6) out vec3 v_tangent;
layout(location = 7) out vec3 v_bitangent;
layout(location = 8) out vec3 v_local_pos;
layout(location = 9) out vec3 v_hub_radial;

const uint kFlagInstanced = 8192u;

vec3 rotate(vec4 q, vec3 v) {
  return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
  vec3 p = in_position;
  vec4 q = normalize(in_frame);
  vec3 n = rotate(q, vec3(1.0, 0.0, 0.0));
  vec3 t = rotate(q, vec3(0.0, 1.0, 0.0));
  vec3 b = rotate(q, vec3(0.0, 0.0, -1.0));
  uint flags = uint(round(draw.params.y));
  v_instance = uint(gl_InstanceIndex);
  mat4 model_mat = (flags & kFlagInstanced) != 0u ? (car_instances[v_instance].model * draw.model) : draw.model;
  vec4 world = model_mat * vec4(p, 1.0);
  mat3 normal_matrix = transpose(inverse(mat3(model_mat)));
  vec3 wn = normalize(normal_matrix * n);
  vec3 wt = normalize(normal_matrix * t);
  vec3 wb = normalize(normal_matrix * b);

  // Cosmetic impact-zone mesh dent deformation ONLY on exterior crushable panels
  // (kOpaque=0, kPaint=1, kLamp=3, kChrome=9, kCarbon=10; excludes kGlass=2, kInterior=4,
  // kGauge=5, kTyre=6, kRim=7, kCaliper=8, kWorld=11). Weighted by per-vertex .carbin crush
  // weight in_light.w when present, with clean fallback for legacy bakes without crush weights.
  uint klass = uint(round(draw.params.x));
  bool crushable_class = (klass == 0u || klass == 1u || klass == 3u || klass == 9u || klass == 10u);
  if ((flags & kFlagInstanced) == 0u && crushable_class) {
    float z_front = (1.0 - smoothstep(-1.95, -0.75, world.z)) * frame.damage_zones.x;
    float z_rear = smoothstep(0.75, 1.95, world.z) * frame.damage_zones.y;
    float x_left = (1.0 - smoothstep(-0.95, -0.55, world.x)) * frame.damage_zones.z;
    float x_right = smoothstep(0.55, 0.95, world.x) * frame.damage_zones.w;
    bool has_car_crush = (uint(max(frame.rim_tint_dirt.w, 0.0) + 0.5) & 2u) != 0u;
    float crush_w = (in_light.w > 0.003) ? in_light.w : (has_car_crush ? 0.0 : 0.65);
    float dent = clamp(max(max(z_front, z_rear), max(x_left, x_right)), 0.0, 1.0) * crush_w;
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
  v_tangent = wt;
  v_bitangent = wb;
  v_local_pos = p;
  v_hub_radial = mat3(model_mat) * vec3(0.0, p.y, p.z);
  v_uv0 = vec2(draw.uv0.x + draw.uv0.y * in_uv0.x, draw.uv0.z + draw.uv0.w * in_uv0.y);
  v_uv1 = vec2(draw.uv1.x + draw.uv1.y * in_uv1.x, draw.uv1.z + draw.uv1.w * in_uv1.y);
  // The first lighting byte behaves as occlusion: about 100 open to the sky.
  v_occlusion = clamp(in_light.x * 255.0 / 100.0, 0.0, 1.0);
  // Glass and lamp covers (class 2) are drawn in the game's transparent
  // pass with their own matrix (AUTOSHOW_REFERENCE, up to about 1 px off).
  gl_Position = (klass == 2u ? frame.transparent_view_projection : frame.view_projection) * world;
}
