#version 450
// carview additive lamp & streetlamp glow fragment shader (CAR_LAMPS.md §4.3).
layout(constant_id = 0) const bool kLinearOut = false;

layout(set = 1, binding = 0) uniform sampler2D glow_tex[7];

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in int v_tex;
layout(location = 3) flat in float v_world_corona;

layout(location = 0) out vec4 out_color;

void main() {
  if (v_color.a <= 0.001) discard;
  // Strictly circular radial envelope so square quad corners are always exact zero
  // even under high night auto-exposure (+4 EV) and bloom.
  vec2 d = v_uv * 2.0 - 1.0;
  float r2 = dot(d, d);
  if (r2 >= 1.0) discard;
  float r = sqrt(r2);
  float radial_fade = 1.0 - smoothstep(0.72, 1.0, r);

  vec3 col;
  if (v_world_corona >= 0.5) {
    // World streetlamp / outpost / festival fixture corona: pinpoint bulb core + soft atmospheric halo
    // (never a hard-edged bright cyan-white orb under night auto-exposure).
    float halo = pow(max(1.0 - r, 0.0), 2.6) * 0.65;
    float core = exp(-r2 * 22.0) * 0.90;
    float mask = (halo + core) * radial_fade;
    if (mask <= 0.001) discard;
    col = v_color.rgb * mask;
    if (kLinearOut) col = min(col, vec3(3.5));
  } else {
    vec4 t;
    if (v_tex == 0) t = texture(glow_tex[0], v_uv);
    else if (v_tex == 1) t = texture(glow_tex[1], v_uv);
    else if (v_tex == 2) t = texture(glow_tex[2], v_uv);
    else if (v_tex == 3) t = texture(glow_tex[3], v_uv);
    else if (v_tex == 4) t = texture(glow_tex[4], v_uv);
    else if (v_tex == 5) t = texture(glow_tex[5], v_uv);
    else t = texture(glow_tex[6], v_uv);

    // FH1 headlight_glow_*.xds are sRGB RGB-only additive textures (alpha == 255 everywhere,
    // with a 1/255 blue quantization floor in the corners of glow_1 and glow_4).
    // Subtract the 2/255 floor, decode sRGB->linear, and apply the circular edge fade.
    vec3 tex_lin = pow(max(t.rgb - vec3(2.0 / 255.0), vec3(0.0)), vec3(2.2)) * radial_fade;
    if (max(tex_lin.r, max(tex_lin.g, tex_lin.b)) <= 0.0005) discard;
    col = tex_lin * v_color.rgb;
    if (kLinearOut) col = min(col, vec3(64.0));
  }
  out_color = vec4(col, clamp(v_color.a, 0.0, 1.0));
}
