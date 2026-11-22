// Shared body of world.vert and world_gpu.vert (GPU_DRIVEN defined by the latter).
// carview: Colorado's render models with their own textures
// (WORLD-MATERIALS, "Minimal recipe"). Positions relative to the scenery's
// origin in the game frame; the push constant's model matrix takes them to
// the frame's space.

// Reversed depth (the drive): the sky sits at 0 rather than 1.
layout(constant_id = 1) const bool kReverseZ = false;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_normal;  // snorm, game frame; zero when the layout has none
layout(location = 2) in vec2 in_uv0;     // surface UV, dequantised
layout(location = 3) in vec2 in_uv1;     // lightmap UV
layout(location = 4) in vec4 in_colour;  // vertex colour, unorm RGBA
layout(location = 5) in ivec4 in_centre;  // a prop's origin less the corner, decimetres

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

#ifdef GPU_DRIVEN
// The GPU-driven world (world_gpu.cpp): the draw index is the instance (the
// indirect command's firstInstance); the per-draw values come from a table.
// A global stands in for the legacy push block, so the code below is shared.
struct DrawGpu {
  vec4 tint;
  vec4 params;
  uvec4 tex0;
  uvec4 tex1;
  vec4 box_min;
  vec4 box_max;
  uvec4 draw;
  vec4 shift;
};
layout(std430, set = 2, binding = 0) readonly buffer Draws { DrawGpu items[]; };
layout(push_constant) uniform Push {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 swap_pad;  // x: swap progress of the draws marked incoming
  vec4 pad;
} push;
struct DrawView {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 tint;
  vec4 params;
};
DrawView draw;
layout(location = 6) flat out uint v_draw;
#else
layout(push_constant) uniform Draw {
  mat4 model;
  vec4 sun;     // xyz towards the sun in the frame's space
  vec4 up;      // xyz up in the frame's space, w exposure
  vec4 tint;    // flat colour of an untextured material
  vec4 params;  // x blend mode, y flags, z far-ring level
} draw;
#endif

const uint kFlagSky = 2u, kFlagBillboard = 32u, kFlagLodNear = 128u, kFlagLodFar = 256u, kFlagLodOne = 512u,
           kFlagAnimFlag = 131072u, kFlagGrass = 262144u;
// Far-ring levels are pushed away from the eye by this much each (ours):
// unchanged on screen, behind the near terrain where both are drawn.
const float kFarPush = 0.03;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec4 v_colour;
// The prop's fade weights (ours): x the LOD band's, y the edge's, by the
// instance's distance; the fragment dithers only while one is between 0
// and 1.
layout(location = 5) flat out vec2 v_fade;

