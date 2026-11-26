// Shared body of world.frag and world_gpu.frag (GPU_DRIVEN defined by the latter).
#ifdef GPU_DRIVEN
#extension GL_EXT_nonuniform_qualifier : require
#endif
#ifdef OPAQUE_NO_DISCARD
layout(early_fragment_tests) in;
#endif
// carview: Colorado's first-pass shading (WORLD-MATERIALS, "Minimal
// recipe"): slot 0's texture (sRGB when its format word says gamma) lit by
// the frame's sun and two-colour sky. Not the game's shading: no layer
// blends, AO, lightmaps or fog yet. Cut-outs (props, trees) write their
// diffuse alpha for alpha to coverage; decals and the lake blend with the
// texture's alpha times the vertex colour's (ours: which alpha the game
// uses is open). Output is encoded to sRGB here (UNORM targets) or linear
// for the drive's post chain (kLinearOut).

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
  mat4 shadow_matrix;  // the frame's space to the shadow map's clip
  vec4 shadow;
  vec4 shadow_bias;
  vec4 world_fade;  // prop LOD distance, its band; the edge fade's start and end
  vec4 drive;
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
  vec4 paint_two_tone;
  vec4 paint_finish;
  vec4 damage_dirt;
  vec4 damage_zones;
  vec4 car_sun_color;
  mat4 shadow_matrix_mid;  // Mid cascade orthographic sun shadow matrix (~90m)
  vec4 shadow_mid_bias;    // x mid bias, y 3-cascade (>0.5), z pcss (0/1/2), w cascades (2/3)
  vec4 phase_b_params;     // x car_detail, y rotor_glow, z frame_jitter, w ssr_active
  vec4 ambient_color;      // rgb TimeOfDayA AmbientColour (prop ambient), w ibl_ambient_scale
} frame;
layout(set = 0, binding = 1) uniform samplerCube environment;
layout(set = 0, binding = 2) uniform sampler2DShadow shadow_map;
layout(set = 0, binding = 3) uniform sampler2D headlight_cookie;
layout(set = 0, binding = 11) uniform sampler2D shadow_depth_raw;

struct HeadlightBeam {
  mat4 proj;          // render-space pos -> headlight clip space
  vec4 origin_range;  // xyz origin in render space, w range (m)
  vec4 dir_power;     // xyz forward unit dir, w intensity
  vec4 color;         // rgb tint (xenon/halogen/HID), w unused
};
layout(set = 0, binding = 4) uniform Headlights {
  vec4 count_night;   // x active beam count (0..16), y night_scale (0..1), z time_sec, w unused
  HeadlightBeam beams[16];
} headlights;

#ifdef GPU_DRIVEN
// Bindless (world_gpu.cpp): one shared sampler + array of sampled images
// (split descriptors keep per-stage sampler count at 1 for MoltenVK/Android).
layout(set = 1, binding = 0) uniform sampler world_sampler;
layout(set = 1, binding = 1) uniform texture2D textures[];
struct DrawGpu {
  vec4 tint;
  vec4 params;
  uvec4 tex0;  // albedo, layer_b, splat, light_map
  uvec4 tex1;  // normal_map, ao_map, layer_c, bit 0: fades in with the swap
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
  vec4 swap_pad;
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
uvec4 g_tex0, g_tex1;
#define albedo sampler2D(textures[nonuniformEXT(g_tex0.x)], world_sampler)
#define layer_b sampler2D(textures[nonuniformEXT(g_tex0.y)], world_sampler)
#define splat_map sampler2D(textures[nonuniformEXT(g_tex0.z)], world_sampler)
#define light_map sampler2D(textures[nonuniformEXT(g_tex0.w)], world_sampler)
#define normal_map sampler2D(textures[nonuniformEXT(g_tex1.x)], world_sampler)
#define ao_map sampler2D(textures[nonuniformEXT(g_tex1.y)], world_sampler)
#define layer_c sampler2D(textures[nonuniformEXT(g_tex1.z)], world_sampler)
#define spec_map sampler2D(textures[nonuniformEXT(g_tex1.w >> 1u)], world_sampler)
layout(location = 6) flat in uint v_draw;
#else
layout(set = 1, binding = 0) uniform sampler2D albedo;
layout(set = 1, binding = 1) uniform sampler2D layer_b;     // Blend_B
layout(set = 1, binding = 2) uniform sampler2D splat_map;   // Splat
layout(set = 1, binding = 3) uniform sampler2D light_map;   // Light_Map (night emissive spill)
layout(set = 1, binding = 4) uniform sampler2D normal_map;  // NormalMap (DXN/BC5, Sub-Milestone 3)
layout(set = 1, binding = 5) uniform sampler2D ao_map;      // AO / AOMap (Sub-Milestone 3)
layout(set = 1, binding = 6) uniform sampler2D layer_c;     // Blend_C (Sub-Milestone 3)
layout(set = 1, binding = 7) uniform sampler2D spec_map;    // SpecularMap

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 tint;
  vec4 params;
} draw;
#endif

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in vec4 v_colour;
layout(location = 5) flat in vec2 v_fade;  // the instance's fade weights (world.vert)
layout(location = 7) in vec2 v_uv2;        // TEXCOORD2 lightmap / AO UV
layout(location = 8) flat in float v_emissive_thresh;

layout(location = 0) out vec4 out_color;

const uint kOpaque = 0u, kCoverage = 1u, kAlpha = 2u, kAdditive = 3u;
const uint kFlagTextured = 1u, kFlagSky = 2u, kFlagVertexBlend = 4u, kFlagSplat = 8u, kFlagVertexFade = 16u,
           kFlagBillboard = 32u, kFlagAlphaTest = 64u, kFlagLodNear = 128u, kFlagLodFar = 256u, kFlagLodOne = 512u,
           kFlagUnlitVertexColor = 1024u, kFlagLightMap = 2048u, kFlagNormalMap = 4096u, kFlagAoMap = 8192u,
           kFlagLayerC = 16384u, kFlagWater = 32768u, kFlagVertexLit = 65536u, kFlagAnimFlag = 131072u,
           kFlagSpecMap = 524288u,
           kFlagRoad = 1048576u, kFlagDecalNoise = 2097152u, kFlagWaterNormalB = 4194304u,
           kFlagEmissiveMap = 8388608u;
const uint kFlagRoadNoise = kFlagDecalNoise;       // bit 21 when kFlagRoad is set (splat_map = H_ROAD Noise)
const uint kFlagRoadModulate = kFlagWaterNormalB;  // bit 22 when kFlagRoad is set (spec_map = H_ROAD Modulate)

// The screen-door threshold of this pixel (interleaved gradient noise): one
// pattern shared by every draw, so fades that split a pixel between two
// draws by it leave neither a gap nor a double (ours).
float dither_threshold() {
  return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

// The sky gradient (ours, linear): pale haze at the horizon to blue above.
const vec3 kSkyHorizon = vec3(0.42, 0.56, 0.72);
const vec3 kSkyZenith = vec3(0.05, 0.16, 0.50);

const float kFogNear = 200.0, kFogFar = 4000.0, kFogMax = 0.9;

// The frame's lighting constants (daytime free-roam dump).
const vec3 kSunColour = vec3(0.9695, 0.8358, 0.584);
const vec3 kSkyColour = vec3(0.1687, 0.2229, 0.2746);
const vec3 kGroundColour = vec3(0.1788, 0.1729, 0.1024);

// The sky (ours, until a sky spec): the gradient, a warm glow and a disc
// around the sun, and two layers of value-noise clouds on a plane above
// the eye, thinning towards the horizon.
float sky_hash(vec2 p) {  // no sin: stable at large coordinates
  vec3 q = fract(vec3(p.xyx) * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return fract((q.x + q.y) * q.z);
}
float sky_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(sky_hash(i), sky_hash(i + vec2(1, 0)), f.x), mix(sky_hash(i + vec2(0, 1)), sky_hash(i + vec2(1, 1)), f.x),
             f.y);
}
float sky_fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 5; ++i) {
    v += a * sky_noise(p);
    p = p * 2.03 + vec2(17.1, 9.2);
    a *= 0.5;
  }
  return v;
}
vec3 sky_colour(vec3 dir, vec3 up, vec3 sun) {
  float h = dot(dir, up);
  float hc = clamp(h, 0.0, 1.0);
  float toward = max(dot(dir, sun), 0.0);
  if (frame.sun_color.w >= 0.0) {
    // Dynamic Time-of-Day sky reflection & fallback (SPEC-SKY-AND-TOD §§3-4):
    // scales horizon, zenith, sun scatter, and clouds by actual TOD luminance and sun elevation
    // so midnight lakes and cockpit rearview mirrors reflect dark starlit night sky instead of noon cyan.
    float sun_elev = dot(sun, up);
    float day_vis = smoothstep(-0.08, 0.16, sun_elev);
    float night_amt = clamp(frame.sky_color.w, 0.0, 1.0);
    vec3 tod_horizon = mix(frame.fog_color.rgb * 1.45, frame.sky_color.rgb * 0.72, night_amt * 0.55);
    vec3 tod_zenith = frame.sky_color.rgb * mix(1.25, 0.45, night_amt);
    vec3 sky = mix(tod_horizon, tod_zenith, pow(hc, 0.55));
    // Warm horizon in-scattering only while sun is near or above horizon.
    sky = mix(sky, frame.fog_scatter.rgb, pow(toward, 6.0) * (1.0 - hc) * 0.55 * day_vis);
    sky += frame.sun_color.rgb * (pow(toward, 64.0) * 0.55 + pow(toward, 900.0) * 5.0) * day_vis;
    if (h > 0.02) {
      vec3 side = normalize(abs(up.y) < 0.9 ? cross(up, vec3(0, 1, 0)) : cross(up, vec3(1, 0, 0)));
      vec3 fwd = cross(side, up);
      vec2 uv = vec2(dot(dir, side), dot(dir, fwd)) / (h + 0.08);
      uv = clamp(uv, vec2(-40.0), vec2(40.0));
      float c = sky_fbm(uv * 1.6);
      float cover = smoothstep(0.48, 0.78, c) * smoothstep(0.02, 0.22, h);
      vec3 cloud_lit = frame.sky_color.rgb * mix(1.4, 0.55, night_amt) +
                       frame.sun_color.rgb * (0.65 * max(sun_elev, 0.0) * day_vis);
      vec3 cloud = mix(cloud_lit * 0.65, cloud_lit * 1.15, smoothstep(0.5, 0.9, c));
      cloud += frame.sun_color.rgb * pow(toward, 8.0) * 0.30 * day_vis;
      sky = mix(sky, cloud, cover * mix(0.82, 0.30, night_amt));
    }
    return max(sky, vec3(0.0)) * (h < 0.0 ? 0.65 : 1.0);
  }
  vec3 sky = mix(kSkyHorizon, kSkyZenith, pow(hc, 0.55));
  // Haze warms towards the sun, low down.
  sky = mix(sky, vec3(0.80, 0.70, 0.55), pow(toward, 6.0) * (1.0 - hc) * 0.55);
  sky += kSunColour * (pow(toward, 64.0) * 0.6 + pow(toward, 900.0) * 6.0);
  if (h > 0.02) {  // none lower: the clouds thin out to nothing there
    // A plane 1 unit above: its point along dir, in the up frame.
    vec3 side = normalize(abs(up.y) < 0.9 ? cross(up, vec3(0, 1, 0)) : cross(up, vec3(1, 0, 0)));
    vec3 fwd = cross(side, up);
    vec2 uv = vec2(dot(dir, side), dot(dir, fwd)) / (h + 0.08);
    uv = clamp(uv, vec2(-40.0), vec2(40.0));
    float c = sky_fbm(uv * 1.6);
    float cover = smoothstep(0.48, 0.78, c) * smoothstep(0.02, 0.22, h);
    vec3 cloud = mix(vec3(0.62, 0.65, 0.70), vec3(1.05, 1.0, 0.94), smoothstep(0.5, 0.9, c));
    cloud += kSunColour * pow(toward, 8.0) * 0.35;
    sky = mix(sky, cloud, cover * 0.85);
  }
  return sky * (h < 0.0 ? 0.85 : 1.0);
}

