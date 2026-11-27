#version 450
precision highp int;
precision mediump float;
// carview real-time sky shader (SPEC-SKY-AND-TOD §3):
//   mode 0: atmosphere gradient dome + elliptical sun haze + sky dither
//   mode 1: sun/moon billboard disc + far clouds (2 layers) + close lit clouds
//           (2 layers) + sky horizon fog skirt, alpha-blended over sky + stars.
//   mode 2: combined single-pass daytime sky (atmosphere + disc + clouds + fog)
//           used when star_fade <= 0.005 to halve sky fullscreen passes & bandwidth.
// All view directions V are in game frame (+Y up, +X East, +Z North).

layout(early_fragment_tests) in;
layout(constant_id = 0) const bool kLinearOut = false;

layout(set = 0, binding = 0) uniform SkyUniforms {
  // Camera basis in game frame: V = normalize(cam_fwd + ndc.x * cam_right - ndc.y * cam_up)
  vec4 cam_fwd;      // xyz forward, w mode (0 = atmo, 1 = disc+clouds+fog)
  vec4 cam_right;    // xyz right * tan_half_fov_x, w time_days (tod_sec / 86400)
  vec4 cam_up;       // xyz up * tan_half_fov_y, w real_time_sec

  vec4 sun_dir;      // xyz SunDir (game frame), w sky_gain
  vec4 sun_obj_dir;  // xyz SunObjectDir (game frame), w sun_disc_radius_rad
  vec4 haze_dir;     // xyz HazeObjectDir (game frame), w is_moon (0 or 1)

  vec4 atmo_top;     // rgb AtmosphereTopColour, w atmo_color_power
  vec4 atmo_bottom;  // rgb AtmosphereBottomColour, w atmo_clamp_lo
  vec4 atmo_haze;    // rgb AtmosphereHazeColour, w atmo_clamp_hi
  vec4 atmo_haze_p;  // x power, y intensity, z elliptical_scale, w alpha

  vec4 sun_obj_col;  // rgb SunObjectColour, w SunObjectAlpha

  vec4 far_dense;        // rgb FarCloudDenseColour, w far_thin_intensity
  vec4 far_thin;         // rgb FarCloudThinColour, w far_alpha_power
  vec4 far_params;       // x front_centre_a, y front_edge_a, z front_coverage, w back_coverage
  vec4 far_back_dense;   // rgb FarCloudBackDenseColour, w back_centre_a
  vec4 far_back_thin;    // rgb FarCloudBackThinColour, w back_edge_a
  vec4 far_scroll;       // xy front scroll uv, zw back scroll uv

  vec4 close_top_col;    // rgb CloseCloudTopColour, w top_power
  vec4 close_bot_col;    // rgb CloseCloudBottomColour, w bot_power
  vec4 close_back_col;   // rgb CloseCloudBackColour, w back_power
  vec4 close_dense_col;  // rgb CloseCloudDenseColour, w dense_intensity
  vec4 close_thin_col;   // rgb CloseCloudThinColour, w thin_intensity
  vec4 close_intens;     // x back_int, y top_int, z bot_int, w alpha_curve
  vec4 close_masks;      // x top_mask_pow, y bot_mask_pow, z top_alpha, w bot_alpha
  vec4 close_layers;     // x front_alpha, y back_alpha, z front_rev_off, w back_rev_off
  vec4 close_back_dense_col; // rgb CloseCloudDenseColourBack, w sky_saturation (1.0 main, cubeSkySaturation cubemap)
  vec4 close_back_thin_col;  // rgb CloseCloudThinColourBack, w CloseCloudBottomIntensityBack
  vec4 close_back_bot_col;   // rgb CloseCloudBottomColourBack, w CloseCloudBottomPowerBack

  vec4 fog_color;        // rgb FogColour, w sky_fog_start_norm
  vec4 fog_scatter;      // rgb FogSunScatterColour, w FogSunScatterPower
  vec4 fog_sky;          // x sky_fog_end_norm, y sky_fog_density, z has_textures, w star_fade
} sky;

