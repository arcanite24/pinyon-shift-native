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
} frame;
layout(set = 0, binding = 1) uniform samplerCube environment;
layout(set = 0, binding = 2) uniform sampler2DShadow shadow_map;
layout(set = 0, binding = 5) uniform sampler2D mirror_map;

layout(set = 1, binding = 0) uniform sampler2D slot0;
layout(set = 1, binding = 1) uniform sampler2D slot1;
layout(set = 1, binding = 2) uniform sampler2D livery;

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

layout(location = 0) out vec4 out_color;

const uint kOpaque = 0u, kPaint = 1u, kGlass = 2u, kLamp = 3u, kInterior = 4u, kGauge = 5u,
           kTyre = 6u, kRim = 7u, kCaliper = 8u, kChrome = 9u, kCarbon = 10u, kWorld = 11u;
const uint kFlagSlot1 = 1u, kFlagMetallic = 2u, kFlagLivery = 8u, kFlagNoSlot0 = 16u, kFlagStripe = 32u, kFlagFlat = 64u,
           kFlagRaceLivery = 128u, kFlagGrille = 256u, kFlagCutout = 512u, kFlagCover = 1024u,
           kFlagLampEmissive = 2048u, kFlagMirror = 4096u;

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

layout(constant_id = 0) const bool kLinearOut = false;
vec3 out_rgb(vec3 c) { return kLinearOut ? min(max(c, vec3(0.0)), vec3(4.0)) : encode_srgb(c); }

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

vec3 drive_env(vec3 dir, float lod) {
  vec3 up = normalize(frame.drive.xyz), sun = normalize(frame.key_light.xyz);
  float rough = lod / 7.0;
  float h = dot(dir, up);
  vec3 sun_col = frame.sun_color.w >= 0.0 ? frame.sun_color.rgb : kSunColour;
  vec3 hor_col = frame.sun_color.w >= 0.0 ? max(frame.fog_color.rgb * 2.4, frame.sky_color.rgb * 1.4) : kSkyHorizon;
  vec3 zen_col = frame.sun_color.w >= 0.0 ? frame.sky_color.rgb * 0.75 : kSkyZenith;
  vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.ground_color.rgb * 1.6 : kDirtReflect;
  vec3 sky = mix(hor_col, zen_col, pow(clamp(h, 0.0, 1.0), 0.55));
  float toward = max(dot(dir, sun), 0.0);
  vec3 haze_c = frame.sun_color.w >= 0.0 ? frame.fog_scatter.rgb : vec3(0.80, 0.70, 0.55);
  sky = mix(sky, haze_c, pow(toward, 6.0) * (1.0 - clamp(h, 0.0, 1.0)) * 0.55);
  sky += sun_col * pow(toward, mix(48.0, 6.0, rough)) * mix(0.9, 0.25, rough);
  float edge = mix(0.02, 0.5, rough);
  return mix(gnd_col * (0.85 + 0.15 * clamp(-h * 3.0, 0.0, 1.0)), sky, smoothstep(-edge, edge, h));
}

float sample_cascade(vec4 s, float x_off) {
  vec2 uv = vec2((s.x * 0.5 + 0.5) * 0.5 + x_off, s.y * 0.5 + 0.5);
  float tx = frame.shadow.y * 0.25;
  float ty = frame.shadow.y * 0.5;
  return 0.25 * (texture(shadow_map, vec3(uv + vec2(-tx, -ty), s.z)) +
                 texture(shadow_map, vec3(uv + vec2(tx, -ty), s.z)) +
                 texture(shadow_map, vec3(uv + vec2(-tx, ty), s.z)) +
                 texture(shadow_map, vec3(uv + vec2(tx, ty), s.z)));
}