// The drive renders linear HDR for its post chain (post.frag), clipped at
// 4.0 as the game's resolve is; the showroom encodes sRGB here.
layout(constant_id = 0) const bool kLinearOut = false;

// Distance haze (WORLD-MATERIALS, Lighting: FogConsts2 (200, 4000, 0.9) is
// start, end and the most fog; the ramp between and the colour are ours):
// an exponential rise from the start to most of the maximum at the end, towards the sky's own haze in
// that direction, so far scenery dissolves into the horizon, tinted warm
// towards the sun.
// The post chain's warm grade (showroom_main's kPostBalance, ours) is fitted
// to the lit ground and would tint the sky and haze pink: they are written
// pre-divided by it, so they reach the screen as designed (the drive only).
const vec3 kPostGrade = vec3(1.10, 0.88, 0.88);
vec3 sky_grade(vec3 c) { return kLinearOut ? c / kPostGrade : c; }

vec3 haze_colour(vec3 dir, vec3 up, vec3 sun) {
  float hc = clamp(dot(dir, up), 0.0, 1.0);
  vec3 c = mix(kSkyHorizon, kSkyZenith, pow(hc, 0.55));
  float toward = max(dot(dir, sun), 0.0);
  return sky_grade(mix(c, vec3(0.80, 0.70, 0.55), pow(toward, 6.0) * (1.0 - hc) * 0.55));
}
vec3 apply_haze(vec3 colour, vec3 position, vec3 up, vec3 sun) {
  vec3 to = position - frame.eye.xyz;
  float d = length(to);
  vec3 view_dir = to / max(d, 1e-3);
  if (frame.sun_color.w >= 0.0) {
    // Dynamic TOD atmospheric fog (SPEC-SKY-AND-TOD §4): exponential distance +
    // altitude smoothstep fade + Mie-like sun in-scattering.
    float fog_start = frame.fog_color.w;
    float fog_dens = max(frame.fog_params.x, 0.0);
    float t_dist = exp2(-max(d - fog_start, 0.0) * fog_dens);
    float fog_dist = 1.0 - t_dist;
    float world_y = dot(to, up) + frame.car_light_bot.w;
    float h_span = max(frame.fog_params.z - frame.fog_params.y, 1.0);
    float h_u = clamp((world_y - frame.fog_params.y) / h_span, 0.0, 1.0);
    float h_smooth = h_u * h_u * (3.0 - 2.0 * h_u);
    // Raise the maximum fog cap smoothly from 0.92 toward 1.0 across the far-ring boundary so
    // distant UberLOD/MIDDIST mountain silhouettes merge cleanly into the sky horizon fog skirt.
    float max_fog = mix(0.92, 1.0, smoothstep(3200.0, 7500.0, d));
    float fog_amt = clamp(fog_dist * (1.0 - h_smooth * frame.fog_params.w), 0.0, max_fog);
    float far_edge = smoothstep(6500.0, 11500.0, d);
    fog_amt = max(fog_amt, far_edge);
    float sc_dist = clamp(d / 8000.0, 0.0, 1.0);
    float sc_ang = pow(clamp(dot(view_dir, sun), 0.0, 1.0), max(frame.fog_scatter.w, 1.0));
    vec3 fog_col = mix(frame.fog_color.rgb, frame.fog_scatter.rgb, sc_dist * sc_ang);
    return colour * (1.0 - fog_amt) + fog_amt * fog_col;
  }
  // Legacy fallback when TOD is not set.
  float f = kFogMax * (1.0 - exp(-max(d - kFogNear, 0.0) / ((kFogFar - kFogNear) * 0.47)));
  return mix(colour, haze_colour(view_dir, up, sun), f);
}

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 out_rgb(vec3 c) { return kLinearOut ? min(max(c, vec3(0.0)), vec3(64.0)) : encode_srgb(c); }

// 3-cascade stabilized D32 CSM + Contact-Hardening PCSS (Near ~20m, Mid ~90m, Far ~320m)
// with fast 2-cascade PCF fallback on low/low-power presets.
const vec2 kPcssDisk[16] = vec2[16](
    vec2(-0.613392,  0.617481), vec2( 0.170019, -0.040254),
    vec2(-0.299417,  0.791925), vec2( 0.645680,  0.493210),
    vec2(-0.651784,  0.717887), vec2( 0.421003,  0.027070),
    vec2(-0.817194, -0.271096), vec2(-0.705374, -0.668203),
    vec2( 0.977050, -0.108615), vec2( 0.063326,  0.142369),
    vec2( 0.203528,  0.214331), vec2(-0.667531,  0.326090),
    vec2(-0.098422, -0.295755), vec2(-0.885922,  0.215369),
    vec2( 0.566637,  0.605213), vec2( 0.039766, -0.396100));

float sample_cascade_pcf(vec4 s, float x_off, float inv_cascades) {
  vec2 uv = vec2((s.x * 0.5 + 0.5) * inv_cascades + x_off, s.y * 0.5 + 0.5);
  float u_lo = x_off + frame.shadow.y * inv_cascades;
  float u_hi = x_off + inv_cascades - frame.shadow.y * inv_cascades;
  if (frame.shadow.w > 1.5) {
    float tx = frame.shadow.y * inv_cascades * 0.5;
    float ty = frame.shadow.y * 0.5;
    vec2 uv0 = vec2(clamp(uv.x - tx, u_lo, u_hi), clamp(uv.y - ty, 0.0005, 0.9995));
    vec2 uv1 = vec2(clamp(uv.x + tx, u_lo, u_hi), clamp(uv.y - ty, 0.0005, 0.9995));
    vec2 uv2 = vec2(clamp(uv.x - tx, u_lo, u_hi), clamp(uv.y + ty, 0.0005, 0.9995));
    vec2 uv3 = vec2(clamp(uv.x + tx, u_lo, u_hi), clamp(uv.y + ty, 0.0005, 0.9995));
    return 0.25 * (texture(shadow_map, vec3(uv0, s.z)) + texture(shadow_map, vec3(uv1, s.z)) +
                   texture(shadow_map, vec3(uv2, s.z)) + texture(shadow_map, vec3(uv3, s.z)));
  }
  return texture(shadow_map, vec3(vec2(clamp(uv.x, u_lo, u_hi), uv.y), s.z));
}

