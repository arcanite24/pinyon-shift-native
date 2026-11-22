#version 450
// carview: jitter-aware temporal accumulation & spatial-temporal upscaler (TAA-U fallback / preview).

layout(set = 0, binding = 0) uniform sampler2D curr_color;
layout(set = 0, binding = 1) uniform sampler2D history_color;
layout(set = 0, binding = 2) uniform sampler2D motion_tex;

layout(push_constant) uniform TemporalPush {
  vec4 render_size;  // xy = render W,H, zw = 1/render W,H
  vec4 output_size;  // xy = output W,H, zw = 1/output W,H
  vec4 params;       // xy = jitter_uv, z = reset (1 = discard history), w = blend_max (0.9)
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 sample_catmull_rom(sampler2D tex, vec2 uv, vec2 tex_size, vec2 inv_size) {
  vec2 pos = uv * tex_size;
  vec2 tc = floor(pos - 0.5) + 0.5;
  vec2 f = pos - tc;
  vec2 f2 = f * f;
  vec2 f3 = f2 * f;
  vec2 w0 = f2 - 0.5 * (f3 + f);
  vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
  vec2 w3 = 0.5 * (f3 - f2);
  vec2 w2 = 1.0 - w0 - w1 - w3;
  vec2 s0 = w0 + w1;
  vec2 s1 = w2 + w3;
  vec2 t0 = (tc - 1.0 + w1 / max(s0, vec2(1e-5))) * inv_size;
  vec2 t1 = (tc + 1.0 + w3 / max(s1, vec2(1e-5))) * inv_size;
  return (texture(tex, vec2(t0.x, t0.y)).rgb * s0.x * s0.y +
          texture(tex, vec2(t1.x, t0.y)).rgb * s1.x * s0.y +
          texture(tex, vec2(t0.x, t1.y)).rgb * s0.x * s1.y +
          texture(tex, vec2(t1.x, t1.y)).rgb * s1.x * s1.y);
}

void main() {
  vec2 src_uv = clamp(v_uv - pc.params.xy, vec2(0.0), vec2(1.0));
  vec2 d = pc.render_size.zw;
  vec3 c0 = texture(curr_color, src_uv).rgb;
  vec3 n0 = texture(curr_color, src_uv + vec2(-d.x, 0.0)).rgb;
  vec3 n1 = texture(curr_color, src_uv + vec2( d.x, 0.0)).rgb;
  vec3 n2 = texture(curr_color, src_uv + vec2(0.0, -d.y)).rgb;
  vec3 n3 = texture(curr_color, src_uv + vec2(0.0,  d.y)).rgb;
  vec3 cmin = min(c0, min(min(n0, n1), min(n2, n3)));
  vec3 cmax = max(c0, max(max(n0, n1), max(n2, n3)));

  if (pc.params.z > 0.5) {
    out_color = vec4(c0, 0.0);
    return;
  }

  vec2 vel = texture(motion_tex, v_uv).rg;
  vec2 hist_uv = v_uv - vel;
  if (any(lessThan(hist_uv, vec2(0.0))) || any(greaterThan(hist_uv, vec2(1.0)))) {
    out_color = vec4(c0, 0.0);
    return;
  }

  vec3 hist = sample_catmull_rom(history_color, hist_uv, pc.output_size.xy, pc.output_size.zw);
  hist = clamp(hist, cmin, cmax);
  float motion_px = length(vel * pc.output_size.xy);
  float alpha = mix(pc.params.w, 0.45, clamp(motion_px * 0.04, 0.0, 1.0));
  out_color = vec4(mix(c0, hist, alpha), 0.0);
}
