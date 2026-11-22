#version 450
// carview additive lamp & streetlamp glow fragment shader (CAR_LAMPS.md §4.3).
layout(constant_id = 0) const bool kLinearOut = false;

layout(set = 1, binding = 0) uniform sampler2D glow_tex[7];

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in int v_tex;

layout(location = 0) out vec4 out_color;

void main() {
  if (v_color.a <= 0.001) discard;
  vec4 t;
  if (v_tex == 0) t = texture(glow_tex[0], v_uv);
  else if (v_tex == 1) t = texture(glow_tex[1], v_uv);
  else if (v_tex == 2) t = texture(glow_tex[2], v_uv);
  else if (v_tex == 3) t = texture(glow_tex[3], v_uv);
  else if (v_tex == 4) t = texture(glow_tex[4], v_uv);
  else if (v_tex == 5) t = texture(glow_tex[5], v_uv);
  else t = texture(glow_tex[6], v_uv);

  // Use radial texture brightness * alpha; additive blend (SRC_ALPHA, ONE).
  float mask = max(t.a, dot(t.rgb, vec3(0.3333)));
  if (mask <= 0.004) discard;
  vec3 col = t.rgb * v_color.rgb;
  if (kLinearOut) col = min(col, vec3(4.0));
  out_color = vec4(col, clamp(mask * v_color.a, 0.0, 1.0));
}