float drive_shadow(vec3 p, vec3 n) {
  if (frame.shadow.x < 0.5) return 1.0;
  vec4 sn = frame.shadow_matrix * vec4(p + n * frame.shadow_bias.x * 2.0, 1.0);
  float edge_n = max(abs(sn.x), abs(sn.y));
  bool near_ok = edge_n < 1.0 && sn.z > 0.0 && sn.z < 1.0;
  float lit_n = near_ok ? sample_cascade(sn, 0.0) : 1.0;
  if (frame.shadow_far_bias.y > 0.5) {
    if (near_ok && edge_n < 0.78) return mix(frame.shadow.z, 1.0, lit_n);
    vec4 sf = frame.shadow_matrix_far * vec4(p + n * frame.shadow_far_bias.x * 2.0, 1.0);
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

vec3 env(vec3 dir, float lod) {
  if (frame.drive.w > 0.5) {
    if (frame.shadow_far_bias.z > 0.5) {
      vec3 gdir = normalize(vec3(dot(dir, frame.env_basis_x.xyz), dot(dir, frame.env_basis_y.xyz),
                                 dot(dir, frame.env_basis_z.xyz)));
      return textureLod(environment, gdir, lod).rgb;
    }
    return drive_env(dir, lod);
  }
  return textureLod(environment, dir, lod).rgb * frame.options.y;
}

void main() {
  uint klass = uint(draw.params.x + 0.5);
  uint flags = uint(draw.params.y + 0.5);
  vec4 t0 = (flags & kFlagNoSlot0) != 0u ? vec4(1.0, 1.0, 1.0, 0.0) : texture(slot0, v_uv0);
  vec4 t1 = vec4(1.0);
  if ((flags & kFlagSlot1) != 0u) t1 = texture(slot1, klass == kCarbon ? v_uv1 : v_uv0);

  vec3 view = normalize(frame.eye.xyz - v_position);
  vec3 n = normalize(v_normal);
  if (dot(n, view) < 0.0) n = -n;
  vec3 r = reflect(-view, n);
  float nv = clamp(dot(n, view), 0.0, 1.0);

  vec3 base = (flags & kFlagFlat) != 0u ? draw.tint.rgb : t0.rgb;
  float reflectivity = 0.04;
  float gloss = 0.3;
  float metal = 0.0;
  float alpha = 1.0;
  vec3 emissive = vec3(0.0);
  float flake_boost = 0.0;

  if ((flags & kFlagGrille) != 0u) {
    if ((flags & kFlagCutout) != 0u && t1.a < 0.5) discard;
    base = t1.rgb;
  }

  if (klass == kPaint) {
    vec3 paint_col = draw.tint.rgb;
    int finish = int(frame.paint_finish.x + 0.5);
    if (finish == 1) {
      // Metallic Flake (ShaderSettings/Metallic.xml): crisp sparkle & metallic depth.
      float flake = hash21(floor(v_position.xz * 260.0 + v_position.y * 260.0));
      flake_boost = smoothstep(0.76, 0.99, flake) * 0.45;
      metal = max(frame.options.w, 0.52);
      gloss = 0.92;
      reflectivity = 0.08;
    } else if (finish == 2) {
      // Matte (ShaderSettings/Matte.xml): satin sheen, low environment mirror.
      metal = 0.04;
      gloss = 0.22;
      reflectivity = 0.025;
    } else if (finish == 3) {
      // Two-Tone Pearlescent (List_SpecialColors: TwoToneScale/Bias/Power).
      float tt_scale = max(frame.paint_finish.y, 0.15);
      float tt_bias = frame.paint_finish.z;
      float tt_pow = max(frame.paint_two_tone.w, 1.0);
      float fres = clamp(pow(1.0 - nv, clamp(tt_pow * 0.25, 0.8, 5.0)) * (1.2 + tt_scale * 2.0) + tt_bias, 0.0, 1.0);
      paint_col = mix(paint_col, frame.paint_two_tone.rgb, fres);
      float flake = hash21(floor(v_position.xz * 240.0 + v_position.y * 240.0));
      flake_boost = smoothstep(0.74, 0.99, flake) * 0.55;
      metal = 0.62;
      gloss = 0.94;
      reflectivity = 0.10;
    } else if (finish == 4) {
      // Chrome special finish.
      metal = 0.94;
      gloss = 0.97;
      reflectivity = 0.72;
    } else if (finish == 5) {
      // Carbon Fiber weave special finish.
      vec2 wuv = v_position.xz * 55.0 + v_position.yy * 55.0;
      float weave = sin(wuv.x) * cos(wuv.y);
      paint_col = mix(paint_col * 0.22, paint_col * 0.52, 0.5 + 0.5 * weave);
      metal = 0.18;
      gloss = 0.88;
      reflectivity = 0.06;
    } else if (finish == 6) {
      // Wood grain special finish.
      float ring = sin((v_position.z * 16.0 + fbm(v_position.xz * 6.0) * 4.0) * 3.14159);
      paint_col = mix(paint_col * 0.45, paint_col * 1.05, 0.5 + 0.5 * ring);
      metal = 0.02;
      gloss = 0.55;
      reflectivity = 0.04;
    } else if (finish == 7) {
      // Camouflage special finish.
      float c1 = fbm(v_position.xz * 3.2 + v_position.y * 2.4);
      float c2 = fbm(v_position.xz * 5.8 - v_position.y * 3.1 + 7.3);
      vec3 dark_c = paint_col * 0.30;
      vec3 mid_c = mix(paint_col, frame.paint_two_tone.rgb, 0.6);
      paint_col = c1 < 0.42 ? dark_c : (c2 > 0.56 ? mid_c : paint_col);
      metal = 0.0;
      gloss = 0.18;
      reflectivity = 0.02;
    } else {
      reflectivity = 0.05;
      gloss = 0.9;
      if ((flags & kFlagMetallic) != 0u) metal = frame.options.w;
    }

    vec3 texel = (flags & kFlagStripe) != 0u ? t0.rgb * frame.stripe.rgb : t0.rgb;
    base = mix(paint_col, texel, t0.a);
    if ((flags & kFlagRaceLivery) != 0u) base = t0.rgb;

    // Livery layer: options.z > 1.5 composites custom 1024^2 RGBA vinyls;
    // options.z in (0.5, 1.5) draws the legacy magenta mask overlay.
    if ((flags & kFlagLivery) != 0u && frame.options.z > 1.5) {
      vec4 liv = texture(livery, v_uv0);
      float mask_a = liv.a * (1.0 - t0.a);
      base = mix(base, liv.rgb, mask_a);
      // Vinyl decals have glossy clear-over-vinyl response.
      metal = mix(metal, 0.08, mask_a * 0.85);
      gloss = mix(gloss, 0.86, mask_a * 0.75);
    } else if ((flags & kFlagLivery) != 0u && frame.options.z > 0.5) {
      base = mix(base, vec3(1.0, 0.0, 1.0), 0.5 * texture(livery, v_uv0).r * (1.0 - t0.a));
    }
  } else if (klass == kCaliper) {
    base = mix(draw.tint.rgb, t0.rgb, t0.a);
    gloss = 0.65;
  } else if (klass == kGlass) {
    float wtint = clamp(frame.paint_finish.w, 0.0, 1.0);
    base = mix(vec3(0.01, 0.012, 0.016), vec3(0.002, 0.003, 0.004), wtint);
    reflectivity = 0.06;
    gloss = 0.95;
    alpha = mix(0.60, 0.94, wtint);
    if ((flags & kFlagCover) != 0u) {
      base = draw.tint.rgb;
      alpha = draw.tint.r - draw.tint.g > 0.1 ? 0.72 : 0.16;
    }
    if ((flags & kFlagLampEmissive) != 0u) {
      emissive = draw.tint.rgb;
    }
    // Cracked windshield / glass spiderweb fracture lines when damage_dirt.z > 0.
    float crack = frame.damage_dirt.z;
    if (crack > 0.05 && (flags & kFlagCover) == 0u) {
      vec2 cp = v_position.xz * 12.0 + v_position.y * 8.0;
      float radial = abs(sin(atan(cp.y, cp.x) * 9.0 + fbm(cp * 2.5) * 3.5));
      float rings = abs(sin(length(cp) * 7.0 + fbm(cp * 3.0) * 2.0));
      float line_mask = smoothstep(0.14, 0.02, min(radial, rings)) * smoothstep(0.05, 0.55, crack);
      base = mix(base, vec3(0.75, 0.80, 0.86), line_mask * 0.75);
      alpha = clamp(alpha + line_mask * 0.35, 0.0, 0.98);
      gloss = mix(gloss, 0.25, line_mask);
    }
  } else if (klass == kLamp) {
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    reflectivity = 0.3;
    gloss = 0.85;
    metal = 0.5;
    if ((flags & kFlagLampEmissive) != 0u) {
      emissive = draw.tint.rgb;
    }
  } else if (klass == kInterior || klass == kGauge) {
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    if (klass == kInterior) base = ((flags & kFlagSlot1) != 0u ? t1.rgb : vec3(0.01)) * draw.tint.rgb;
    if (klass == kGauge && (flags & kFlagLampEmissive) != 0u) {
      emissive = base * draw.tint.rgb;
    }
    gloss = 0.2;
  } else if (klass == kTyre) {
    base = (flags & kFlagSlot1) != 0u ? pow(t1.rgb, vec3(2.2)) : vec3(0.03);
    // Dynamic off-road tyre mud & dust coating (damage_dirt.y).
    if (frame.damage_dirt.y > 0.01) {
      float mud_n = fbm(v_position.xz * 18.0 + v_position.y * 14.0);
      float mud = clamp(frame.damage_dirt.y * (0.55 + 0.65 * mud_n), 0.0, 0.88);
      base = mix(base, vec3(0.16, 0.11, 0.065), mud);
    }
    gloss = 0.15;
  } else if (klass == kRim) {
    // Allow painted aftermarket rims when draw.tint != white.
    base *= draw.tint.rgb;
    float wblur = clamp(draw.params.w, 0.0, 1.0);
    if (wblur > 0.01 && (flags & kFlagNoSlot0) == 0u) {
      vec2 duv = v_uv0 - vec2(0.5);
      vec3 acc = t0.rgb * draw.tint.rgb;
      float span = wblur * 0.52;
      for (int k = -3; k <= 3; ++k) {
        if (k == 0) continue;
        float a = float(k) * (span / 3.0);
        float ca = cos(a), sa = sin(a);
        vec2 ruv = vec2(0.5) + vec2(ca * duv.x - sa * duv.y, sa * duv.x + ca * duv.y);
        acc += texture(slot0, ruv).rgb * draw.tint.rgb;
      }
      base = mix(base, acc / 7.0, wblur * 0.92);
      base = mix(base, vec3(dot(base, vec3(0.3, 0.59, 0.11))) * 1.08, wblur * 0.22);
    }
    gloss = mix(0.75, 0.86, clamp(draw.params.w, 0.0, 1.0));
    metal = 0.6;
  } else if (klass == kChrome) {
    float wblur = clamp(draw.params.w, 0.0, 1.0);
    if (wblur > 0.01 && (flags & kFlagNoSlot0) == 0u) {
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
    gloss = 0.95;
    metal = 0.9;
  } else if (klass == kWorld) {
    base = draw.tint.rgb;
    reflectivity = 0.02;
    gloss = 0.05;
  } else if (klass == kCarbon) {
    base = t0.rgb * ((flags & kFlagSlot1) != 0u ? t1.rgb : vec3(1.0));
    reflectivity = 0.05;
    gloss = 0.85;
  }

  // Cosmetic scratches, scuffs & exposed primer/metal on impacted body zones,
  // plus dynamic lower-body & wheel-arch dirt/mud accumulation.
  if (klass == kPaint || klass == kOpaque || klass == kCarbon || klass == kRim) {
    float z_front = smoothstep(-0.65, -1.95, v_position.z) * frame.damage_zones.x;
    float z_rear = smoothstep(0.65, 1.95, v_position.z) * frame.damage_zones.y;
    float x_left = smoothstep(-0.50, -0.95, v_position.x) * frame.damage_zones.z;
    float x_right = smoothstep(0.50, 0.95, v_position.x) * frame.damage_zones.w;
    float local_dmg = clamp(max(max(z_front, z_rear), max(x_left, x_right)), 0.0, 1.0);
    if (local_dmg > 0.02 && frame.damage_dirt.w > 0.01) {
      // Fine directional scratch streaks + scuffed grey primer/bare metal patches.
      float streaks = fbm(vec2(v_position.z * 34.0, v_position.y * 95.0 + v_position.x * 22.0));
      float blotch = fbm(v_position.xz * 7.5 + v_position.y * 9.0);
      float scuff = smoothstep(0.42, 0.74, blotch * 0.55 + streaks * 0.65) * local_dmg * frame.damage_dirt.w;
      vec3 bare_metal = mix(vec3(0.18, 0.19, 0.21), vec3(0.58, 0.60, 0.64), smoothstep(0.65, 0.90, streaks));
      base = mix(base, bare_metal, clamp(scuff * 0.92, 0.0, 0.92));
      gloss = mix(gloss, 0.14, clamp(scuff * 0.85, 0.0, 0.85));
      metal = mix(metal, 0.35, clamp(scuff * 0.50, 0.0, 0.50));
    }
    if (frame.damage_dirt.x > 0.01) {
      // Lower-body road grit & wheel-arch mud splatter (strongest near ground y < 0.55 m).
      float low_mask = smoothstep(0.72, 0.02, v_position.y);
      float splat = fbm(v_position.xz * 9.5 + v_position.y * 12.0);
      float specks = vnoise(v_position.xz * 42.0 + v_position.y * 38.0);
      float dirt = clamp(frame.damage_dirt.x * (low_mask * (0.65 + 0.55 * splat) + 0.22 * splat) *
                             (0.65 + 0.55 * specks),
                         0.0, 0.88);
      vec3 mud_col = mix(vec3(0.18, 0.12, 0.075), vec3(0.28, 0.20, 0.13), splat);
      base = mix(base, mud_col, dirt);
      gloss = mix(gloss, 0.08, dirt * 0.92);
      metal = mix(metal, 0.0, dirt * 0.95);
    }
  }

  float occlusion = mix(0.35, 1.0, v_occlusion);
  if (klass == kInterior || klass == kGauge) occlusion *= 0.4;
  vec3 key = normalize(frame.key_light.xyz), fill = normalize(frame.fill_light.xyz);
  vec3 diffuse_light = env(n, 7.0) * frame.tint_light.rgb * occlusion +
                       (frame.key_light.w * max(dot(n, key), 0.0) * mix(0.5, 1.0, occlusion) +
                        frame.fill_light.w * max(dot(n, fill), 0.0) * occlusion) * frame.tint_light.rgb;
  float fresnel = reflectivity + (1.0 - reflectivity) * pow(1.0 - nv, 5.0);
  vec3 specular_colour = mix(vec3(fresnel), base * mix(0.5, 1.0, fresnel), metal);
  vec3 reflection = env(r, (1.0 - gloss) * 7.0) * frame.reflect_tint.rgb * occlusion;
  vec3 h = normalize(view + key);
  vec3 highlight = pow(max(dot(n, h), 0.0), mix(8.0, 400.0, gloss)) * frame.key_light.w * gloss * frame.tint_light.rgb;
  if (flake_boost > 0.0) {
    highlight += base * flake_boost * pow(max(dot(n, h), 0.0), 18.0) * frame.key_light.w;
  }

  if (frame.drive.w > 0.5) {
    vec3 up = normalize(frame.drive.xyz);
    vec3 sun_col = frame.sun_color.w >= 0.0 ? frame.sun_color.rgb : kSunColour;
    vec3 sky_col = frame.sun_color.w >= 0.0 ? frame.car_light_top.rgb : kSkyColour;
    vec3 gnd_col = frame.sun_color.w >= 0.0 ? frame.car_light_bot.rgb : kGroundColour;
    float shadow_scale = frame.sun_color.w >= 0.0 ? clamp(frame.sun_color.w, 0.0, 1.0) : 1.0;
    float ndl = max(dot(n, key), 0.0);
    float raw_lit = ndl > 0.0 ? drive_shadow(v_position, n) : 1.0;
    float lit = mix(1.0, raw_lit, shadow_scale);
    diffuse_light = sun_col * ndl * lit * mix(0.6, 1.0, occlusion) +
                    mix(gnd_col, sky_col, 0.5 + 0.5 * dot(n, up)) * occlusion * 1.4;
    reflection *= mix(0.5, 1.0, lit);
    float power = mix(8.0, 400.0, gloss);
    highlight = sun_col * pow(max(dot(n, h), 0.0), power) * (power + 8.0) * 0.04 * ndl * lit * gloss;
  }
  vec3 colour = base * (1.0 - metal) * diffuse_light + specular_colour * (reflection + highlight) + emissive;
  if ((flags & kFlagMirror) != 0u) {
    if (frame.shadow_far_bias.w > 0.5) {
      float rz = max(r.z, 0.2);
      vec2 muv = vec2(0.5 - (r.x / rz) / (2.0 * 1.3773), 0.5 - (r.y / rz) / (2.0 * 0.3443));
      if (muv.x >= 0.0 && muv.x <= 1.0 && muv.y >= 0.0 && muv.y <= 1.0) {
        colour = texture(mirror_map, muv).rgb * 0.9;
      } else {
        colour = env(r, 0.0) * 0.85;
      }
    } else {
      colour = env(r, 0.5) * 0.85;
    }
    alpha = 1.0;
  } else if (klass == kGlass) {
    colour = base * alpha + specular_colour * (reflection + highlight) + emissive;
    alpha = clamp(alpha + fresnel * 0.5, 0.0, 1.0);
  }
  colour *= frame.options.x;
  out_color = vec4(out_rgb(colour), alpha);
}
