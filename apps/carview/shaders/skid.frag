#version 450
// carview skidmark & tyre-rut fragment shader (SPEC-VFX-AND-PARTICLES).

layout(constant_id = 0) const bool kLinearOut = false;

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

layout(set = 1, binding = 0) uniform sampler2D skid_tex;

layout(push_constant) uniform SkidPush {
  mat4 model;
  vec4 sun_dir;  // xyz towards sun, w = exposure
  vec4 up_dir;   // xyz up, w = is_rut (0 asphalt, 1 rut)
} push_c;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_color;

layout(location = 0) out vec4 out_color;

float sample_cascade(vec4 s, float x_off, float inv_c) {
  vec2 uv = vec2((s.x * 0.5 + 0.5) * inv_c + x_off, s.y * 0.5 + 0.5);
  return texture(shadow_map, vec3(uv, s.z));
}

float skid_shadow(vec3 p, vec3 n) {
  if (frame.shadow.x < 0.5) return 1.0;
  bool three_c = frame.shadow_mid_bias.y > 0.5;
  float inv_c = three_c ? (1.0 / 3.0) : 0.5;
  vec4 sn = frame.shadow_matrix * vec4(p + n * frame.shadow_bias.x, 1.0);
  float edge_n = max(abs(sn.x), abs(sn.y));
  bool near_ok = edge_n < 1.0 && sn.z > 0.0 && sn.z < 1.0;
  float lit_n = near_ok ? sample_cascade(sn, 0.0, inv_c) : 1.0;
  if (three_c) {
    if (near_ok && edge_n < 0.80) return clamp(lit_n, 0.0, 1.0);
    vec4 sm = frame.shadow_matrix_mid * vec4(p + n * frame.shadow_mid_bias.x, 1.0);
    float edge_m = max(abs(sm.x), abs(sm.y));
    bool mid_ok = edge_m < 1.0 && sm.z > 0.0 && sm.z < 1.0;
    float lit_m = mid_ok ? sample_cascade(sm, inv_c, inv_c) : 1.0;
    if (near_ok) return clamp(mix(lit_n, lit_m, smoothstep(0.80, 0.98, edge_n)), 0.0, 1.0);
    if (mid_ok) return clamp(lit_m, 0.0, 1.0);
    return 1.0;
  }
  if (near_ok) return clamp(lit_n, 0.0, 1.0);
  return 1.0;
}

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
  vec4 tex = texture(skid_tex, v_uv);
  // Feather lateral ribbon edges so retail treadmark.xds strips blend seamlessly into road/gravel.
  float edge_feather = smoothstep(0.0, 0.08, v_uv.x) * (1.0 - smoothstep(0.92, 1.0, v_uv.x));
  float alpha = tex.a * v_color.a * edge_feather;
  if (alpha <= 0.005) discard;

  vec3 n = normalize(v_normal);
  vec3 sun = normalize(push_c.sun_dir.xyz);
  vec3 up = normalize(push_c.up_dir.xyz);
  vec3 sun_col = frame.sun_color.w >= 0.0 ? frame.sun_color.rgb : vec3(0.9695, 0.8358, 0.5840);
  vec3 sky_col = frame.sun_color.w >= 0.0 ? frame.sky_color.rgb : vec3(0.1687, 0.2229, 0.2746);
  vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.ground_color.rgb : vec3(0.1788, 0.1729, 0.1024);
  float shadow_scale = frame.sun_color.w >= 0.0 ? clamp(frame.sun_color.w, 0.0, 1.0) : clamp(frame.shadow.z, 0.0, 1.0);

  float ndl = max(dot(n, sun), 0.0);
  float sh = mix(1.0, skid_shadow(v_world_pos, n), shadow_scale);
  vec3 lit = sun_col * (ndl * sh) + mix(gnd_col, sky_col, 0.5 + 0.5 * dot(n, up)) * 1.15;

  // Retail treadmark.xds modulation: dark rubber tread on asphalt (TextureIndex 0),
  // warm dusty ochre/gravel rut on dirt/grass/gravel (TextureIndex 1).
  bool is_rut = push_c.up_dir.w > 0.5;
  float tread_luma = dot(tex.rgb, vec3(0.299, 0.587, 0.114));
  vec3 asphalt_tint = mix(vec3(0.025, 0.025, 0.028), v_color.rgb, 0.85);
  vec3 base = is_rut ? (tex.rgb * mix(vec3(0.48, 0.37, 0.27), v_color.rgb * vec3(0.62, 0.52, 0.40), 0.5))
                     : (asphalt_tint * mix(vec3(0.45), max(tex.rgb, vec3(tread_luma * 0.55)), 0.85));
  vec3 col = base * lit * push_c.sun_dir.w;

  // Headlight illumination on skids/ruts at night.
  int n_beams = int(headlights.count_night.x + 0.5);
  float night_s = headlights.count_night.y;
  if (n_beams > 0 && night_s > 0.01) {
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
      col += base * headlights.beams[i].color.rgb * (cookie * att * headlights.beams[i].dir_power.w * night_s);
    }
  }

  float eye_dist = length(frame.eye.xyz - v_world_pos);
  if (frame.sun_color.w >= 0.0) {
    float fog_amt = clamp(1.0 - exp2(-max(eye_dist - frame.fog_color.w, 0.0) * max(frame.fog_params.x, 0.0)), 0.0, 0.9);
    col = mix(col, frame.fog_color.rgb, fog_amt);
  }
  // Smooth distance fade past 120 m so tracks dissolve cleanly into road horizon.
  alpha *= 1.0 - smoothstep(100.0, 160.0, eye_dist);

  vec3 out_c = kLinearOut ? min(max(col, vec3(0.0)), vec3(4.0)) : encode_srgb(col);
  out_color = vec4(out_c, clamp(alpha, 0.0, 1.0));
}
