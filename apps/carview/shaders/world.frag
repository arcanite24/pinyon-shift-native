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
} frame;
layout(set = 0, binding = 2) uniform sampler2DShadow shadow_map;

layout(set = 1, binding = 0) uniform sampler2D albedo;
layout(set = 1, binding = 1) uniform sampler2D layer_b;    // Blend_B
layout(set = 1, binding = 2) uniform sampler2D splat_map;  // Splat

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
           kFlagBillboard = 32u, kFlagAlphaTest = 64u, kFlagLodNear = 128u, kFlagLodFar = 256u, kFlagLodOne = 512u;

// The screen-door threshold of this pixel (interleaved gradient noise): one
// pattern shared by every draw, so fades that split a pixel between two
// draws by it leave neither a gap nor a double (ours).
float dither_threshold() {
  return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

// The sky gradient (ours, linear): pale haze at the horizon to blue above.
const vec3 kSkyHorizon = vec3(0.42, 0.56, 0.72);
const vec3 kSkyZenith = vec3(0.05, 0.16, 0.50);

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

vec3 encode_srgb(vec3 c) {
  c = max(c, vec3(0.0));
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// The drive renders linear HDR for its post chain (post.frag), clipped at
// 4.0 as the game's resolve is; the showroom encodes sRGB here.
layout(constant_id = 0) const bool kLinearOut = false;
vec3 out_rgb(vec3 c) { return kLinearOut ? min(max(c, vec3(0.0)), vec3(4.0)) : encode_srgb(c); }

// The drive's sun shadow (ours): one orthographic map around the car.
// shadow: x on, y one texel in the map's UV, z the sun's share left in
// shadow, w taps (1 or 4); shadow_bias.x the normal offset in metres.
// 1 where lit; off (the showroom) always 1.
float sun_shadow(vec3 p, vec3 n) {
  if (frame.shadow.x < 0.5) return 1.0;
  vec4 s = frame.shadow_matrix * vec4(p + n * frame.shadow_bias.x, 1.0);
  float edge = max(abs(s.x), abs(s.y));
  if (edge >= 1.0 || s.z <= 0.0 || s.z >= 1.0) return 1.0;
  vec2 uv = s.xy * 0.5 + 0.5;
  float lit;
  if (frame.shadow.w > 1.5) {
    float t = frame.shadow.y * 0.5;
    lit = 0.25 * (texture(shadow_map, vec3(uv + vec2(-t, -t), s.z)) + texture(shadow_map, vec3(uv + vec2(t, -t), s.z)) +
                  texture(shadow_map, vec3(uv + vec2(-t, t), s.z)) + texture(shadow_map, vec3(uv + vec2(t, t), s.z)));
  } else {
    lit = texture(shadow_map, vec3(uv, s.z));
  }
  // Faded out towards the box's edge.
  lit = mix(lit, 1.0, smoothstep(0.8, 1.0, edge));
  return mix(frame.shadow.z, 1.0, lit);
}

void main() {
  uint blend = uint(draw.params.x + 0.5);
  uint flags = uint(draw.params.y + 0.5);
  if ((flags & kFlagSky) != 0u) {
    out_color = vec4(out_rgb(sky_colour(normalize(v_position), normalize(draw.up.xyz), normalize(draw.sun.xyz))), 1.0);
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
  vec4 t;
  if ((flags & kFlagTextured) != 0u) {
    t = texture(albedo, v_uv0 * draw.tint.xy);
    if ((flags & (kFlagVertexBlend | kFlagSplat)) != 0u) {
      vec4 b = texture(layer_b, v_uv0 * draw.tint.zw);
      float w = (flags & kFlagSplat) != 0u ? texture(splat_map, v_uv1).r : v_colour.r;
      t = mix(t, b, w);
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

  vec3 up = normalize(draw.up.xyz);
  // The authored normal (not turned to the eye: terrain and leaf cards above
  // the eye would go dark); leaf cards without one take the up axis.
  vec3 n = dot(v_normal, v_normal) > 1e-6 ? normalize(v_normal) : up;
  vec3 sun = normalize(draw.sun.xyz);
  float ndl = max(dot(n, sun), 0.0);
  // Tree sprites cast but do not receive: in the shadow pass they face the
  // sun, here the eye, so they would shadow themselves (ours).
  float shade = ndl > 0.0 && (flags & kFlagBillboard) == 0u ? sun_shadow(v_position, n) : 1.0;
  vec3 light = kSunColour * ndl * shade + mix(kGroundColour, kSkyColour, 0.5 + 0.5 * dot(n, up));
  vec3 colour = t.rgb * light * draw.up.w * (draw.params.w > 0.0 ? draw.params.w : 1.0);

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
