#version 450
// Soft multiply-blended elliptical/car-footprint under-car contact shadow.
// Pipeline uses Multiply blend (srcColor=ZERO, dstColor=SRC_COLOR) so output
// rgb = 1.0 leaves the road untouched and ~0.16 grounds the car chassis & tyres.

layout(location = 0) in vec2 v_uv;       // [-1..1] across car footprint
layout(location = 1) flat in vec4 v_params;  // x strength, y min_dark, z corner_r

layout(location = 0) out vec4 out_color;

float rounded_box_sdf(vec2 p, vec2 b, float r) {
  vec2 d = abs(p) - b + vec2(r);
  return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - r;
}

void main() {
  float strength = clamp(v_params.x, 0.0, 1.0);
  float min_dark = clamp(v_params.y, 0.05, 0.95);
  float corner_r = max(v_params.z, 0.15);

  // Soft outer body AO + tight dark core directly under the chassis sill.
  float sd_outer = rounded_box_sdf(v_uv, vec2(0.76, 0.80), corner_r);
  float body_outer = 1.0 - smoothstep(-0.08, 0.24, sd_outer);
  float sd_core = rounded_box_sdf(v_uv, vec2(0.56, 0.64), corner_r * 0.8);
  float body_core = 1.0 - smoothstep(-0.18, 0.16, sd_core);

  // Four wheel contact patch darkenings (near LF/RF/LR/RR corners).
  vec2 w = abs(v_uv) - vec2(0.58, 0.54);
  float wheel_ao = exp(-dot(w * vec2(5.2, 4.4), w * vec2(5.2, 4.4)));

  float ao = clamp(body_outer * 0.52 + body_core * 0.44 + wheel_ao * 0.35, 0.0, 1.0);
  // Smooth fade to zero strictly inside quad bounds so edges never show hard clipping.
  float edge_fade = smoothstep(1.0, 0.82, max(abs(v_uv.x), abs(v_uv.y)));
  ao *= edge_fade * strength;

  float mul = mix(1.0, min_dark, ao);
  out_color = vec4(vec3(mul), 1.0);
}