float sample_cascade_pcss(vec4 s, float x_off, float inv_cascades, float pcss_mode, float cascade_scale) {
  if (pcss_mode < 0.5) return sample_cascade_pcf(s, x_off, inv_cascades);
  vec2 uv = vec2((s.x * 0.5 + 0.5) * inv_cascades + x_off, s.y * 0.5 + 0.5);
  float texel_x = frame.shadow.y * inv_cascades;
  float texel_y = frame.shadow.y;
  float u_lo = x_off + texel_x;
  float u_hi = x_off + inv_cascades - texel_x;

  // IGN-rotated Poisson disk eliminates banding without temporal flicker.
  float ang = 6.2831853 * fract(dither_threshold() + frame.phase_b_params.z * 0.61803399);
  float ca = cos(ang), sa = sin(ang);
  mat2 rot = mat2(ca, -sa, sa, ca);

  // 1. Blocker search on raw D32 shadow depth (center + 8 rotated Poisson taps).
  float search_r = (pcss_mode > 1.5 ? 6.5 : 5.0) * cascade_scale;
  vec2 search_uv = vec2(texel_x, texel_y) * search_r;
  float blocker_sum = 0.0;
  int blockers = 0;
  float d_center = texture(shadow_depth_raw, vec2(clamp(uv.x, u_lo, u_hi), clamp(uv.y, 0.0005, 0.9995))).r;
  if (d_center < s.z - 0.00020) {
    blocker_sum += d_center;
    ++blockers;
  }
  for (int i = 0; i < 8; ++i) {
    vec2 o = (rot * kPcssDisk[i * 2]) * search_uv;
    vec2 suv = vec2(clamp(uv.x + o.x, u_lo, u_hi), clamp(uv.y + o.y, 0.0005, 0.9995));
    float d = texture(shadow_depth_raw, suv).r;
    if (d < s.z - 0.00025) {
      blocker_sum += d;
      ++blockers;
    }
  }
  if (blockers == 0) {
    return texture(shadow_map, vec3(vec2(clamp(uv.x, u_lo, u_hi), clamp(uv.y, 0.0005, 0.9995)), s.z));
  }

  // 2. Contact-hardening penumbra radius (orthographic depth span = 2 * 520 m = 1040 m).
  float avg_blocker = blocker_sum / float(blockers);
  float blocker_dist_m = max(s.z - avg_blocker, 0.0) * 1040.0;
  float max_r = pcss_mode > 1.5 ? 8.5 : 6.0;
  float penumbra = clamp(blocker_dist_m * 0.62 * cascade_scale, 0.85, max_r);
  vec2 filter_uv = vec2(texel_x, texel_y) * penumbra;

  // 3. Variable-radius IGN-rotated Poisson PCF (12-tap High / 16-tap Ultra).
  int taps = pcss_mode > 1.5 ? 16 : 12;
  float lit = 0.0;
  for (int i = 0; i < taps; ++i) {
    vec2 o = (rot * kPcssDisk[i]) * filter_uv;
    vec2 suv = vec2(clamp(uv.x + o.x, u_lo, u_hi), clamp(uv.y + o.y, 0.0005, 0.9995));
    lit += texture(shadow_map, vec3(suv, s.z));
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

float sun_shadow(vec3 p, vec3 n) {
  if (frame.shadow.x < 0.5) return 1.0;
  bool three_c = frame.shadow_mid_bias.y > 0.5;
  float inv_c = three_c ? (1.0 / 3.0) : 0.5;
  float pcss_mode = frame.shadow_mid_bias.z;

  vec4 sn = frame.shadow_matrix * vec4(p + n * frame.shadow_bias.x, 1.0);
  float edge_n = max(abs(sn.x), abs(sn.y));
  bool near_ok = edge_n < 1.0 && sn.z > 0.0 && sn.z < 1.0;
  float lit_n = near_ok ? sample_cascade_pcss(sn, 0.0, inv_c, pcss_mode, 1.0) : 1.0;

  if (three_c) {
    if (near_ok && edge_n < 0.80) return clamp(lit_n, 0.0, 1.0);
    vec4 sm = frame.shadow_matrix_mid * vec4(p + n * frame.shadow_mid_bias.x, 1.0);
    float edge_m = max(abs(sm.x), abs(sm.y));
    bool mid_ok = edge_m < 1.0 && sm.z > 0.0 && sm.z < 1.0;
    float lit_m = mid_ok ? sample_cascade_pcss(sm, inv_c, inv_c, pcss_mode, 0.35) : 1.0;
    if (near_ok) {
      float bn = smoothstep(0.80, 0.98, edge_n);
      return clamp(mix(lit_n, lit_m, bn), 0.0, 1.0);
    }
    if (mid_ok && edge_m < 0.80) return clamp(lit_m, 0.0, 1.0);
    vec4 sf = frame.shadow_matrix_far * vec4(p + n * frame.shadow_far_bias.x, 1.0);
    float edge_f = max(abs(sf.x), abs(sf.y));
    float lit_f = 1.0;
    if (edge_f < 1.0 && sf.z > 0.0 && sf.z < 1.0) {
      lit_f = mix(sample_cascade_pcf(sf, 2.0 * inv_c, inv_c), 1.0, smoothstep(0.84, 1.0, edge_f));
    }
    float bm = mid_ok ? smoothstep(0.80, 0.98, edge_m) : 1.0;
    return clamp(mix(lit_m, lit_f, bm), 0.0, 1.0);
  }

  if (frame.shadow_far_bias.y > 0.5) {
    if (near_ok && edge_n < 0.78) return clamp(lit_n, 0.0, 1.0);
    vec4 sf = frame.shadow_matrix_far * vec4(p + n * frame.shadow_far_bias.x, 1.0);
    float edge_f = max(abs(sf.x), abs(sf.y));
    float lit_f = 1.0;
    if (edge_f < 1.0 && sf.z > 0.0 && sf.z < 1.0) {
      lit_f = mix(sample_cascade_pcf(sf, 0.5, 0.5), 1.0, smoothstep(0.82, 1.0, edge_f));
    }
    float blend = near_ok ? smoothstep(0.78, 0.98, edge_n) : 1.0;
    return clamp(mix(lit_n, lit_f, blend), 0.0, 1.0);
  }
  if (!near_ok) return 1.0;
  return clamp(mix(lit_n, 1.0, smoothstep(0.8, 1.0, edge_n)), 0.0, 1.0);
}

// Cotangent-frame tangent-space normal perturbation from DXN/BC5 normal map slope dxy in [-1, 1].
vec3 perturb_normal_dxy(vec3 n, vec3 pos, vec2 uv, vec2 dxy) {
  float nz = sqrt(max(1.0 - dot(dxy, dxy), 0.01));
  vec3 dp1 = dFdx(pos);
  vec3 dp2 = dFdy(pos);
  vec2 duv1 = dFdx(uv);
  vec2 duv2 = dFdy(uv);
  vec3 dp2perp = cross(dp2, n);
  vec3 dp1perp = cross(n, dp1);
  vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
  vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
  float invmax = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-8);
  if (invmax > 1e3) return n;
  return normalize(T * (dxy.x * invmax) + B * (dxy.y * invmax) + n * nz);
}

vec3 perturb_normal(vec3 n, vec3 pos, vec2 uv, vec2 nm_xy) {
  return perturb_normal_dxy(n, pos, uv, nm_xy * 2.0 - 1.0);
}

vec3 render_to_game_dir(vec3 d) {
  return normalize(vec3(dot(d, frame.env_basis_x.xyz), dot(d, frame.env_basis_y.xyz), dot(d, frame.env_basis_z.xyz)));
}

// Mikkelsen (JCGT 2022) Practical Real-Time Hex-Tiling with analytical derivative rotation
// (preserves 16x anisotropic filtering at grazing angles while breaking up macro terrain repetition).
vec2 hex_hash22(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.xx + p3.yz) * p3.zy);
}

mat2 hex_rot(float a) {
  float c = cos(a), s = sin(a);
  return mat2(c, -s, s, c);
}

struct HexTileSample {
  vec2 uv1, uv2, uv3;
  vec2 dx1, dy1, dx2, dy2, dx3, dy3;
  mat2 r1, r2, r3;
  vec3 w;
};

HexTileSample hex_tile_setup(vec2 uv) {
  vec2 ddx = dFdx(uv);
  vec2 ddy = dFdy(uv);

  // Skew UV onto simplex/hexagonal lattice (side length ~1.0 in UV tile space).
  const float kScale = 0.65;
  vec2 st = uv * kScale;
  mat2 to_grid = mat2(1.0, 0.0, -0.57735027, 1.15470054);
  vec2 g = to_grid * st;
  vec2 gi = floor(g);
  vec2 gf = fract(g);

  vec2 v1, v2, v3;
  float w1, w2, w3;
  if (gf.x > gf.y) {
    v1 = gi;
    v2 = gi + vec2(1.0, 0.0);
    v3 = gi + vec2(1.0, 1.0);
    w1 = 1.0 - gf.x;
    w2 = gf.x - gf.y;
    w3 = gf.y;
  } else {
    v1 = gi;
    v2 = gi + vec2(0.0, 1.0);
    v3 = gi + vec2(1.0, 1.0);
    w1 = 1.0 - gf.y;
    w2 = gf.y - gf.x;
    w3 = gf.x;
  }

  vec2 h1 = hex_hash22(v1);
  vec2 h2 = hex_hash22(v2);
  vec2 h3 = hex_hash22(v3);

  mat2 r1 = hex_rot(h1.x * 6.2831853);
  mat2 r2 = hex_rot(h2.x * 6.2831853);
  mat2 r3 = hex_rot(h3.x * 6.2831853);

  HexTileSample s;
  s.uv1 = r1 * uv + h1; s.dx1 = r1 * ddx; s.dy1 = r1 * ddy; s.r1 = r1;
  s.uv2 = r2 * uv + h2; s.dx2 = r2 * ddx; s.dy2 = r2 * ddy; s.r2 = r2;
  s.uv3 = r3 * uv + h3; s.dx3 = r3 * ddx; s.dy3 = r3 * ddy; s.r3 = r3;
  s.w = pow(max(vec3(w1, w2, w3), vec3(0.0)), vec3(5.0));
  return s;
}

vec3 hex_tile_weights(vec4 c1, vec4 c2, vec4 c3, vec3 base_w) {
  // Contrast-preserving soft-max weights prevent muddy grey blending across hex borders.
  vec3 lum = vec3(dot(c1.rgb, vec3(0.299, 0.587, 0.114)),
                  dot(c2.rgb, vec3(0.299, 0.587, 0.114)),
                  dot(c3.rgb, vec3(0.299, 0.587, 0.114)));
  vec3 w = base_w * exp2(lum * 2.0);
  return w / max(w.x + w.y + w.z, 1e-5);
}

vec4 hex_tile_combine(vec4 c1, vec4 c2, vec4 c3, vec3 base_w) {
  vec3 w = hex_tile_weights(c1, c2, c3, base_w);
  return c1 * w.x + c2 * w.y + c3 * w.z;
}

