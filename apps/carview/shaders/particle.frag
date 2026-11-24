#version 450
// carview batched/instanced soft particle fragment shader (SPEC-VFX-AND-PARTICLES).
// Supports linear-depth soft fading (_soft), 2-cascade sun shadow receiving,
// dynamic ToD hemisphere/sun lighting, night headlight beam illumination, and
// cockpit near-clip protection.

layout(constant_id = 0) const bool kLinearOut = false;
layout(constant_id = 1) const bool kReverseZ = false;

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
  vec4 damage_zones;
  vec4 car_sun_color;
  mat4 shadow_matrix_mid;
  vec4 shadow_mid_bias;
  vec4 phase_b_params;
} frame;
layout(set = 0, binding = 2) uniform sampler2DShadow shadow_map;
layout(set = 0, binding = 3) uniform sampler2D headlight_cookie;

struct HeadlightBeam {
  mat4 proj;
  vec4 origin_range;
  vec4 dir_power;
  vec4 color;
};
layout(set = 0, binding = 4) uniform Headlights {
  vec4 count_night;
  HeadlightBeam beams[16];
} headlights;

layout(set = 1, binding = 0) uniform sampler2D particle_atlas;
layout(set = 1, binding = 1) uniform sampler2D scene_depth;

layout(push_constant) uniform ParticlePush {
  vec4 viewport_near;  // xy = 1/width, 1/height; z = near_plane (m); w = cockpit_clip_dist (m)
  vec4 sun_dir;        // xyz towards sun in render space, w = exposure
  vec4 up_dir;         // xyz up in render space, w = has_scene_depth (0 or 1)
} push_c;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) in vec4 v_light;
layout(location = 4) in vec4 v_extra;

layout(location = 0) out vec4 out_color;

float sample_cascade(vec4 s, float x_off, float inv_c) {
  vec2 uv = vec2((s.x * 0.5 + 0.5) * inv_c + x_off, s.y * 0.5 + 0.5);
  return texture(shadow_map, vec3(uv, s.z));
}

float particle_shadow(vec3 p) {
  if (frame.shadow.x < 0.5) return 1.0;
  bool three_c = frame.shadow_mid_bias.y > 0.5;
  float inv_c = three_c ? (1.0 / 3.0) : 0.5;
  vec4 sn = frame.shadow_matrix * vec4(p, 1.0);
  float edge_n = max(abs(sn.x), abs(sn.y));
  bool near_ok = edge_n < 1.0 && sn.z > 0.0 && sn.z < 1.0;
  float lit_n = near_ok ? sample_cascade(sn, 0.0, inv_c) : 1.0;
  if (three_c) {
    if (near_ok && edge_n < 0.80) return mix(frame.shadow.z, 1.0, lit_n);
    vec4 sm = frame.shadow_matrix_mid * vec4(p, 1.0);
    float edge_m = max(abs(sm.x), abs(sm.y));
    bool mid_ok = edge_m < 1.0 && sm.z > 0.0 && sm.z < 1.0;
    float lit_m = mid_ok ? sample_cascade(sm, inv_c, inv_c) : 1.0;
    if (near_ok) {
      return mix(frame.shadow.z, 1.0, mix(lit_n, lit_m, smoothstep(0.80, 0.98, edge_n)));
    }
    if (mid_ok && edge_m < 0.80) return mix(frame.shadow.z, 1.0, lit_m);
    vec4 sf = frame.shadow_matrix_far * vec4(p, 1.0);
    float edge_f = max(abs(sf.x), abs(sf.y));
    float lit_f = (edge_f < 1.0 && sf.z > 0.0 && sf.z < 1.0) ? sample_cascade(sf, 2.0 * inv_c, inv_c) : 1.0;
    float bm = mid_ok ? smoothstep(0.80, 0.98, edge_m) : 1.0;
    return mix(frame.shadow.z, 1.0, mix(lit_m, lit_f, bm));
  }
  if (frame.shadow_far_bias.y > 0.5 && (!near_ok || edge_n > 0.78)) {
    vec4 sf = frame.shadow_matrix_far * vec4(p, 1.0);
    float edge_f = max(abs(sf.x), abs(sf.y));
    float lit_f = (edge_f < 1.0 && sf.z > 0.0 && sf.z < 1.0) ? sample_cascade(sf, 0.5, 0.5) : 1.0;
    float blend = near_ok ? smoothstep(0.78, 0.98, edge_n) : 1.0;
    return mix(frame.shadow.z, 1.0, mix(lit_n, lit_f, blend));
  }
  return mix(frame.shadow.z, 1.0, lit_n);
}