layout(set = 0, binding = 1) uniform sampler2D tex_disc;       // Sun.xds or active MoonN.xds
layout(set = 0, binding = 2) uniform sampler2D tex_dither;     // SkyDither.xds
layout(set = 0, binding = 3) uniform sampler2D tex_far_front;  // FarCloudMapFront0.xds
layout(set = 0, binding = 4) uniform sampler2D tex_far_back;   // FarCloudMapBack0.xds
layout(set = 0, binding = 5) uniform sampler2D tex_far_mask;   // FarCloudMask.xds
layout(set = 0, binding = 6) uniform sampler2D tex_close_ad_f; // CloseCloudAlphaDensityFront0.xds
layout(set = 0, binding = 7) uniform sampler2D tex_close_ad_b; // CloseCloudAlphaDensityBack0.xds
layout(set = 0, binding = 8) uniform sampler2D tex_close_am_f; // CloseCloudAmbientFront0.xds
layout(set = 0, binding = 9) uniform sampler2D tex_close_am_b; // CloseCloudAmbientBack0.xds
layout(set = 0, binding = 10) uniform sampler2D tex_close_tb_f;// CloseCloudTopBottomAmbientFront0.xds
layout(set = 0, binding = 11) uniform sampler2D tex_close_tb_b;// CloseCloudTopBottomAmbientBack0.xds

layout(location = 0) in highp vec2 v_ndc;
layout(location = 1) in highp vec2 v_uv;

layout(location = 0) out mediump vec4 out_color;

const highp float kPi = 3.14159265359;
const highp float kTwoPi = 6.28318530718;

