#version 450
// carview showroom: car shader with Paint Shop finishes (Normal, Metallic
// flake, Matte, Two-Tone Pearlescent, Chrome, Carbon, Wood, Camo), 1024^2
// composited LiveryMasks vinyl layer stack, window tint, cosmetic impact
// scratches/dents, cracked glass, and dynamic body/tyre dirt & mud.
// Spec: FORMAT-CARS (Materials, Paint, Brakes, Lights, glass and shadow).

layout(set = 0, binding = 0) uniform Frame {
  mat4 view_projection;
  vec4 eye;
  vec4 key_light;
  vec4 fill_light;
  vec4 options;
  vec4 tint_light;
  vec4 reflect_tint;
  mat4 transparent_view_projection;  // glass and lamp covers
  vec4 stripe;  // the factory stripe colour (linear rgb)
  mat4 shadow_matrix;  // the drive's sun shadow (world.frag's block)
  vec4 shadow;
  vec4 shadow_bias;
  vec4 world_fade;
  vec4 drive;  // xyz world up in the car frame, w 1 in the drive: sun and sky lighting
  vec4 sun_color;      // rgb dynamic sun colour, w shadow_intensity [0..1] (<0 uses legacy noon)
  vec4 sky_color;      // rgb dynamic HLTopColour, w switch_on_lights [0..1]
  vec4 ground_color;   // rgb dynamic HLBottomColour, w day_night_post [0..1]
  vec4 fog_color;      // rgb dynamic FogColour, w fog_start
  vec4 fog_scatter;    // rgb dynamic FogSunScatterColour, w fog_scatter_power
  vec4 fog_params;     // x fog_density_scale, y height_start, z height_end, w height_fade_amount
  vec4 car_light_top;  // rgb CarHLColourTop, w ibl_ambient_scale
  vec4 car_light_bot;  // rgb CarHLColourBottom, w eye_game_y
  mat4 shadow_matrix_far;  // Far cascade orthographic sun shadow matrix
  vec4 shadow_far_bias;    // x far normal bias, y far on (>0.5), z dyn env cube on (>0.5), w mirror on (>0.5)
  vec4 env_basis_x;        // xyz game X in render space, w real_time_sec
  vec4 env_basis_y;        // xyz game Y in render space
  vec4 env_basis_z;        // xyz game Z in render space
  vec4 paint_two_tone;     // rgb secondary linear color, w two_tone_power
  vec4 paint_finish;       // x finish (0..7), y two_tone_scale, z two_tone_bias, w window_tint (0..1)
  vec4 damage_dirt;        // x body_dirt, y tyre_dirt, z glass_crack, w scratch_amount
  vec4 damage_zones;       // x front, y rear, z left, w right (0..1)
  vec4 car_sun_color;      // rgb sun scaled by IBLDirectScaleCar, w ibl_direct_car (<0 fallback)
  mat4 shadow_matrix_mid;  // Mid cascade orthographic sun shadow matrix (~90m)
  vec4 shadow_mid_bias;    // x mid bias, y 3-cascade (>0.5), z pcss (0/1/2), w cascades (2/3)
  vec4 phase_b_params;     // x car_detail, y rotor_glow, z frame_jitter, w ssr_active
  vec4 ambient_color;      // rgb TimeOfDayA AmbientColour, w ibl_ambient_scale
  vec4 cockpit_mfd;        // x speed_norm, y tach_norm, z fuel_norm, w boost_norm
  vec4 cockpit_mfd_ext;    // x gear (-1=R,0=N,1..8), y speed_display, z flags, w headlight_amt
  vec4 damage_corners;     // x corner_fl, y corner_fr, z corner_rl, w corner_rr
  vec4 paint_shader_a;     // x fresnel_index, y fresnel_scalar, z spec_cube_scalar, w spec_amount
  vec4 paint_shader_b;     // x spec_amount2, y spec_power2, z spec_amount3, w spec_power3
  vec4 paint_shader_c;     // xyz metal_color, w metal_power
  vec4 paint_shader_d;     // x metal_bias, y metal_scale, z spec_power, w gauge_emissive_scale
  vec4 region_two_tone[3]; // 0=hood, 1=wing, 2=mirrors: rgb secondary linear, w two_tone_power
  vec4 region_finish[3];   // 0=hood, 1=wing, 2=mirrors: x finish, y tt_scale, z tt_bias, w active
  vec4 rim_shader;         // x fresnel_index, y fresnel_scalar, z cube_scale, w has_override
  vec4 rim_tint_dirt;      // xyz rim_tint, w bit0=has_car_dirt_mask bit1=has_car_crush_weights
} frame;
layout(set = 0, binding = 1) uniform samplerCube environment;
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
layout(set = 0, binding = 5) uniform sampler2D mirror_map;
struct CarInstanceGpu {
  mat4 model;
  vec4 stripe;
  vec4 lamp_rgb[30];
  vec4 pad;
};
layout(std430, set = 0, binding = 6) readonly buffer CarInstances { CarInstanceGpu car_instances[]; };
layout(set = 0, binding = 7) uniform samplerCube static_environment;
layout(set = 0, binding = 8) uniform sampler2D shared_dirt_pattern;
layout(set = 0, binding = 9) uniform sampler2D shared_dirt_mask;
layout(set = 0, binding = 10) uniform sampler2D shared_glass_crack;
layout(set = 0, binding = 11) uniform sampler2D shadow_depth_raw;

layout(set = 1, binding = 0) uniform sampler2D slot0;
layout(set = 1, binding = 1) uniform sampler2D slot1;
layout(set = 1, binding = 2) uniform sampler2D livery;
layout(set = 1, binding = 3) uniform sampler2D normal_map;
layout(set = 1, binding = 4) uniform sampler2D damage_map;

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 uv0;
  vec4 uv1;
  vec4 tint;
  vec4 params;
} draw;

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in float v_occlusion;
layout(location = 5) flat in uint v_instance;
layout(location = 6) in vec3 v_tangent;
layout(location = 7) in vec3 v_bitangent;
layout(location = 8) in vec3 v_local_pos;
layout(location = 9) in vec3 v_hub_radial;

layout(location = 0) out vec4 out_color;

const uint kOpaque = 0u, kPaint = 1u, kGlass = 2u, kLamp = 3u, kInterior = 4u, kGauge = 5u,
           kTyre = 6u, kRim = 7u, kCaliper = 8u, kChrome = 9u, kCarbon = 10u, kWorld = 11u;
const uint kFlagSlot1 = 1u, kFlagMetallic = 2u, kFlagBlurRim = 4u, kFlagLivery = 8u, kFlagNoSlot0 = 16u, kFlagStripe = 32u, kFlagFlat = 64u,
           kFlagRaceLivery = 128u, kFlagGrille = 256u, kFlagCutout = 512u, kFlagCover = 1024u,
           kFlagLampEmissive = 2048u, kFlagMirror = 4096u, kFlagInstanced = 8192u,
           kFlagNormalMap = 524288u, kFlagNormalUv1 = 1048576u, kFlagDamageMap = 2097152u,
           kFlagCarbonTex = 4194304u, kFlagHasBlurMesh = 8388608u;

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

layout(constant_id = 0) const bool kLinearOut = false;
vec3 filmic_showroom(vec3 x) {
  // Soft Hable shoulder for standalone UNORM showroom (drive.w < 0.5) so HDR
  // staticcarcubemap.xpr 80-97 studio strips roll cleanly without clipping white paint.
  const float A = 0.22, B = 0.30, C = 0.10, D = 0.20, E = 0.01, F = 0.30;
  vec3 num = (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
  float w = 8.0;
  float den = (w * (A * w + C * B) + D * E) / (w * (A * w + B) + D * F) - E / F;
  return clamp(num / den, vec3(0.0), vec3(1.0));
}
vec3 out_rgb(vec3 c) {
  if (kLinearOut) return min(max(c, vec3(0.0)), vec3(64.0));
  if (frame.drive.w < 0.5) return encode_srgb(filmic_showroom(max(c, vec3(0.0)) * 1.65));
  return encode_srgb(c);
}

const vec3 kSunColour = vec3(0.9695, 0.8358, 0.584);
const vec3 kSkyColour = vec3(0.1687, 0.2229, 0.2746);
const vec3 kGroundColour = vec3(0.1788, 0.1729, 0.1024);
const vec3 kSkyHorizon = vec3(0.42, 0.56, 0.72);
const vec3 kSkyZenith = vec3(0.05, 0.16, 0.50);
const vec3 kDirtReflect = vec3(0.50, 0.29, 0.15);

float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

float vnoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash21(i), hash21(i + vec2(1.0, 0.0)), u.x),
             mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 4; ++i) {
    v += a * vnoise(p);
    p *= 2.07;
    a *= 0.5;
  }
  return v;
}

vec2 hash22(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.xx + p3.yz) * p3.zy);
}

// Isotropic GGX normal distribution function.
float ggx_d(float nh, float alpha) {
  float a2 = alpha * alpha;
  float denom = nh * nh * (a2 - 1.0) + 1.0;
  return a2 / max(3.14159265 * denom * denom, 1e-5);
}

// Burley / Heitz Anisotropic GGX NDF for radial brushed-iron brake rotors.
float aniso_ggx_d(vec3 n, vec3 h, vec3 t, vec3 b, float at, float ab) {
  float th = dot(t, h);
  float bh = dot(b, h);
  float nh = max(dot(n, h), 0.0);
  float a2 = at * ab;
  vec3 d = vec3(ab * th, at * bh, a2 * nh);
  float d2 = dot(d, d);
  float s = a2 / max(d2, 1e-6);
  return (a2 * s * s) / 3.14159265;
}

// Multiscale anti-aliased stochastic metallic flake glint (Deliot & Belcour 2023):
// projects pixel footprint into continuous pyramid mip levels so flakes sparkle crisply up close
// and converge smoothly into broad metallic GGX at speed/distance (zero shimmer at 150+ mph).
float eval_multiscale_flake(vec3 local_pos, vec3 n, vec3 h, vec3 T, vec3 B, float freq, float detail_on) {
  // Triplanar dominant-plane projection in car-local space (locked to body panels, never swims).
  vec3 an = abs(n);
  vec2 fuv = (an.y >= an.x && an.y >= an.z) ? local_pos.xz
           : (an.x >= an.z)                 ? local_pos.zy
                                            : local_pos.xy;
  vec2 duv = max(abs(dFdx(fuv)), abs(dFdy(fuv)));
  float fp = max(duv.x, duv.y);
  float lod = log2(max(fp * freq, 1.0));
  // Smooth convergence to zero discrete shimmer when pixel footprint spans many flakes.
  float vis = 1.0 - smoothstep(0.25, 3.2, lod);
  if (vis <= 0.002) return 0.0;

  float l0 = floor(clamp(lod, 0.0, 3.0));
  float l1 = min(l0 + 1.0, 3.0);
  float frac_l = clamp(lod - l0, 0.0, 1.0);

  float s0 = freq / exp2(l0);
  float s1 = freq / exp2(l1);
  vec2 h0 = hash22(floor(fuv * s0)) * 2.0 - 1.0;
  vec3 nf0 = normalize(n + (T * h0.x + B * h0.y) * 0.22);
  float g0 = min(ggx_d(max(dot(nf0, h), 0.0), 0.24) * 0.016, 1.25);
  if (detail_on < 0.5) return g0 * vis;
  vec2 h1 = hash22(floor(fuv * s1) + vec2(37.1, 91.7)) * 2.0 - 1.0;
  vec3 nf1 = normalize(n + (T * h1.x + B * h1.y) * 0.18);
  float g1 = min(ggx_d(max(dot(nf1, h), 0.0), 0.28) * 0.016, 1.15);
  return mix(g0, g1, frac_l) * vis;
}

// Planckian blackbody thermal ramp (900 K -> 1450 K) for glowing brake rotors under hard braking.
vec3 blackbody_rotor_rgb(float heat) {
  float h = clamp(heat, 0.0, 1.0);
  if (h <= 0.02) return vec3(0.0);
  float t = (h - 0.02) / 0.98;
  float radiance = t * t * (kLinearOut ? 11.5 : 3.2);
  vec3 tint = vec3(clamp(0.85 + 0.15 * t, 0.0, 1.0),
                   clamp(0.06 + 0.54 * t * t, 0.0, 1.0),
                   clamp(0.01 + 0.22 * t * t * t, 0.0, 1.0));
  return tint * radiance;
}

