#version 450
// carview: Phase B Volumetric Sun Shafts, Aerial Perspective & Night Headlight Cones.
// Quarter-res (High) or Half-res (Ultra) IGN-jittered 3D ray march sampling the
// 3-cascade D32 sun CSM shadow map + up to 8 active vehicle headlight cones with
// Henyey-Greenstein forward scattering and height-stratified Colorado valley mist.
//
// Modes (pc.vol_params.z):
//   0 = jittered 3D volumetric ray march -> RGBA16F (in_scattered_rgb, transmittance)
//   1 = 5-tap depth-aware bilateral spatial denoise

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;
layout(set = 0, binding = 2) uniform sampler2D vol_in;

struct VolBeam {
  vec4 origin_range;  // xyz origin (render space), w range (m)
  vec4 dir_power;     // xyz unit forward dir (render space), w intensity
  vec4 color_cos;     // rgb beam tint, w cos(half_cone_angle)
};

layout(set = 0, binding = 3) uniform VolumetricUbo {
  mat4 inv_view_proj;
  mat4 shadow_matrix;
  mat4 shadow_matrix_mid;
  mat4 shadow_matrix_far;
  vec4 eye_time;      // xyz camera eye (render space), w time_sec
  vec4 sun_dir_vis;   // xyz unit dir towards sun (render space), w sun_elev_vis [0..1]
  vec4 sun_col_str;   // rgb sun scatter tint, w shaft_strength
  vec4 fog_col_dens;  // rgb fog/ambient mist color, w base_density
  vec4 shadow_p;      // x shadows_on, y three_cascades, z night_factor, w beam_count (0..8)
  vec4 up_eyey;       // xyz world_up (render space), w eye_game_y
  vec4 height_p;      // x height_start, y height_end, z height_fade, w frame_jitter [0..1]
  VolBeam beams[8];
} vol;

