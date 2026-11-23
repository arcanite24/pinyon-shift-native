#version 450
// carview: McGuire et al. (2012) Tile-Max (mode 0) & Neighbor-Max (mode 1) velocity reduction.
// Runs at 1/16th resolution (16x16 pixel tiles) in RG16F for < 0.04 ms total overhead.

layout(set = 0, binding = 0) uniform sampler2D src_vel;

layout(push_constant) uniform MblurTilePush {
  vec4 params;  // xy = 1/src_size, z = mode (0 = TileMax 16x16, 1 = NeighborMax 3x3), w = car_protect_m
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec2 out_vel;

void main() {
  int mode = int(pc.params.z + 0.5);
  vec2 inv_src = pc.params.xy;

  if (mode == 0) {
    // Tile-Max: 4x4 stratified sample grid across the 16x16 pixel tile.
    // Ignores near player-car pixels (< car_protect_m) so the chase-cam car never dilates a false halo.
    vec2 tile_origin = v_uv - inv_src * 6.0;
    vec2 best_v = vec2(0.0);
    float best_len2 = 0.0;
    float protect_m = max(pc.params.w, 0.0);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        vec2 suv = clamp(tile_origin + vec2(float(x), float(y)) * (inv_src * 4.0), vec2(0.001), vec2(0.999));
        vec4 mv = texture(src_vel, suv);
        // Suppress foreground player-car velocity in chase camera to eliminate silhouette smear.
        float car_w = protect_m > 0.1 ? smoothstep(protect_m * 0.72, protect_m * 1.35, mv.a) : 1.0;
        vec2 v = mv.rg * car_w;
        float l2 = dot(v, v);
        if (l2 > best_len2) {
          best_len2 = l2;
          best_v = v;
        }
      }
    }
    out_vel = best_v;
    return;
  }

  // Neighbor-Max: 3x3 tile dilation so moving edges smear smoothly across tile boundaries.
  vec2 best_v = vec2(0.0);
  float best_len2 = 0.0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      vec2 suv = clamp(v_uv + vec2(float(dx), float(dy)) * inv_src, vec2(0.001), vec2(0.999));
      vec2 v = texture(src_vel, suv).rg;
      float l2 = dot(v, v);
      if (l2 > best_len2) {
        best_len2 = l2;
        best_v = v;
      }
    }
  }
  out_vel = best_v;
}