vec3 drive_env(vec3 dir, float lod) {
  vec3 up = normalize(frame.drive.xyz), sun = normalize(frame.key_light.xyz);
  float rough = lod / 7.0;
  float h = dot(dir, up);
  float sun_vis = frame.sun_color.w >= 0.0 ? smoothstep(-0.08, 0.14, dot(sun, up)) : 1.0;
  vec3 sun_col = frame.sun_color.w >= 0.0 ? frame.sun_color.rgb : kSunColour;
  vec3 hor_col = frame.sun_color.w >= 0.0 ? max(frame.fog_color.rgb * 2.0, frame.sky_color.rgb * 1.2) : kSkyHorizon;
  vec3 zen_col = frame.sun_color.w >= 0.0 ? frame.sky_color.rgb * 0.75 : kSkyZenith;
  vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.ground_color.rgb * 1.6 : kDirtReflect;
  vec3 sky = mix(hor_col, zen_col, pow(clamp(h, 0.0, 1.0), 0.55));
  float toward = max(dot(dir, sun), 0.0);
  vec3 haze_c = frame.sun_color.w >= 0.0 ? frame.fog_scatter.rgb : vec3(0.80, 0.70, 0.55);
  sky = mix(sky, haze_c, pow(toward, 6.0) * (1.0 - clamp(h, 0.0, 1.0)) * 0.55 * sun_vis);
  sky += sun_col * pow(toward, mix(48.0, 6.0, rough)) * mix(0.9, 0.25, rough) * sun_vis;
  float edge = mix(0.02, 0.5, rough);
  return mix(gnd_col * (0.85 + 0.15 * clamp(-h * 3.0, 0.0, 1.0)), sky, smoothstep(-edge, edge, h));
}

const vec2 kPcssDisk[16] = vec2[16](
    vec2(-0.613392,  0.617481), vec2( 0.170019, -0.040254),
    vec2(-0.299417,  0.791925), vec2( 0.645680,  0.493210),
    vec2(-0.651784,  0.717887), vec2( 0.421003,  0.027070),
    vec2(-0.817194, -0.271096), vec2(-0.705374, -0.668203),
    vec2( 0.977050, -0.108615), vec2( 0.063326,  0.142369),
    vec2( 0.203528,  0.214331), vec2(-0.667531,  0.326090),
    vec2(-0.098422, -0.295755), vec2(-0.885922,  0.215369),
    vec2( 0.566637,  0.605213), vec2( 0.039766, -0.396100));

