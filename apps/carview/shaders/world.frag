#version 450
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
} frame;
layout(set = 0, binding = 1) uniform samplerCube environment;
layout(set = 0, binding = 2) uniform sampler2DShadow shadow_map;
layout(set = 0, binding = 3) uniform sampler2D headlight_cookie;

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

layout(set = 1, binding = 0) uniform sampler2D albedo;
layout(set = 1, binding = 1) uniform sampler2D layer_b;     // Blend_B
layout(set = 1, binding = 2) uniform sampler2D splat_map;   // Splat
layout(set = 1, binding = 3) uniform sampler2D light_map;   // Light_Map (night emissive spill)
layout(set = 1, binding = 4) uniform sampler2D normal_map;  // NormalMap (DXN/BC5, Sub-Milestone 3)
layout(set = 1, binding = 5) uniform sampler2D ao_map;      // AO / AOMap (Sub-Milestone 3)
layout(set = 1, binding = 6) uniform sampler2D layer_c;     // Blend_C (Sub-Milestone 3)

layout(push_constant) uniform Draw {
  mat4 model;
  vec4 sun;
  vec4 up;
  vec4 tint;
  vec4 params;
} draw;

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 4) in vec4 v_colour;
layout(location = 5) flat in vec2 v_fade;  // the instance's fade weights (world.vert)

layout(location = 0) out vec4 out_color;

const uint kOpaque = 0u, kCoverage = 1u, kAlpha = 2u, kAdditive = 3u;
const uint kFlagTextured = 1u, kFlagSky = 2u, kFlagVertexBlend = 4u, kFlagSplat = 8u, kFlagVertexFade = 16u,
           kFlagBillboard = 32u, kFlagAlphaTest = 64u, kFlagLodNear = 128u, kFlagLodFar = 256u, kFlagLodOne = 512u,
           kFlagUnlitVertexColor = 1024u, kFlagLightMap = 2048u, kFlagNormalMap = 4096u, kFlagAoMap = 8192u,
           kFlagLayerC = 16384u, kFlagWater = 32768u;

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
  vec3 sky = mix(kSkyHorizon, kSkyZenith, pow(hc, 0.55));
  float toward = max(dot(dir, sun), 0.0);
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
    float fog_amt = clamp(fog_dist * (1.0 - h_smooth * frame.fog_params.w), 0.0, 0.92);
    float sc_dist = clamp(d / 3500.0, 0.0, 1.0);
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

vec3 out_rgb(vec3 c) { return kLinearOut ? min(max(c, vec3(0.0)), vec3(4.0)) : encode_srgb(c); }

// Multi-cascade sun shadow (2x1 atlas: left half Near ~28 m, right half Far ~185 m).
float sample_cascade(vec4 s, float x_off) {
  vec2 uv = vec2((s.x * 0.5 + 0.5) * 0.5 + x_off, s.y * 0.5 + 0.5);
  if (frame.shadow.w > 1.5) {
    float tx = frame.shadow.y * 0.25;
    float ty = frame.shadow.y * 0.5;
    return 0.25 * (texture(shadow_map, vec3(uv + vec2(-tx, -ty), s.z)) +
                   texture(shadow_map, vec3(uv + vec2(tx, -ty), s.z)) +
                   texture(shadow_map, vec3(uv + vec2(-tx, ty), s.z)) +
                   texture(shadow_map, vec3(uv + vec2(tx, ty), s.z)));
  }
  return texture(shadow_map, vec3(uv, s.z));
}