#define SAMPLE_HEX_TILE_STATE(TEX, UV, HS, W_OUT, DST)                                                      \
  do {                                                                                                      \
    HS = hex_tile_setup(UV);                                                                                \
    vec4 _c1 = textureGrad(TEX, HS.uv1, HS.dx1, HS.dy1);                                                    \
    vec4 _c2 = textureGrad(TEX, HS.uv2, HS.dx2, HS.dy2);                                                    \
    vec4 _c3 = textureGrad(TEX, HS.uv3, HS.dx3, HS.dy3);                                                    \
    W_OUT = hex_tile_weights(_c1, _c2, _c3, HS.w);                                                          \
    DST = _c1 * W_OUT.x + _c2 * W_OUT.y + _c3 * W_OUT.z;                                                    \
  } while (false)

#define SAMPLE_HEX_TILE(TEX, UV, DST)                                                                       \
  do {                                                                                                      \
    HexTileSample _hs;                                                                                      \
    vec3 _hw;                                                                                               \
    SAMPLE_HEX_TILE_STATE(TEX, UV, _hs, _hw, DST);                                                          \
  } while (false)

#define SAMPLE_HEX_TILE_NORMAL_DXY(TEX, HS, W, DXY_OUT)                                                     \
  do {                                                                                                      \
    vec2 _n1 = transpose(HS.r1) * (textureGrad(TEX, HS.uv1, HS.dx1, HS.dy1).xy * 2.0 - 1.0);                \
    vec2 _n2 = transpose(HS.r2) * (textureGrad(TEX, HS.uv2, HS.dx2, HS.dy2).xy * 2.0 - 1.0);                \
    vec2 _n3 = transpose(HS.r3) * (textureGrad(TEX, HS.uv3, HS.dx3, HS.dy3).xy * 2.0 - 1.0);                \
    DXY_OUT = _n1 * W.x + _n2 * W.y + _n3 * W.z;                                                            \
  } while (false)

#define SAMPLE_HEX_TILE_WEIGHTED(TEX, HS, W, DST)                                                           \
  do {                                                                                                      \
    vec4 _s1 = textureGrad(TEX, HS.uv1, HS.dx1, HS.dy1);                                                    \
    vec4 _s2 = textureGrad(TEX, HS.uv2, HS.dx2, HS.dy2);                                                    \
    vec4 _s3 = textureGrad(TEX, HS.uv3, HS.dx3, HS.dy3);                                                    \
    DST = _s1 * W.x + _s2 * W.y + _s3 * W.z;                                                                \
  } while (false)

