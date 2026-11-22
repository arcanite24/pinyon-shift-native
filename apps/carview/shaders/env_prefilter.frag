#version 450
// Cross-face GGX importance-sampled cubemap prefilter pass for dynamic reflections.
// Samples mip 0 of the dynamic cubemap across all 6 faces seamlessly (zero cube seams)
// and writes roughness-convolved mip levels 1..kDynEnvMips-1 for metallic basecoat & diffuse IBL.

layout(set = 0, binding = 0) uniform samplerCube src_cube;

layout(push_constant) uniform Prefilter {
  vec4 params;  // x face [0..5], y roughness [0..1], z mip_level, w inv_size
} p;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265359;

vec3 cube_dir(int face, vec2 uv) {
  vec2 st = uv * 2.0 - 1.0;
  float u = st.x, v = st.y;
  if (face == 0) return normalize(vec3( 1.0, -v, -u));
  if (face == 1) return normalize(vec3(-1.0, -v,  u));
  if (face == 2) return normalize(vec3(   u,  1.0,  v));
  if (face == 3) return normalize(vec3(   u, -1.0, -v));
  if (face == 4) return normalize(vec3(   u, -v,  1.0));
  return normalize(vec3(-u, -v, -1.0));
}

float radical_inverse_vdc(uint bits) {
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint n) {
  return vec2(float(i) / float(n), radical_inverse_vdc(i));
}

vec3 importance_sample_ggx(vec2 xi, vec3 n, float roughness) {
  float a = roughness * roughness;
  float phi = 2.0 * kPi * xi.x;
  float cos_theta = sqrt((1.0 - xi.y) / max(1.0 + (a * a - 1.0) * xi.y, 1e-5));
  float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
  vec3 h = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
  vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
  vec3 t = normalize(cross(up, n));
  vec3 b = cross(n, t);
  return normalize(t * h.x + b * h.y + n * h.z);
}

void main() {
  int face = int(p.params.x + 0.5);
  float roughness = clamp(p.params.y, 0.04, 1.0);
  vec3 N = cube_dir(face, v_uv);
  vec3 R = N;
  vec3 V = R;

  const uint kSamples = 16u;
  vec3 acc = vec3(0.0);
  float wsum = 0.0;
  for (uint i = 0u; i < kSamples; ++i) {
    vec2 xi = hammersley(i, kSamples);
    vec3 H = importance_sample_ggx(xi, N, roughness);
    vec3 L = normalize(2.0 * dot(V, H) * H - V);
    float ndl = max(dot(N, L), 0.0);
    if (ndl > 0.0) {
      vec3 c = min(max(textureLod(src_cube, L, 0.0).rgb, vec3(0.0)), vec3(64.0));
      acc += c * ndl;
      wsum += ndl;
    }
  }
  vec3 col = wsum > 1e-4 ? acc / wsum : textureLod(src_cube, N, 0.0).rgb;
  out_color = vec4(col, 1.0);
}