float ign_shadow() {
  return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

// Receiver-plane depth gradient (dZ/dU, dZ/dV) computed in uniform control flow so wide PCSS
// and PCF Poisson kernels track curved car body panels (decklids, fenders) without acne striping.
vec2 receiver_plane_depth_gradient(vec2 uv, float z) {
  vec3 dsdx = dFdx(vec3(uv, z));
  vec3 dsdy = dFdy(vec3(uv, z));
  float det = dsdx.x * dsdy.y - dsdy.x * dsdx.y;
  if (abs(det) <= 1e-10) return vec2(0.0);
  float inv_det = 1.0 / det;
  vec2 g = vec2(dsdy.y * dsdx.z - dsdx.y * dsdy.z,
                dsdx.x * dsdy.z - dsdy.x * dsdx.z) * inv_det;
  return clamp(g, vec2(-0.5), vec2(0.5));
}

float sample_cascade_pcf(vec4 s, float x_off, float inv_cascades, vec2 dz_duv) {
  vec2 uv = vec2((s.x * 0.5 + 0.5) * inv_cascades + x_off, s.y * 0.5 + 0.5);
  float tx = frame.shadow.y * inv_cascades * 0.5;
  float ty = frame.shadow.y * 0.5;
  float u_lo = x_off + frame.shadow.y * inv_cascades;
  float u_hi = x_off + inv_cascades - frame.shadow.y * inv_cascades;
  const vec2 offs[4] = vec2[4](vec2(-tx, -ty), vec2(tx, -ty), vec2(-tx, ty), vec2(tx, ty));
  float lit = 0.0;
  for (int i = 0; i < 4; ++i) {
    vec2 suv = vec2(clamp(uv.x + offs[i].x, u_lo, u_hi), clamp(uv.y + offs[i].y, 0.0005, 0.9995));
    float tz = s.z + clamp(dot(dz_duv, suv - uv), -0.004, 0.004);
    lit += texture(shadow_map, vec3(suv, tz));
  }
  return 0.25 * lit;
}

float sample_cascade_pcss(vec4 s, float x_off, float inv_cascades, float pcss_mode, float cascade_scale, vec2 dz_duv) {
  if (pcss_mode < 0.5) return sample_cascade_pcf(s, x_off, inv_cascades, dz_duv);
  vec2 uv = vec2((s.x * 0.5 + 0.5) * inv_cascades + x_off, s.y * 0.5 + 0.5);
  float texel_x = frame.shadow.y * inv_cascades;
  float texel_y = frame.shadow.y;
  float u_lo = x_off + texel_x;
  float u_hi = x_off + inv_cascades - texel_x;
  float ang = 6.2831853 * fract(ign_shadow() + frame.phase_b_params.z * 0.61803399);
  float ca = cos(ang), sa = sin(ang);
  mat2 rot = mat2(ca, -sa, sa, ca);

  float search_r = (pcss_mode > 1.5 ? 5.2 : 4.0) * cascade_scale;
  vec2 search_uv = vec2(texel_x, texel_y) * search_r;
  float blocker_sum = 0.0;
  int blockers = 0;
  vec2 cuv = vec2(clamp(uv.x, u_lo, u_hi), clamp(uv.y, 0.0005, 0.9995));
  float d_center = texture(shadow_depth_raw, cuv).r;
  if (d_center < s.z - 0.00024) {
    blocker_sum += (s.z - d_center);
    ++blockers;
  }
  for (int i = 0; i < 8; ++i) {
    vec2 o = (rot * kPcssDisk[i * 2]) * search_uv;
    vec2 suv = vec2(clamp(uv.x + o.x, u_lo, u_hi), clamp(uv.y + o.y, 0.0005, 0.9995));
    float ref_z = s.z + clamp(dot(dz_duv, suv - uv), -0.004, 0.004);
    float d = texture(shadow_depth_raw, suv).r;
    if (d < ref_z - 0.00028) {
      blocker_sum += (ref_z - d);
      ++blockers;
    }
  }
  if (blockers == 0) {
    return texture(shadow_map, vec3(cuv, s.z));
  }
  float avg_dz = blocker_sum / float(blockers);
  float blocker_dist_m = max(avg_dz, 0.0) * 1040.0;
  float penumbra = clamp(blocker_dist_m * 0.52 * cascade_scale, 0.75, pcss_mode > 1.5 ? 6.5 : 5.0);
  vec2 filter_uv = vec2(texel_x, texel_y) * penumbra;
  int taps = pcss_mode > 1.5 ? 16 : 12;
  float lit = 0.0;
  for (int i = 0; i < taps; ++i) {
    vec2 o = (rot * kPcssDisk[i]) * filter_uv;
    vec2 suv = vec2(clamp(uv.x + o.x, u_lo, u_hi), clamp(uv.y + o.y, 0.0005, 0.9995));
    float tap_z = s.z + clamp(dot(dz_duv, suv - uv), -0.004, 0.004);
    lit += texture(shadow_map, vec3(suv, tap_z));
  }
  return lit / float(taps);
}

vec3 decode_shadow_color() {
  uint bits = floatBitsToUint(frame.shadow_bias.w);
  if ((bits >> 24u) != 0x3Fu) return vec3(1.0);
  return vec3(float(bits & 255u), float((bits >> 8u) & 255u), float((bits >> 16u) & 255u)) * (1.0 / 255.0);
}

vec3 shadow_ambient_tint(float occluded) {
  if (occluded <= 0.001) return vec3(1.0);
  vec3 sc = decode_shadow_color();
  float luma = dot(sc, vec3(0.2126, 0.7152, 0.0722));
  vec3 chroma = luma > 0.01 ? clamp(sc / luma, vec3(0.65), vec3(1.35)) : vec3(1.0);
  float depth = mix(0.76, 1.0, clamp(luma, 0.0, 1.0));
  return mix(vec3(1.0), chroma * depth, clamp(occluded, 0.0, 1.0));
}

float drive_shadow(vec3 p, vec3 n) {
  if (frame.shadow.x < 0.5) return 1.0;
  bool three_c = frame.shadow_mid_bias.y > 0.5;
  float inv_c = three_c ? (1.0 / 3.0) : 0.5;
  float pcss_mode = frame.shadow_mid_bias.z;

  // Slope-scaled normal offset + uniform-flow receiver-plane gradients prevent curved-panel
  // PCSS shadow-acne striping (e.g., R34 rear trunk lid under spoiler) while keeping crisp contact shadows.
  vec3 sun_dir = normalize(frame.key_light.xyz);
  float slope_mul = 2.1 + 2.5 * (1.0 - max(dot(n, sun_dir), 0.0));

  vec4 sn = frame.shadow_matrix * vec4(p + n * (frame.shadow_bias.x * slope_mul), 1.0);
  vec2 uvn = vec2((sn.x * 0.5 + 0.5) * inv_c, sn.y * 0.5 + 0.5);
  vec2 g_near = receiver_plane_depth_gradient(uvn, sn.z);

  vec4 sm = frame.shadow_matrix_mid * vec4(p + n * (frame.shadow_mid_bias.x * slope_mul), 1.0);
  vec2 uvm = vec2((sm.x * 0.5 + 0.5) * inv_c + inv_c, sm.y * 0.5 + 0.5);
  vec2 g_mid = receiver_plane_depth_gradient(uvm, sm.z);

  float far_off = three_c ? (2.0 * inv_c) : 0.5;
  vec4 sf = frame.shadow_matrix_far * vec4(p + n * (frame.shadow_far_bias.x * slope_mul), 1.0);
  vec2 uvf = vec2((sf.x * 0.5 + 0.5) * inv_c + far_off, sf.y * 0.5 + 0.5);
  vec2 g_far = receiver_plane_depth_gradient(uvf, sf.z);

  float edge_n = max(abs(sn.x), abs(sn.y));
  bool near_ok = edge_n < 1.0 && sn.z > 0.0 && sn.z < 1.0;
  float lit_n = near_ok ? sample_cascade_pcss(sn, 0.0, inv_c, pcss_mode, 1.0, g_near) : 1.0;

  if (three_c) {
    if (near_ok && edge_n < 0.80) return clamp(lit_n, 0.0, 1.0);
    float edge_m = max(abs(sm.x), abs(sm.y));
    bool mid_ok = edge_m < 1.0 && sm.z > 0.0 && sm.z < 1.0;
    float lit_m = mid_ok ? sample_cascade_pcss(sm, inv_c, inv_c, pcss_mode, 0.35, g_mid) : 1.0;
    if (near_ok) {
      float bn = smoothstep(0.80, 0.98, edge_n);
      return clamp(mix(lit_n, lit_m, bn), 0.0, 1.0);
    }
    if (mid_ok && edge_m < 0.80) return clamp(lit_m, 0.0, 1.0);
    float edge_f = max(abs(sf.x), abs(sf.y));
    float lit_f = 1.0;
    if (edge_f < 1.0 && sf.z > 0.0 && sf.z < 1.0) {
      lit_f = mix(sample_cascade_pcf(sf, 2.0 * inv_c, inv_c, g_far), 1.0, smoothstep(0.84, 1.0, edge_f));
    }
    float bm = mid_ok ? smoothstep(0.80, 0.98, edge_m) : 1.0;
    return clamp(mix(lit_m, lit_f, bm), 0.0, 1.0);
  }

  if (frame.shadow_far_bias.y > 0.5) {
    if (near_ok && edge_n < 0.78) return clamp(lit_n, 0.0, 1.0);
    float edge_f = max(abs(sf.x), abs(sf.y));
    float lit_f = 1.0;
    if (edge_f < 1.0 && sf.z > 0.0 && sf.z < 1.0) {
      lit_f = mix(sample_cascade_pcf(sf, 0.5, 0.5, g_far), 1.0, smoothstep(0.82, 1.0, edge_f));
    }
    float blend = near_ok ? smoothstep(0.78, 0.98, edge_n) : 1.0;
    return clamp(mix(lit_n, lit_f, blend), 0.0, 1.0);
  }
  if (!near_ok) return 1.0;
  return clamp(mix(lit_n, 1.0, smoothstep(0.8, 1.0, edge_n)), 0.0, 1.0);
}

vec3 env(vec3 dir, float lod) {
  if (frame.drive.w > 0.5) {
    if (frame.shadow_far_bias.z > 0.5) {
      vec3 gdir = normalize(vec3(dot(dir, frame.env_basis_x.xyz), dot(dir, frame.env_basis_y.xyz),
                                 dot(dir, frame.env_basis_z.xyz)));
      return textureLod(environment, gdir, lod).rgb;
    }
    return drive_env(dir, lod);
  }
  return textureLod(static_environment, dir, lod).rgb * frame.options.y;
}

// Crisp procedural 7-segment digit evaluator (0..9, 10='N', 11='r') in local cell p in [0,1]^2.
float mfd_seg_h(vec2 p, vec2 c, float hw, float hh) {
  vec2 d = abs(p - c);
  return (1.0 - smoothstep(hh * 0.45, hh, d.y)) * (1.0 - smoothstep(hw * 0.72, hw, d.x + d.y * 0.75));
}
float mfd_seg_v(vec2 p, vec2 c, float hw, float hh) {
  vec2 d = abs(p - c);
  return (1.0 - smoothstep(hw * 0.45, hw, d.x)) * (1.0 - smoothstep(hh * 0.72, hh, d.y + d.x * 0.75));
}
float mfd_digit(vec2 p, int digit) {
  // Bit order: 0=top, 1=tr, 2=br, 3=bot, 4=bl, 5=tl, 6=mid
  const int kMask[12] = int[12](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x37, 0x50);
  int m = kMask[clamp(digit, 0, 11)];
  float v = 0.0;
  if ((m & 0x01) != 0) v = max(v, mfd_seg_h(p, vec2(0.50, 0.88), 0.34, 0.075));
  if ((m & 0x02) != 0) v = max(v, mfd_seg_v(p, vec2(0.84, 0.69), 0.09, 0.21));
  if ((m & 0x04) != 0) v = max(v, mfd_seg_v(p, vec2(0.84, 0.31), 0.09, 0.21));
  if ((m & 0x08) != 0) v = max(v, mfd_seg_h(p, vec2(0.50, 0.12), 0.34, 0.075));
  if ((m & 0x10) != 0) v = max(v, mfd_seg_v(p, vec2(0.16, 0.31), 0.09, 0.21));
  if ((m & 0x20) != 0) v = max(v, mfd_seg_v(p, vec2(0.16, 0.69), 0.09, 0.21));
  if ((m & 0x40) != 0) v = max(v, mfd_seg_h(p, vec2(0.50, 0.50), 0.34, 0.075));
  return v;
}

vec3 eval_cockpit_mfd(vec2 muv) {
  float spd_n = clamp(frame.cockpit_mfd.x, 0.0, 1.0);
  float rpm_n = clamp(frame.cockpit_mfd.y, 0.08, 1.0);
  float fuel_n = clamp(frame.cockpit_mfd.z, 0.05, 1.0);
  float boost_n = clamp(frame.cockpit_mfd.w, 0.0, 1.0);
  uint mfd_flags = uint(max(frame.cockpit_mfd_ext.z, 0.0) + 0.5);
  bool has_live = (mfd_flags & 256u) != 0u;
  bool is_metric = !has_live || ((mfd_flags & 1u) != 0u);
  bool warn_abs = (mfd_flags & 2u) != 0u;
  bool warn_tcs = (mfd_flags & 4u) != 0u;
  bool warn_hb = (mfd_flags & 8u) != 0u;
  bool ind_highbeam = (mfd_flags & 16u) != 0u;
  bool ind_turn_l = (mfd_flags & 32u) != 0u;
  bool ind_turn_r = (mfd_flags & 64u) != 0u;
  bool shift_led = (mfd_flags & 128u) != 0u;

  // Dark sapphire-charcoal backlit OLED background with subtle radial vignette & horizontal scanlines.
  vec2 dc = muv - vec2(0.5);
  float bg_glow = exp(-dot(dc * vec2(1.4, 2.0), dc * vec2(1.4, 2.0)) * 2.2);
  float scan = 0.92 + 0.08 * sin(muv.y * 140.0);
  vec3 col = mix(vec3(0.012, 0.020, 0.034), vec3(0.032, 0.068, 0.115), bg_glow) * scan;

  // Thin backlit outer frame border & header dividers.
  float bx = min(muv.x, 1.0 - muv.x);
  float by = min(muv.y, 1.0 - muv.y);
  float frame_line = (1.0 - smoothstep(0.012, 0.028, min(bx, by))) * step(0.006, min(bx, by));
  col += vec3(0.10, 0.34, 0.52) * frame_line * 0.65;
  float div_top = (1.0 - smoothstep(0.004, 0.014, abs(muv.y - 0.72))) * step(0.05, muv.x) * step(muv.x, 0.95);
  float div_bot = (1.0 - smoothstep(0.004, 0.014, abs(muv.y - 0.24))) * step(0.05, muv.x) * step(muv.x, 0.95);
  col += vec3(0.12, 0.32, 0.46) * max(div_top, div_bot) * 0.55;

  // Top zone (y in [0.76, 0.92]): 16-segment LED tachometer & pulsing shift-light bar,
  // flanked by left/right green turn-signal arrows.
  if (muv.y > 0.76 && muv.y < 0.91) {
    if (muv.x > 0.04 && muv.x < 0.11) {
      vec2 ap = vec2((muv.x - 0.04) / 0.07, (muv.y - 0.76) / 0.15);
      float tri = step(abs(ap.y - 0.5) * 1.4, ap.x);
      col += ind_turn_l ? vec3(0.12, 1.15, 0.26) * tri * 1.1 : vec3(0.02, 0.05, 0.03) * tri;
    } else if (muv.x > 0.89 && muv.x < 0.96) {
      vec2 ap = vec2((muv.x - 0.89) / 0.07, (muv.y - 0.76) / 0.15);
      float tri = step(abs(ap.y - 0.5) * 1.4, 1.0 - ap.x);
      col += ind_turn_r ? vec3(0.12, 1.15, 0.26) * tri * 1.1 : vec3(0.02, 0.05, 0.03) * tri;
    } else if (muv.x > 0.12 && muv.x < 0.88) {
      float tx = (muv.x - 0.12) / 0.76;
      float seg_f = tx * 16.0;
      float seg_idx = floor(seg_f);
      float seg_loc = fract(seg_f);
      float in_seg = step(0.12, seg_loc) * step(seg_loc, 0.88);
      float seg_t = (seg_idx + 0.5) / 16.0;
      float seg_h = mix(0.38, 1.0, seg_t);
      float ty = (muv.y - 0.76) / 0.15;
      if (in_seg > 0.5 && ty >= 0.0 && ty <= seg_h) {
        bool lit = seg_t <= max(rpm_n, 0.12);
        vec3 seg_col = seg_t > 0.84 ? vec3(1.15, 0.14, 0.08)
                       : (seg_t > 0.68 ? vec3(1.05, 0.58, 0.06) : vec3(0.24, 0.86, 1.08));
        if (shift_led) {
          float pulse = 0.65 + 0.35 * sin(frame.env_basis_x.w * 28.0);
          seg_col = mix(seg_col, vec3(1.25, 0.12, 0.10), 0.75 * pulse);
          lit = true;
        }
        col += lit ? seg_col * 0.95 : vec3(0.04, 0.08, 0.11);
      }
    }
  }

  // Center-left zone: 3-digit 7-segment authentic speed readout (km/h or MPH per user unit setting).
  int spd_val = has_live
                    ? int(clamp(floor(frame.cockpit_mfd_ext.y + 0.5), 0.0, 499.0))
                    : int(clamp(floor(spd_n * 260.0 + 0.5), 0.0, 499.0));
  int d_h = (spd_val / 100) % 10;
  int d_t = (spd_val / 10) % 10;
  int d_o = spd_val % 10;
  if (muv.y > 0.28 && muv.y < 0.68 && muv.x > 0.08 && muv.x < 0.65) {
    float ly = (muv.y - 0.32) / 0.34;
    if (ly >= 0.0 && ly <= 1.0) {
      float dig = 0.0;
      float ghost = 0.0;
      if (muv.x >= 0.10 && muv.x <= 0.26) {
        vec2 dp = vec2((muv.x - 0.10) / 0.16, ly);
        ghost = mfd_digit(dp, 8);
        if (d_h > 0) dig = mfd_digit(dp, d_h);
      } else if (muv.x >= 0.28 && muv.x <= 0.44) {
        vec2 dp = vec2((muv.x - 0.28) / 0.16, ly);
        ghost = mfd_digit(dp, 8);
        if (d_h > 0 || d_t > 0) dig = mfd_digit(dp, d_t);
      } else if (muv.x >= 0.46 && muv.x <= 0.62) {
        vec2 dp = vec2((muv.x - 0.46) / 0.16, ly);
        ghost = mfd_digit(dp, 8);
        dig = mfd_digit(dp, d_o);
      }
      col += vec3(0.025, 0.05, 0.07) * ghost;
      col += vec3(0.76, 0.94, 1.05) * dig * 1.15;
    } else if (muv.y > 0.285 && muv.y < 0.312 && muv.x >= 0.44 && muv.x <= 0.62) {
      // Subtle unit bar indicator (cool cyan = KM/H, warm amber-white = MPH).
      col += is_metric ? vec3(0.22, 0.72, 0.95) * 0.75 : vec3(0.95, 0.74, 0.36) * 0.75;
    }
  }

  // Center-right zone: framed real gear badge (r / N / 1..8 from live vehicle.gear()).
  if (muv.y > 0.28 && muv.y < 0.68 && muv.x > 0.70 && muv.x < 0.92) {
    vec2 gp = vec2((muv.x - 0.70) / 0.22, (muv.y - 0.28) / 0.40);
    float g_box = 1.0 - smoothstep(0.04, 0.09, min(min(gp.x, 1.0 - gp.x), min(gp.y, 1.0 - gp.y)));
    vec3 box_col = shift_led ? vec3(1.15, 0.12, 0.08) : vec3(0.85, 0.20, 0.08);
    col += box_col * g_box * 0.55;
    int raw_gear = has_live ? int(round(frame.cockpit_mfd_ext.x)) : 0;
    int gear_glyph = (raw_gear < 0) ? 11 : (raw_gear == 0 ? 10 : clamp(raw_gear, 1, 9));
    vec2 dp = clamp((gp - vec2(0.16, 0.12)) / vec2(0.68, 0.76), vec2(0.0), vec2(1.0));
    if (gp.x > 0.16 && gp.x < 0.84 && gp.y > 0.12 && gp.y < 0.88) {
      float gd = mfd_digit(dp, gear_glyph);
      vec3 gcol = (raw_gear < 0) ? vec3(1.10, 0.82, 0.18) : vec3(1.15, 0.34, 0.10);
      col += gcol * gd * 1.25;
    }
  }

  // Bottom zone (y in [0.06, 0.20]): fuel/boost bars + active warning lamps (ABS, TCS, Handbrake, HighBeam).
  if (muv.y > 0.13 && muv.y < 0.20) {
    if (muv.x > 0.08 && muv.x < 0.46) {
      float fx = (muv.x - 0.08) / 0.38;
      col += fx <= fuel_n ? vec3(0.18, 0.78, 0.42) * 0.75 : vec3(0.03, 0.06, 0.08);
    } else if (muv.x > 0.54 && muv.x < 0.92) {
      float px = (muv.x - 0.54) / 0.38;
      float bar = max(boost_n, rpm_n * 0.75);
      col += px <= bar ? vec3(0.98, 0.62, 0.14) * 0.80 : vec3(0.03, 0.06, 0.08);
    }
  } else if (muv.y > 0.05 && muv.y < 0.11) {
    // 4 compact warning telltales: ABS (amber), TCS (amber), Handbrake (red), High Beam (blue).
    if (muv.x > 0.10 && muv.x < 0.26) {
      col += warn_abs ? vec3(1.15, 0.72, 0.08) * 0.95 : vec3(0.03, 0.04, 0.05);
    } else if (muv.x > 0.32 && muv.x < 0.48) {
      col += warn_tcs ? vec3(1.15, 0.66, 0.06) * 0.95 : vec3(0.03, 0.04, 0.05);
    } else if (muv.x > 0.54 && muv.x < 0.70) {
      col += warn_hb ? vec3(1.20, 0.12, 0.10) * 1.05 : vec3(0.03, 0.04, 0.05);
    } else if (muv.x > 0.76 && muv.x < 0.92) {
      col += ind_highbeam ? vec3(0.18, 0.58, 1.35) * 1.05 : vec3(0.03, 0.04, 0.05);
    }
  }
  return col;
}

void main() {
  uint klass = uint(round(draw.params.x));
  // Round-to-nearest without +0.5 so odd flag masks >= 2^23 (kFlagHasBlurMesh | kFlagSlot1)
  // decode bit-for-bit identically without round-to-even bit corruption.
  uint flags = uint(round(draw.params.y));
  vec4 draw_tint = draw.tint;
  vec3 stripe_rgb = frame.stripe.rgb;
  uint submat = uint(max(draw.params.z, 0.0) + 0.5);
  if ((flags & kFlagInstanced) != 0u) {
    stripe_rgb = car_instances[v_instance].stripe.rgb;
    uint ch = (flags >> 14u) & 31u;
    if (ch > 0u && ch < 30u) {
      vec3 lrgb = car_instances[v_instance].lamp_rgb[ch].rgb;
      if (max(lrgb.r, max(lrgb.g, lrgb.b)) > 1e-4) {
        draw_tint.rgb = lrgb;
        flags |= kFlagLampEmissive;
      }
    }
  }
  vec4 t0 = (flags & kFlagNoSlot0) != 0u ? vec4(1.0, 1.0, 1.0, 0.0) : texture(slot0, v_uv0);
  vec4 t1 = vec4(1.0);
  if ((flags & kFlagSlot1) != 0u) {
    vec2 uv1_s = (klass == kCarbon) ? (v_uv1 * 1.5) : v_uv0;
    t1 = texture(slot1, uv1_s);
  }

  vec3 view = normalize(frame.eye.xyz - v_position);
  float eye_dist = length(frame.eye.xyz - v_position);
  vec3 n_geom = normalize(v_normal);
  float flip_n = dot(n_geom, view) < 0.0 ? -1.0 : 1.0;
  vec3 n = n_geom * flip_n;
  // Construct orthonormal tangent frame for normal maps, multiscale flakes, and close-up clearcoat orange-peel.
  vec3 T_frame = v_tangent;
  vec3 B_frame = v_bitangent;
  if (length(T_frame) > 1e-4 && length(B_frame) > 1e-4) {
    T_frame = normalize(T_frame - n * dot(T_frame, n));
    B_frame = normalize(B_frame - n * dot(B_frame, n));
  } else {
    vec3 up_ref = abs(n.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    T_frame = normalize(cross(up_ref, n));
    B_frame = cross(n, T_frame);
  }
  // Tangent-space normal map perturbation (carbonFiberNormal, sidewall/tread NRM, leather/plastic/stitches NRM).
  if ((flags & kFlagNormalMap) != 0u) {
    vec2 nm_uv = (klass == kCarbon) ? (v_uv1 * 1.5) : ((flags & kFlagNormalUv1) != 0u ? v_uv1 : v_uv0);
    vec2 nm_xy = texture(normal_map, nm_uv).xy * 2.0 - 1.0;
    float nm_strength = (klass == kInterior) ? 1.35 : (klass == kTyre ? 1.15 : 1.0);
    nm_xy *= nm_strength;
    float nz = sqrt(max(1.0 - dot(nm_xy, nm_xy), 0.04));
    n = normalize(T_frame * nm_xy.x + B_frame * nm_xy.y + n * nz);
  }
  vec3 r = reflect(-view, n);
  float nv = clamp(dot(n, view), 0.0, 1.0);

  vec3 base = (flags & kFlagFlat) != 0u ? draw_tint.rgb : t0.rgb;
  float reflectivity = 0.04;
  float gloss = 0.3;
  float metal = 0.0;
  float alpha = 1.0;
  vec3 emissive = vec3(0.0);
  float flake_boost = 0.0;
  bool is_clearcoat = false;
  float coat_mask = 1.0;
  float paint_metallic = 0.0;
  float paint_scale = 1.0;
  float glass_crack_vein = 0.0;

  if ((flags & kFlagGrille) != 0u) {
    if ((flags & kFlagCutout) != 0u && t1.a < 0.5) discard;
    base = t1.rgb;
  }

  if (klass == kPaint) {
    vec3 paint_col = draw_tint.rgb;
    if ((flags & kFlagInstanced) != 0u && car_instances[v_instance].lamp_rgb[0].w > 0.5) {
      paint_col = car_instances[v_instance].lamp_rgb[0].rgb;
    }
    // Select per-region paint finish & two-tone overrides (1=hood, 2=wing, 3=mirrors) when active,
    // falling back to per-draw override (draw.params.w > 0.5) or body-wide frame.paint_finish.
    bool use_reg = ((flags & kFlagInstanced) == 0u || frame.drive.w < 0.5) &&
                   submat >= 1u && submat <= 3u && frame.region_finish[submat - 1u].w > 0.5;
    vec4 reg_fin = use_reg ? frame.region_finish[submat - 1u] : frame.paint_finish;
    vec4 reg_tt = use_reg ? frame.region_two_tone[submat - 1u] : frame.paint_two_tone;
    int finish = use_reg
                     ? int(reg_fin.x + 0.5)
                     : ((draw.params.w > 0.5)
                            ? (int(draw.params.w + 0.5) - 1)
                            : (((flags & kFlagInstanced) == 0u || frame.drive.w < 0.5)
                                   ? int(frame.paint_finish.x + 0.5)
                                   : (((flags & kFlagMetallic) != 0u) ? 1 : 0)));
    float xml_paint_scale = clamp(draw_tint.a, 0.15, 1.85);
    if (finish == 1) {
      // Metallic Flake (ShaderSettings/Metallic.xml): multiscale anti-aliased sparkle + dual-lobe metallic depth.
      flake_boost = 1.0;
      is_clearcoat = true;
      paint_metallic = 1.0;
      paint_scale = 0.50;
      gloss = 0.94;
      reflectivity = 0.0465;
    } else if (finish == 2) {
      // Matte (ShaderSettings/Matte.xml): satin sheen, low environment mirror.
      metal = 0.04;
      gloss = 0.22;
      reflectivity = 0.025;
    } else if (finish == 3) {
      // Two-Tone Pearlescent (List_SpecialColors): refracted viewing angle inside clearcoat (IOR=1.55).
      float sin2_t = max(1.0 - nv * nv, 0.0) / (1.55 * 1.55);
      float cos_t = sqrt(max(1.0 - sin2_t, 0.0));
      float tt_scale = max(reg_fin.y, 0.15);
      float tt_bias = reg_fin.z;
      float tt_pow = max(reg_tt.w, 1.0);
      float fres = clamp(pow(1.0 - cos_t, clamp(tt_pow * 0.25, 0.8, 5.0)) * (1.35 + tt_scale * 2.2) + tt_bias, 0.0, 1.0);
      paint_col = mix(paint_col, reg_tt.rgb, fres);
      flake_boost = 1.15;
      is_clearcoat = true;
      paint_metallic = 1.0;
      paint_scale = 0.50;
      gloss = 0.95;
      reflectivity = 0.0465;
    } else if (finish == 4) {
      // Chrome special finish.
      metal = 0.94;
      gloss = 0.97;
      reflectivity = 0.72;
    } else if (finish == 5) {
      // Carbon Fiber weave special finish: sample authentic Shared/carbonfiber.xds +
      // Shared/carbonfibernormal.xds in car-local triplanar space when bound, else procedural weave.
      vec3 an_c = abs(n);
      vec2 cuv = (an_c.y >= an_c.x && an_c.y >= an_c.z) ? v_local_pos.xz
               : (an_c.x >= an_c.z)                     ? v_local_pos.zy
                                                        : v_local_pos.xy;
      if ((flags & kFlagCarbonTex) != 0u) {
        vec2 cf_uv = cuv * 12.0;
        vec3 cf_diff = texture(slot1, cf_uv).rgb;
        vec2 cf_nm = (texture(normal_map, cf_uv).xy * 2.0 - 1.0) * 0.85;
        float cf_nz = sqrt(max(1.0 - dot(cf_nm, cf_nm), 0.04));
        n = normalize(T_frame * cf_nm.x + B_frame * cf_nm.y + n * cf_nz);
        r = reflect(-view, n);
        nv = clamp(dot(n, view), 0.0, 1.0);
        paint_col = mix(cf_diff * 0.65, paint_col * cf_diff * 1.45, 0.72);
      } else {
        vec2 wuv = v_local_pos.xz * 55.0 + v_local_pos.yy * 55.0;
        float weave = sin(wuv.x) * cos(wuv.y);
        paint_col = mix(paint_col * 0.22, paint_col * 0.52, 0.5 + 0.5 * weave);
      }
      is_clearcoat = true;
      paint_metallic = 0.20;
      paint_scale = 0.90;
      gloss = 0.92;
      reflectivity = 0.047;
    } else if (finish == 6) {
      // Wood grain special finish (car-local space).
      float ring = sin((v_local_pos.z * 16.0 + fbm(v_local_pos.xz * 6.0) * 4.0) * 3.14159);
      paint_col = mix(paint_col * 0.45, paint_col * 1.05, 0.5 + 0.5 * ring);
      is_clearcoat = true;
      paint_metallic = 0.0;
      paint_scale = 1.0;
      gloss = 0.78;
      reflectivity = 0.047;
    } else if (finish == 7) {
      // Camouflage special finish (car-local space).
      float c1 = fbm(v_local_pos.xz * 3.2 + v_local_pos.y * 2.4);
      float c2 = fbm(v_local_pos.xz * 5.8 - v_local_pos.y * 3.1 + 7.3);
      vec3 dark_c = paint_col * 0.30;
      vec3 mid_c = mix(paint_col, reg_tt.rgb, 0.6);
      paint_col = c1 < 0.42 ? dark_c : (c2 > 0.56 ? mid_c : paint_col);
      metal = 0.0;
      gloss = 0.18;
      reflectivity = 0.02;
    } else {
      // FH1 stock paint (Normal.xml / Metallic.xml / ColorShaderSettings*.xml): 2-layer clearcoat;
      // metallic rows get PaintScale=0.5 + blurred metallic cube + dual flake lobes.
      is_clearcoat = true;
      reflectivity = 0.047;
      gloss = 0.95;
      if ((flags & kFlagMetallic) != 0u) {
        paint_metallic = 1.0;
        paint_scale = 0.50;
        flake_boost = 0.72;
      }
    }

    // Apply per-car ColorShaderSettings*.xml / ShaderSettings MetalColor flip-flop when active.
    // On instanced AI/traffic metallic cars, derive a clean OEM grazing-angle flop from the
    // instance's own paint color instead of borrowing the player car's custom MetalColor.
    if (!use_reg && (finish == 0 || finish == 1 || finish == 2)) {
      if ((flags & kFlagInstanced) == 0u) {
        float m_scale = frame.paint_shader_d.y;
        if (m_scale > 0.001) {
          float m_pow = clamp(frame.paint_shader_c.w, 0.25, 16.0);
          float m_bias = frame.paint_shader_d.x;
          float m_t = clamp(pow(1.0 - nv, m_pow) * m_scale + m_bias, 0.0, 1.0);
          paint_col = mix(paint_col, frame.paint_shader_c.rgb, m_t * (1.0 - t0.a));
        }
      } else if (finish == 1) {
        vec3 inst_flop = paint_col * vec3(0.58, 0.60, 0.68);
        float m_t = clamp(pow(1.0 - nv, 3.2) * 0.28, 0.0, 0.45);
        paint_col = mix(paint_col, inst_flop, m_t * (1.0 - t0.a));
      }
    }

    vec3 texel = (flags & kFlagStripe) != 0u ? t0.rgb * stripe_rgb : t0.rgb;
    base = mix(paint_col, texel, t0.a);
    // Apply runtime Shared/ShaderSettings/{Normal,Metallic,Matte}.xml PaintScale when provided.
    if (abs(xml_paint_scale - 1.0) > 0.01) {
      paint_scale = xml_paint_scale;
    }
    // Non-paint baked atlas details (badges, black trim) are not metallic flake.
    paint_metallic *= (1.0 - t0.a);
    paint_scale = mix(paint_scale, 1.0, t0.a);
    if ((flags & kFlagRaceLivery) != 0u) {
      base = t0.rgb;
      paint_metallic = 0.0;
      paint_scale = 1.0;
    }

    // Livery layer: options.z > 1.5 composites custom 1024^2 RGBA vinyls;
    // options.z in (0.5, 1.5) draws the legacy magenta mask overlay.
    if ((flags & kFlagLivery) != 0u && frame.options.z > 1.5) {
      vec4 liv = texture(livery, v_uv0);
      float mask_a = liv.a * (1.0 - t0.a);
      base = mix(base, liv.rgb, mask_a);
      // Vinyl decals sit under glossy clearcoat without metallic flake.
      paint_metallic *= (1.0 - mask_a * 0.85);
      paint_scale = mix(paint_scale, 1.0, mask_a);
      metal = mix(metal, 0.04, mask_a * 0.85);
      gloss = mix(gloss, 0.90, mask_a * 0.75);
    } else if ((flags & kFlagLivery) != 0u && frame.options.z > 0.5) {
      base = mix(base, vec3(1.0, 0.0, 1.0), 0.5 * texture(livery, v_uv0).r * (1.0 - t0.a));
    }
  } else if (klass == kCaliper) {
    base = mix(draw_tint.rgb, t0.rgb, t0.a);
    gloss = 0.65;
  } else if (klass == kGlass) {
    if (submat == 6u) {
      // Rear-window defroster heater lines (defrost_lines, 69 base cars): thin translucent amber wires.
      base = vec3(0.24, 0.08, 0.02);
      reflectivity = 0.02;
      gloss = 0.35;
      alpha = 0.24;
    } else if ((flags & kFlagCover) != 0u) {
      // FH1 two-pass detail_glass_red / amber / clear lamp covers (SHOWROOM_LIGHTING.md:187):
      // submat 7 = Pass 1 Multiply (DST_COLOR, ZERO) tinted by LightsSampler + GlassColor;
      // submat 8 = Pass 2 Additive (ONE, ONE) sharp Fresnel env reflection + sun specular.
      bool is_red = draw_tint.r > 0.15 && draw_tint.g < 0.05;
      bool is_amber = draw_tint.r > 0.35 && draw_tint.g > 0.10 && draw_tint.b < 0.05;
      if (submat == 7u) {
        vec3 lens_tex = (flags & kFlagSlot1) != 0u ? mix(vec3(1.0), t1.rgb, 0.45) : vec3(1.0);
        vec3 trans = is_red ? vec3(0.76, 0.08, 0.06) :
                     is_amber ? vec3(0.92, 0.50, 0.08) :
                                vec3(0.86, 0.89, 0.92);
        trans = clamp(trans * lens_tex, vec3(0.03), vec3(1.0));
        out_color = vec4(trans, 1.0);
        return;
      }
      base = draw_tint.rgb;
      reflectivity = is_red ? 0.06 : is_amber ? 0.05 : 0.08;
      gloss = 0.98;
      alpha = is_red ? 0.65 : is_amber ? 0.45 : 0.16;
    } else {
      // FH1 Normal.xml <window>: GlassColor=(0.0, 0.10, 0.08, 0.29), FresnelIndex=4.35, FresnelScalar=0.70.
      // Exclude front windshield (submat == 20, glassF) from heavy dark window tinting so cockpit
      // view remains crisp and clear even at 100% window tint (max subtle optical tint ~0.14..0.22).
      bool is_windshield = (submat == 20u);
      float raw_wtint = ((flags & kFlagInstanced) == 0u) ? clamp(frame.paint_finish.w, 0.0, 1.0) : 0.0;
      float wtint = is_windshield ? (raw_wtint * 0.15) : raw_wtint;
      base = mix(vec3(0.0, 0.055, 0.045), vec3(0.002, 0.005, 0.004), wtint);
      reflectivity = 0.047;
      gloss = 0.97;
      alpha = is_windshield ? mix(0.14, 0.22, raw_wtint) : mix(0.29, 0.88, wtint);
      if ((flags & kFlagLampEmissive) != 0u) {
        emissive = draw_tint.rgb;
      }
      // Real FH1 Shared/glass_texture.xds fracture & shatter-out damage (1:1 UV0 layout):
      // - Left half (u < 0.5): laminated windshield spiderweb cracks in RGB, alpha=1 (never punches out).
      // - Right half (u >= 0.5): tempered rear/side glass cracks in RGB, alpha=0 inside shattered hole.
      // Only apply player car damage (never leak onto instanced traffic/AI cars or non-window glass).
      if ((flags & kFlagInstanced) == 0u && submat != 9u) {
        float win_dmg = 0.0;
        if (submat == 20u) {
          win_dmg = max(frame.damage_dirt.z, smoothstep(0.25, 0.92, frame.damage_zones.x));
        } else if (submat == 21u) {
          win_dmg = smoothstep(0.22, 0.92, frame.damage_zones.y);
        } else if (submat == 22u) {
          win_dmg = smoothstep(0.22, 0.92, frame.damage_zones.z);
        } else if (submat == 23u) {
          win_dmg = smoothstep(0.22, 0.92, frame.damage_zones.w);
        } else if (v_uv0.x < 0.5) {
          win_dmg = max(frame.damage_dirt.z, smoothstep(0.25, 0.92, frame.damage_zones.x));
        } else if (v_local_pos.z > 0.55 && abs(v_local_pos.x) < 0.65) {
          win_dmg = smoothstep(0.22, 0.92, frame.damage_zones.y);
        } else if (v_local_pos.x < -0.40) {
          win_dmg = smoothstep(0.22, 0.92, frame.damage_zones.z);
        } else if (v_local_pos.x > 0.40) {
          win_dmg = smoothstep(0.22, 0.92, frame.damage_zones.w);
        }
        if (win_dmg > 0.04) {
          vec2 guv = clamp(v_uv0, vec2(0.002), vec2(0.998));
          vec4 gt = texture(shared_glass_crack, guv);
          bool tempered = (submat == 21u || submat == 22u || submat == 23u || guv.x >= 0.5);
          // Heavy tempered-glass crash shatters out the center pane where Shared/glass_texture alpha < 0.5,
          // leaving realistic jagged cracked glass shards around the perimeter frame.
          if (tempered && win_dmg > 0.72 && gt.a < 0.5) {
            discard;
          }
          // Crack veins live strictly in RGB (never alpha, which is the glass presence mask!).
          float crack_rgb = max(gt.r, max(gt.g, gt.b));
          if (tempered && win_dmg <= 0.72) {
            // While tempered glass is cracked but not yet shattered out, map left-half spiderweb
            // veins across the intact center so cracks span the pane without any opaque white fill.
            vec2 spider_uv = vec2(clamp((guv.x - 0.5) * 0.96 + 0.02, 0.004, 0.496), guv.y);
            vec3 sp = texture(shared_glass_crack, spider_uv).rgb;
            crack_rgb = max(crack_rgb, max(sp.r, max(sp.g, sp.b)));
          }
          float dmg_vis = smoothstep(0.04, 0.85, win_dmg);
          // Threshold above DXT5 block noise (~0.04) so intact glass between crack veins stays 100% clear.
          float vein = smoothstep(mix(0.30, 0.08, dmg_vis), 0.60, crack_rgb) * dmg_vis;
          glass_crack_vein = vein;
          base = mix(base, vec3(0.46, 0.54, 0.52), vein * 0.70);
          alpha = clamp(alpha + vein * 0.36, 0.0, 0.88);
          gloss = mix(gloss, 0.30, vein);
        }
      }
    }
  } else if (klass == kLamp) {
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    reflectivity = 0.3;
    gloss = 0.85;
    metal = 0.5;
    // Independent 4-corner lamp breakage check (player car): FL/FR headlights and RL/RR taillights
    // break independently on off-center impacts using frame.damage_corners (threshold 0.45).
    float lamp_alive = 1.0;
    if ((flags & kFlagInstanced) == 0u) {
      bool is_front = (v_local_pos.z < 0.0);
      bool is_left = (v_local_pos.x < 0.0);
      float corner_dmg = is_front
                             ? (is_left ? frame.damage_corners.x : frame.damage_corners.y)
                             : (is_left ? frame.damage_corners.z : frame.damage_corners.w);
      if (corner_dmg <= 0.001 && frame.damage_dirt.w > 0.01) {
        float lf = smoothstep(0.55, 1.75, -v_local_pos.z) * frame.damage_zones.x;
        float lr = smoothstep(0.55, 1.75, v_local_pos.z) * frame.damage_zones.y;
        float ll = smoothstep(0.35, 0.88, -v_local_pos.x) * frame.damage_zones.z;
        float lri = smoothstep(0.35, 0.88, v_local_pos.x) * frame.damage_zones.w;
        corner_dmg = clamp(max(lf, lr) * 0.72 + max(ll, lri) * 0.35, 0.0, 1.0) * frame.damage_dirt.w;
      }
      if (corner_dmg >= 0.45) {
        float shatter = smoothstep(0.45, 0.78, corner_dmg);
        lamp_alive = 0.0;
        base *= mix(0.55, 0.25, shatter);
        gloss *= mix(0.45, 0.18, shatter);
      } else if (corner_dmg > 0.22) {
        lamp_alive = mix(1.0, 0.65, smoothstep(0.22, 0.45, corner_dmg));
      }
    }
    if ((flags & kFlagLampEmissive) != 0u) {
      // Modulate HDR lamp emission by lights_lod0.xds reflector detail and Fresnel angle so
      // individual rings, housings and bezels read clearly at night instead of flat solid discs.
      float luma = dot(base, vec3(0.299, 0.587, 0.114));
      float lens_detail = mix(0.14, 1.38, smoothstep(0.03, 0.72, luma));
      float ang = 0.36 + 0.64 * pow(nv, 1.15);
      emissive = draw_tint.rgb * lens_detail * ang * (kLinearOut ? 1.85 : 0.95) * lamp_alive;
    }
  } else if (klass == kInterior || klass == kGauge) {
    if ((flags & kFlagCutout) != 0u && (flags & kFlagSlot1) != 0u && t1.a < 0.5) discard;
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    float head_amt = clamp(frame.cockpit_mfd_ext.w, 0.0, 1.0);
    float gauge_scale = clamp(frame.paint_shader_d.w / 3.0, 0.0, 3.0);
    if (submat == 15u) {
      // Analog gauge needle (speed_min/tach_min/fuel_min/boost_min): daytime painted needle,
      // illuminated at night/dusk/tunnel proportional to HeadLightAmount * GaugeEmissiveScale.
      base = mix(vec3(0.78, 0.12, 0.025), base, 0.25);
      float needle_emit = (flags & kFlagLampEmissive) != 0u
                              ? max(draw_tint.r, max(draw_tint.g, draw_tint.b)) * 0.35
                              : (head_amt * gauge_scale);
      emissive = vec3(0.95, 0.18, 0.04) * needle_emit * (kLinearOut ? 0.85 : 0.65);
      gloss = 0.35;
    } else if (submat == 16u) {
      // Active backlit digital cockpit MFD / cluster TFT display (RPM bar, speed, gear, telemetry).
      vec2 muv = (draw.uv1.y > 0.01 && draw.uv1.w > 0.01)
                     ? clamp((v_local_pos.xy - draw.uv1.xz) * draw.uv1.yw, vec2(0.0), vec2(1.0))
                     : clamp(v_uv0, vec2(0.0), vec2(1.0));
      vec3 mfd_lit = eval_cockpit_mfd(muv);
      base = vec3(0.015, 0.022, 0.032) + mfd_lit * 0.15;
      float mfd_gain = kLinearOut ? 0.72 : 0.65;
      emissive = mfd_lit * mfd_gain;
      gloss = 0.88;
      reflectivity = 0.06;
    } else if (submat == 17u) {
      // Authentic FH1 interior_emissive_LOD0.xds backlit instrument cluster:
      // unlit printed dial face in daylight, backlit via HeadLightAmount * GaugeEmissiveScale at night.
      vec3 dial_bg = (flags & kFlagNoSlot0) == 0u ? t0.rgb * 0.35 : vec3(0.015, 0.016, 0.018);
      vec3 mark = (flags & kFlagSlot1) != 0u ? t1.rgb : vec3(0.0);
      base = max(dial_bg, mark * 0.58);
      float dial_emit = (flags & kFlagLampEmissive) != 0u
                            ? max(draw_tint.r, max(draw_tint.g, draw_tint.b)) * 0.38
                            : (head_amt * gauge_scale);
      emissive = mark * dial_emit * (kLinearOut ? 1.15 : 0.90);
      gloss = 0.32;
    } else if (klass == kInterior) {
      // FORMAT-CARS: interior techniques sample interior_LOD0 (t1) directly scaled by
      // per-technique draw_tint (0.3..0.5 plastic/cloth/leather, ~0.85 metal); they do not
      // read body atlas slot0, and t1.a is 0 across diffuse areas so must not mask t1.rgb.
      if ((flags & kFlagSlot1) != 0u) {
        base = t1.rgb * draw_tint.rgb;
      } else {
        vec3 fallback_col = (submat == 5u) ? vec3(0.24, 0.25, 0.27) : vec3(0.045, 0.048, 0.052);
        base = fallback_col * draw_tint.rgb;
      }
      if (submat == 5u) {
        // Cockpit brushed metal / metal2 trim.
        metal = 0.60;
        gloss = 0.65;
        reflectivity = 0.22;
      } else {
        gloss = 0.22;
      }
    } else {
      // Analog gauge face: mask transparent or bright mint-green surround pixels to matte black bezel housing.
      if ((flags & kFlagSlot1) != 0u) {
        float surround_a = smoothstep(0.15, 0.55, t1.a);
        float mint_excess = smoothstep(0.04, 0.16, t1.g - t1.r * 1.08) * smoothstep(0.12, 0.26, t1.g) *
                            smoothstep(0.08, 0.22, t1.b);
        float keep_dial = surround_a * (1.0 - mint_excess * 0.94);
        base = mix(vec3(0.010, 0.012, 0.014), base, keep_dial);
        if ((flags & kFlagLampEmissive) != 0u) {
          emissive = base * draw_tint.rgb * keep_dial;
        }
      } else if ((flags & kFlagLampEmissive) != 0u) {
        emissive = base * draw_tint.rgb;
      }
      gloss = 0.28;
    }
  } else if (klass == kTyre) {
    // BC3_SRGB already decodes sidewall/tread textures to linear in hardware (no extra pow(2.2)!).
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : vec3(0.035);
    // Dynamic off-road tyre mud & dust coating (damage_dirt.y) evaluated in wheel-local space
    // (v_local_pos) so mud rotates 1:1 with the spinning tyre instead of sliding across it!
    // Guarded to player car so off-road mud never leaks onto instanced highway traffic/AI tyres.
    if ((flags & kFlagInstanced) == 0u && frame.damage_dirt.y > 0.01) {
      float mud_n = fbm(v_local_pos.yz * 18.0 + vec2(v_local_pos.x * 14.0, -v_local_pos.x * 11.0));
      float mud = clamp(frame.damage_dirt.y * (0.55 + 0.65 * mud_n), 0.0, 0.88);
      base = mix(base, vec3(0.16, 0.11, 0.065), mud);
    }
    reflectivity = 0.035;
    gloss = 0.18;
  } else if (klass == kRim) {
    // FH1 wheel_lod0.xds stores center-cap emblems/badges with t0.a=1 and leaves paintable alloy
    // spokes transparent (t0.a=0, rgb=0). Composite alloy metal under t0.a so stock & painted rims
    // render as gleaming metallic alloy instead of black!
    vec3 rim_default = (submat == 1u) ? vec3(0.76, 0.78, 0.82)
                     : (submat == 2u) ? vec3(0.055, 0.06, 0.065)
                     : (submat == 3u) ? vec3(0.36, 0.37, 0.39)
                     : (submat == 4u) ? vec3(0.46, 0.48, 0.52)
                                      : vec3(0.56, 0.58, 0.62);
    vec3 rim_xml_tint = (frame.rim_shader.w > 0.5 && (flags & kFlagInstanced) == 0u) ? frame.rim_tint_dirt.rgb : vec3(1.0);
    vec3 rim_paint = (submat == 2u || submat == 3u) ? rim_default : (rim_default * draw_tint.rgb * rim_xml_tint);
    float wblur = clamp(draw.params.w, 0.0, 1.0);
    if ((flags & kFlagBlurRim) != 0u) {
      // Retail blur_rim / blur_lip geometry textured with wheel0..3.xds on slot1:
      // cross-fade cleanly in at speed and discard when wheel is stationary.
      float blur_vis = smoothstep(0.05, 0.55, wblur);
      if (blur_vis <= 0.004) discard;
      vec4 btex = (flags & kFlagSlot1) != 0u ? t1 : t0;
      base = mix(rim_paint, btex.rgb * draw_tint.rgb * rim_xml_tint, 0.85);
      alpha = clamp(btex.a * blur_vis, 0.0, 1.0);
      if (alpha <= 0.01) discard;
    } else {
      if ((flags & kFlagHasBlurMesh) != 0u && wblur > 0.45) {
        // Cross-fade static 3D spokes out as retail blur_rim disc reaches full opacity.
        float fade_out = smoothstep(0.45, 0.85, wblur);
        if (fade_out >= 0.99 || ign_shadow() < fade_out) discard;
      }
      if ((flags & kFlagNoSlot0) == 0u) {
        base = mix(rim_paint, t0.rgb, t0.a);
      } else {
        base = rim_paint;
      }
      // Only run fallback UV-smear loop when wheel lacks retail blur_rim geometry.
      if ((flags & kFlagHasBlurMesh) == 0u && wblur > 0.01 && (flags & kFlagNoSlot0) == 0u) {
        vec2 duv = v_uv0 - vec2(0.5);
        vec3 acc = base;
        float span = wblur * 0.52;
        for (int k = -3; k <= 3; ++k) {
          if (k == 0) continue;
          float a = float(k) * (span / 3.0);
          float ca = cos(a), sa = sin(a);
          vec2 ruv = vec2(0.5) + vec2(ca * duv.x - sa * duv.y, sa * duv.x + ca * duv.y);
          vec4 s0 = texture(slot0, ruv);
          acc += mix(rim_paint, s0.rgb, s0.a);
        }
        base = mix(base, acc / 7.0, wblur * 0.92);
        base = mix(base, vec3(dot(base, vec3(0.3, 0.59, 0.11))) * 1.08, wblur * 0.22);
      }
    }
    if (submat == 1u) {
      // outer_rim (Tire/Normal.xml: PaintScale=0, CubeScale=1 pure chrome lip).
      metal = 0.94;
      gloss = 0.96;
      reflectivity = 0.72;
    } else if (submat == 2u) {
      // inner_rim / wheel_black (Tire/Normal.xml: dark matte barrel, CubeScale=0).
      metal = 0.12;
      gloss = 0.22;
      reflectivity = 0.03;
    } else if (submat == 3u) {
      // rotor / brake / drum (brushed iron disc).
      metal = 0.65;
      gloss = 0.52;
      reflectivity = 0.24;
    } else if (submat == 4u) {
      // hub (machined alloy).
      metal = 0.72;
      gloss = 0.70;
      reflectivity = 0.35;
    } else {
      // Standard painted/machined alloy rim spokes (Tire/Normal.xml + RimShaderSettings.xml).
      gloss = mix(0.80, 0.90, wblur);
      metal = 0.66;
      reflectivity = (frame.rim_shader.w > 0.5 && (flags & kFlagInstanced) == 0u)
                         ? clamp(0.42 * frame.rim_shader.y * frame.rim_shader.z, 0.08, 0.90)
                         : 0.42;
    }
  } else if (klass == kChrome) {
    float wblur = clamp(draw.params.w, 0.0, 1.0);
    if ((flags & kFlagBlurRim) != 0u) {
      float blur_vis = smoothstep(0.05, 0.55, wblur);
      if (blur_vis <= 0.004) discard;
      vec4 btex = (flags & kFlagSlot1) != 0u ? t1 : t0;
      base = mix(vec3(0.76, 0.78, 0.82), btex.rgb, 0.75);
      alpha = clamp(btex.a * blur_vis, 0.0, 1.0);
      if (alpha <= 0.01) discard;
    } else {
      if ((flags & kFlagHasBlurMesh) != 0u && wblur > 0.45) {
        float fade_out = smoothstep(0.45, 0.85, wblur);
        if (fade_out >= 0.99 || ign_shadow() < fade_out) discard;
      }
      if ((flags & kFlagHasBlurMesh) == 0u && wblur > 0.01 && (flags & kFlagNoSlot0) == 0u) {
        vec2 duv = v_uv0 - vec2(0.5);
        vec3 acc = t0.rgb;
        float span = wblur * 0.52;
        for (int k = -3; k <= 3; ++k) {
          if (k == 0) continue;
          float a = float(k) * (span / 3.0);
          float ca = cos(a), sa = sin(a);
          vec2 ruv = vec2(0.5) + vec2(ca * duv.x - sa * duv.y, sa * duv.x + ca * duv.y);
          acc += texture(slot0, ruv).rgb;
        }
        base = mix(base, acc / 7.0, wblur * 0.88);
      }
    }
    gloss = 0.96;
    metal = 0.92;
    reflectivity = 0.80;
  } else if (klass == kWorld) {
    base = draw_tint.rgb;
    reflectivity = 0.02;
    gloss = 0.05;
  } else if (klass == kCarbon) {
    base = t0.rgb * ((flags & kFlagSlot1) != 0u ? t1.rgb : vec3(1.0));
    is_clearcoat = true;
    paint_metallic = 0.12;
    paint_scale = 0.85;
    reflectivity = 0.06;
    gloss = 0.88;
  } else if (klass == kOpaque) {
    if ((flags & kFlagGrille) != 0u) {
      gloss = 0.25;
      reflectivity = 0.04;
    } else if ((flags & kFlagFlat) != 0u) {
      gloss = 0.15;
      reflectivity = 0.03;
    } else if (submat == 18u) {
      // Undercarriage / chassis / frame / black & rubber trim: sample Shared/undercarriage.xds
      // when bound in slot 1, modulated by @atlas AO (t0) so undersides never render bright white.
      vec3 ao = (flags & kFlagNoSlot0) == 0u ? mix(vec3(0.35), clamp(t0.rgb, vec3(0.0), vec3(1.0)), 0.65) : vec3(1.0);
      vec3 uc = (flags & kFlagSlot1) != 0u ? mix(vec3(0.028, 0.030, 0.033), t1.rgb * 0.35, 0.70)
                                           : vec3(0.028, 0.030, 0.033);
      base = uc * ao;
      gloss = 0.10;
      reflectivity = 0.02;
    } else if ((flags & kFlagNoSlot0) != 0u) {
      // Untextured kOpaque fallback: dark matte charcoal instead of (1,1,1) white.
      base = vec3(0.040, 0.042, 0.046);
      gloss = 0.15;
      reflectivity = 0.025;
    } else if (t0.a < 0.10 && n_geom.y < -0.45) {
      // Downward-facing unpainted underbody panels on @atlas: keep matte underbody charcoal.
      base = vec3(0.030, 0.032, 0.035) * mix(vec3(0.40), clamp(t0.rgb, vec3(0.0), vec3(1.0)), 0.60);
      gloss = 0.10;
      reflectivity = 0.02;
    }
  }

  // Real FH1 damage_LOD0.xds impact scratches/scuffs & Shared/dirt_pattern + dirt_mask accumulation.
  // Only apply player damage/dirt to the player car (not instanced traffic/AI cars).
  if ((flags & kFlagInstanced) == 0u &&
      (klass == kPaint || klass == kOpaque || klass == kCarbon || klass == kRim ||
       (klass == kLamp && (flags & kFlagDamageMap) != 0u))) {
    float z_front = smoothstep(0.65, 1.95, -v_local_pos.z) * frame.damage_zones.x;
    float z_rear = smoothstep(0.65, 1.95, v_local_pos.z) * frame.damage_zones.y;
    float x_left = smoothstep(0.50, 0.95, -v_local_pos.x) * frame.damage_zones.z;
    float x_right = smoothstep(0.50, 0.95, v_local_pos.x) * frame.damage_zones.w;
    float local_dmg = clamp(max(max(z_front, z_rear), max(x_left, x_right)), 0.0, 1.0);
    if (local_dmg > 0.02 && frame.damage_dirt.w > 0.01) {
      float dmg_amt = clamp(local_dmg * frame.damage_dirt.w, 0.0, 1.0);
      // Directional scrape streaks along car-local panels for subtle scuff breakup on fallback meshes.
      vec2 scrape_uv = vec2(v_local_pos.z * 7.5 + v_local_pos.x * 3.0, v_local_pos.y * 22.0);
      float streak = smoothstep(0.48, 0.84, fbm(scrape_uv));
      float scratch_mask = 0.0;
      vec3 dmg_col = vec3(0.14, 0.145, 0.155);
      if ((flags & kFlagDamageMap) != 0u) {
        vec4 dm = texture(damage_map, v_uv0);
        // FH1 damage_LOD0.xds uses the same paint-mask alpha convention as nodamage_LOD0.xds:
        // alpha=0 is clean paint (whose unshown RGB canvas differs from nodamage's white canvas),
        // and alpha>0 is authored scratches/chips/scuffs. Only use RGB delta where both atlases are opaque (BC1).
        float alpha_delta = clamp(dm.a - t0.a, 0.0, 1.0);
        float opaque_delta = smoothstep(0.08, 0.35, length(dm.rgb - t0.rgb)) * step(0.95, min(t0.a, dm.a));
        float tex_dmg = max(alpha_delta, opaque_delta);
        scratch_mask = clamp(tex_dmg * smoothstep(0.02, 0.55, dmg_amt) * (0.45 + 0.55 * dmg_amt), 0.0, 0.92);
        // Clamp damage_map luminance so bright/empty atlas texels never paint white patches;
        // preserve dark charcoal primer scrapes and expose brushed bare metal on deep core gouges.
        vec3 dm_safe = min(dm.rgb, vec3(0.36));
        vec3 bare_metal = mix(vec3(0.10, 0.105, 0.115), vec3(0.28, 0.30, 0.33), smoothstep(0.45, 0.85, tex_dmg));
        dmg_col = mix(dm_safe, bare_metal, smoothstep(0.50, 0.90, tex_dmg) * 0.55);
      } else {
        vec3 dp = texture(shared_dirt_pattern, v_uv0 * 4.0).rgb;
        scratch_mask = clamp(streak * dmg_amt * 0.65, 0.0, 0.65);
        dmg_col = mix(vec3(0.12, 0.125, 0.135), min(dp, vec3(0.28)), 0.35);
      }
      base = mix(base, dmg_col, scratch_mask);
      gloss = mix(gloss, 0.14, scratch_mask * 0.85);
      metal = mix(metal, 0.45, smoothstep(0.50, 0.88, scratch_mask));
      coat_mask *= (1.0 - scratch_mask * 0.92);
    }
    if (frame.damage_dirt.x > 0.01) {
      float low_mask = 1.0 - smoothstep(0.02, 0.76, v_local_pos.y);
      vec2 dirt_uv = v_local_pos.xz * 0.75 + vec2(v_local_pos.y * 0.9, v_local_pos.y * 0.6);
      vec4 dpat = texture(shared_dirt_pattern, dirt_uv * 2.5);
      bool has_car_dirt = (uint(max(frame.rim_tint_dirt.w, 0.0) + 0.5) & 1u) != 0u;
      float m = 0.5;
      float dirt = 0.0;
      if (has_car_dirt && (flags & kFlagNoSlot0) == 0u && klass != kRim) {
        // Sample authentic per-car dirt_mask.xds on atlas UV0 for authored wheel-arch/rocker splatter.
        vec4 dmsk = texture(shared_dirt_mask, v_uv0);
        m = clamp(max(dmsk.r, max(dmsk.g, dmsk.a)), 0.05, 1.0);
        dirt = clamp(frame.damage_dirt.x * mix(0.45, 1.05, low_mask) * m, 0.0, 0.90);
      } else {
        vec4 dmsk = texture(shared_dirt_mask, dirt_uv * 1.2);
        m = clamp(max(dmsk.r, dmsk.a), 0.15, 1.0);
        dirt = clamp(frame.damage_dirt.x * (low_mask * 0.85 + 0.20) * m, 0.0, 0.88);
      }
      vec3 mud_col = mix(vec3(0.18, 0.12, 0.075), dpat.rgb * 0.45, 0.65);
      base = mix(base, mud_col, dirt);
      gloss = mix(gloss, 0.08, dirt * 0.92);
      metal = mix(metal, 0.0, dirt * 0.95);
      coat_mask *= (1.0 - dirt * 0.95);
    }
  }

  float occlusion = mix(0.35, 1.0, v_occlusion);
  if (klass == kInterior || klass == kGauge) occlusion *= 0.72;
  vec3 key = normalize(frame.key_light.xyz), fill = normalize(frame.fill_light.xyz);
  vec3 h = normalize(view + key);
  float nh = max(dot(n, h), 0.0);
  float ndl = max(dot(n, key), 0.0);

  vec3 diffuse_light = env(n, 6.0) * frame.tint_light.rgb * occlusion +
                       (frame.key_light.w * ndl * mix(0.5, 1.0, occlusion) +
                        frame.fill_light.w * max(dot(n, fill), 0.0) * occlusion) * frame.tint_light.rgb;
  vec3 direct_col = frame.key_light.w * frame.tint_light.rgb;
  float lit = 1.0;
  float occ_refl = occlusion;

  if (frame.drive.w > 0.5) {
    vec3 up = normalize(frame.drive.xyz);
    // Use car-specific sun color scaled by IBLDirectScaleCar (0.6238 vs env 1.30).
    vec3 sun_col = frame.car_sun_color.w >= 0.0
                       ? frame.car_sun_color.rgb
                       : (frame.sun_color.w >= 0.0 ? frame.sun_color.rgb * 0.48 : kSunColour * 0.48);
    vec3 sky_col = frame.sun_color.w >= 0.0 ? frame.car_light_top.rgb : kSkyColour;
    vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.car_light_bot.rgb : kGroundColour;
    // Provide subtle ground-bounce floor when TimeOfDayA CarHLColourBottom is zero so undersides aren't black voids.
    if (dot(gnd_col, vec3(1.0)) < 0.02) {
      vec3 world_gnd = frame.sun_color.w >= 0.0 ? frame.ground_color.rgb : kGroundColour;
      gnd_col = max(world_gnd * 0.45, sky_col * 0.16);
    }
    float shadow_scale = frame.sun_color.w >= 0.0 ? clamp(frame.sun_color.w, 0.0, 1.0) : clamp(frame.shadow.z, 0.0, 1.0);
    float raw_lit = ndl > 0.0 ? drive_shadow(v_position, n) : 1.0;
    lit = mix(1.0, raw_lit, shadow_scale);
    float ibl_amb = frame.sun_color.w >= 0.0 ? max(frame.car_light_top.w, 0.01) : 1.0;
    vec3 sh_amb_tint = shadow_ambient_tint((1.0 - lit) * shadow_scale);
    diffuse_light = sun_col * ndl * lit * mix(0.6, 1.0, occlusion) +
                    mix(gnd_col, sky_col, 0.5 + 0.5 * dot(n, up)) * ibl_amb * occlusion * sh_amb_tint;
    // Projected headlight beams illuminating other vehicles at night (CAR_LAMPS.md §5).
    int hl_count = int(headlights.count_night.x + 0.5);
    float hl_night = headlights.count_night.y;
    if (hl_count > 0 && hl_night > 0.004) {
      vec3 hl_sum = vec3(0.0);
      for (int i = 0; i < hl_count && i < 16; ++i) {
        vec3 origin = headlights.beams[i].origin_range.xyz;
        float range = headlights.beams[i].origin_range.w;
        vec3 to_p = v_position - origin;
        float d2 = dot(to_p, to_p);
        if (d2 >= range * range || d2 < 1.2) continue;
        vec3 bdir = headlights.beams[i].dir_power.xyz;
        if (dot(to_p, bdir) <= 0.35) continue;
        vec4 c = headlights.beams[i].proj * vec4(v_position, 1.0);
        if (c.w <= 0.1) continue;
        vec2 buv = c.xy / c.w * 0.5 + 0.5;
        if (buv.x <= 0.0 || buv.x >= 1.0 || buv.y <= 0.0 || buv.y >= 1.0) continue;
        vec4 ck = texture(headlight_cookie, buv);
        float pattern = max(max(ck.r, ck.g), ck.b) * max(ck.a, 0.25);
        float dist = sqrt(d2);
        float att = pow(clamp(1.0 - dist / range, 0.0, 1.0), 1.45);
        float edge = smoothstep(0.0, 0.08, buv.x) * (1.0 - smoothstep(0.92, 1.0, buv.x)) *
                     smoothstep(0.0, 0.08, buv.y) * (1.0 - smoothstep(0.92, 1.0, buv.y));
        float facing = max(dot(n, -to_p / dist), 0.0);
        hl_sum += headlights.beams[i].color.rgb * pattern * att * edge * facing * headlights.beams[i].dir_power.w;
      }
      diffuse_light += hl_sum * hl_night * 0.85;
    }
    direct_col = sun_col;
    // DynamicEnvLightmapDampner = 0.30 (0.50 for chrome per Normal.xml).
    float damp = (klass == kChrome) ? 0.50 : 0.35;
    occ_refl = occlusion * mix(damp, 1.0, lit);
  }

  float fresnel = reflectivity + (1.0 - reflectivity) * pow(1.0 - nv, 5.0);
  vec3 specular_colour = mix(vec3(fresnel), base * mix(0.5, 1.0, fresnel), metal);
  vec3 reflection = env(r, (1.0 - gloss) * 6.0) * frame.reflect_tint.rgb * occ_refl;
  vec3 highlight = vec3(0.0);
  if (frame.drive.w > 0.5) {
    float power = mix(8.0, 400.0, gloss);
    highlight = direct_col * pow(nh, power) * (power + 8.0) * 0.04 * ndl * lit * gloss;
  } else {
    highlight = pow(nh, mix(8.0, 400.0, gloss)) * direct_col * gloss;
  }

  vec3 colour;
  if (is_clearcoat) {
    // Phase B Energy-Conserving 2-Layer Clearcoat + Multiscale Anti-Aliased Metallic Flake BRDF:
    // Close-up (< 6 m) clearcoat-only orange-peel micro-normal on high/ultra gives authentic painted-panel realism.
    vec3 n_coat = n;
    if (frame.phase_b_params.x > 0.5 && eye_dist < 6.0 && coat_mask > 0.05) {
      float peel_fade = (1.0 - smoothstep(2.0, 6.0, eye_dist)) * coat_mask;
      vec3 an_p = abs(n);
      vec2 puv = (an_p.y >= an_p.x && an_p.y >= an_p.z) ? v_local_pos.xz
               : (an_p.x >= an_p.z)                     ? v_local_pos.zy
                                                        : v_local_pos.xy;
      vec2 gp = puv * 115.0;
      float p0 = vnoise(gp);
      vec2 g_peel = vec2(vnoise(gp + vec2(0.35, 0.0)) - p0, vnoise(gp + vec2(0.0, 0.35)) - p0);
      n_coat = normalize(n + (T_frame * g_peel.x + B_frame * g_peel.y) * (0.045 * peel_fade));
    }
    vec3 r_coat = reflect(-view, n_coat);
    float nv_c = clamp(dot(n_coat, view), 0.0, 1.0);
    float nl_c = max(dot(n_coat, key), 0.0);
    float nh_c = max(dot(n_coat, h), 0.0);

    // Layer 1 (Clearcoat): retail FresnelIndex/FresnelScalar/SpecularCubeScalar driven dielectric
    // clearcoat (defaults 1.55 / 0.90 / 1.0 -> rich automotive saturation, controlled grazing mirror).
    bool is_inst = (flags & kFlagInstanced) != 0u;
    float p_fidx = is_inst ? 1.55 : clamp(frame.paint_shader_a.x, 1.0, 5.0);
    float p_fscl = is_inst ? 0.90 : clamp(frame.paint_shader_a.y, 0.0, 1.5);
    float p_cube = is_inst ? 1.00 : clamp(frame.paint_shader_a.z, 0.05, 2.0);
    float fres_exp = clamp(3.45 + (p_fidx - 1.55) * 0.85, 2.2, 5.5);
    float fres_gain = clamp(0.48 * (p_fscl / 0.90), 0.0, 0.78);
    float F_coat_v = clamp(0.040 + fres_gain * pow(1.0 - nv_c, fres_exp), 0.040, 0.62) * coat_mask;
    float F_coat_l = clamp(0.040 + fres_gain * pow(1.0 - nl_c, fres_exp), 0.040, 0.62) * coat_mask;
    float coat_env_scale = (frame.drive.w > 0.5 ? 0.58 : 0.72) * p_cube;
    vec3 coat_refl = env(r_coat, 0.65) * frame.reflect_tint.rgb * occ_refl * coat_env_scale;
    float coat_ggx = ggx_d(nh_c, 0.065) * 0.022;
    float xml_spec1 = (!is_inst && frame.paint_shader_a.w > 0.001)
                          ? pow(nh_c, clamp(frame.paint_shader_d.z, 8.0, 256.0)) * frame.paint_shader_a.w * 0.35
                          : 0.0;
    vec3 coat_spec = direct_col * (coat_ggx + xml_spec1) * nl_c * lit * coat_mask;

    // Layer 2 (Basecoat + Multiscale Anti-Aliased Metallic Flakes, energy-conserved by (1 - F_coat)).
    float base_atten_direct = (1.0 - F_coat_v) * (1.0 - F_coat_l);
    float base_atten_ibl = 1.0 - F_coat_v;
    vec3 basecoat = base * paint_scale * diffuse_light * base_atten_ibl;
    if (paint_metallic > 0.01) {
      vec3 met_cube = env(r, 3.4) * frame.reflect_tint.rgb * occ_refl * p_cube;
      // Continuous dual-lobe metallic GGX sheen scaled by ShaderSettings SpecAmount2/3 & SpecPower2/3.
      vec4 ps_b = is_inst ? vec4(3.0, 12.0, 2.0, 4.0) : frame.paint_shader_b;
      float s2_gain = clamp(ps_b.x / 7.0, 0.25, 2.2);
      float s2_alpha = clamp(sqrt(2.0 / max(ps_b.y + 2.0, 3.0)) * 0.52, 0.14, 0.45);
      float s3_gain = clamp(ps_b.z / 6.0, 0.25, 2.2);
      float s3_alpha = clamp(sqrt(2.0 / max(ps_b.w + 2.0, 3.0)) * 0.82, 0.28, 0.72);
      float lobe_mid = ggx_d(nh, s2_alpha) * 0.15 * s2_gain;
      float lobe_broad = ggx_d(nh, s3_alpha) * 0.24 * s3_gain;
      vec3 flake_tint = mix(base, vec3(0.95), 0.35);
      basecoat += base * met_cube * 0.36 * paint_metallic * base_atten_ibl;
      basecoat += base * direct_col * (lobe_mid + lobe_broad) * ndl * lit * paint_metallic * base_atten_direct;
      if (flake_boost > 0.01) {
        float glint = eval_multiscale_flake(v_local_pos, n, h, T_frame, B_frame, 220.0, frame.phase_b_params.x);
        basecoat += flake_tint * direct_col * glint * flake_boost * ndl * lit * paint_metallic * base_atten_direct;
      }
    }
    colour = basecoat + F_coat_v * coat_refl + coat_spec + emissive;
  } else if (klass == kRim && submat == 3u) {
    // Phase B Radial Anisotropic GGX Brake Rotors + Planckian Blackbody Thermal Glow under hard braking.
    vec3 rad_v = v_hub_radial - n * dot(v_hub_radial, n);
    float rlen = length(rad_v);
    vec3 R_axis = rlen > 1e-4 ? (rad_v / rlen) : T_frame;
    vec3 T_axis = normalize(cross(n, R_axis));
    float d_aniso = aniso_ggx_d(n, h, T_axis, R_axis, 0.09, 0.45);
    vec3 rotor_f0 = mix(vec3(0.04), base * 1.15, metal);
    vec3 rotor_f = rotor_f0 + (vec3(1.0) - rotor_f0) * pow(1.0 - nv, 5.0);
    vec3 aniso_spec = direct_col * rotor_f * (d_aniso * 0.18) * ndl * lit;
    // Planckian thermal glow across the swept brake-pad friction ring (900 K -> 1450 K).
    // Guarded to player car so hard braking never makes instanced traffic/AI rotors glow.
    float disc_r = length(v_local_pos.yz);
    float pad_ring = smoothstep(0.065, 0.115, disc_r) * (1.0 - smoothstep(0.165, 0.24, disc_r));
    float rotor_heat = (flags & kFlagInstanced) == 0u ? frame.phase_b_params.y : 0.0;
    vec3 thermal_glow = blackbody_rotor_rgb(rotor_heat) * pad_ring;
    colour = base * (1.0 - metal) * diffuse_light + rotor_f * reflection * 0.85 + aniso_spec + thermal_glow + emissive;
  } else {
    colour = base * (1.0 - metal) * diffuse_light + specular_colour * (reflection + highlight) + emissive;
    if (klass == kTyre) {
      // Tire/Normal.xml: broad satin rubber specular (SpecularPower = 5.0) + soft grazing rubber sheen.
      float rubber_spec = pow(nh, 5.0) * 0.16 * ndl * lit;
      float rubber_rim = pow(1.0 - nv, 3.0) * 0.06;
      colour += direct_col * rubber_spec + env(n, 5.0) * rubber_rim * occlusion;
    }
  }

  if ((flags & kFlagMirror) != 0u) {
    if (frame.shadow_far_bias.w > 0.5 && draw.uv1.y > 0.0 && draw.uv1.w > 0.0) {
      // Dedicated 3-band cockpit mirror atlas (1024x256):
      // submat 10 = mirrorLeft [0..1/3], 11 = mirrorMiddle [1/3..2/3], 12 = mirrorRight [2/3..1].
      // Map each mirror glass quad 1:1 to its dedicated 342x256 rear-facing camera band
      // using car-space XY bounds stored in draw.uv1 (rendering.toml:1167).
      float u_lo = 0.338, u_hi = 0.662;
      if (submat == 10u) {
        u_lo = 0.004;
        u_hi = 0.329;
      } else if (submat == 12u) {
        u_lo = 0.671;
        u_hi = 0.996;
      }
      float u_loc = clamp((v_position.x - draw.uv1.x) * draw.uv1.y, 0.0, 1.0);
      float v_loc = clamp(1.0 - (v_position.y - draw.uv1.z) * draw.uv1.w, 0.0, 1.0);
      // Rear camera right is -X (driver's left), so u_band = 1 - u_loc keeps left on the left.
      vec2 muv = vec2(mix(u_lo, u_hi, 1.0 - u_loc), clamp(v_loc, 0.01, 0.99));
      colour = texture(mirror_map, muv).rgb * 0.92;
    } else {
      colour = env(r, 0.25) * 0.85;
    }
    alpha = 1.0;
  } else if (klass == kGlass) {
    if ((flags & kFlagCover) != 0u && submat == 8u) {
      // Pass 2 Additive reflection + sun glint on detail_glass_red/amber/clear covers.
      bool is_red = draw.tint.r > 0.15 && draw.tint.g < 0.05;
      bool is_amber = draw.tint.r > 0.35 && draw.tint.g > 0.10 && draw.tint.b < 0.05;
      float f_idx = is_red ? 1.65 : is_amber ? 1.45 : 3.55;
      float f_scl = (is_red || is_amber) ? 0.70 : 1.00;
      float F_cov = clamp(0.05 + f_scl * pow(1.0 - nv, f_idx), 0.05, 0.95);
      vec3 cov_refl = env(r, 0.0) * frame.reflect_tint.rgb * occ_refl;
      vec3 cov_spec = direct_col * pow(nh, 128.0) * 2.5 * ndl * lit;
      // Subtle translucent plastic diffuse scattering on red/amber lamp lenses (P1-3)
      // so unlit daylight rear fog/tail lenses read deep ruby-red instead of dull grey-brown.
      vec3 lens_diff = is_red ? (vec3(0.065, 0.002, 0.002) * diffuse_light)
                     : is_amber ? (vec3(0.14, 0.055, 0.004) * diffuse_light)
                     : vec3(0.0);
      colour = lens_diff + F_cov * cov_refl + cov_spec;
      alpha = 1.0;
    } else if (submat == 6u) {
      colour = base * (diffuse_light + vec3(0.02)) * alpha;
    } else {
      // FH1 window glass: strong grazing Fresnel (4.35 / 0.70), sharp mip-0 reflection, clear straight-through.
      // Lit base tint avoids unlit khaki/tan rear-window glow under high midnight post-exposure.
      float F_glass = clamp(0.047 + 0.70 * pow(1.0 - nv, 4.35), 0.047, 0.92);
      vec3 glass_refl = env(r, 0.0) * frame.reflect_tint.rgb * occ_refl * (1.0 - glass_crack_vein * 0.55);
      vec3 glass_spec = direct_col * pow(nh, 140.0) * 2.2 * ndl * lit * (1.0 - glass_crack_vein * 0.50);
      vec3 crack_scatter = vec3(0.48, 0.56, 0.54) * (diffuse_light + direct_col * 0.25) * glass_crack_vein * 0.42;
      colour = base * diffuse_light * alpha + F_glass * glass_refl + glass_spec + crack_scatter + emissive;
      alpha = clamp(alpha + F_glass * 0.65, 0.0, 0.96);
    }
  }
  colour *= frame.options.x;
  float out_a = alpha;
  if ((flags & kFlagBlurRim) != 0u) {
    // Premultiplied alpha output for glass_ blended pipeline (ONE, ONE_MINUS_SRC_ALPHA).
    colour *= alpha;
    out_a = alpha;
  } else if (kLinearOut && klass != kGlass && (flags & kFlagMirror) == 0u) {
    // Encode automotive surface gloss into HDR alpha for Phase B Hi-Z SSR (0 on matte/tyre/interior).
    if (is_clearcoat) {
      out_a = clamp(0.52 * coat_mask, 0.0, 0.58);
    } else if (klass == kChrome) {
      out_a = 0.95;
    } else if (klass == kRim) {
      out_a = (submat == 1u) ? 0.85 : (submat == 2u ? 0.0 : 0.45);
    } else {
      out_a = 0.0;
    }
  }
  out_color = vec4(out_rgb(colour), out_a);
}
