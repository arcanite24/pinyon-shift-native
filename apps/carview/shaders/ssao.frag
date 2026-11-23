#version 450
// carview: half-res Scalable Ambient Obscurance (SAO) + separable bilateral blur.
// Mode 0: SAO AO estimate from resolved depth into R8_UNORM.
// Mode 1: Horizontal bilateral blur (src = raw AO, depth_tex = depth).
// Mode 2: Vertical bilateral blur (src = H-blurred AO, depth_tex = depth).

layout(set = 0, binding = 0) uniform sampler2D src_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;

layout(push_constant) uniform SsaoPush {
  vec4 proj_params;  // x = proj00, y = proj11, z = near_plane, w = far_plane
  vec4 ao_params;    // x = radius_m, y = bias_m, z = taps (6/10/16), w = mode (0/1/2)
  vec4 texel_step;   // xy = inv_target_size, z = reverse_z (0 or 1), w = strength
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_ao;

float linear_depth(vec2 uv) {
  float z = texture(depth_tex, clamp(uv, vec2(0.001), vec2(0.999))).r;
  float near_p = max(pc.proj_params.z, 0.05);
  if (pc.texel_step.z > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = max(pc.proj_params.w, near_p + 1.0);
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

vec3 view_pos_from_depth(vec2 uv, float d) {
  vec2 ndc = uv * 2.0 - 1.0;
  float p00 = abs(pc.proj_params.x) > 1e-4 ? pc.proj_params.x : 1.0;
  float p11 = abs(pc.proj_params.y) > 1e-4 ? pc.proj_params.y : -1.0;
  return vec3(ndc.x * d / p00, ndc.y * d / p11, -d);
}

const float kBayer4x4[16] = float[](
    0.0 / 16.0,  8.0 / 16.0,  2.0 / 16.0, 10.0 / 16.0,
   12.0 / 16.0,  4.0 / 16.0, 14.0 / 16.0,  6.0 / 16.0,
    3.0 / 16.0, 11.0 / 16.0,  1.0 / 16.0,  9.0 / 16.0,
   15.0 / 16.0,  7.0 / 16.0, 13.0 / 16.0,  5.0 / 16.0
);

float compute_sao(vec2 uv) {
  float center_d = linear_depth(uv);
  if (center_d > 500.0) return 1.0;

  vec3 P = view_pos_from_depth(uv, center_d);
  vec3 dPdx = dFdx(P);
  vec3 dPdy = dFdy(P);
  vec3 N = cross(dPdy, dPdx);
  float nlen = length(N);
  if (nlen < 1e-6) return 1.0;
  N /= nlen;
  if (dot(N, normalize(-P)) < 0.0) N = -N;

  float radius = max(pc.ao_params.x, 0.2);
  float proj_scale = abs(pc.proj_params.y) * 0.5;
  float screen_r = clamp((radius * proj_scale) / max(center_d, 0.5), 0.004, 0.085);
  int taps = clamp(int(pc.ao_params.z + 0.5), 4, 24);
  float bias = max(pc.ao_params.y, 0.015) + center_d * 0.0025;

  ivec2 px = ivec2(gl_FragCoord.xy);
  float jitter = kBayer4x4[(px.y & 3) * 4 + (px.x & 3)];
  float angle0 = jitter * 6.2831853;
  float aspect = abs(pc.proj_params.y / (abs(pc.proj_params.x) > 1e-4 ? pc.proj_params.x : 1.0));

  float occ = 0.0;
  float r2 = radius * radius;
  for (int i = 0; i < taps; ++i) {
    float alpha = (float(i) + 0.5) / float(taps);
    float ang = angle0 + alpha * 6.2831853 * 3.0;
    vec2 dir = vec2(cos(ang), sin(ang) * aspect);
    vec2 suv = clamp(uv + dir * (screen_r * alpha), vec2(0.002), vec2(0.998));
    float sd = linear_depth(suv);
    if (sd > 500.0) continue;
    vec3 Q = view_pos_from_depth(suv, sd);
    vec3 V = Q - P;
    float vv = dot(V, V);
    float vn = dot(V, N);
    float f = max(r2 - vv, 0.0) / r2;
    occ += f * f * f * max((vn - bias) / max(vv + 0.04, 0.04), 0.0);
  }
  float ao = 1.0 - clamp(occ * (2.6 / float(taps)), 0.0, 0.88);
  // Fade smoothly past 180 m so distant hills stay crisp.
  float dist_fade = smoothstep(180.0, 80.0, center_d);
  return mix(1.0, ao, dist_fade);
}

float bilateral_blur(vec2 uv, vec2 dir) {
  float center_ao = texture(src_tex, uv).r;
  float center_d = linear_depth(uv);
  if (center_d > 500.0) return 1.0;

  const float w_off[5] = float[](0.06136, 0.24477, 0.38774, 0.24477, 0.06136);
  float sum = center_ao * w_off[2];
  float wsum = w_off[2];
  float depth_scale = 1.0 / max(center_d * 0.12, 0.15);
  for (int i = -2; i <= 2; ++i) {
    if (i == 0) continue;
    vec2 suv = clamp(uv + dir * float(i), vec2(0.001), vec2(0.999));
    float sd = linear_depth(suv);
    float sao = texture(src_tex, suv).r;
    float dz = abs(sd - center_d) * depth_scale;
    float w = w_off[i + 2] * exp(-dz * dz);
    sum += sao * w;
    wsum += w;
  }
  return sum / max(wsum, 1e-4);
}

void main() {
  int mode = int(pc.ao_params.w + 0.5);
  float v = 1.0;
  if (mode == 0) {
    v = compute_sao(v_uv);
  } else if (mode == 1) {
    v = bilateral_blur(v_uv, vec2(pc.texel_step.x, 0.0));
  } else {
    v = bilateral_blur(v_uv, vec2(0.0, pc.texel_step.y));
  }
  out_ao = vec4(v, v, v, 1.0);
}