void main() {
#ifdef GPU_DRIVEN
  draw.model = push.model;
  draw.sun = push.sun;
  draw.up = push.up;
  draw.tint = items[v_draw].tint;
  draw.params = items[v_draw].params;
  g_tex0 = items[v_draw].tex0;
  g_tex1 = items[v_draw].tex1;
  // Draws new to this scenery fade in by the swap's progress; the rest are as they were (2: none).
  draw.sun.w = (g_tex1.w & 1u) != 0u ? push.swap_pad.x : 2.0;
#endif
  uint blend = uint(draw.params.x + 0.5);
  // Round-to-nearest without +0.5 so odd flag masks >= 2^23 (kFlagEmissiveMap | kFlagTextured)
  // decode bit-for-bit identically without flipping bit 0 into kFlagSky (bit 1).
  uint flags = uint(round(draw.params.y));
  if ((flags & kFlagSky) != 0u) {
    out_color = vec4(out_rgb(sky_grade(sky_colour(normalize(v_position), normalize(draw.up.xyz), normalize(draw.sun.xyz)))), 1.0);
    return;
  }
#ifndef OPAQUE_NO_DISCARD
  // Cross-fades by the screen door (ours). A swapped-in scenery takes the
  // pixels under t, the one it replaces the rest (sun.w: 2 none, t, 3 + t).
  // A prop's LOD00 gives way to its LOD02 over the LOD band, and the far
  // levels fade out before the gather's edge, by distance from the eye.
  float door = dither_threshold();
  float swap = draw.sun.w;
  if (swap < 1.5 ? door >= swap : swap > 2.5 && door < swap - 3.0) discard;
  if ((flags & (kFlagLodNear | kFlagLodFar | kFlagLodOne)) != 0u && frame.world_fade.y > 0.0) {
    float s = v_fade.x, e = v_fade.y;
    if ((flags & kFlagLodNear) != 0u ? door < s : door < e || ((flags & kFlagLodFar) != 0u && door >= s)) discard;
  } else if ((flags & kFlagBillboard) != 0u && frame.world_fade.y > 0.0) {
    // Complementary 3D-to-billboard distance dither cross-fade band (v_fade.x = fade-in, v_fade.y = far fade-out)
    // using the exact same IGN door threshold as 3D tree props for seamless zero-gap transitions.
    if (door >= v_fade.x || door < v_fade.y) discard;
  }
#endif
  // Terrain layers (WORLD-MATERIALS, Terrain shading): tint holds the
  // tilings, Blend_A's in xy and Blend_B's in zw; the weight is the vertex
  // colour's R (H_VBLND, baked from c4) or Splat's R (H_BLND2/3, uv1
  // pre-tiled). Blend_C is not drawn (its weight is not settled).
  // Unlit premultiplied-alpha vertex colour overlay (e.g. 3D driving/braking line ribbon),
  // OR animated unlit emissive LED gantry countdown / 8x2 flipbook display when kFlagTextured is set.
  if ((flags & kFlagUnlitVertexColor) != 0u) {
    if ((flags & kFlagTextured) != 0u) {
      vec3 up_led = normalize(draw.up.xyz);
      vec3 sun_led = normalize(draw.sun.xyz);
      float t_led = max(frame.env_basis_x.w, 0.0);
      float mask_rep = clamp(draw.tint.z, 4.0, 128.0);
      vec2 anim_uv = v_uv0;
      float emissive_boost = 1.35;
      if (draw.tint.w < 1.5) {
        // 8-row vertical race start gantry countdown atlas:
        // rows 0..4 = 0:05..0:01, rows 5..6 = green GO!, row 7 = Horizon Festival TV.
        // Synced to the 6.0 s race start sequence (vehicle.time() starts at 0.0 on race entry).
        int row_idx = 7;
        float beat = 1.0;
        if (t_led < 1.5) {
          row_idx = 0;
          beat = 1.0 + 0.30 * exp(-fract(t_led / 1.5) * 4.0);
        } else if (t_led < 3.0) {
          row_idx = 1;
          beat = 1.0 + 0.30 * exp(-fract((t_led - 1.5) / 1.5) * 4.0);
        } else if (t_led < 4.0) {
          row_idx = 2;  // 0:03
          beat = 1.05 + 0.38 * exp(-fract(t_led) * 4.5);
        } else if (t_led < 5.0) {
          row_idx = 3;  // 0:02
          beat = 1.10 + 0.42 * exp(-fract(t_led) * 4.5);
        } else if (t_led < 6.0) {
          row_idx = 4;  // 0:01
          beat = 1.15 + 0.48 * exp(-fract(t_led) * 4.5);
        } else if (t_led < 10.5) {
          // Energetic pulsing green GO banner for 4.5 s after launch.
          row_idx = 5 + int(floor(fract(t_led * 2.5) * 2.0));
          beat = 1.25 + 0.30 * sin(t_led * 15.707963);
        } else {
          // Live race / free-roam state: show Horizon Festival TV logo banner.
          row_idx = 7;
          beat = 0.95 + 0.08 * sin(t_led * 2.2);
        }
        anim_uv = vec2(v_uv0.x, fract(v_uv0.y + float(row_idx) * 0.125));
        emissive_boost = 1.55 * beat;
      } else {
        // 8x2 (16-frame) animated festival TV / billboard flipbook display.
        int frame_idx = int(mod(floor(t_led * 0.75), 16.0));
        int col_i = frame_idx % 8;
        int row_i = frame_idx / 8;
        anim_uv = vec2(fract(v_uv0.x + float(col_i) * 0.125), fract(v_uv0.y + float(row_i) * 0.5));
        emissive_boost = 1.30;
      }
      vec3 led_rgb = texture(albedo, anim_uv).rgb;
      if ((flags & kFlagVertexBlend) != 0u) {
        vec3 mask_rgb = texture(layer_b, v_uv0 * mask_rep).rgb;
        float mask_luma = clamp(dot(mask_rgb, vec3(0.3333)), 0.0, 1.0);
        led_rgb *= mix(0.62, 1.18, mask_luma);
      }
      vec3 led_col = led_rgb * emissive_boost * draw.up.w;
      led_col = apply_haze(led_col, v_position, up_led, sun_led);
      out_color = vec4(out_rgb(led_col), kLinearOut ? 0.0 : 1.0);
      return;
    }
    if (draw.params.w > 1.5) {
      // 1:1 Forza Horizon 1 3D driving/braking line:
      // Sleek continuous feathered translucent ribbon with subtle world-locked forward chevrons.
      // v_uv0.x = lateral coordinate u in [-1, 1], v_uv0.y = world-locked arc length s (meters).

      // Recover a single monotonic, continuous braking-heat scalar u_heat in [0, 1] from
      // drive::driving_line_color() (Green {44,147,44} -> Yellow {192,164,52} -> Red {192,39,52}),
      // staying smooth and artifact-free across GPU-interpolated quads.
      float t_lo = clamp((v_colour.r - 0.17255) / (0.75294 - 0.17255), 0.0, 1.0);
      float g_expected = 0.57647 + 0.06667 * t_lo;
      float t_hi = clamp((g_expected - v_colour.g) / (0.64314 - 0.15294), 0.0, 1.0);
      float u_heat = 0.5 * (t_lo + t_hi);

      // Perceptually smooth 4-stop sRGB gradient (converted to linear afterward so yellow,
      // amber-orange, and crimson red never snap abruptly under gamma encoding):
      //   0.00 = crisp translucent FH1 emerald-teal throttle
      //   0.48 = warm golden yellow coast/lift
      //   0.74 = rich amber-orange early braking
      //   1.00 = vivid crimson red full braking
      vec3 srgb_green  = vec3(0.16, 0.74, 0.42);
      vec3 srgb_yellow = vec3(0.95, 0.80, 0.18);
      vec3 srgb_orange = vec3(0.97, 0.45, 0.10);
      vec3 srgb_red    = vec3(0.92, 0.13, 0.15);

      vec3 srgb_col = mix(srgb_green, srgb_yellow, smoothstep(0.00, 0.48, u_heat));
      srgb_col = mix(srgb_col, srgb_orange, smoothstep(0.46, 0.76, u_heat));
      srgb_col = mix(srgb_col, srgb_red,    smoothstep(0.72, 1.00, u_heat));

      const float kChevronPitch = 2.6;   // meters between successive chevrons (matches showroom_draw.cpp)
      const float kChevronSweep = 0.78;  // sleek shallow V-wing sweep matched to 0.38 m half-width
      float au = abs(v_uv0.x);
      float chev_s = v_uv0.y + au * kChevronSweep;
      float cell_u = chev_s / kChevronPitch;
      float phase = fract(cell_u);

      float aa_s = max(fwidth(cell_u), 0.012);
      float aa_u = max(fwidth(v_uv0.x), 0.014);

      // Sleek feathered continuous translucent ribbon base (no hard rectangular edges).
      float lat_ribbon = 1.0 - smoothstep(0.48 - aa_u, 0.94 + aa_u, au);
      float spine_soft = 1.0 - smoothstep(0.0, 0.65, au);

      // Crisp, understated forward-pointing directional chevron embedded inside the ribbon.
      float wing_taper = au * 0.065;
      float back_edge = smoothstep(0.54 + wing_taper - aa_s * 0.85, 0.59 + wing_taper + aa_s * 0.85, phase);
      float front_edge = 1.0 - smoothstep(0.82 - aa_s * 0.85, 0.87 + aa_s * 0.85, phase);
      float chev_raw = back_edge * front_edge;
      // Smoothly filter chevrons in the far distance so arrows dissolve cleanly into the ribbon.
      float far_filter = smoothstep(0.18, 0.48, aa_s);
      float chev_mask = mix(chev_raw, 0.28, far_filter);
      float chev_lat = 1.0 - smoothstep(0.62 - aa_u, 0.90 + aa_u, au);
      float chev = chev_mask * chev_lat;

      // Subtle leading-edge chevron sheen (refined, never blown-out neon).
      float lead_rim = smoothstep(0.68, 0.83, phase) * front_edge * (1.0 - far_filter) * chev_lat;
      float highlight = clamp(lead_rim * 0.34 + chev * 0.14 + spine_soft * 0.08, 0.0, 0.45);
      vec3 srgb_lit = mix(srgb_col, min(srgb_col + vec3(0.15, 0.15, 0.13), vec3(1.0)), highlight);
      vec3 lin_col = pow(max(srgb_lit, vec3(0.0)), vec3(2.2));
      float tod_gain = mix(1.0, 0.74, clamp(frame.sky_color.w, 0.0, 1.0));

      // Continuous translucent ribbon + slightly stronger directional chevrons; braking zones
      // gain modest extra presence over calm throttle segments for immediate readability.
      float brake_presence = mix(1.0, 1.18, smoothstep(0.45, 0.90, u_heat));
      float ribbon_a = lat_ribbon * (0.26 + 0.08 * spine_soft);
      float chev_a = chev * 0.38;
      float shape_a = clamp((ribbon_a + chev_a) * brake_presence, 0.0, 0.78);
      float a = shape_a * v_colour.a;
      vec3 encoded = a > 0.004 ? out_rgb(lin_col * draw.up.w * tod_gain) : vec3(0.0);
      out_color = vec4(encoded * a, a);
      return;
    }
    vec3 c = pow(v_colour.rgb, vec3(2.2));
    vec3 encoded = out_rgb(c * draw.up.w);
    float a = v_colour.a;
    out_color = vec4(encoded * a, a);
    return;
  }
  vec3 up = normalize(draw.up.xyz);
  vec3 sun = normalize(draw.sun.xyz);
  vec3 view = normalize(frame.eye.xyz - v_position);
  float eye_dist = length(frame.eye.xyz - v_position);
  vec3 n_geom;
  if (dot(v_normal, v_normal) > 1e-6) {
    n_geom = normalize(v_normal);
  } else {
    // Stride-32 flag cloth meshes store no vertex normal; derive geometric face normal cleanly.
    vec3 fn = cross(dFdx(v_position), dFdy(v_position));
    n_geom = dot(fn, fn) > 1e-10 ? normalize(fn) : up;
  }
  // Two-sided cloth flags & alpha-cutout props flip their shading normal when viewed from behind.
  if ((blend == kCoverage || (flags & kFlagAnimFlag) != 0u) && (flags & kFlagBillboard) == 0u &&
      dot(n_geom, view) < 0.0) {
    n_geom = -n_geom;
  }
  vec3 sun_col = frame.sun_color.w >= 0.0 ? frame.sun_color.rgb : kSunColour;
  vec3 sky_col = frame.sun_color.w >= 0.0 ? frame.sky_color.rgb : kSkyColour;
  vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.ground_color.rgb : kGroundColour;
  float shadow_scale = frame.sun_color.w >= 0.0 ? clamp(frame.sun_color.w, 0.0, 1.0) : clamp(frame.shadow.z, 0.0, 1.0);

  // LAKE / WATER shader (kFlagWater):
  // - Dual-scrolling authored water normal maps (NormalMap A / B) driven by gentle directional
  //   river-current flow + 2-octave crisp analytical micro-ripple specular (replaces 26x sky_noise)
  // - Physically-based Schlick Fresnel (F0=0.02) & distance-adapted dynamic cubemap reflections
  // - Beer-Lambert Colorado alpine lake depth absorption & sunlit subsurface scatter
  // - Sparkling dual-lobe GGX sun highlights + smooth animated shoreline foam
  if ((flags & kFlagWater) != 0u) {
    float t_sec = frame.env_basis_x.w;
    float wind = clamp(max(frame.env_basis_y.w, 0.85), 0.65, 2.2);
    // Stable world-space horizontal coordinates in meters (passed via v_uv1 from world_vert).
    vec2 wp = v_uv1;
    // Exact horizontal world tangent frame in render space.
    vec3 t_x = normalize(frame.env_basis_x.xyz);
    vec3 t_z = normalize(frame.env_basis_z.xyz);

    float dist_atten = 1.0 / (1.0 + eye_dist * 0.010);
    float micro_atten = 1.0 / (1.0 + eye_dist * 0.048);

    // Gentle directional river-current / alpine breeze flow with subtle spatial meander.
    float meander = 0.24 * sin(wp.x * 0.014 + wp.y * 0.011);
    float f_ang = 0.60 + meander;
    vec2 flow_a = vec2(cos(f_ang), sin(f_ang));
    vec2 flow_b = vec2(-flow_a.y, flow_a.x);
    vec2 grad = vec2(0.0);
    float crest = 0.0;

    // Primary wave detail: dual-scrolling authored water normal maps when bound (or compact 2-wave analytical fallback).
    if ((flags & kFlagNormalMap) != 0u) {
      vec2 wuv_a = wp * 0.145 + flow_a * (t_sec * 0.044 * wind);
      vec2 wp_rot = mat2(0.80, -0.60, 0.60, 0.80) * wp;
      vec2 wuv_b = wp_rot * 0.225 + (flow_a * 0.42 + flow_b * 0.78) * (t_sec * 0.036 * wind);
      vec2 nm_a = texture(normal_map, wuv_a).xy * 2.0 - 1.0;
      vec2 nm_b = ((flags & kFlagWaterNormalB) != 0u)
                      ? (texture(layer_b, wuv_b).xy * 2.0 - 1.0)
                      : (texture(normal_map, wuv_b).xy * 2.0 - 1.0);
      grad += (nm_a * 0.21 + nm_b * 0.17) * dist_atten;
      crest += clamp(dot(nm_a, vec2(0.62, 0.78)) + dot(nm_b, vec2(-0.52, 0.74)), -1.0, 1.0) * 0.55;
    } else {
      // Compact 2-swell analytical fallback when a water material has no file-backed normal map.
      vec2 k0 = flow_a * 0.82 + flow_b * 0.28;
      vec2 k1 = flow_b * 1.18 - flow_a * 0.44;
      float s0 = dot(wp, k0) - t_sec * 1.05 * wind;
      float s1 = dot(wp, k1) - t_sec * 1.42 * wind;
      grad += (k0 * cos(s0) * 0.065 + k1 * cos(s1) * 0.052) * dist_atten;
      crest += (sin(s0) * 0.35 + sin(s1) * 0.25);
    }

    // 2-octave crisp analytical micro-ripples for lively capillary sun glints near the camera.
    vec2 m1_k = flow_a * 4.8 + flow_b * 2.1;
    vec2 m2_k = flow_b * 11.5 - flow_a * 6.4;
    float p1 = dot(wp, m1_k) - t_sec * (2.25 * wind);
    float p2 = dot(wp, m2_k) + t_sec * (3.45 * wind);
    grad += (m1_k * (cos(p1) * 0.0085) + m2_k * (cos(p2) * 0.0042)) * micro_atten;
    crest += (sin(p1) * 0.22 + sin(p2) * 0.14);

    vec3 nw = normalize(n_geom + (t_x * grad.x + t_z * grad.y));
    // Smooth Fresnel normal avoids harsh 0->1 flicker on individual capillary ripples.
    vec3 nw_fres = normalize(mix(n_geom, nw, 0.55));
    float nv = clamp(dot(nw_fres, view), 0.001, 1.0);

    // Physically-based Schlick Fresnel for air-water interface (IOR 1.333 -> F0 = 0.02).
    float fresnel = 0.02 + 0.98 * pow(1.0 - nv, 5.0);

    // Crisp dynamic cubemap reflections with smooth horizon lift so ripples never sample dark mud.
    vec3 nw_refl = normalize(mix(n_geom, nw, 0.78));
    vec3 r_dir = reflect(-view, nw_refl);
    float r_up = dot(r_dir, up);
    if (r_up < 0.06) {
      r_dir = normalize(r_dir + up * (0.06 - r_up));
    }
    float refl_mip = mix(0.25, 1.25, smoothstep(14.0, 220.0, eye_dist));
    vec3 refl;
    if (frame.shadow_far_bias.z > 0.5) {
      vec3 env_c = textureLod(environment, render_to_game_dir(r_dir), refl_mip).rgb;
      vec3 sky_r = sky_colour(r_dir, up, sun);
      // Softly blend toward sky near horizon angles so environment cubemap floor never dark-bands.
      float hor_lift = smoothstep(0.05, 0.22, dot(r_dir, up));
      refl = mix(mix(sky_r * 0.65, env_c, 0.65), env_c, hor_lift);
    } else {
      refl = sky_colour(r_dir, up, sun);
    }

    // Beer-Lambert Colorado mountain-lake depth absorption using vertex shoreline alpha.
    float shore_a = clamp(v_colour.a, 0.0, 1.0);
    // Estimate optical water depth: shallow near shore (0.20 m), deep open lake (6.5+ m).
    float depth_m = mix(0.20, 6.8, smoothstep(0.02, 0.85, shore_a)) / max(nv, 0.24);
    vec3 extinction = vec3(0.46, 0.15, 0.095);
    vec3 transmittance = exp(-depth_m * extinction);
    float sun_elev = dot(up, sun);
    float sun_above = smoothstep(-0.04, 0.14, sun_elev);
    // Scale lake body scattering by live TOD sun + sky irradiance (no hardcoded daylight floor)
    // so midnight auto-exposure (+4 EV) keeps lakes dark starlit indigo rather than glowing cyan.
    vec3 water_illum = max(sun_col * (max(sun_elev, 0.0) * sun_above * 0.72) + sky_col * 1.40, vec3(0.0015));
    vec3 shallow_tint = vec3(0.052, 0.172, 0.174) * water_illum + gnd_col * 0.09;
    vec3 deep_tint = vec3(0.011, 0.062, 0.104) * water_illum + sky_col * 0.10;
    vec3 body_col = mix(deep_tint, shallow_tint, transmittance);

    // Subtle subsurface turquoise scatter on sunlit wave crests (gated on sun above horizon).
    float sss_ang = pow(clamp(dot(view, -sun + nw * 0.35), 0.0, 1.0), 3.5);
    float crest_mask = clamp(crest * 0.5 + 0.5, 0.0, 1.0);
    body_col += vec3(0.016, 0.088, 0.092) * sun_col * (sss_ang * crest_mask * 0.32 * sun_above);

    // Sparkling dual-lobe GGX sun specular (sharp micro-ripple glints + soft solar reflection trail).
    vec3 h_w = normalize(view + sun);
    float nh = max(dot(nw, h_w), 0.0);
    float nl_w = max(dot(nw, sun), 0.0);
    float a1 = 0.026 * 0.026;
    float d_g1 = nh * nh * (a1 - 1.0) + 1.0;
    float ggx_glint = (a1 / max(3.14159265 * d_g1 * d_g1, 1e-5)) * 0.105;
    float a2 = 0.17 * 0.17;
    float d_g2 = nh * nh * (a2 - 1.0) + 1.0;
    float ggx_sheen = (a2 / max(3.14159265 * d_g2 * d_g2, 1e-5)) * 0.045;
    float shade_w = mix(1.0, sun_shadow(v_position, n_geom), shadow_scale);
    vec3 spec_col = sun_col * (ggx_glint + ggx_sheen) * nl_w * shade_w * sun_above;

    // Combine refracted depth body + Fresnel sky/mountain reflection + sparkling sun glints.
    vec3 w_col = mix(body_col, refl, clamp(fresnel * 0.92 + 0.05, 0.05, 0.92)) + spec_col;

    // Smooth animated shoreline foam where shallow water laps against gravel/rock banks.
    float shore_zone = (1.0 - smoothstep(0.05, 0.52, shore_a)) * smoothstep(0.0, 0.05, shore_a);
    float foam_band = 0.0;
    if (shore_zone > 0.005) {
      float lap = 0.5 + 0.5 * sin(t_sec * 1.9 - shore_a * 14.0 + dot(wp, flow_a) * 0.65);
      float foam_break = sky_noise(wp * 2.6 + flow_a * (t_sec * 0.34));
      foam_band = smoothstep(0.46, 0.78, foam_break * 0.62 + lap * 0.24 + shore_zone * 0.36 + crest * 0.08) *
                  shore_zone;
      vec3 foam_col = (sky_col * 0.85 + sun_col * (0.42 * shade_w * sun_above)) * 1.10;
      w_col = mix(w_col, foam_col, clamp(foam_band * 0.58, 0.0, 0.68));
    }

    w_col *= draw.up.w;
    w_col = apply_haze(w_col, v_position, up, sun);
    vec3 w_enc = out_rgb(w_col);
    // Smooth shoreline transparency: clear edge tapering into deep reflective alpine lake.
    float depth_opac = 1.0 - transmittance.r;
    float wa = clamp(mix(shore_a * 0.72, 0.96, clamp(fresnel * 0.65 + depth_opac * 0.65 + foam_band * 0.4, 0.0, 1.0)),
                     0.0, 0.98);
#ifndef OPAQUE_NO_DISCARD
    // Avoid overwriting shore terrain depth with invisible shoreline margin triangles now that world_water_ writes depth.
    if (blend == kAlpha && wa < 0.02) discard;
#endif
    out_color = blend == kAlpha ? vec4(w_enc * wa, wa) : vec4(w_enc, 1.0);
    return;
  }

  vec4 t;
  bool use_hex = false;
  HexTileSample hex_a;
  vec3 hex_w_a = vec3(1.0, 0.0, 0.0);
  HexTileSample hex_b;
  vec3 hex_w_b = vec3(1.0, 0.0, 0.0);
  vec2 uv_b_saved = v_uv0 * draw.tint.zw;
  float layer_b_w = 0.0;
  float layer_c_w = 0.0;
  float road_spec_mod = 1.0;
  if ((flags & kFlagTextured) != 0u) {
    // Mikkelsen Hex-Tiling on organic multi-layer terrain blends (never on directional roads or billboards).
    use_hex = frame.shadow_bias.z > 0.5 &&
              (flags & (kFlagVertexBlend | kFlagSplat)) != 0u &&
              (flags & (kFlagRoad | kFlagBillboard)) == 0u &&
              eye_dist < 1100.0;
    vec2 uv_a = v_uv0 * draw.tint.xy;
    if (use_hex) {
      SAMPLE_HEX_TILE_STATE(albedo, uv_a, hex_a, hex_w_a, t);
    } else {
      t = texture(albedo, uv_a);
    }
    if ((flags & (kFlagVertexBlend | kFlagSplat)) != 0u) {
      vec2 uv_b = v_uv0 * draw.tint.zw;
      uv_b_saved = uv_b;
      vec4 b;
      if (use_hex) {
        SAMPLE_HEX_TILE_STATE(layer_b, uv_b, hex_b, hex_w_b, b);
      } else {
        b = texture(layer_b, uv_b);
      }
      if ((flags & kFlagSplat) != 0u) {
        vec4 sp = texture(splat_map, v_uv1);
        layer_b_w = clamp(sp.r, 0.0, 1.0);
        t = mix(t, b, layer_b_w);
        if ((flags & kFlagLayerC) != 0u) {
          vec2 tc = unpackHalf2x16(floatBitsToUint(draw.params.w));
          if (abs(tc.x) < 1e-4) tc.x = draw.tint.z;
          if (abs(tc.y) < 1e-4) tc.y = draw.tint.w;
          vec2 uv_c = v_uv0 * tc;
          vec4 c;
          if (use_hex) {
            SAMPLE_HEX_TILE(layer_c, uv_c, c);
          } else {
            c = texture(layer_c, uv_c);
          }
          // FH1 H_BLND3 modulates Blend_C weight by Blend_C alpha (WORLD_MATERIALS.md:735).
          float wc = clamp(sp.g * mix(0.45, 1.0, c.a), 0.0, 1.0);
          layer_c_w = wc;
          t = mix(t, c, wc);
        }
      } else {
        layer_b_w = clamp(v_colour.r, 0.0, 1.0);
        t = mix(t, b, layer_b_w);
        if ((flags & kFlagRoad) != 0u && (flags & kFlagLayerC) != 0u) {
          vec2 tc = unpackHalf2x16(floatBitsToUint(draw.params.w));
          if (abs(tc.x) < 1e-4) tc.x = draw.tint.z;
          if (abs(tc.y) < 1e-4) tc.y = draw.tint.w;
          vec4 c = texture(layer_c, v_uv0 * tc);
          layer_c_w = clamp(v_colour.g, 0.0, 1.0);
          t = mix(t, c, layer_c_w);
        }
      }
    }
    // H_ROAD macro variation: Noise (slot 2 -> splat_map) & Modulate (slot 6 -> spec_map).
    if ((flags & kFlagRoad) != 0u) {
      if ((flags & kFlagRoadNoise) != 0u) {
        vec3 r_noise = texture(splat_map, v_uv1).rgb;
        t.rgb *= mix(vec3(0.84), vec3(1.16), r_noise);
        road_spec_mod *= mix(0.75, 1.25, dot(r_noise, vec3(0.299, 0.587, 0.114)));
      }
      if ((flags & kFlagRoadModulate) != 0u) {
        vec3 r_mod = texture(spec_map, v_uv1).rgb;
        t.rgb *= clamp(mix(vec3(0.65), vec3(1.35), r_mod), vec3(0.55), vec3(1.40));
        road_spec_mod *= mix(0.80, 1.20, dot(r_mod, vec3(0.3333)));
      }
    }
  } else if ((flags & kFlagVertexLit) != 0u) {
    t = vec4(pow(v_colour.rgb, vec3(2.2)), v_colour.a);
  } else {
    t = vec4(draw.tint.rgb, 1.0);
  }
  // Additive world effects (DIFF_GLOW_*, ADD_DIFF_OPAC_RGBA): unlit colour masked by alpha,
  // added over what is behind (alpha 0 under ONE, ONE_MINUS_SRC_ALPHA).
  if (blend == kAdditive) {
    out_color = vec4(out_rgb(t.rgb * t.a * draw.up.w), 0.0);
    return;
  }
#ifndef OPAQUE_NO_DISCARD
  // Execute cutout alpha-test before normal mapping, shadows, specular, and headlights.
  if (blend == kCoverage && (flags & kFlagAlphaTest) != 0u && t.a < 0.42) discard;
#endif
  // Tree sprites: soft linearize vertex tint without crushing distant conifers to black silhouettes.
  if ((flags & kFlagBillboard) != 0u) t.rgb *= 1.85 * pow(max(v_colour.rgb, vec3(0.0)), vec3(1.35));

  vec3 n = n_geom;
  bool spec_slot_is_norm_b = false;
  if ((flags & kFlagNormalMap) != 0u && (flags & kFlagBillboard) == 0u) {
    // Extended normal-map fade distances keep highway asphalt, gravel shoulders, and rocky hillsides
    // crisp well beyond 175 m (zero reversed smoothstep).
    float nm_near = (flags & kFlagRoad) != 0u ? 220.0 : 180.0;
    float nm_far = (flags & kFlagRoad) != 0u ? 420.0 : 340.0;
    float fade_nm = 1.0 - smoothstep(nm_near, nm_far, eye_dist);
    if (fade_nm > 0.001) {
      vec2 nm_uv = v_uv0 * draw.tint.xy;
      vec2 nm_dxy;
      if (use_hex) {
        SAMPLE_HEX_TILE_NORMAL_DXY(normal_map, hex_a, hex_w_a, nm_dxy);
      } else {
        nm_dxy = texture(normal_map, nm_uv).xy * 2.0 - 1.0;
      }
      vec3 np = perturb_normal_dxy(n_geom, v_position, nm_uv, nm_dxy);
      float nm_strength = 0.78;
      // Blend Layer B's authored DXN/BC5 normal map when bound in spec_map on _NORM2 road/terrain effects.
      if ((flags & kFlagSpecMap) != 0u && (flags & (kFlagVertexBlend | kFlagSplat)) != 0u) {
        vec4 sp_b_probe = texture(spec_map, uv_b_saved);
        if (sp_b_probe.b < 0.002 && sp_b_probe.r > 0.05 && sp_b_probe.g > 0.05) {
          spec_slot_is_norm_b = true;
          if (layer_b_w > 0.01) {
            vec2 nm_b_dxy;
            if (use_hex) {
              SAMPLE_HEX_TILE_NORMAL_DXY(spec_map, hex_b, hex_w_b, nm_b_dxy);
            } else {
              nm_b_dxy = sp_b_probe.xy * 2.0 - 1.0;
            }
            vec3 np_b = perturb_normal_dxy(n_geom, v_position, uv_b_saved, nm_b_dxy);
            np = normalize(mix(np, np_b, layer_b_w));
          }
        }
      }
      if (!spec_slot_is_norm_b && (flags & kFlagRoad) != 0u && (layer_b_w > 0.01 || layer_c_w > 0.01)) {
        // When only Layer A's asphalt normal map is bound, soften it cleanly on gravel/dirt shoulders.
        nm_strength *= mix(1.0, 0.58, layer_b_w) * mix(1.0, 0.45, layer_c_w);
      }
      n = normalize(mix(n_geom, np, fade_nm * nm_strength));
    }
  }

  // TrackSettings.xml TrackLightMapParams (scale & AO floor) on _LM draws without LayerC/Road/Decal params.
  float lm_scale = 1.4;
  float ao_floor = 0.05;
  if ((flags & kFlagLightMap) != 0u && (flags & (kFlagLayerC | kFlagRoad | kFlagVertexFade)) == 0u) {
    vec2 lmp = unpackHalf2x16(floatBitsToUint(draw.params.w));
    if (lmp.x > 0.01 && lmp.x <= 6.0) {
      lm_scale = lmp.x;
      if (lmp.y >= 0.0 && lmp.y <= 0.5) ao_floor = lmp.y;
    }
  }

  // FH1 terrain/world AO at TEXCOORD2 (v_uv2, separate from Splat uv1): ao_floor + (1 - ao_floor) * AO.
  float ao = 1.0;
  if ((flags & kFlagAoMap) != 0u && (flags & kFlagBillboard) == 0u) {
    vec3 ao_s = texture(ao_map, v_uv2).rgb;
    ao = ao_floor + (1.0 - ao_floor) * dot(ao_s, vec3(0.3333));
  }

  float ndl = max(dot(n, sun), 0.0);
  float raw_shade = 1.0;
  if ((flags & kFlagBillboard) != 0u) {
    // Enable sun shadow reception on tree billboards using stable world-up receiver bias
    // so forested hillsides and roadside conifers darken naturally inside mountain/building shadows.
    raw_shade = mix(0.32, 1.0, sun_shadow(v_position, up));
  } else if (ndl > 0.0) {
    raw_shade = sun_shadow(v_position, n_geom);
  }
  float shade = mix(1.0, raw_shade, shadow_scale);
  float ibl_amb = frame.sun_color.w >= 0.0 ? max(frame.car_light_top.w, 0.01) : 1.0;
  vec3 hemi_amb = mix(gnd_col, sky_col, 0.5 + 0.5 * dot(n, up)) * ibl_amb;
  if (frame.sun_color.w >= 0.0) {
    if ((flags & (kFlagLodNear | kFlagLodFar | kFlagLodOne)) != 0u && (flags & kFlagBillboard) == 0u) {
      // SKY_AND_TOD §2.4: AmbientColour lifts shadowed world props cleanly in daylight.
      hemi_amb += frame.ambient_color.rgb * 0.35 * ibl_amb;
    } else if ((flags & (kFlagRoad | kFlagSky)) == 0u) {
      // Subtle ambient lift on distant Colorado forested hillsides so calibrated daylight
      // exposure preserves warm hillside detail matching FH1 without washing out roads.
      hemi_amb += frame.ambient_color.rgb * 0.18 * ibl_amb;
    }
  }
  hemi_amb *= shadow_ambient_tint((1.0 - shade) * shadow_scale);
  vec3 light = (sun_col * ndl * shade + hemi_amb) * ao;
  vec3 colour = t.rgb * light * draw.up.w;

  // Soft view-dependent backlit subsurface translucency on tree billboards and 3D cutout foliage.
  bool is_foliage_sss = ((flags & kFlagBillboard) != 0u) ||
                        (blend == kCoverage && (flags & (kFlagLodNear | kFlagLodFar | kFlagLodOne)) != 0u &&
                         (flags & kFlagAnimFlag) == 0u);
  if (is_foliage_sss) {
    float sun_elev_fol = clamp(dot(up, sun), 0.0, 1.0);
    float sss_vis = smoothstep(0.02, 0.22, sun_elev_fol);
    if (sss_vis > 0.001) {
      float back_scatter = pow(clamp(dot(view, -sun), 0.0, 1.0), 3.0);
      float rim_wrap = clamp(dot(-n, sun) * 0.5 + 0.5, 0.0, 1.0);
      float sss_gain = ((flags & kFlagBillboard) != 0u ? 0.42 : 0.30) * sss_vis;
      vec3 fol_sss = t.rgb * vec3(1.12, 1.24, 0.76) * sun_col * (back_scatter * 0.72 + rim_wrap * 0.28) *
                     mix(0.25, 1.0, shade) * sss_gain * ao;
      colour += fol_sss * draw.up.w;
    }
  }

  // Per-material Blinn-Phong sun specular: H_ROAD asphalt glossiness & SpecularMap modulation,
  // preserving distinct Layer B/C roughness/specular contrast on H_ROAD_DIFF2/3 and H_BLND2/3.
  float surf_gloss = 0.0;
  float spec_mask = 1.0;
  bool has_real_spec = (flags & kFlagSpecMap) != 0u && !spec_slot_is_norm_b && (flags & kFlagBillboard) == 0u;
  if (has_real_spec) {
    vec4 sp_sample;
    if (use_hex) {
      SAMPLE_HEX_TILE_WEIGHTED(spec_map, hex_a, hex_w_a, sp_sample);
    } else {
      sp_sample = texture(spec_map, v_uv0 * draw.tint.xy);
    }
    if ((flags & (kFlagVertexBlend | kFlagSplat)) != 0u && (flags & kFlagNormalMap) != 0u &&
        sp_sample.b < 0.002 && sp_sample.r > 0.05 && sp_sample.g > 0.05) {
      has_real_spec = false;
    } else {
      spec_mask = dot(sp_sample.rgb, vec3(0.3333));
    }
  }
  float road_layer_spec = mix(1.0, 0.40, layer_b_w) * mix(1.0, 0.26, layer_c_w);
  float road_layer_gloss = mix(1.0, 0.34, layer_b_w) * mix(1.0, 0.22, layer_c_w);
  float terrain_layer_spec = mix(1.0, 0.50, layer_b_w) * mix(1.0, 0.36, layer_c_w);
  float terrain_layer_gloss = mix(32.0, 12.0, clamp(layer_b_w * 0.72 + layer_c_w * 0.88, 0.0, 1.0));
  if ((flags & kFlagRoad) != 0u && (flags & kFlagBillboard) == 0u) {
    surf_gloss = clamp((0.20 + 0.28 * spec_mask) * road_spec_mod * road_layer_spec, 0.04, 0.52);
  } else if (has_real_spec) {
    surf_gloss = clamp((0.10 + 0.22 * spec_mask) * terrain_layer_spec, 0.0, 0.34);
  }
  if (ndl > 0.0 && (flags & kFlagBillboard) == 0u) {
    vec3 h_v = normalize(view + sun);
    float nv_s = clamp(dot(n, view), 0.0, 1.0);
    if ((flags & kFlagRoad) != 0u) {
      // FH1 H_ROAD asphalt specular (TrackSettings.xml RoadSpecular / RoadGlossiness)
      // with crisp Layer A asphalt vs matte Layer B/C gravel/shoulder contrast.
      float road_spec_scale = 0.72;
      float road_gloss = 42.0;
      if ((flags & kFlagLayerC) == 0u) {
        vec2 rp = unpackHalf2x16(floatBitsToUint(draw.params.w));
        if (rp.x > 0.01 && rp.x <= 4.0) road_spec_scale = rp.x;
        if (rp.y >= 4.0 && rp.y <= 256.0) road_gloss = rp.y;
      }
      float eff_gloss = max(road_gloss * road_layer_gloss, 6.0);
      float fres_r = 0.04 + 0.30 * pow(1.0 - nv_s, 4.0);
      float nh_r = max(dot(n, h_v), 0.0);
      float spec_r = (pow(nh_r, eff_gloss) * 0.62 + pow(nh_r, max(eff_gloss * 0.24, 4.0)) * 0.14) *
                     (road_spec_scale / 0.72) * road_layer_spec * fres_r * spec_mask * road_spec_mod * ndl * shade * ao;
      colour += sun_col * spec_r * draw.up.w;
    } else if (((flags & kFlagNormalMap) != 0u) || has_real_spec) {
      float fres = 0.04 + 0.25 * pow(1.0 - nv_s, 4.0);
      float spec = pow(max(dot(n, h_v), 0.0), terrain_layer_gloss) * fres * spec_mask * terrain_layer_spec *
                   ndl * shade * ao * 0.45;
      colour += sun_col * spec * draw.up.w;
    }
  }

  // Projected vehicle road headlight cones (CAR_LAMPS.md §5): cookie spotlight
  // evaluated in world space, scaled by SwitchOnLights night factor.
  int hl_count = int(headlights.count_night.x + 0.5);
  float hl_night = headlights.count_night.y;
  if (hl_count > 0 && hl_night > 0.004) {
    vec3 hl_sum = vec3(0.0);
    for (int i = 0; i < hl_count && i < 16; ++i) {
      vec3 origin = headlights.beams[i].origin_range.xyz;
      float range = headlights.beams[i].origin_range.w;
      vec3 to_p = v_position - origin;
      float d2 = dot(to_p, to_p);
      if (d2 >= range * range || d2 < 0.04) continue;
      vec3 bdir = headlights.beams[i].dir_power.xyz;
      float fwd_dot = dot(to_p, bdir);
      if (fwd_dot <= 0.15) continue;
      vec4 c = headlights.beams[i].proj * vec4(v_position, 1.0);
      if (c.w <= 0.05) continue;
      vec2 buv = c.xy / c.w * 0.5 + 0.5;
      if (buv.x <= 0.0 || buv.x >= 1.0 || buv.y <= 0.0 || buv.y >= 1.0) continue;
      vec4 ck = texture(headlight_cookie, buv);
      float pattern = max(max(ck.r, ck.g), ck.b) * max(ck.a, 0.25);
      float dist = sqrt(d2);
      float att = pow(clamp(1.0 - dist / range, 0.0, 1.0), 1.35);
      float edge = smoothstep(0.0, 0.08, buv.x) * (1.0 - smoothstep(0.92, 1.0, buv.x)) *
                   smoothstep(0.0, 0.08, buv.y) * (1.0 - smoothstep(0.92, 1.0, buv.y));
      // Zero back-face illumination floor: gentle grazing wrap illuminates flat road ahead
      // while strictly fading to 0.0 on back-facing walls, barriers, and hillside berms.
      float nl_hl = dot(n, -to_p / dist);
      float facing = smoothstep(-0.05, 0.02, nl_hl) * mix(0.35, 1.0, clamp(nl_hl, 0.0, 1.0));
      hl_sum += headlights.beams[i].color.rgb * pattern * att * edge * facing * headlights.beams[i].dir_power.w;
    }
    colour += t.rgb * hl_sum * hl_night * draw.up.w;
  }

  // World night Light_Map (slot 7) & staggered dusk/dawn EmissiveMapSampler (slot 3) (SKY_AND_TOD §6.1-6.2):
  // Light_Map is baked night light (lamp pools, lit facades) ADDED to the
  // radiance, Light_Map.rgb * max(SwitchOnLights, c3.w), not a multiply on
  // the surface albedo (the decoded terrain shaders add it before fog).
  // EmissiveMap (slot 3) is self-emission (lit windows / neon signs).
  // Dual _EMIS + _LM building props set BOTH flags, reading Light_Map from light_map and EmissiveMap from layer_c.
  // c3.w (emissive_force) encoded in fractional part of draw.params.z enables daytime interior/sign glow.
  float emissive_force = clamp((draw.params.z - floor(draw.params.z + 0.01)) / 0.45, 0.0, 1.0);
  bool has_lm = (flags & kFlagLightMap) != 0u;
  bool has_em = (flags & kFlagEmissiveMap) != 0u;
  if (has_lm) {
    float lm_gain = max(frame.sky_color.w, emissive_force);
    if (lm_gain > 0.002) {
      vec3 lm = texture(light_map, v_uv2).rgb;
      colour += lm * lm_gain * lm_scale * draw.up.w;
    }
  }
  if (has_em) {
    float lit_flag = step(v_emissive_thresh, frame.sky_color.w);
    float em_gain = max(lit_flag * frame.sky_color.w, emissive_force);
    if (em_gain > 0.002) {
      vec2 em_uv = v_uv0 * draw.tint.xy;
      vec3 em = has_lm ? texture(layer_c, em_uv).rgb : texture(light_map, em_uv).rgb;
      colour += em * em_gain * 1.6 * draw.up.w;
    }
  }

  // The hub rooms' interiors (day_night_post -1; Q222, decoded H_INT_*
  // pixel shaders): no sun, the light baked in the Light_Map at TEXCOORD2
  // over the AO's green channel lifting the room's LightAmbient,
  // albedo x c0.z x (LM + (AO + 0.05) x ambient); the emissive surfaces add
  // their EmissiveMap (at the diffuse's UV) x c0.z. params.w holds (c0.z,
  // c2.x); the cube reflection (c2.x, lit by the absent sun and the
  // ambient) is left out. H_INT_ROAD (the paint shop's floor) lifts by a
  // sky / ground hemisphere instead; both take the frame's ambient here.
  if (frame.ground_color.w < -0.5 && (has_lm || has_em)) {
    vec2 ip = unpackHalf2x16(floatBitsToUint(draw.params.w));
    vec3 amb = frame.ambient_color.rgb;
    if (has_lm) {
      float ao_i = (flags & kFlagAoMap) != 0u ? texture(ao_map, v_uv2).g : 1.0;
      colour = t.rgb * ip.x * (texture(light_map, v_uv2).rgb + (ao_i + 0.05) * amb);
    } else {
      colour = t.rgb * ip.x * amb + texture(light_map, v_uv0 * draw.tint.xy).rgb * ip.x;
    }
    colour *= draw.up.w;
  }

  colour = apply_haze(colour, v_position, up, sun);

  float alpha = 1.0;
#ifndef OPAQUE_NO_DISCARD
  if (blend == kCoverage) {
    // Alpha to coverage, sharpened to the texel edge (alpha-test discard ran early above).
    alpha = clamp((t.a - 0.42) / max(fwidth(t.a), 1e-4) + 0.5, 0.0, 1.0);
  } else if (blend == kAlpha) {
    // H_VBLND_DECAL with Noise_Sampler (c4.y >= 0) or Mask (c4.y < 0):
    if ((flags & kFlagVertexFade) != 0u && (flags & kFlagDecalNoise) != 0u) {
      vec2 c4 = unpackHalf2x16(floatBitsToUint(draw.params.w));
      vec4 mask_s = texture(splat_map, v_uv0 * draw.tint.zw);
      if (c4.y < 0.0) {
        // H_VBLND_DECAL*_MASK*: modulate decal alpha by mask texture (alpha when shaped, else RGB luma) * vertex fade.
        float mask_val = (mask_s.a < 0.99) ? mask_s.a : dot(mask_s.rgb, vec3(0.3333));
        alpha = t.a * mask_val * v_colour.r;
      } else {
        float fade = clamp(0.5 + c4.x * (v_colour.a - 0.5) - c4.y * (mask_s.a - 0.5), 0.0, 1.0);
        alpha = t.a * fade;
      }
    } else if ((flags & kFlagRoad) != 0u) {
      // H_ROAD_*_BLEND_* samples Blend_A without multiplying by Blend_A.a (WORLD_MATERIALS.md:640).
      alpha = clamp(v_colour.a, 0.0, 1.0);
    } else {
      alpha = t.a * ((flags & kFlagVertexFade) != 0u ? v_colour.r : v_colour.a);
    }
  }
#endif
  vec3 encoded = out_rgb(colour);
  // Blended draws are premultiplied (ONE, ONE_MINUS_SRC_ALPHA); opaque HDR encodes surface gloss in .a for Hi-Z SSR.
  if (blend == kAlpha) {
    out_color = vec4(encoded * alpha, alpha);
  } else if (blend == kOpaque && kLinearOut) {
    out_color = vec4(encoded, surf_gloss);
  } else {
    out_color = vec4(encoded, alpha);
  }
}