mediump vec3 encode_srgb(mediump vec3 c) {
  c = clamp(c, vec3(0.0), vec3(1.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

mediump vec3 out_rgb(mediump vec3 c) { return kLinearOut ? min(max(c, vec3(0.0)), vec3(64.0)) : encode_srgb(c); }

void main() {
  highp vec3 V = normalize(sky.cam_fwd.xyz + v_ndc.x * sky.cam_right.xyz - v_ndc.y * sky.cam_up.xyz);
  int mode = int(sky.cam_fwd.w + 0.5);

  vec3 base_lin_rgb = vec3(0.0);
  if (mode == 0 || mode == 2) {
    // 1. Atmosphere gradient dome + elliptical sun haze (SPEC-SKY-AND-TOD §3.2).
    // Use a smooth C1 exponential horizon-to-zenith falloff anchored at V.y = 0 so
    // AtmosphereBottomClampPower (~0.06) and small dawn/dusk AtmosphereColourPower (~0.05)
    // never produce a flat blinding horizon strip with a 2-pixel cliff into dark zenith.
    float hi = max(sky.atmo_haze.w, 0.15);
    float vy = max(V.y, 0.0);
    float scale = clamp(sky.atmo_top.w * 1.45, 0.11, 0.45) * clamp(hi / 0.50, 0.5, 1.5);
    float h_t = (1.0 - exp(-vy / scale)) / (1.0 - exp(-1.0 / scale));
    vec3 base_rgb = mix(sky.atmo_bottom.rgb, sky.atmo_top.rgb, h_t);

    vec3 S = normalize(sky.haze_dir.xyz);
    vec3 horiz_s = vec3(S.x, 0.0, S.z);
    float hl = length(horiz_s);
    vec3 T = hl > 1e-4 ? vec3(-horiz_s.z / hl, 0.0, horiz_s.x / hl) : vec3(1.0, 0.0, 0.0);
    float haze_dot = clamp(dot(V, S), 0.0, 1.0) *
                     clamp(1.0 + (sky.atmo_haze_p.z - 1.0) * abs(dot(V, T)), 0.0, 1.0);
    float haze_t = clamp(sky.atmo_haze_p.y * pow(max(haze_dot, 0.0), max(sky.atmo_haze_p.x, 0.5)), 0.0, 1.0) *
                   sky.atmo_haze_p.w;

    vec3 lin_rgb = mix(base_rgb, sky.atmo_haze.rgb, haze_t) * sky.sun_dir.w;
    // Golden-hour HDR solar Mie aureole: broad warm glow across the sunward horizon
    // feeding the bloom pass cleanly when the sun is low, while keeping midday crisp and night dark.
    if (sky.haze_dir.w < 0.5) {
      float sun_elev = S.y;
      float day_vis = smoothstep(-0.06, 0.12, sun_elev);
      float golden = (1.0 - smoothstep(0.06, 0.52, max(sun_elev, 0.0))) * day_vis;
      float cos_s = max(dot(V, S), 0.0);
      vec3 glow_col = mix(sky.atmo_haze.rgb, max(sky.fog_scatter.rgb, sky.sun_obj_col.rgb * 0.85), 0.62);
      float cos_s2 = cos_s * cos_s;
      float wide_glow = (cos_s2 * cos_s2 * cos_s2) * (1.0 - smoothstep(0.0, 0.55, vy)) * (0.22 + 0.68 * golden);
      float core_glow = pow(cos_s, 28.0) * (0.32 + 0.85 * golden);
      lin_rgb += glow_col * (wide_glow + core_glow) * sky.sun_dir.w * day_vis;
    }
    // Below the geometric horizon, transition smoothly into the exact world fog colour so
    // distant terrain gaps or far-ring silhouettes never reveal a glowing cyan/white strip.
    if (V.y < 0.0) {
      float below_t = 1.0 - smoothstep(-0.055, 0.0, V.y);
      lin_rgb = mix(lin_rgb, sky.fog_color.rgb, below_t);
    }
    // Subtle sky gradient dither scaled by luminance so night exposure does not amplify it.
    float dither = (texture(tex_dither, gl_FragCoord.xy / 128.0).r - 0.5) * 0.0004;
    lin_rgb = max(lin_rgb + vec3(dither), vec3(0.0));
    base_lin_rgb = lin_rgb;
    if (mode == 0) {
      float sky_sat = clamp(sky.close_back_dense_col.w, 0.0, 2.0);
      float sky_luma = dot(lin_rgb, vec3(0.2126, 0.7152, 0.0722));
      lin_rgb = max(mix(vec3(sky_luma), lin_rgb, sky_sat), vec3(0.0));
      out_color = vec4(out_rgb(lin_rgb), 1.0);
      return;
    }
  }

  // Mode 1: Sun/Moon disc -> Far clouds -> Close lit clouds -> Sky horizon fog skirt.
  // Accumulated in true premultiplied linear Porter-Duff OVER space (zero dark-halo double-premul).
  vec3 acc_premul = vec3(0.0);
  float acc_a = 0.0;
  float sky_gain = max(sky.sun_dir.w, 0.1);
  {
    // 3. Sun / Moon billboard disc + soft HDR solar corona (SPEC-SKY-AND-TOD §3.4).
    vec3 D = normalize(sky.sun_obj_dir.xyz);
    float rad = max(sky.sun_obj_dir.w, 0.005);
    float cos_ang = dot(V, D);
    if (cos_ang > 0.0 && sky.sun_obj_col.w > 0.001) {
      float day_vis = smoothstep(-0.06, 0.10, D.y);
      float golden = (1.0 - smoothstep(0.06, 0.52, max(D.y, 0.0))) * day_vis;
      float horizon_vis = smoothstep(-0.03, 0.05, V.y);
      if (sky.haze_dir.w < 0.5 && day_vis > 0.001) {
        // Soft radial solar corona that melts the disc into golden-hour HDR bloom.
        float corona = pow(cos_ang, 72.0) * mix(0.65, 1.85, golden) +
                       pow(cos_ang, 320.0) * mix(1.40, 3.40, golden);
        vec3 corona_col = mix(sky.sun_obj_col.rgb, max(sky.fog_scatter.rgb, sky.sun_obj_col.rgb), 0.35 * golden);
        acc_premul += corona_col * (corona * horizon_vis * day_vis * sky.sun_obj_col.w);
      }
      vec3 up_ref = abs(D.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
      vec3 right_d = normalize(cross(up_ref, D));
      vec3 up_d = cross(D, right_d);
      vec3 proj = V / max(cos_ang, 1e-4) - D;
      vec2 duv = vec2(dot(proj, right_d), -dot(proj, up_d)) / (rad * 2.0) + vec2(0.5);
      if (duv.x >= 0.0 && duv.x <= 1.0 && duv.y >= 0.0 && duv.y <= 1.0) {
        vec4 tex = texture(tex_disc, duv);
        // Sun/Moon textures are authored in sRGB; decode to linear radiance.
        vec3 disc_rgb = pow(max(tex.rgb, vec3(0.0)), vec3(2.2)) * sky.sun_obj_col.rgb;
        if (sky.haze_dir.w < 0.5) disc_rgb *= mix(3.2, 5.2, golden);  // solar disc HDR bloom punch
        float disc_a = clamp(tex.a * sky.sun_obj_col.w * smoothstep(-0.02, 0.04, V.y), 0.0, 1.0);
        acc_premul = disc_rgb * disc_a + acc_premul * (1.0 - disc_a);
        acc_a = disc_a + acc_a * (1.0 - disc_a);
      }
    }

    // 4. Far high-altitude clouds (2 scrolling layers, SPEC-SKY-AND-TOD §3.5).
    if (sky.fog_sky.z > 0.5 && V.y > 0.005) {
      highp vec2 plane_uv = V.xz / (V.y + 0.14) * 0.22;
      float horizon_t = smoothstep(0.02, 0.30, V.y);

      // Back far layer
      highp vec2 uv_b = plane_uv + sky.far_scroll.zw;
      float mb1 = texture(tex_far_mask, uv_b * 1.7 + vec2(0.13, 0.27)).r;
      float mb2 = texture(tex_far_mask, uv_b * 0.85 - sky.far_scroll.zw * 0.5).r;
      float raw_b = texture(tex_far_back, uv_b).r * mb1 * mb2;
      float dens_b = clamp((raw_b - (1.0 - sky.far_params.w)) * 1.8, 0.0, 1.0);
      float far_exp = clamp(sky.far_thin.w * 0.03, 0.35, 3.0);
      float alpha_b = pow(dens_b, far_exp) *
                      mix(sky.far_back_thin.w, sky.far_back_dense.w, horizon_t) * horizon_t * 0.55;
      vec3 col_b = mix(sky.far_back_thin.rgb * sky.far_dense.w, sky.far_back_dense.rgb, dens_b) * sky_gain;

      // Front far layer
      highp vec2 uv_f = plane_uv * 1.15 + sky.far_scroll.xy;
      float mf1 = texture(tex_far_mask, uv_f * 1.5 + vec2(0.41, 0.19)).r;
      float mf2 = texture(tex_far_mask, uv_f * 0.75 + sky.far_scroll.xy * 0.4).r;
      float raw_f = texture(tex_far_front, uv_f).r * mf1 * mf2;
      float dens_f = clamp((raw_f - (1.0 - sky.far_params.z)) * 1.8, 0.0, 1.0);
      float alpha_f = pow(dens_f, far_exp) *
                      mix(sky.far_params.y, sky.far_params.x, horizon_t) * horizon_t * 0.65;
      vec3 col_f = mix(sky.far_thin.rgb * sky.far_dense.w, sky.far_dense.rgb, dens_f) * sky_gain;

      float far_a = clamp(alpha_f + alpha_b * (1.0 - alpha_f), 0.0, 1.0);
      vec3 far_premul = col_f * alpha_f + col_b * alpha_b * (1.0 - alpha_f);
      if (far_a > 0.001) {
        acc_premul = far_premul + acc_premul * (1.0 - far_a);
        acc_a = far_a + acc_a * (1.0 - far_a);
      }

      // 5. Close lit dome clouds (2 rotating bands, SPEC-SKY-AND-TOD §3.6).
      highp float az = atan(V.z, V.x) / kTwoPi + 0.5;
      // Clamp V slightly inside (0,1) so REPEAT sampler never wraps a bright/dark seam at the horizon.
      highp float elev_v = clamp(1.0 - (asin(clamp(V.y, 0.0, 1.0)) / (kPi * 0.5)), 0.002, 0.998);
      // Unwrap 1.0 <-> 0.0 azimuth wrap derivatives so 2x2 quads crossing -X (or fract wrap)
      // never spike implicit mip gradients into the coarsest mip (eliminates the 1-px sky seam).
      highp vec2 cuv_dx = vec2(fract(dFdx(az) + 0.5) - 0.5, dFdx(elev_v));
      highp vec2 cuv_dy = vec2(fract(dFdy(az) + 0.5) - 0.5, dFdy(elev_v));
      float sun_dot = clamp(dot(V, D), 0.0, 1.0);
      // Gentle half-Lambert wrap keeps clouds away from the sun softly lit at dawn/dusk.
      float sun_wrap = clamp(dot(V, D) * 0.5 + 0.5, 0.0, 1.0);
      // Hoist identical sun phase terms shared by front and back close cloud layers.
      float back_phase = pow(sun_dot, max(sky.close_back_col.w, 0.5));
      float top_phase = mix(0.32, 1.0, pow(sun_wrap, max(sky.close_top_col.w * 0.5, 0.5)));
      float top_mask_pow = max(sky.close_masks.x, 0.25);
      float bot_mask_pow = max(sky.close_masks.y, 0.25);
      float alpha_curve = clamp(sky.close_intens.w * 0.10, 0.45, 2.2);

      highp vec2 cuv_f = vec2(fract(az + sky.close_layers.z), elev_v);
      vec2 ad_f = textureGrad(tex_close_ad_f, cuv_f, cuv_dx, cuv_dy).rg;
      float am_f = textureGrad(tex_close_am_f, cuv_f, cuv_dx, cuv_dy).r;
      vec2 tb_f = textureGrad(tex_close_tb_f, cuv_f, cuv_dx, cuv_dy).rg;

      vec3 base_f = mix(sky.close_dense_col.rgb * sky.close_dense_col.w,
                        sky.close_thin_col.rgb * sky.close_thin_col.w, am_f);
      vec3 back_f = sky.close_back_col.rgb * sky.close_intens.x * back_phase * (1.0 - ad_f.g);
      vec3 top_f = sky.close_top_col.rgb * sky.close_intens.y * top_phase * pow(tb_f.r, top_mask_pow);
      vec3 bot_f = sky.close_bot_col.rgb * sky.close_intens.z *
                   mix(0.28, 1.0, pow(sun_wrap, max(sky.close_bot_col.w * 0.5, 0.5))) *
                   pow(tb_f.g, bot_mask_pow);
      vec3 lit_f = (base_f + back_f + top_f + bot_f) * sky_gain;
      float ca_f = pow(smoothstep(0.03, 0.96, ad_f.r), alpha_curve) * sky.close_layers.x;

      highp vec2 cuv_b = vec2(fract(az + sky.close_layers.w), elev_v);
      vec2 ad_b = textureGrad(tex_close_ad_b, cuv_b, cuv_dx, cuv_dy).rg;
      float am_b = textureGrad(tex_close_am_b, cuv_b, cuv_dx, cuv_dy).r;
      vec2 tb_b = textureGrad(tex_close_tb_b, cuv_b, cuv_dx, cuv_dy).rg;
      vec3 base_b = mix(sky.close_back_dense_col.rgb * sky.close_dense_col.w,
                        sky.close_back_thin_col.rgb * sky.close_thin_col.w, am_b);
      vec3 back_b = sky.close_back_col.rgb * sky.close_intens.x * 0.5 * back_phase * (1.0 - ad_b.g);
      vec3 top_b = sky.close_top_col.rgb * sky.close_intens.y * 0.5 * top_phase * pow(tb_b.r, top_mask_pow);
      vec3 bot_b = sky.close_back_bot_col.rgb * sky.close_back_thin_col.w *
                   mix(0.28, 1.0, pow(sun_wrap, max(sky.close_back_bot_col.w * 0.5, 0.5))) *
                   pow(tb_b.g, bot_mask_pow);
      vec3 lit_b = (base_b + back_b + top_b + bot_b) * sky_gain;
      float ca_b = pow(smoothstep(0.03, 0.96, ad_b.r), alpha_curve) * sky.close_layers.y;

      // Soft horizon dissolution so clouds melt into the horizon mist instead of hard-clipping.
      float h_fade = smoothstep(0.03, 0.22, V.y) *
                     mix(sky.close_masks.w, sky.close_masks.z, clamp(V.y * 2.5, 0.0, 1.0));
      float ca_f_h = clamp(ca_f * h_fade, 0.0, 1.0);
      float ca_b_h = clamp(ca_b * h_fade, 0.0, 1.0);
      float close_a = clamp(ca_f_h + ca_b_h * (1.0 - ca_f_h), 0.0, 1.0);
      vec3 close_premul = lit_f * ca_f_h + lit_b * ca_b_h * (1.0 - ca_f_h);
      if (close_a > 0.001) {
        acc_premul = close_premul + acc_premul * (1.0 - close_a);
        acc_a = close_a + acc_a * (1.0 - close_a);
      }
    }

    // 6. Sky horizon fog skirt (SPEC-SKY-AND-TOD §3.7).
    // Reaches full world fog opacity at and below V.y = 0, feathering smoothly up into the sky
    // so distant far-ring mountain silhouettes merge seamlessly with zero bright horizon seam.
    float f_start = sky.fog_color.w;
    float f_end = max(sky.fog_sky.x, f_start + 0.10);
    float ft = clamp((V.y - f_start) / (f_end - f_start), 0.0, 1.0);
    float below_h = 1.0 - smoothstep(-0.008, 0.015, V.y);
    float fog_a = clamp(max((1.0 - ft * ft * (3.0 - 2.0 * ft)) * sky.fog_sky.y, below_h), 0.0, 1.0);
    float scatter = pow(clamp(dot(V, normalize(sky.sun_dir.xyz)), 0.0, 1.0), max(sky.fog_scatter.w, 1.0));
    vec3 fog_rgb = mix(sky.fog_color.rgb, sky.fog_scatter.rgb, scatter);
    if (fog_a > 0.001) {
      acc_premul = fog_rgb * fog_a + acc_premul * (1.0 - fog_a);
      acc_a = fog_a + acc_a * (1.0 - fog_a);
    }
  }

  float sky_sat = clamp(sky.close_back_dense_col.w, 0.0, 2.0);
  if (mode == 2) {
    vec3 combined = acc_premul + base_lin_rgb * (1.0 - acc_a);
    float c_luma = dot(combined, vec3(0.2126, 0.7152, 0.0722));
    combined = max(mix(vec3(c_luma), combined, sky_sat), vec3(0.0));
    out_color = vec4(out_rgb(combined), 1.0);
    return;
  }
  vec3 straight_rgb = acc_a > 1e-4 ? (acc_premul / acc_a) : vec3(0.0);
  float acc_luma = dot(straight_rgb, vec3(0.2126, 0.7152, 0.0722));
  straight_rgb = max(mix(vec3(acc_luma), straight_rgb, sky_sat), vec3(0.0));
  vec3 enc = out_rgb(straight_rgb);
  out_color = vec4(enc * acc_a, acc_a);
}
