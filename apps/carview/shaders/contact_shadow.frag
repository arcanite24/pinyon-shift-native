#version 450
// Phase B Universal Under-Car Soft Contact AO Grounding (< 0.05 ms on all 4 tiers):
// Analytical rounded-box chassis AO + 4 per-car wheel tire contact patches modulated by
// dynamic suspension clearance so player, AI, and traffic cars sit firmly planted in sun,
// shade, overcast, and night.
// Pipeline uses Multiply blend (srcColor=ZERO, dstColor=SRC_COLOR, alpha preserved).

layout(location = 0) in vec2 v_uv;          // [-1..1] across car footprint
layout(location = 1) flat in vec4 v_params; // x strength, y min_dark, z corner_r, w clearance [0..1]
layout(location = 2) flat in vec4 v_wheels; // x front_u, y front_v, z rear_u, w rear_v

layout(location = 0) out vec4 out_color;

float rounded_box_sdf(vec2 p, vec2 b, float r) {
  vec2 d = abs(p) - b + vec2(r);
  return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - r;
}

void main() {
  float strength = clamp(v_params.x, 0.0, 1.0);
  float min_dark = clamp(v_params.y, 0.05, 0.95);
  float corner_r = max(v_params.z, 0.15);
  float clearance = clamp(v_params.w, 0.0, 1.0);

  // Suspension clearance softens penumbra spread and gently lifts under-chassis core darkness.
  float spread = mix(0.20, 0.34, clearance);
  float core_w = mix(0.50, 0.34, clearance);
  float outer_w = mix(0.54, 0.44, clearance);

  // Soft outer body AO + tight dark core directly under the chassis sill.
  float sd_outer = rounded_box_sdf(v_uv, vec2(0.75, 0.80), corner_r);
  float body_outer = 1.0 - smoothstep(-0.08, spread, sd_outer);
  float sd_core = rounded_box_sdf(v_uv, vec2(0.54, 0.64), corner_r * 0.82);
  float body_core = 1.0 - smoothstep(-0.18, spread * 0.72, sd_core);

  // Four per-car wheel contact patch darkenings aligned to actual LF/RF & LR/RR track/wheelbase.
  vec2 wf = vec2(abs(v_uv.x) - v_wheels.x, v_uv.y - v_wheels.y);
  vec2 wr = vec2(abs(v_uv.x) - v_wheels.z, v_uv.y - v_wheels.w);
  float tire_sharp = mix(5.8, 3.8, clearance);
  float front_tires = exp(-dot(wf * vec2(tire_sharp, tire_sharp * 0.82), wf * vec2(tire_sharp, tire_sharp * 0.82)));
  float rear_tires = exp(-dot(wr * vec2(tire_sharp, tire_sharp * 0.82), wr * vec2(tire_sharp, tire_sharp * 0.82)));
  float wheel_ao = max(front_tires, rear_tires);

  float ao = clamp(body_outer * outer_w + body_core * core_w + wheel_ao * 0.42, 0.0, 1.0);
  // Smooth fade to zero strictly inside quad bounds so edges never show hard clipping.
  float edge_fade = 1.0 - smoothstep(0.82, 1.0, max(abs(v_uv.x), abs(v_uv.y)));
  ao *= edge_fade * strength * mix(1.0, 0.76, clearance);

  float mul = mix(1.0, min_dark, ao);
  out_color = vec4(vec3(mul), 1.0);
}
