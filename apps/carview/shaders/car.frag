#version 450
// carview: atlas texel, paint where the atlas alpha is 0 on the body,
// one directional light, the vertex's baked occlusion and a highlight.

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(push_constant) uniform Constants {
  mat4 view_projection;
  vec4 light_direction;
  vec4 paint;
  vec4 eye;
} pc;

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec2 v_extra;

layout(location = 0) out vec4 out_color;

const float kKindTextured = 0.0;
const float kKindPaint = 1.0;
const float kKindGlass = 2.0;

void main() {
  vec4 texel = texture(atlas, v_uv);
  vec3 view = normalize(pc.eye.xyz - v_position);
  vec3 normal = normalize(v_normal);
  // The viewer sees either side of a part.
  if (dot(normal, view) < 0.0) normal = -normal;
  vec3 light = normalize(pc.light_direction.xyz);
  float diffuse = max(dot(normal, light), 0.0);
  float occlusion = clamp(v_extra.x, 0.25, 1.0);
  vec3 half_vector = normalize(view + light);
  float highlight = pow(max(dot(normal, half_vector), 0.0), 60.0);

  float kind = v_extra.y;
  vec3 base = texel.rgb;
  float shine = 0.15;
  if (abs(kind - kKindPaint) < 0.5) {
    base = mix(pc.paint.rgb, texel.rgb, texel.a);
    shine = 0.6;
  } else if (abs(kind - kKindGlass) < 0.5) {
    base = texel.rgb * 0.35;
    shine = 0.9;
  }
  vec3 color = base * (0.30 * occlusion + 0.80 * diffuse * mix(0.6, 1.0, occlusion)) +
               shine * highlight;
  out_color = vec4(color, 1.0);
}
