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
layout(location = 6) in vec2 in_uv2;     // TEXCOORD2 lightmap / AO UV (separate from Splat uv1)

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
  vec4 paint_two_tone;
  vec4 paint_finish;
  vec4 damage_dirt;
  vec4 damage_zones;
  vec4 car_sun_color;
  mat4 shadow_matrix_mid;
  vec4 shadow_mid_bias;
  vec4 phase_b_params;
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
           kFlagWater = 32768u, kFlagVertexLit = 65536u, kFlagAnimFlag = 131072u, kFlagGrass = 262144u;
// Far-ring levels are pushed away from the eye by this much each (ours):
// unchanged on screen, behind the near terrain where both are drawn.
const float kFarPush = 0.03;

// Dynamic world-space horizontal wind direction with gentle Colorado mountain breeze veering
// (replaces hardcoded constant direction with multi-axis time-modulated wind azimuth).
vec2 dynamic_wind_xz(float t_sec, float wind_spd) {
  float ang = 0.6435 + 0.38 * sin(t_sec * 0.075 * max(wind_spd, 0.4)) + 0.16 * cos(t_sec * 0.19);
  return vec2(cos(ang), sin(ang));
}

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec4 v_colour;
// The prop's fade weights (ours): x the LOD band's, y the edge's, by the
// instance's distance; the fragment dithers only while one is between 0
// and 1.
layout(location = 5) flat out vec2 v_fade;
layout(location = 7) out vec2 v_uv2;
layout(location = 8) flat out float v_emissive_thresh;

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
  // Round-to-nearest without +0.5: IEEE-754 float32 is exact for all integer flag masks
  // up to 2^24, whereas adding 0.5 to odd flags >= 2^23 (kFlagEmissiveMap | kFlagTextured)
  // rounds to even, clearing kFlagTextured (bit 0) and spuriously setting kFlagSky (bit 1).
  uint flags = uint(round(draw.params.y));
  v_fade = vec2(0.0);
  // Deterministic per-instance dusk/dawn switch-on threshold (ModelData.x in [0.06, 0.94])
  // computed only for emissive-map draws (skips SFU sin() on >98% of world vertices).
  if ((flags & 8388608u) != 0u) {
    vec3 prop_cell = floor((in_pos + vec3(in_centre.xyz) * 0.1) * 0.25);
    v_emissive_thresh = 0.06 + 0.88 * fract(sin(dot(prop_cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
  } else {
    v_emissive_thresh = 0.06;
  }
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
      v_uv2 = vec2(0.0);
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
    v_uv2 = vec2(0.0);
    v_colour = vec4(1.0);
    gl_Position = frame.view_projection * vec4(frame.eye.xyz + dir, 1.0);
    gl_Position.z = kReverseZ ? 0.0 : gl_Position.w * 0.9999999;
    return;
  }
  // Night-only vertex-lit stage searchlight / laser cones (rgba.a in (0.02, 0.22)):
  // collapse off-screen in daylight so festival laser beams only illuminate at dusk/night.
  if ((flags & kFlagVertexLit) != 0u && in_colour.a > 0.01 && in_colour.a < 0.22 && frame.sky_color.w < 0.08) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_position = vec3(0.0);
    v_normal = vec3(0.0);
    v_uv0 = vec2(0.0);
    v_uv1 = vec2(0.0);
    v_uv2 = vec2(0.0);
    v_colour = vec4(0.0);
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
    float eye_dist = length(to_eye);
    v_fade = vec2(1.0, 0.0);
    if (frame.world_fade.y > 0.0) {
      float near_start = 8.0;
      float near_end = near_start + min(max(frame.world_fade.y, 12.0), 18.0);
      float s = smoothstep(near_start, near_end, eye_dist);
      float far_end = max(frame.world_fade.w * 4.5, 1750.0);
      float far_start = far_end - max(frame.world_fade.y * 8.0, 180.0);
      float e = smoothstep(far_start, far_end, eye_dist);
      v_fade = vec2(s, e);
      if (s <= 0.001 || e >= 0.999) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_position = vec3(0.0);
        v_normal = vec3(0.0);
        v_uv0 = vec2(0.0);
        v_uv1 = vec2(0.0);
        v_uv2 = vec2(0.0);
        v_colour = vec4(0.0);
        return;
      }
    }
    vec3 r = cross(u, to_eye);
    float len = length(r);
    r = len > 1e-4 ? r / len : vec3(1.0, 0.0, 0.0);
    vec3 bb_sway = vec3(0.0);
    float t_bb = frame.env_basis_x.w;
    float wspd_bb = max(frame.env_basis_y.w, 0.4);
    float wamp_bb = clamp(frame.env_basis_z.w, 0.0, 0.85);
    if (in_uv1.y > 0.01 && wamp_bb > 0.001) {
      vec2 w_xz = dynamic_wind_xz(t_bb, wspd_bb);
      vec3 wind_world = frame.env_basis_x.xyz * w_xz.x + frame.env_basis_z.xyz * w_xz.y;
      wind_world -= u * dot(wind_world, u);
      float wlen = length(wind_world);
      vec3 wind_dir = wlen > 1e-4 ? (wind_world / wlen) : r;
      vec3 wind_side = cross(u, wind_dir);
      float phase = dot(in_pos.xz, w_xz * 0.20) - t_bb * wspd_bb * 1.15;
      float gust = sin(phase) + 0.35 * sin(phase * 2.3 + 1.1);
      float cross_gust = sin(phase * 1.47 + 2.4) * 0.22;
      float amp = wamp_bb * min(in_uv1.y * 0.045, 0.40);
      bb_sway = (wind_dir * gust + wind_side * cross_gust) * amp;
    }
    world.xyz += r * in_uv1.x + u * in_uv1.y + bb_sway;
    v_position = world.xyz;
    v_normal = normalize(u * 0.65 + normalize(to_eye + vec3(0.0, 0.001, 0.0)) * 0.35);
    v_uv0 = in_uv0;
    v_uv1 = in_uv1;
    v_uv2 = vec2(0.0);
    v_colour = in_colour;
    gl_Position = frame.view_projection * world;
    return;
  }
  v_position = world.xyz;
  v_normal = dot(in_normal.xyz, in_normal.xyz) > 0.01 ? normal_matrix * in_normal.xyz : vec3(0.0);
  uint blend_v = uint(draw.params.x + 0.5);
  if ((flags & kFlagAnimFlag) != 0u) {
    // Waving flag cloth displacement (ANIM_FLAG*): pin pole side (uv0.x ~ 0 / low vertex-alpha weight),
    // ripple free edge smoothly along a horizontal wind-perpendicular axis so curved feather flags never shred.
    float t = frame.env_basis_x.w;
    float wspd = clamp(max(frame.env_basis_y.w, 0.8), 0.8, 2.2);
    float wamp = clamp(frame.env_basis_z.w, 0.05, 0.45);
    vec2 w_xz = dynamic_wind_xz(t, wspd);
    float pin = clamp(in_uv0.x, 0.0, 1.0) * smoothstep(0.0, 0.25, max(in_colour.a, in_uv0.x));
    float phase = dot(in_pos.xyz, vec3(w_xz.x * 1.35, 0.65, w_xz.y * 1.35));
    float wave = sin(t * (2.6 + wspd * 1.4) - phase) + 0.25 * sin(t * 5.4 - phase * 1.6);
    vec3 horiz_n = vec3(v_normal.x, 0.0, v_normal.z);
    vec3 fallback_dir = normalize(frame.env_basis_x.xyz * w_xz.x + frame.env_basis_z.xyz * w_xz.y);
    vec3 wave_dir = length(horiz_n) > 0.15 ? normalize(horiz_n) : fallback_dir;
    world.xyz += wave_dir * (wave * pin * (0.04 + wamp * 0.12));
    v_position = world.xyz;
  } else if ((flags & kFlagGrass) != 0u) {
    // Wind-swaying grass quads (_0x00005007): top of blade sways along dynamic multi-axis wind.
    float t = frame.env_basis_x.w;
    float wspd = max(frame.env_basis_y.w, 0.8);
    float wamp = clamp(frame.env_basis_z.w, 0.05, 0.50);
    vec2 w_xz = dynamic_wind_xz(t, wspd);
    vec2 w_perp = vec2(-w_xz.y, w_xz.x);
    float tip = clamp(1.0 - in_uv0.y, 0.0, 1.0);
    float phase = dot(in_pos.xz, w_xz * 0.42 + w_perp * 0.11) - t * (1.8 + wspd);
    float gust = sin(phase) + 0.30 * sin(phase * 2.1 + 1.3);
    float cross_sway = sin(phase * 1.55 + 2.1) * 0.25;
    float amp = (0.06 + wamp * 0.25) * tip;
    vec3 wind_main = frame.env_basis_x.xyz * w_xz.x + frame.env_basis_z.xyz * w_xz.y;
    vec3 wind_cross = frame.env_basis_x.xyz * w_perp.x + frame.env_basis_z.xyz * w_perp.y;
    world.xyz += (wind_main * gust + wind_cross * cross_sway) * amp;
    v_position = world.xyz;
  } else if (blend_v == 1u && (flags & (kFlagLodNear | kFlagLodFar | kFlagLodOne)) != 0u &&
             (flags & kFlagWater) == 0u) {
    // Subtle height-weighted 3D tree canopy sway on alpha-cutout foliage props (kFlagLodNear / kFlagLodFar).
    // in_centre holds (prop_origin - in_pos) in decimetres, so rel_y is exact height above prop base in metres.
    float rel_y = -float(in_centre.y) * 0.1;
    float wamp = clamp(frame.env_basis_z.w, 0.0, 0.65);
    if (rel_y > 1.2 && wamp > 0.001) {
      float t = frame.env_basis_x.w;
      float wspd = max(frame.env_basis_y.w, 0.5);
      vec2 w_xz = dynamic_wind_xz(t, wspd);
      vec2 w_perp = vec2(-w_xz.y, w_xz.x);
      float height_w = smoothstep(1.2, 8.5, rel_y);
      vec3 prop_base = in_pos + vec3(in_centre.xyz) * 0.1;
      float trunk_phase = dot(prop_base.xz, w_xz * 0.18) - t * wspd * 1.1;
      float leaf_phase = dot(in_pos.xyz, vec3(1.9, 1.3, 1.7)) - t * (2.8 + wspd * 1.6);
      float main_sway = (sin(trunk_phase) + 0.30 * sin(trunk_phase * 2.17 + 0.9)) * min(rel_y * 0.024, 0.22);
      float leaf_flutter = sin(leaf_phase) * 0.032;
      vec3 wind_main = frame.env_basis_x.xyz * w_xz.x + frame.env_basis_z.xyz * w_xz.y;
      vec3 wind_cross = frame.env_basis_x.xyz * w_perp.x + frame.env_basis_z.xyz * w_perp.y;
      world.xyz += (wind_main * main_sway + wind_cross * leaf_flutter) * (height_w * min(wamp, 0.55));
      v_position = world.xyz;
    }
  }
  v_uv0 = in_uv0;
  // Pass stable game-world meter coordinates (in_pos.xz) to world_frag for water wave synthesis.
  v_uv1 = (flags & kFlagWater) != 0u ? in_pos.xz : in_uv1;
  v_uv2 = in_uv2;
  v_colour = in_colour;
  vec4 placed = world;
  if (draw.params.z > 0.5) {
    // Push MIDDIST (1) +4% (min 180m) and UberLOD (2) +18% (min 550m) along the
    // eye ray so coarse far terrain never bleeds above or swallows near road/car
    // geometry even when approaching far_inner before an async regather swaps.
    vec3 ray = world.xyz - frame.eye.xyz;
    float dist = length(ray);
    float far_push = draw.params.z > 1.5 ? 0.18 : 0.04;
    float min_dist = draw.params.z > 1.5 ? 550.0 : 180.0;
    float scale = max(1.0 + far_push, min_dist / max(dist, 0.5));
    placed.xyz = frame.eye.xyz + ray * scale;
  }
  gl_Position = frame.view_projection * placed;
}