layout(push_constant) uniform VolPush {
  vec4 proj_params;  // x = near_plane, y = far_plane, z = reverse_z (0/1), w = unused
  vec4 vol_params;   // x = steps (16/24), y = 1/w, z = mode (0=march, 1=denoise), w = 1/h
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_vol;

bool is_sky_z(float z) {
  return pc.proj_params.z > 0.5 ? (z <= 1e-4) : (z >= 0.9999);
}

float linear_depth_m(float z) {
  float near_p = max(pc.proj_params.x, 0.05);
  if (pc.proj_params.z > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = max(pc.proj_params.y, near_p + 1.0);
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

float ign(vec2 px) {
  return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

// Henyey-Greenstein scattering phase function (g in [-1, 1]).
float hg_phase(float cos_theta, float g) {
  float g2 = g * g;
  float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 0.02);
  return (1.0 - g2) / (4.0 * 3.14159265 * pow(denom, 1.5));
}

float sample_csm_vol(vec3 p) {
  if (vol.shadow_p.x < 0.5) return 1.0;
  bool three_c = vol.shadow_p.y > 0.5;
  float inv_c = three_c ? (1.0 / 3.0) : 0.5;

  vec4 sn = vol.shadow_matrix * vec4(p, 1.0);
  if (max(abs(sn.x), abs(sn.y)) < 0.98 && sn.z > 0.0 && sn.z < 1.0) {
    vec2 uv = vec2((sn.x * 0.5 + 0.5) * inv_c, sn.y * 0.5 + 0.5);
    return texture(shadow_map, vec3(uv, sn.z));
  }
  if (three_c) {
    vec4 sm = vol.shadow_matrix_mid * vec4(p, 1.0);
    if (max(abs(sm.x), abs(sm.y)) < 0.98 && sm.z > 0.0 && sm.z < 1.0) {
      vec2 uv = vec2((sm.x * 0.5 + 0.5) * inv_c + inv_c, sm.y * 0.5 + 0.5);
      return texture(shadow_map, vec3(uv, sm.z));
    }
  }
  vec4 sf = vol.shadow_matrix_far * vec4(p, 1.0);
  if (max(abs(sf.x), abs(sf.y)) < 0.98 && sf.z > 0.0 && sf.z < 1.0) {
    float x_off = three_c ? (2.0 * inv_c) : 0.5;
    vec2 uv = vec2((sf.x * 0.5 + 0.5) * inv_c + x_off, sf.y * 0.5 + 0.5);
    return texture(shadow_map, vec3(uv, sf.z));
  }
  return 1.0;
}

void main() {
  int mode = int(pc.vol_params.z + 0.5);
  if (mode == 1) {
    // Depth-aware 5-tap bilateral spatial denoise (eliminates ray-march step banding cleanly).
    float d_c = linear_depth_m(texture(depth_tex, v_uv).r);
    vec4 acc = texture(vol_in, v_uv) * 2.0;
    float wsum = 2.0;
    vec2 step_uv = vec2(pc.vol_params.y, pc.vol_params.w);
    const vec2 offs[4] = vec2[4](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
    float z_tol = max(d_c * 0.08, 2.5);
    for (int i = 0; i < 4; ++i) {
      vec2 suv = clamp(v_uv + offs[i] * step_uv, vec2(0.001), vec2(0.999));
      float sd = linear_depth_m(texture(depth_tex, suv).r);
      float w = exp(-abs(sd - d_c) / z_tol);
      acc += texture(vol_in, suv) * w;
      wsum += w;
    }
    out_vol = acc / max(wsum, 1e-4);
    return;
  }

  // Pass 0: Jittered 3D volumetric ray march in render space.
  float z = texture(depth_tex, v_uv).r;
  bool sky = is_sky_z(z);

  // Reconstruct unit world ray direction from clip NDC through inv_view_proj.
  vec2 ndc = v_uv * 2.0 - 1.0;
  vec4 wp = vol.inv_view_proj * vec4(ndc, 0.5, 1.0);
  vec3 world_pt = wp.xyz / max(abs(wp.w), 1e-5) * (wp.w >= 0.0 ? 1.0 : -1.0);
  vec3 eye = vol.eye_time.xyz;
  vec3 ray_vec = world_pt - eye;
  float rlen = length(ray_vec);
  if (rlen < 1e-4) {
    out_vol = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  vec3 ray_dir = ray_vec / rlen;

  float scene_dist = 260.0;
  if (!sky) {
    vec4 hp = vol.inv_view_proj * vec4(ndc, z, 1.0);
    if (abs(hp.w) > 1e-6) {
      vec3 hit_pt = hp.xyz / hp.w;
      scene_dist = min(length(hit_pt - eye), 260.0);
    } else {
      scene_dist = min(linear_depth_m(z), 260.0);
    }
  }

  vec3 sun_dir = normalize(vol.sun_dir_vis.xyz);
  float sun_vis = clamp(vol.sun_dir_vis.w, 0.0, 1.0);
  float night = clamp(vol.shadow_p.z, 0.0, 1.0);
  int beam_count = clamp(int(vol.shadow_p.w + 0.5), 0, 8);
  vec3 world_up = normalize(vol.up_eyey.xyz);

  // Golden-hour boost: sun shafts peak dramatically when sun is low over Colorado mountains
  // (elevation 0..25 deg), while staying crisp and subtle at high noon.
  float sun_elev = clamp(dot(sun_dir, world_up), 0.0, 1.0);
  float golden_hour = (1.0 - smoothstep(0.06, 0.65, sun_elev)) * sun_vis;
  float cos_sun = dot(ray_dir, sun_dir);
  float sun_phase = mix(0.22, hg_phase(cos_sun, 0.72) * 1.35 + hg_phase(cos_sun, 0.25) * 0.45, 0.85);

  int steps = clamp(int(pc.vol_params.x + 0.5), 8, 32);
  float jitter = fract(ign(gl_FragCoord.xy) + vol.height_p.w * 0.61803399);
  float max_march = min(scene_dist, 240.0);
  float min_march = 1.2;
  if (max_march <= min_march) {
    out_vol = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  // Base mist density driven by TOD fog + golden-hour/night valley atmosphere.
  float base_dens = clamp(vol.fog_col_dens.w * 0.95 + golden_hour * 0.0022 + night * 0.0018, 0.0004, 0.012);
  float h_start = vol.height_p.x;
  float h_span = max(vol.height_p.y - h_start, 20.0);
  float h_fade_amt = clamp(vol.height_p.z, 0.0, 0.95);

  vec3 scatter_acc = vec3(0.0);
  float transmittance = 1.0;
  float prev_d = min_march;

  for (int i = 0; i < steps; ++i) {
    float u = (float(i) + jitter) / float(steps);
    // Quadratic distribution keeps dense slices near the car (1.2..45 m for crisp headlight cones)
    // while reaching 240 m out across Colorado canyons for golden-hour god rays.
    float d = mix(min_march, max_march, u * (0.28 + 0.72 * u));
    float ds = max(d - prev_d, 0.15);
    prev_d = d;

    vec3 P = eye + ray_dir * d;
    float world_y = dot(P - eye, world_up) + vol.up_eyey.w;
    float hu = clamp((world_y - h_start) / h_span, 0.0, 1.0);
    float h_smooth = hu * hu * (3.0 - 2.0 * hu);
    float dens = base_dens * (1.0 - h_smooth * h_fade_amt * 0.75);

    vec3 step_in = vec3(0.0);

    // 1. Shadowed volumetric sun shafts + warm aerial perspective scattering.
    if (sun_vis > 0.01) {
      float lit = sample_csm_vol(P);
      float shaft_gain = vol.sun_col_str.w * (0.45 + 1.45 * golden_hour);
      // Aerial perspective ambient mist + directional Mie sun shafts.
      step_in += vol.sun_col_str.rgb * (lit * sun_phase * shaft_gain);
      step_in += vol.fog_col_dens.rgb * (0.14 + 0.12 * lit) * sun_vis;
    }

    // 2. Night volumetric vehicle headlight cones slicing through Colorado valley mist.
    if (night > 0.02 && beam_count > 0 && d < 120.0) {
      for (int bi = 0; bi < beam_count; ++bi) {
        vec3 orig = vol.beams[bi].origin_range.xyz;
        float range = max(vol.beams[bi].origin_range.w, 1.0);
        vec3 to_p = P - orig;
        float d2 = dot(to_p, to_p);
        if (d2 >= range * range || d2 < 0.25) continue;
        float dist = sqrt(d2);
        vec3 ldir = to_p / dist;
        float ang_cos = dot(ldir, vol.beams[bi].dir_power.xyz);
        float cone_cos = vol.beams[bi].color_cos.w;
        if (ang_cos <= cone_cos) continue;
        // Tighter core falloff keeps distinct left/right volumetric cones without whiteout.
        float cone_w = smoothstep(cone_cos, mix(cone_cos, 1.0, 0.58), ang_cos);
        cone_w = cone_w * cone_w;
        // Clear zone right in front of the windshield (0..2.4 m) so cockpit view stays razor sharp.
        float near_w = smoothstep(2.0, 7.0, dist);
        float dist_w = pow(clamp(1.0 - dist / range, 0.0, 1.0), 1.8) / (1.0 + 0.028 * d2);
        // Balanced Mie scatter (gentle forward glow from cockpit, vivid beams from oncoming/chase).
        float beam_phase = 0.22 + 0.42 * hg_phase(dot(ray_dir, ldir), 0.48) + 0.65 * hg_phase(dot(-ray_dir, ldir), 0.52);
        // Compress physical HDR candela power cleanly into atmospheric scattering range.
        float raw_p = max(vol.beams[bi].dir_power.w, 0.0);
        float power = (min(raw_p, 2.0) * 0.35 + log2(1.0 + max(raw_p - 2.0, 0.0)) * 0.14) * night;
        step_in += vol.beams[bi].color_cos.rgb * (cone_w * near_w * dist_w * beam_phase * power * 1.15);
      }
    }

    float ext = exp(-dens * ds);
    scatter_acc += transmittance * step_in * (dens * ds);
    transmittance *= ext;
    if (transmittance < 0.08) break;
  }

  // Clamp subtle aerial perspective transmittance so existing world_frag horizon fog never double-darkens.
  float final_trans = mix(1.0, clamp(transmittance, 0.78, 1.0), 0.38);
  out_vol = vec4(clamp(scatter_acc, vec3(0.0), vec3(2.5)), final_trans);
}
