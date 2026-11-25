#version 450
// carview: Screen-Space Contact Shadows (SSCS).
// Short-range perspective-correct depth ray march along view-space sun direction
// (8..12 IGN-jittered taps with distance-adaptive thickness, bias, and screen-edge fade).
// Grounds tires, front splitters, curbs, fence posts, rocks, grass, and cockpit details
// cleanly across all 4 hardware tiers (including MoltenVK & Android).

layout(set = 0, binding = 0) uniform sampler2D depth_tex;

layout(push_constant) uniform SscsPush {
  vec4 proj_params;   // x = proj00, y = proj11, z = near_plane, w = far_plane
  vec4 sun_view;      // xyz = unit sun direction in view space, w = sun_elev_vis [0..1]
  vec4 sscs_params;   // x = steps (8..12), y = strength [0..1], z = reverse_z (0/1), w = frame_jitter [0..1]
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_shadow;

float linear_depth(vec2 uv) {
  float z = texture(depth_tex, clamp(uv, vec2(0.0005), vec2(0.9995))).r;
  float near_p = max(pc.proj_params.z, 0.05);
  if (pc.sscs_params.z > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = max(pc.proj_params.w, near_p + 1.0);
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

vec3 view_pos_from_depth(vec2 uv, float d) {
  vec2 ndc = uv * 2.0 - 1.0;
  float p00 = abs(pc.proj_params.x) > 1e-4 ? pc.proj_params.x : 1.0;
  float p11 = abs(pc.proj_params.y) > 1e-4 ? pc.proj_params.y : -1.0;
  return vec3(ndc.x * d / p00, ndc.y * d / p11, -d);
}

vec2 project_view_to_uv(vec3 p) {
  float d = max(-p.z, 1e-4);
  vec2 ndc = vec2(p.x * pc.proj_params.x / d, p.y * pc.proj_params.y / d);
  return ndc * 0.5 + 0.5;
}

float ign(vec2 px) {
  return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

void main() {
  float sun_vis = clamp(pc.sun_view.w, 0.0, 1.0);
  float strength = clamp(pc.sscs_params.y, 0.0, 1.0);
  if (sun_vis < 0.01 || strength < 0.01) {
    out_shadow = vec4(1.0);
    return;
  }

  float center_d = linear_depth(v_uv);
  // Contact shadows focus on near/mid detail (< 140 m) where CSM texels peter-pan.
  if (center_d > 140.0) {
    out_shadow = vec4(1.0);
    return;
  }

  vec3 P = view_pos_from_depth(v_uv, center_d);
  vec3 sun_v = pc.sun_view.xyz;
  float slen = length(sun_v);
  if (slen < 1e-4) {
    out_shadow = vec4(1.0);
    return;
  }
  sun_v /= slen;

  // World-space ray length scales gently with view distance (crisp 0.35 m under tires up to 1.8 m mid-range).
  float ray_len = clamp(center_d * 0.032, 0.32, 1.80);
  float near_p = max(pc.proj_params.z, 0.05);
  // Prevent ray from crossing behind the camera near plane.
  if (P.z + sun_v.z * ray_len > -near_p * 1.15) {
    float safe_t = (-near_p * 1.15 - P.z) / max(sun_v.z, 1e-4);
    ray_len = min(ray_len, max(safe_t, 0.0));
  }
  if (ray_len < 0.04) {
    out_shadow = vec4(1.0);
    return;
  }

  int steps = clamp(int(pc.sscs_params.x + 0.5), 6, 16);
  float jitter = fract(ign(gl_FragCoord.xy) + pc.sscs_params.w * 0.61803399);

  // Adaptive self-shadow bias & thin-object thickness threshold to avoid grazing false shadows.
  float bias = clamp(0.025 + center_d * 0.0032, 0.025, 0.22);
  float thickness = clamp(0.095 + center_d * 0.010, 0.095, 0.45);

  float occ = 0.0;
  for (int i = 0; i < steps; ++i) {
    float t = (float(i) + 0.35 + jitter * 0.65) / float(steps);
    // Slightly denser early steps for crisp tire-to-asphalt and curb grounding.
    float frac = t * (0.45 + 0.55 * t);
    vec3 ray_p = P + sun_v * (ray_len * frac);
    vec2 ray_uv = project_view_to_uv(ray_p);
    if (ray_uv.x <= 0.002 || ray_uv.x >= 0.998 || ray_uv.y <= 0.002 || ray_uv.y >= 0.998) break;

    float scene_d = linear_depth(ray_uv);
    float ray_d = -ray_p.z;
    float delta = ray_d - scene_d;
    if (delta > bias && delta < thickness) {
      // Smoothly attenuate hits near the thickness cap and ray end for soft contact edges.
      float thick_fade = 1.0 - smoothstep(thickness * 0.55, thickness, delta);
      float step_fade = 1.0 - smoothstep(0.65, 1.0, frac);
      vec2 edge_d = min(ray_uv, vec2(1.0) - ray_uv);
      float edge_fade = smoothstep(0.0, 0.04, min(edge_d.x, edge_d.y));
      occ = max(occ, thick_fade * step_fade * edge_fade);
      if (occ > 0.96) break;
    }
  }

  float dist_fade = 1.0 - smoothstep(55.0, 135.0, center_d);
  float lit = 1.0 - clamp(occ * strength * sun_vis * dist_fade, 0.0, 0.68);
  out_shadow = vec4(lit, lit, lit, 1.0);
}
