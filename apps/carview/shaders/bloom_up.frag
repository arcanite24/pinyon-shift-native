#version 450
// carview: Physically-Based Jimenez / Dual-Kawase 3x3 Tent-Filter Bloom Upsample.
// Descriptor layout matches post_set_layout_ (binding 0 = finer mip `src`, binding 1 = coarser mip `aux`).
// Push constants match Post (128 bytes):
//   mode.x = weight_cur (weight of finer mip `src`)
//   mode.y = aux_texel_x (1 / coarser_w * filter_radius)
//   mode.z = aux_texel_y (1 / coarser_h * filter_radius)
//   mode.w = weight_prev (weight of upsampled coarser mip `aux`)

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D aux;

layout(push_constant) uniform Post {
  vec4 mode;     // x weight_cur, yz filter_step (1/aux_size * radius), w weight_prev
  vec4 abc;
  vec4 def;
  vec4 desat;
  vec4 bloom_p;
  vec4 balance;
  vec4 exp_keys;
  vec4 exp_ev;
} p;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 sample_aux(vec2 uv) {
  return max(texture(aux, clamp(uv, vec2(0.0005), vec2(0.9995))).rgb, vec3(0.0));
}

void main() {
  vec2 d = p.mode.yz;

  // 9-tap 3x3 tent filter (1, 2, 1 / 2, 4, 2 / 1, 2, 1) / 16:
  // yields wide, silky, artifact-free HDR halation on headlights, taillights & sun glints.
  vec3 a = sample_aux(v_uv + vec2(-d.x,  d.y));
  vec3 b = sample_aux(v_uv + vec2( 0.0,  d.y));
  vec3 c = sample_aux(v_uv + vec2( d.x,  d.y));

  vec3 dd = sample_aux(v_uv + vec2(-d.x, 0.0));
  vec3 ee = sample_aux(v_uv);
  vec3 ff = sample_aux(v_uv + vec2( d.x, 0.0));

  vec3 g = sample_aux(v_uv + vec2(-d.x, -d.y));
  vec3 h = sample_aux(v_uv + vec2( 0.0, -d.y));
  vec3 i = sample_aux(v_uv + vec2( d.x, -d.y));

  vec3 upsampled = (ee * 4.0 + (b + dd + ff + h) * 2.0 + (a + c + g + i)) * (1.0 / 16.0);
  vec3 cur = max(texture(src, clamp(v_uv, vec2(0.0005), vec2(0.9995))).rgb, vec3(0.0));
  out_color = vec4(cur * p.mode.x + upsampled * p.mode.w, 1.0);
}