void main() {
#ifdef GPU_DRIVEN
  const uint did = uint(gl_InstanceIndex);
  draw.model = push.model;
  draw.sun = push.sun;
  draw.up = push.up;
  draw.tint = items[did].tint;
  draw.params = items[did].params;
  v_draw = did;
#endif
#ifdef GPU_DRIVEN
  vec3 in_pos = in_position + items[did].shift.xyz;  // the origin difference of a resident draw
#else
  vec3 in_pos = in_position;
#endif
  uint flags = uint(draw.params.y + 0.5);
  v_fade = vec2(0.0);
  // A prop's level, picked per instance: its LOD00 inside the LOD band,
  // its LOD02 outside it, both dithered only across the band; the far
  // levels fade out at the gather's edge. An instance wholly hidden goes
  // off screen (every corner the same point outside the clip volume), so
  // its triangles cost no fragments.
  if ((flags & (kFlagLodNear | kFlagLodFar | kFlagLodOne)) != 0u && frame.world_fade.y > 0.0) {
    vec3 centre = (draw.model * vec4(in_pos, 1.0)).xyz + mat3(draw.model) * (vec3(in_centre.xyz) * 0.1);
    float d = length(centre - frame.eye.xyz);
    float s = smoothstep(frame.world_fade.x, frame.world_fade.x + frame.world_fade.y, d);
    float e = smoothstep(frame.world_fade.z, frame.world_fade.w, d);
    v_fade = vec2(s, e);
    bool hidden = (flags & kFlagLodNear) != 0u ? s >= 1.0 : e >= 1.0 || ((flags & kFlagLodFar) != 0u && s <= 0.0);
    if (hidden) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
      v_position = vec3(0.0);
      v_normal = vec3(0.0);
      v_uv0 = vec2(0.0);
      v_uv1 = vec2(0.0);
      v_colour = vec4(0.0);
      return;
    }
  }
  if ((flags & kFlagSky) != 0u) {
    // The sky dome: a direction, centred on the eye, at the far plane.
    vec3 dir = mat3(draw.model) * in_position;
    v_position = dir;
    v_normal = vec3(0.0);
    v_uv0 = vec2(0.0);
    v_uv1 = vec2(0.0);
    v_colour = vec4(1.0);
    gl_Position = frame.view_projection * vec4(frame.eye.xyz + dir, 1.0);
    gl_Position.z = kReverseZ ? 0.0 : gl_Position.w * 0.9999999;
    return;
  }
  vec4 world = draw.model * vec4(in_pos, 1.0);
  // The scenery's transform is orthogonal (the car's pose inverted, with
  // the z mirror; no scale), so it turns normals as it is: no per-vertex
  // inverse.
  mat3 normal_matrix = mat3(draw.model);
  if ((flags & kFlagBillboard) != 0u) {
    // A tree sprite (WORLD-MATERIALS, Tree billboards): the base point B,
    // the trunk axis U in the normal, the corner's offset in uv1. Width
    // runs along R, at right angles to U and to the direction to the
    // camera; height along U.
    vec3 u = normalize(normal_matrix * in_normal.xyz);
    vec3 to_eye = frame.eye.xyz - world.xyz;
    vec3 r = cross(u, to_eye);
    float len = length(r);
    r = len > 1e-4 ? r / len : vec3(1.0, 0.0, 0.0);
    world.xyz += r * in_uv1.x + u * in_uv1.y;
    v_position = world.xyz;
    v_normal = u;
    v_uv0 = in_uv0;
    v_uv1 = in_uv1;
    v_colour = in_colour;
    gl_Position = frame.view_projection * world;
    return;
  }
  v_position = world.xyz;
  v_normal = dot(in_normal.xyz, in_normal.xyz) > 0.01 ? normal_matrix * in_normal.xyz : vec3(0.0);
  if ((flags & kFlagAnimFlag) != 0u) {
    // Waving flag cloth displacement (ANIM_FLAG*): pin pole side (uv0.x ~ 0), ripple free edge.
    float t = frame.env_basis_x.w;
    float wspd = max(frame.env_basis_y.w, 0.8);
    float wamp = clamp(frame.env_basis_z.w, 0.08, 0.65);
    float pin = clamp(in_uv0.x, 0.0, 1.0);
    float phase = dot(in_pos.xyz, vec3(1.7, 0.9, 1.3));
    float wave = sin(t * (3.4 + wspd * 2.0) - phase) + 0.35 * sin(t * 7.2 - phase * 1.8);
    vec3 wave_dir = length(v_normal) > 0.1 ? normalize(v_normal) : normalize(frame.env_basis_x.xyz);
    world.xyz += wave_dir * (wave * pin * (0.14 + wamp * 0.35));
    v_position = world.xyz;
  } else if ((flags & kFlagGrass) != 0u) {
    // Wind-swaying grass quads (_0x00005007): top of blade sways along wind direction.
    float t = frame.env_basis_x.w;
    float wspd = max(frame.env_basis_y.w, 0.8);
    float wamp = clamp(frame.env_basis_z.w, 0.05, 0.50);
    float tip = clamp(1.0 - in_uv0.y, 0.0, 1.0);
    float sway = sin(t * (1.8 + wspd) + dot(in_pos.xz, vec2(0.35, 0.27))) * (0.06 + wamp * 0.25) * tip;
    world.xyz += (frame.env_basis_x.xyz * 0.8 + frame.env_basis_z.xyz * 0.6) * sway;
    v_position = world.xyz;
  }
  v_uv0 = in_uv0;
  v_uv1 = in_uv1;
  v_colour = in_colour;
  vec4 placed = world;
  if (draw.params.z > 0.5) placed.xyz = frame.eye.xyz + (world.xyz - frame.eye.xyz) * (1.0 + kFarPush * draw.params.z);
  gl_Position = frame.view_projection * placed;
}
