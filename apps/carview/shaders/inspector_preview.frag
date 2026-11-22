#version 450
// carview: Ctrl+F8 Frame Inspector buffer preview converter (Color/Depth/AO/Shadow/Motion/Hi-Z).

layout(set = 0, binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform InspectorPush {
  vec4 params;  // x = mode (0=color, 1=depth, 2=ao, 3=shadow, 4=motion, 5=hiz), y = near_plane, z = reverse_z, w = far_plane
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float linearize_depth(float z) {
  float near_p = max(pc.params.y, 0.05);
  if (pc.params.z > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = max(pc.params.w, near_p + 1.0);
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

void main() {
  int mode = int(pc.params.x + 0.5);
  vec4 s = texture(src_tex, v_uv);
  if (mode == 0) {
    // Color (linear HDR or sRGB).
    vec3 rgb = clamp(s.rgb, 0.0, 1.0);
    out_color = vec4(rgb, 1.0);
    return;
  }
  if (mode == 1 || mode == 5) {
    // Depth or Hi-Z: log-compressed depth ramp (near bright -> far dark blue-black).
    float dm = linearize_depth(s.r);
    float d_norm = clamp(1.0 - log2(1.0 + dm) / 10.5, 0.0, 1.0);
    vec3 ramp = mix(vec3(0.06, 0.14, 0.30), vec3(0.95, 0.96, 1.0), d_norm);
    out_color = vec4(ramp, 1.0);
    return;
  }
  if (mode == 2) {
    // SSAO buffer (single channel contact occlusion).
    float ao = clamp(s.r, 0.0, 1.0);
    out_color = vec4(ao, ao * 0.98 + 0.02 * (1.0 - ao), ao, 1.0);
    return;
  }
  if (mode == 3) {
    // Shadow map (D16 depth).
    float sh = clamp(s.r, 0.0, 1.0);
    out_color = vec4(sh, sh, sh, 1.0);
    return;
  }
  if (mode == 4) {
    // Motion vectors (RG velocity scaled cleanly around 0.5 grey).
    vec2 v = s.rg * 40.0;
    vec3 col = clamp(vec3(0.5 + v.x, 0.5 + v.y, 0.5 - 0.5 * (v.x + v.y)), 0.0, 1.0);
    out_color = vec4(col, 1.0);
    return;
  }
  out_color = vec4(s.rgb, 1.0);
}