float sun_shadow(vec3 p, vec3 n) {
  if (frame.shadow.x < 0.5) return 1.0;
  vec4 sn = frame.shadow_matrix * vec4(p + n * frame.shadow_bias.x, 1.0);
  float edge_n = max(abs(sn.x), abs(sn.y));
  bool near_ok = edge_n < 1.0 && sn.z > 0.0 && sn.z < 1.0;
  float lit_n = near_ok ? sample_cascade(sn, 0.0) : 1.0;

  if (frame.shadow_far_bias.y > 0.5) {
    if (near_ok && edge_n < 0.78) return mix(frame.shadow.z, 1.0, lit_n);
    vec4 sf = frame.shadow_matrix_far * vec4(p + n * frame.shadow_far_bias.x, 1.0);
    float edge_f = max(abs(sf.x), abs(sf.y));
    float lit_f = 1.0;
    if (edge_f < 1.0 && sf.z > 0.0 && sf.z < 1.0) {
      lit_f = mix(sample_cascade(sf, 0.5), 1.0, smoothstep(0.82, 1.0, edge_f));
    }
    float blend = near_ok ? smoothstep(0.78, 0.98, edge_n) : 1.0;
    return mix(frame.shadow.z, 1.0, mix(lit_n, lit_f, blend));
  }
  if (!near_ok) return 1.0;
  lit_n = mix(lit_n, 1.0, smoothstep(0.8, 1.0, edge_n));
  return mix(frame.shadow.z, 1.0, lit_n);
}

