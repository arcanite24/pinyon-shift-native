#version 450
// carview showroom: our own car shader and lighting approximation.
//
// Paint shows where the atlas alpha is 0 on paint techniques (the colour
// arrives converted from sRGB to linear); callipers are tinted the same way
// with their unconverted colour. Lighting: a fixed environment cube (the
// disc's lobby faces, or a gradient) for diffuse and reflections, a key and
// a fill light, and the vertex's baked occlusion. Glass is tinted and
// see-through. Output is encoded to sRGB here (the targets are UNORM).
// Spec: FORMAT-CARS (Materials, Paint, Brakes, Lights, glass and shadow).

layout(set = 0, binding = 0) uniform Frame {
  mat4 view_projection;
  vec4 eye;
  vec4 key_light;
  vec4 fill_light;
  vec4 options;
  vec4 tint_light;
  vec4 reflect_tint;
} frame;
layout(set = 0, binding = 1) uniform samplerCube environment;

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
           kTyre = 6u, kRim = 7u, kCaliper = 8u, kChrome = 9u, kCarbon = 10u;
const uint kFlagSlot1 = 1u, kFlagMetallic = 2u, kFlagLivery = 8u, kFlagNoSlot0 = 16u;

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 env(vec3 dir, float lod) {
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
  if (dot(n, view) < 0.0) n = -n;  // either side of a two-sided part
  vec3 r = reflect(-view, n);
  float nv = clamp(dot(n, view), 0.0, 1.0);

  vec3 base = t0.rgb;
  float reflectivity = 0.04;   // at normal incidence
  float gloss = 0.3;           // 0 rough .. 1 mirror
  float metal = 0.0;
  float alpha = 1.0;
  vec3 emissive = vec3(0.0);

  if (klass == kPaint) {
    base = mix(draw.tint.rgb, t0.rgb, t0.a);
    reflectivity = 0.05;
    gloss = 0.9;
    if ((flags & kFlagMetallic) != 0u) metal = frame.options.w;  // flakes: less diffuse, tinted reflection
    if ((flags & kFlagLivery) != 0u && frame.options.z > 0.5)
      base = mix(base, vec3(1.0, 0.0, 1.0), 0.5 * texture(livery, v_uv0).r * (1.0 - t0.a));
  } else if (klass == kCaliper) {
    base = mix(draw.tint.rgb, t0.rgb, t0.a);
    gloss = 0.6;
  } else if (klass == kGlass) {
    // Tinted and dark: the captures show the cabin only dimly through it.
    base = vec3(0.01, 0.012, 0.016);
    reflectivity = 0.06;
    gloss = 0.95;
    alpha = 0.6;
  } else if (klass == kLamp) {
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    reflectivity = 0.3;
    gloss = 0.85;
    metal = 0.5;
  } else if (klass == kInterior || klass == kGauge) {
    base = (flags & kFlagSlot1) != 0u ? t1.rgb : t0.rgb;
    gloss = 0.2;
  } else if (klass == kTyre) {
    // The sidewall and tread sheets are single-channel (BC4, no sRGB form).
    base = (flags & kFlagSlot1) != 0u ? pow(t1.rgb, vec3(2.2)) : vec3(0.03);
    gloss = 0.15;
  } else if (klass == kRim) {
    gloss = 0.75;
    metal = 0.6;
  } else if (klass == kChrome) {
    gloss = 0.95;
    metal = 0.9;
  } else if (klass == kCarbon) {
    base = t0.rgb * ((flags & kFlagSlot1) != 0u ? t1.rgb : vec3(1.0));
    reflectivity = 0.05;
    gloss = 0.85;
  }

  float occlusion = mix(0.35, 1.0, v_occlusion);
  // The cabin is lit through the glass and roof: dimmer than the body.
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

  vec3 colour = base * (1.0 - metal) * diffuse_light + specular_colour * (reflection + highlight) + emissive;
  if (klass == kGlass) {
    // Premultiplied: the tint over what is behind, plus the reflection.
    colour = base * alpha + specular_colour * (reflection + highlight);
    alpha = clamp(alpha + fresnel * 0.5, 0.0, 1.0);
  }
  colour *= frame.options.x;
  out_color = vec4(encode_srgb(colour), alpha);
}
