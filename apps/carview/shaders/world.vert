#version 450
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
} frame;

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 sun;     // xyz towards the sun in the frame's space
  vec4 up;      // xyz up in the frame's space, w exposure
  vec4 tint;    // flat colour of an untextured material
  vec4 params;  // x blend mode, y flags, z far-ring level
} draw;

const uint kFlagSky = 2u, kFlagBillboard = 32u;
// Far-ring levels are pushed away from the eye by this much each (ours):
// unchanged on screen, behind the near terrain where both are drawn.
const float kFarPush = 0.03;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
layout(location = 4) out vec4 v_colour;

void main() {
  uint flags = uint(draw.params.y + 0.5);
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
  vec4 world = draw.model * vec4(in_position, 1.0);
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
  v_uv0 = in_uv0;
  v_uv1 = in_uv1;
  v_colour = in_colour;
  vec4 placed = world;
  if (draw.params.z > 0.5) placed.xyz = frame.eye.xyz + (world.xyz - frame.eye.xyz) * (1.0 + kFarPush * draw.params.z);
  gl_Position = frame.view_projection * placed;
}
