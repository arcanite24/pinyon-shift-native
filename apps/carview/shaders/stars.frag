#version 450
// carview starfield instanced quad fragment shader (SPEC-SKY-AND-TOD §3.3).
layout(constant_id = 0) const bool kLinearOut = false;

layout(location = 0) in float v_alpha;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
  if (v_alpha <= 0.001) discard;
  vec2 d = v_uv - vec2(0.5);
  float r2 = dot(d, d);
  if (r2 > 0.25) discard;
  float soft = clamp(1.0 - r2 * 4.0, 0.0, 1.0);
  float a = v_alpha * soft * soft;
  // Slightly cool-white starlight in HDR linear space.
  vec3 col = vec3(0.92, 0.96, 1.0) * (kLinearOut ? 1.8 : 1.0);
  out_color = vec4(col * a, a);
}