// Cotangent-frame tangent-space normal perturbation from DXN/BC5 normal map.
vec3 perturb_normal(vec3 n, vec3 pos, vec2 uv, vec2 nm_xy) {
  vec2 dxy = nm_xy * 2.0 - 1.0;
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

vec3 render_to_game_dir(vec3 d) {
  return normalize(vec3(dot(d, frame.env_basis_x.xyz), dot(d, frame.env_basis_y.xyz), dot(d, frame.env_basis_z.xyz)));
}

void main() {
  uint blend = uint(draw.params.x + 0.5);
  uint flags = uint(draw.params.y + 0.5);
  if ((flags & kFlagSky) != 0u) {
    out_color = vec4(out_rgb(sky_grade(sky_colour(normalize(v_position), normalize(draw.up.xyz), normalize(draw.sun.xyz)))), 1.0);
    return;
  }
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
  }
  // Terrain layers (WORLD-MATERIALS, Terrain shading): tint holds the
  // tilings, Blend_A's in xy and Blend_B's in zw; the weight is the vertex
  // colour's R (H_VBLND, baked from c4) or Splat's R (H_BLND2/3, uv1
  // pre-tiled). Blend_C is not drawn (its weight is not settled).
  // Unlit premultiplied-alpha vertex colour overlay (e.g. 3D driving/braking line ribbon).
  if ((flags & kFlagUnlitVertexColor) != 0u) {
    vec3 c = pow(v_colour.rgb, vec3(2.2));
    vec3 encoded = out_rgb(c * draw.up.w);
    float a = v_colour.a;
    out_color = vec4(encoded * a, a);
    return;
  }
  vec3 up = normalize(draw.up.xyz);
  vec3 n_geom = dot(v_normal, v_normal) > 1e-6 ? normalize(v_normal) : up;
  vec3 sun = normalize(draw.sun.xyz);
  vec3 view = normalize(frame.eye.xyz - v_position);
  float eye_dist = length(frame.eye.xyz - v_position);
  vec3 sun_col = frame.sun_color.w >= 0.0 ? frame.sun_color.rgb : kSunColour;
  vec3 sky_col = frame.sun_color.w >= 0.0 ? frame.sky_color.rgb : kSkyColour;
  vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.ground_color.rgb : kGroundColour;
  float shadow_scale = frame.sun_color.w >= 0.0 ? clamp(frame.sun_color.w, 0.0, 1.0) : 1.0;

  // LAKE water shader (kFlagWater): animated dual-scroll ripples, Fresnel cubemap reflection,
  // crisp sun specular sparkle, and Colorado mountain-lake depth absorption.
  if ((flags & kFlagWater) != 0u) {
    float t_sec = frame.env_basis_x.w;
    vec2 wuv = v_uv0 * 0.25;
    vec2 uv_a = wuv + vec2(t_sec * 0.018, t_sec * 0.011);
    vec2 uv_b = wuv * 1.45 - vec2(t_sec * 0.014, -t_sec * 0.019);
    vec3 nw = n_geom;
    if ((flags & kFlagNormalMap) != 0u) {
      vec2 na = texture(normal_map, uv_a).xy;
      vec2 nb = texture(normal_map, uv_b).xy;
      nw = perturb_normal(n_geom, v_position, wuv, 0.5 * (na + nb));
    } else {
      // Procedural dual-layer wave ripples when no normal map is bound.
      float h1 = sky_noise(v_position.xz * 0.35 + vec2(t_sec * 0.65, t_sec * 0.35));
      float h2 = sky_noise(v_position.xz * 0.85 - vec2(t_sec * 0.45, -t_sec * 0.55));
      float dx = (sky_noise(v_position.xz * 0.35 + vec2(0.08 + t_sec * 0.65, t_sec * 0.35)) - h1) +
                 0.5 * (sky_noise(v_position.xz * 0.85 + vec2(0.08 - t_sec * 0.45, t_sec * 0.55)) - h2);
      float dz = (sky_noise(v_position.xz * 0.35 + vec2(t_sec * 0.65, 0.08 + t_sec * 0.35)) - h1) +
                 0.5 * (sky_noise(v_position.xz * 0.85 + vec2(-t_sec * 0.45, 0.08 + t_sec * 0.55)) - h2);
      vec3 t_x = normalize(cross(up, abs(up.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0)));
      vec3 t_z = cross(t_x, up);
      nw = normalize(n_geom + (t_x * dx + t_z * dz) * 0.28);
    }
    vec3 r_dir = reflect(-view, nw);
    if (dot(r_dir, up) < 0.02) r_dir = normalize(r_dir + up * (0.04 - dot(r_dir, up)));
    vec3 refl;
    if (frame.shadow_far_bias.z > 0.5) {
      refl = textureLod(environment, render_to_game_dir(r_dir), 1.0).rgb;
    } else {
      refl = mix(sky_col * 1.4, sun_col * 0.5 + sky_col, clamp(dot(r_dir, up), 0.0, 1.0));
    }
    float nv = clamp(dot(nw, view), 0.0, 1.0);
    float fresnel = 0.04 + 0.96 * pow(1.0 - nv, 4.5);
    float day_amt = clamp(dot(up, sun) * 2.5 + 0.25, 0.05, 1.0);
    vec3 deep_water = vec3(0.010, 0.036, 0.052) * (0.35 + 0.65 * day_amt) + gnd_col * 0.08;
    vec3 h_w = normalize(view + sun);
    float spec_w = pow(max(dot(nw, h_w), 0.0), 240.0) * 2.2 + pow(max(dot(nw, h_w), 0.0), 36.0) * 0.25;
    float shade_w = mix(1.0, sun_shadow(v_position, n_geom), shadow_scale);
    vec3 w_col = mix(deep_water, refl, clamp(fresnel * 1.15, 0.05, 0.96)) + sun_col * spec_w * shade_w;
    w_col *= draw.up.w;
    w_col = apply_haze(w_col, v_position, up, sun);
    vec3 w_enc = out_rgb(w_col);
    float wa = clamp(0.76 + fresnel * 0.24, 0.76, 0.98);
    out_color = blend == kAlpha ? vec4(w_enc * wa, wa) : vec4(w_enc, 1.0);
    return;
  }

  vec4 t;
  if ((flags & kFlagTextured) != 0u) {
    t = texture(albedo, v_uv0 * draw.tint.xy);
    if ((flags & (kFlagVertexBlend | kFlagSplat)) != 0u) {
      vec4 b = texture(layer_b, v_uv0 * draw.tint.zw);
      if ((flags & kFlagSplat) != 0u) {
        vec4 sp = texture(splat_map, v_uv1);
        t = mix(t, b, sp.r);
        if ((flags & kFlagLayerC) != 0u) {
          vec4 c = texture(layer_c, v_uv0 * draw.tint.zw);
          t = mix(t, c, sp.g);
        }
      } else {
        t = mix(t, b, v_colour.r);
      }
    }
  } else {
    t = vec4(draw.tint.rgb, 1.0);
  }
  // Glow cards (interim, ours): the texture unlit, added over what is behind
  // (alpha 0 under ONE, ONE_MINUS_SRC_ALPHA).
  if (blend == kAdditive) {
    out_color = vec4(out_rgb(t.rgb * draw.up.w), 0.0);
    return;
  }
  // Tree sprites: the atlas colour times the record's tint, 2 x RGB.
  if ((flags & kFlagBillboard) != 0u) t.rgb *= 2.0 * v_colour.rgb;

  vec3 n = n_geom;
  if ((flags & kFlagNormalMap) != 0u && (flags & kFlagBillboard) == 0u && eye_dist < 140.0) {
    vec2 nm_uv = v_uv0 * draw.tint.xy;
    vec2 nm = texture(normal_map, nm_uv).xy;
    vec3 np = perturb_normal(n_geom, v_position, nm_uv, nm);
    float fade_nm = 1.0 - smoothstep(85.0, 140.0, eye_dist);
    n = normalize(mix(n_geom, np, fade_nm * 0.75));
  }

  float ao = 1.0;
  if ((flags & kFlagAoMap) != 0u && (flags & kFlagBillboard) == 0u) {
    vec3 ao_s = texture(ao_map, v_uv1).rgb;
    ao = mix(0.42, 1.0, dot(ao_s, vec3(0.3333)));
  }

  float ndl = max(dot(n, sun), 0.0);
  float raw_shade = ndl > 0.0 && (flags & kFlagBillboard) == 0u ? sun_shadow(v_position, n_geom) : 1.0;
  float shade = mix(1.0, raw_shade, shadow_scale);
  vec3 light = (sun_col * ndl * shade + mix(gnd_col, sky_col, 0.5 + 0.5 * dot(n, up))) * ao;
  vec3 colour = t.rgb * light * draw.up.w * (draw.params.w > 0.0 ? draw.params.w : 1.0);

  // Subtle per-material Blinn-Phong sun specular for roads, rocks, and buildings.
  if (ndl > 0.0 && (flags & kFlagBillboard) == 0u && (flags & kFlagNormalMap) != 0u) {
    vec3 h_v = normalize(view + sun);
    float nv_s = clamp(dot(n, view), 0.0, 1.0);
    float fres = 0.04 + 0.25 * pow(1.0 - nv_s, 4.0);
    float spec = pow(max(dot(n, h_v), 0.0), 32.0) * fres * ndl * shade * ao * 0.45;
    colour += sun_col * spec * draw.up.w;
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
      float att = pow(clamp(1.0 - dist / range, 0.0, 1.0), 1.5);
      float edge = smoothstep(0.0, 0.08, buv.x) * smoothstep(1.0, 0.92, buv.x) *
                   smoothstep(0.0, 0.08, buv.y) * smoothstep(1.0, 0.92, buv.y);
      float facing = max(dot(n, -to_p / dist), 0.22);
      hl_sum += headlights.beams[i].color.rgb * pattern * att * edge * facing * headlights.beams[i].dir_power.w;
    }
    colour += t.rgb * hl_sum * hl_night * draw.up.w;
  }

  // World night Light_Map emissive spill (SPEC-SKY-AND-TOD §6.1): added in linear HDR
  // before fog, driven by SwitchOnLights (frame.sky_color.w).
  if ((flags & kFlagLightMap) != 0u && frame.sky_color.w > 0.002) {
    vec3 lm = texture(light_map, v_uv1).rgb;
    colour += lm * frame.sky_color.w * 1.4 * draw.up.w;
  }

  colour = apply_haze(colour, v_position, up, sun);

  float alpha = 1.0;
  if (blend == kCoverage) {
    // Without MSAA there is no alpha to coverage: an alpha test at one half.
    if ((flags & kFlagAlphaTest) != 0u && t.a < 0.5) discard;
    // Alpha to coverage, sharpened to the texel edge.
    alpha = clamp((t.a - 0.5) / max(fwidth(t.a), 1e-4) + 0.5, 0.0, 1.0);
  } else if (blend == kAlpha) {
    // H_VBLND_DECAL: the diffuse alpha times the vertex fade (baked into R);
    // road decals: times the vertex colour's alpha (the likely source).
    alpha = t.a * ((flags & kFlagVertexFade) != 0u ? v_colour.r : v_colour.a);
  }
  vec3 encoded = out_rgb(colour);
  // Blended draws are premultiplied (ONE, ONE_MINUS_SRC_ALPHA).
  out_color = blend == kAlpha ? vec4(encoded * alpha, alpha) : vec4(encoded, alpha);
}
