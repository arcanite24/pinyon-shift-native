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
} frame;
layout(set = 0, binding = 1) uniform samplerCube environment;
layout(set = 0, binding = 2) uniform sampler2DShadow shadow_map;
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

layout(location = 0) out vec4 out_color;

const uint kOpaque = 0u, kPaint = 1u, kGlass = 2u, kLamp = 3u, kInterior = 4u, kGauge = 5u,
           kTyre = 6u, kRim = 7u, kCaliper = 8u, kChrome = 9u, kCarbon = 10u, kWorld = 11u;
const uint kFlagSlot1 = 1u, kFlagMetallic = 2u, kFlagLivery = 8u, kFlagNoSlot0 = 16u, kFlagStripe = 32u, kFlagFlat = 64u,
           kFlagRaceLivery = 128u, kFlagGrille = 256u, kFlagCutout = 512u, kFlagCover = 1024u,
           kFlagLampEmissive = 2048u, kFlagMirror = 4096u, kFlagInstanced = 8192u,
           kFlagNormalMap = 524288u, kFlagNormalUv1 = 1048576u, kFlagDamageMap = 2097152u;

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
  return textureLod(static_environment, dir, lod).rgb * frame.options.y;
}

void main() {
  uint klass = uint(draw.params.x + 0.5);
  uint flags = uint(draw.params.y + 0.5);
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
  vec3 n_geom = normalize(v_normal);
  float flip_n = dot(n_geom, view) < 0.0 ? -1.0 : 1.0;
  vec3 n = n_geom * flip_n;
  // Tangent-space normal map perturbation (carbonFiberNormal, sidewall/tread NRM, leather/plastic/stitches NRM).
  if ((flags & kFlagNormalMap) != 0u) {
    vec2 nm_uv = (klass == kCarbon) ? (v_uv1 * 1.5) : ((flags & kFlagNormalUv1) != 0u ? v_uv1 : v_uv0);
    vec2 nm_xy = texture(normal_map, nm_uv).xy * 2.0 - 1.0;
    float nm_strength = (klass == kInterior) ? 1.35 : (klass == kTyre ? 1.15 : 1.0);
    nm_xy *= nm_strength;
    float nz = sqrt(max(1.0 - dot(nm_xy, nm_xy), 0.04));
    vec3 T = v_tangent;
    vec3 B = v_bitangent;
    float tlen = length(T);
    float blen = length(B);
    if (tlen > 1e-4 && blen > 1e-4) {
      T = normalize(T - n * dot(T, n));
      B = normalize(B - n * dot(B, n));
      n = normalize(T * nm_xy.x + B * nm_xy.y + n * nz);
    }
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

  if ((flags & kFlagGrille) != 0u) {
    if ((flags & kFlagCutout) != 0u && t1.a < 0.5) discard;
    base = t1.rgb;
  }

  if (klass == kPaint) {
    vec3 paint_col = draw_tint.rgb;
    int finish = int(frame.paint_finish.x + 0.5);
    if (finish == 1) {
      // Metallic Flake (ShaderSettings/Metallic.xml): crisp sparkle & dual-lobe metallic depth.
      float flake = hash21(floor(v_position.xz * 260.0 + v_position.y * 260.0));
      flake_boost = smoothstep(0.76, 0.99, flake) * 0.55;
      is_clearcoat = true;
      paint_metallic = 1.0;
      paint_scale = 0.50;
      gloss = 0.94;
      reflectivity = 0.047;
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
      flake_boost = smoothstep(0.74, 0.99, flake) * 0.60;
      is_clearcoat = true;
      paint_metallic = 1.0;
      paint_scale = 0.50;
      gloss = 0.95;
      reflectivity = 0.047;
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
      is_clearcoat = true;
      paint_metallic = 0.20;
      paint_scale = 0.90;
      gloss = 0.92;
      reflectivity = 0.047;
    } else if (finish == 6) {
      // Wood grain special finish.
      float ring = sin((v_position.z * 16.0 + fbm(v_position.xz * 6.0) * 4.0) * 3.14159);
      paint_col = mix(paint_col * 0.45, paint_col * 1.05, 0.5 + 0.5 * ring);
      is_clearcoat = true;
      paint_metallic = 0.0;
      paint_scale = 1.0;
      gloss = 0.78;
      reflectivity = 0.047;
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
      // FH1 stock paint (Normal.xml / Metallic.xml): always 2-layer clearcoat;
      // metallic rows get PaintScale=0.5 + blurred metallic cube + dual flake lobes.
      is_clearcoat = true;
      reflectivity = 0.047;
      gloss = 0.95;
      if ((flags & kFlagMetallic) != 0u) {
        paint_metallic = 1.0;
        paint_scale = 0.50;
        float flake = hash21(floor(v_position.xz * 260.0 + v_position.y * 260.0));
        flake_boost = smoothstep(0.80, 0.99, flake) * 0.30;
      }
    }

    vec3 texel = (flags & kFlagStripe) != 0u ? t0.rgb * stripe_rgb : t0.rgb;
    base = mix(paint_col, texel, t0.a);
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
      float wtint = clamp(frame.paint_finish.w, 0.0, 1.0);
      base = mix(vec3(0.0, 0.055, 0.045), vec3(0.002, 0.005, 0.004), wtint);
      reflectivity = 0.047;
      gloss = 0.97;
      alpha = mix(0.29, 0.88, wtint);
      if ((flags & kFlagLampEmissive) != 0u) {
        emissive = draw_tint.rgb;
      }
      // Real FH1 Shared/glass_texture.xds fracture overlay when damage_dirt.z > 0.05.
      float crack = frame.damage_dirt.z;
      if (crack > 0.05) {
        vec4 gt = texture(shared_glass_crack, v_uv0 * 2.5);
        float c_mask = clamp(max(gt.a, dot(gt.rgb, vec3(0.3333))) * smoothstep(0.05, 0.55, crack), 0.0, 1.0);
        base = mix(base, vec3(0.72, 0.78, 0.84), c_mask * 0.75);
        alpha = clamp(alpha + c_mask * 0.42, 0.0, 0.96);
        gloss = mix(gloss, 0.25, c_mask);
      }
    }
  } else if (klass == kLamp) {
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    reflectivity = 0.3;
    gloss = 0.85;
    metal = 0.5;
    if ((flags & kFlagLampEmissive) != 0u) {
      // Modulate HDR lamp emission by lights_lod0.xds reflector detail and Fresnel angle so
      // individual rings, housings and bezels read clearly at night instead of flat solid discs.
      float luma = dot(base, vec3(0.299, 0.587, 0.114));
      float lens_detail = mix(0.14, 1.38, smoothstep(0.03, 0.72, luma));
      float ang = 0.36 + 0.64 * pow(nv, 1.15);
      emissive = draw_tint.rgb * lens_detail * ang * (kLinearOut ? 1.85 : 0.95);
    }
  } else if (klass == kInterior || klass == kGauge) {
    if ((flags & kFlagCutout) != 0u && (flags & kFlagSlot1) != 0u && t1.a < 0.5) discard;
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    if (submat == 15u) {
      // Illuminated analog gauge needle (speed_min/tach_min/fuel_min/boost_min): crisp backlit orange-red.
      base = mix(vec3(0.85, 0.14, 0.03), base, 0.25);
      emissive = vec3(0.95, 0.18, 0.04) * (kLinearOut ? 0.65 : 0.55);
      gloss = 0.35;
    } else if (submat == 16u) {
      // Digital MFD / cockpit LCD screen: dark anti-glare glass with subtle cool backlight instead of flat grey.
      base = mix(vec3(0.012, 0.018, 0.026), base * 0.18, 0.30);
      emissive = vec3(0.014, 0.028, 0.042) + ((flags & kFlagLampEmissive) != 0u ? base * draw_tint.rgb * 0.35 : vec3(0.0));
      gloss = 0.88;
      reflectivity = 0.06;
    } else if (submat == 17u) {
      // Authentic FH1 interior_emissive_LOD0.xds backlit instrument cluster (speedo/tach/oil/fuel markings).
      vec3 dial_bg = (flags & kFlagNoSlot0) == 0u ? t0.rgb * 0.35 : vec3(0.015, 0.016, 0.018);
      vec3 mark = (flags & kFlagSlot1) != 0u ? t1.rgb : vec3(0.0);
      base = max(dial_bg, mark * 0.65);
      float lamp_boost = (flags & kFlagLampEmissive) != 0u ? max(0.75, max(draw_tint.r, max(draw_tint.g, draw_tint.b))) : 0.72;
      emissive = mark * lamp_boost * (kLinearOut ? 1.15 : 0.90);
      gloss = 0.32;
    } else if (klass == kInterior) {
      // FH1 interior materials use a dark charcoal cabin base modulated by nodamage AO (t0),
      // with interior_lod0.xds (t1) composited as crisp alpha-cutout button/badge decals (t1.a).
      vec3 int_base = (submat == 5u) ? vec3(0.24, 0.25, 0.27) : vec3(0.034, 0.036, 0.040);
      vec3 ao = (flags & kFlagNoSlot0) == 0u ? mix(vec3(0.45), t0.rgb, 0.72) : vec3(1.0);
      vec3 mat_base = int_base * ao;
      if ((flags & kFlagSlot1) != 0u) mat_base = mix(mat_base, t1.rgb * 0.85, t1.a);
      base = mat_base * draw_tint.rgb;
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
    // Dynamic off-road tyre mud & dust coating (damage_dirt.y).
    if (frame.damage_dirt.y > 0.01) {
      float mud_n = fbm(v_position.xz * 18.0 + v_position.y * 14.0);
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
    vec3 rim_paint = (submat == 2u || submat == 3u) ? rim_default : (rim_default * draw_tint.rgb);
    if ((flags & kFlagNoSlot0) == 0u) {
      base = mix(rim_paint, t0.rgb, t0.a);
    } else {
      base = rim_paint;
    }
    float wblur = clamp(draw.params.w, 0.0, 1.0);
    if (wblur > 0.01 && (flags & kFlagNoSlot0) == 0u) {
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
      // Standard painted/machined alloy rim spokes (Tire/Normal.xml).
      gloss = mix(0.80, 0.90, clamp(draw.params.w, 0.0, 1.0));
      metal = 0.66;
      reflectivity = 0.42;
    }
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
  }

  // Real FH1 damage_LOD0.xds impact scratches/scuffs & Shared/dirt_pattern + dirt_mask accumulation.
  if (klass == kPaint || klass == kOpaque || klass == kCarbon || klass == kRim) {
    float z_front = smoothstep(-0.65, -1.95, v_position.z) * frame.damage_zones.x;
    float z_rear = smoothstep(0.65, 1.95, v_position.z) * frame.damage_zones.y;
    float x_left = smoothstep(-0.50, -0.95, v_position.x) * frame.damage_zones.z;
    float x_right = smoothstep(0.50, 0.95, v_position.x) * frame.damage_zones.w;
    float local_dmg = clamp(max(max(z_front, z_rear), max(x_left, x_right)), 0.0, 1.0);
    if (local_dmg > 0.02 && frame.damage_dirt.w > 0.01) {
      float dmg_amt = clamp(local_dmg * frame.damage_dirt.w, 0.0, 1.0);
      if ((flags & kFlagDamageMap) != 0u) {
        vec4 dm = texture(damage_map, v_uv0);
        float dmask = clamp(max(dm.a, length(dm.rgb - t0.rgb) * 1.4) * dmg_amt, 0.0, 0.95);
        base = mix(base, dm.rgb, dmask);
        gloss = mix(gloss, 0.15, dmask * 0.85);
        coat_mask *= (1.0 - dmask * 0.90);
      } else {
        vec3 dp = texture(shared_dirt_pattern, v_uv0 * 4.0).rgb;
        float scuff = clamp(dmg_amt * 0.85, 0.0, 0.90);
        base = mix(base, mix(vec3(0.22), dp, 0.35), scuff);
        gloss = mix(gloss, 0.14, scuff * 0.85);
        coat_mask *= (1.0 - scuff * 0.90);
      }
    }
    if (frame.damage_dirt.x > 0.01) {
      float low_mask = smoothstep(0.76, 0.02, v_position.y);
      vec2 dirt_uv = v_position.xz * 0.75 + vec2(v_position.y * 0.9, v_position.y * 0.6);
      vec4 dpat = texture(shared_dirt_pattern, dirt_uv * 2.5);
      vec4 dmsk = texture(shared_dirt_mask, dirt_uv * 1.2);
      float m = clamp(max(dmsk.r, dmsk.a), 0.15, 1.0);
      float dirt = clamp(frame.damage_dirt.x * (low_mask * 0.85 + 0.20) * m, 0.0, 0.88);
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
    float shadow_scale = frame.sun_color.w >= 0.0 ? clamp(frame.sun_color.w, 0.0, 1.0) : 1.0;
    float raw_lit = ndl > 0.0 ? drive_shadow(v_position, n) : 1.0;
    lit = mix(1.0, raw_lit, shadow_scale);
    // Removed ad-hoc * 1.4 ambient boost so clearcoat reflections contrast cleanly against diffuse paint.
    diffuse_light = sun_col * ndl * lit * mix(0.6, 1.0, occlusion) +
                    mix(gnd_col, sky_col, 0.5 + 0.5 * dot(n, up)) * occlusion;
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
    // True FH1 Two-Layer Car Paint BRDF (Normal.xml + Metallic.xml):
    // Layer 1 (Clearcoat): neutral dielectric Fresnel (IOR=1.55, F0=0.047, FresnelScalar=0.90)
    //   sampling sharp mip-0 cubemap + tight white sun highlight.
    // Layer 2 (Basecoat, attenuated by 1 - F_coat): diffuse * PaintScale + (when metallic)
    //   base-tinted blurred glossy cubemap + dual broad metallic flake lobes (Power2=6, Power3=4).
    float F_coat = clamp(0.047 + 0.90 * pow(1.0 - nv, 4.2), 0.047, 0.95) * coat_mask;
    vec3 coat_refl = env(r, 0.0) * frame.reflect_tint.rgb * occ_refl;
    vec3 coat_spec = direct_col * pow(nh, 110.0) * 2.2 * ndl * lit * coat_mask;

    vec3 basecoat = base * paint_scale * diffuse_light;
    if (paint_metallic > 0.01) {
      vec3 met_cube = env(r, 3.2) * frame.reflect_tint.rgb * occ_refl;
      float broad2 = pow(nh, 6.0) * 0.52;
      float broad3 = pow(nh, 4.0) * 0.28;
      basecoat += base * met_cube * 0.55 * paint_metallic;
      basecoat += base * direct_col * (broad2 + broad3) * ndl * lit * paint_metallic;
      if (flake_boost > 0.0) {
        basecoat += base * direct_col * flake_boost * pow(nh, 22.0) * ndl * lit;
      }
    }
    colour = basecoat * (1.0 - F_coat) + F_coat * coat_refl + coat_spec + emissive;
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
      colour = F_cov * cov_refl + cov_spec;
      alpha = 1.0;
    } else if (submat == 6u) {
      colour = base * (diffuse_light + vec3(0.02)) * alpha;
    } else {
      // FH1 window glass: strong grazing Fresnel (4.35 / 0.70), sharp mip-0 reflection, clear straight-through.
      // Lit base tint avoids unlit khaki/tan rear-window glow under high midnight post-exposure.
      float F_glass = clamp(0.047 + 0.70 * pow(1.0 - nv, 4.35), 0.047, 0.92);
      vec3 glass_refl = env(r, 0.0) * frame.reflect_tint.rgb * occ_refl;
      vec3 glass_spec = direct_col * pow(nh, 140.0) * 2.2 * ndl * lit;
      colour = base * diffuse_light * alpha + F_glass * glass_refl + glass_spec + emissive;
      alpha = clamp(alpha + F_glass * 0.65, 0.0, 0.96);
    }
  }
  colour *= frame.options.x;
  out_color = vec4(out_rgb(colour), alpha);
}