float linearize_depth(float d, float near_p) {
  if (kReverseZ) {
    // Reversed-Z infinite far plane: z_ndc = near / z_view -> z_view = near / max(d, eps).
    return near_p / max(d, 1e-5);
  }
  // Standard Z (far ~ 4000 m fallback).
  const float far_p = 4000.0;
  return (near_p * far_p) / max(far_p - d * (far_p - near_p), 1e-4);
}

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
  if (v_color.a <= 0.002) discard;

  // Cockpit interior protection: keep exterior smoke/dust out of the cabin.
  float eye_dist = v_extra.w;
  if (push_c.viewport_near.w > 0.05 && eye_dist < push_c.viewport_near.w) {
    float cock_fade = smoothstep(push_c.viewport_near.w * 0.55, push_c.viewport_near.w, eye_dist);
    if (cock_fade <= 0.01) discard;
  }

  vec4 tex = texture(particle_atlas, v_uv);
  uint flags = uint(v_extra.z + 0.5);
  bool dynamic_light = (flags & 2u) != 0u;
  bool additive = (flags & 4u) != 0u;
  uint tex2_slot = (flags >> 3u) & 0x1Fu;

  // Decode packed soft-depth thickness and <Gradient> row index from v_extra.x when <Texture2> is bound.
  float raw_soft = v_extra.x;
  float soft_m = raw_soft;
  if (tex2_slot > 0u) {
    float grad_q = max(floor(raw_soft * 0.25) - 1.0, 0.0);
    soft_m = fract(raw_soft * 0.25) * 4.0;
    float grad_v = clamp((grad_q + 0.5) / 16.0, 0.03, 0.97);
    // Index along the selected horizontal gradient strip using primary texture luminance.
    float grad_u = clamp(dot(tex.rgb, vec3(0.299, 0.587, 0.114)), 0.02, 0.98);
    // 10x10 particle atlas (2560x2560, 100 cells of 256x256).
    const float inv_grid = 1.0 / 10.0;
    const float pad = 1.5 / 2560.0;
    vec2 cell2 = vec2(float(tex2_slot % 10u), float(tex2_slot / 10u));
    vec2 uv2 = cell2 * inv_grid + vec2(pad) + vec2(grad_u, grad_v) * (inv_grid - 2.0 * pad);
    vec4 tex2 = texture(particle_atlas, uv2);
    tex.rgb = mix(tex.rgb, tex2.rgb, clamp(tex2.a, 0.65, 1.0));
  }

  float alpha = tex.a * v_color.a;
  if (additive) {
    alpha = max(tex.a, dot(tex.rgb, vec3(0.3333))) * v_color.a;
  } else if (v_color.a < 0.95) {
    // Soften radial alpha falloff on translucent smoke/dust plumes (e.g. BI_Dust_Small / tyre smoke)
    // so stacked burst billboards stay wispy instead of forming sharp opaque cotton-ball edges,
    // while leaving opaque cutout debris (leaves/splinters with v_color.a == 1) crisp.
    float soft_tex_a = pow(clamp(tex.a, 0.0, 1.0), 1.45);
    float stack_atten = dynamic_light ? 0.90 : 0.68;
    alpha = soft_tex_a * v_color.a * stack_atten;
  }
  if (alpha <= 0.004) discard;

  // Soft-particle depth fade (_soft / _animated_soft / _gradient_soft).
  if (soft_m > 0.01 && push_c.up_dir.w > 0.5) {
    vec2 suv = gl_FragCoord.xy * push_c.viewport_near.xy;
    float sd = texture(scene_depth, suv).r;
    float near_p = max(push_c.viewport_near.z, 0.05);
    float z_scene = linearize_depth(sd, near_p);
    float z_part = linearize_depth(gl_FragCoord.z, near_p);
    float dz = z_scene - z_part;
    if (dz <= 0.0) discard;
    alpha *= smoothstep(0.0, soft_m, dz);
  }
  if (push_c.viewport_near.w > 0.05) {
    alpha *= smoothstep(push_c.viewport_near.w * 0.55, push_c.viewport_near.w, eye_dist);
  }
  if (alpha <= 0.003) discard;

  // Linearize XML sRGB particle vertex colour tint so non-additive dust/smoke/debris albedos
  // match the linear HDR world lighting pipeline (particle_atlas is already SRGB-decoded).
  vec3 tint_lin = additive ? v_color.rgb : pow(max(v_color.rgb, vec3(0.0)), vec3(2.2));
  vec3 col = tex.rgb * tint_lin;
  if (!additive) {
    // All non-additive particles (including FH1 dynamic_lighting=0 dust puffs like BI_Dust_Small)
    // receive ToD hemisphere + sun illumination, exposure, and atmospheric fog so dusk/night
    // wreckage puffs never glow pure unlit white.
    vec3 sun_col = frame.sun_color.w >= 0.0 ? frame.sun_color.rgb : vec3(0.9695, 0.8358, 0.5840);
    vec3 sky_col = frame.sun_color.w >= 0.0 ? frame.sky_color.rgb : vec3(0.1687, 0.2229, 0.2746);
    vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.ground_color.rgb : vec3(0.1788, 0.1729, 0.1024);
    float shadow_scale = frame.sun_color.w >= 0.0 ? clamp(frame.sun_color.w, 0.0, 1.0) : 1.0;

    float sh = dynamic_light
                   ? mix(1.0, particle_shadow(v_world_pos), shadow_scale * clamp(v_light.w, 0.0, 1.0))
                   : 1.0;
    float amb_w = clamp(v_light.x * 0.25 + 0.12, 0.06, 0.85);
    float sun_w = clamp(v_light.y * 0.36, 0.12, 1.05);
    float sky_w = clamp(v_light.z * 0.38, 0.12, 1.10);

    vec3 lit = (sky_col * 0.62 + gnd_col * 0.38) * (sky_w + amb_w) + sun_col * (sun_w * sh);

    // Night headlight beam illumination scaled by HeadLightPower, softly shoulder-compressed
    // so close impact puffs inside the headlight cones never blow out to solid white.
    int n_beams = int(headlights.count_night.x + 0.5);
    float night_s = headlights.count_night.y;
    float hl_pow = v_extra.y;
    if (n_beams > 0 && night_s > 0.01 && hl_pow > 0.01) {
      vec3 hl_acc = vec3(0.0);
      for (int i = 0; i < 16; ++i) {
        if (i >= n_beams) break;
        vec3 to_p = v_world_pos - headlights.beams[i].origin_range.xyz;
        float dist = length(to_p);
        float range = headlights.beams[i].origin_range.w;
        if (dist > range || dist < 0.2) continue;
        vec4 lp = headlights.beams[i].proj * vec4(v_world_pos, 1.0);
        if (lp.w <= 0.1) continue;
        vec2 luv = lp.xy / lp.w * 0.5 + 0.5;
        if (luv.x < 0.0 || luv.x > 1.0 || luv.y < 0.0 || luv.y > 1.0) continue;
        float cookie = texture(headlight_cookie, luv).r;
        float att = pow(clamp(1.0 - dist / range, 0.0, 1.0), 1.8);
        hl_acc += headlights.beams[i].color.rgb * (cookie * att * headlights.beams[i].dir_power.w * 0.45);
      }
      vec3 hl_scaled = hl_acc * (night_s * clamp(hl_pow, 0.0, 1.5));
      lit += hl_scaled / (vec3(1.0) + hl_scaled * 1.15);
    }
    col *= lit * push_c.sun_dir.w;

    // Distance atmospheric fog blend on alpha-blended plumes.
    if (frame.sun_color.w >= 0.0) {
      float fog_start = frame.fog_color.w;
      float fog_dens = max(frame.fog_params.x, 0.0);
      float fog_amt = clamp(1.0 - exp2(-max(eye_dist - fog_start, 0.0) * fog_dens), 0.0, 0.85);
      col = mix(col, frame.fog_color.rgb, fog_amt);
    }
  } else {
    col *= push_c.sun_dir.w * 2.2;
  }

  vec3 out_c = kLinearOut ? min(max(col, vec3(0.0)), vec3(4.0)) : encode_srgb(col);
  out_color = vec4(out_c, clamp(alpha, 0.0, 1.0));
}
