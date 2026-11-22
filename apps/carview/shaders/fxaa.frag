#version 450
// carview: FXAA 3.11 edge-aware anti-aliasing pass (preserves alpha for HDR UI mask).

layout(set = 0, binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform FxaaPush {
  vec4 rcp_frame;  // x = 1/w, y = 1/h, z = subpix (0.75), w = edge_threshold (0.125)
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float fxaa_luma(vec3 rgb) {
  return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main() {
  vec2 rcp = pc.rcp_frame.xy;
  vec4 center = texture(src_tex, v_uv);
  float lumaM = fxaa_luma(center.rgb);
  float lumaN = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(0, -1)).rgb);
  float lumaS = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(0, 1)).rgb);
  float lumaW = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(-1, 0)).rgb);
  float lumaE = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(1, 0)).rgb);

  float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
  float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
  float range = rangeMax - rangeMin;
  float edgeThresh = pc.rcp_frame.w > 0.0 ? pc.rcp_frame.w : 0.125;
  if (range < max(0.0312, rangeMax * edgeThresh)) {
    out_color = center;
    return;
  }

  float lumaNW = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(-1, -1)).rgb);
  float lumaNE = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(1, -1)).rgb);
  float lumaSW = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(-1, 1)).rgb);
  float lumaSE = fxaa_luma(textureOffset(src_tex, v_uv, ivec2(1, 1)).rgb);

  float lumaNS = lumaN + lumaS;
  float lumaWE = lumaW + lumaE;
  float subpixRcpRange = 1.0 / range;
  float subpixNSWE = lumaNS + lumaWE;
  float edgeHorz1 = (-2.0 * lumaM) + lumaNS;
  float edgeVert1 = (-2.0 * lumaM) + lumaWE;

  float lumaNESE = lumaNE + lumaSE;
  float lumaNWNE = lumaNW + lumaNE;
  float edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
  float edgeVert2 = (-2.0 * lumaN) + lumaNWNE;

  float lumaNWSW = lumaNW + lumaSW;
  float lumaSWSE = lumaSW + lumaSE;
  float edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
  float edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
  float edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
  float edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
  float edgeHorz = abs(edgeHorz3) + edgeHorz4;
  float edgeVert = abs(edgeVert3) + edgeVert4;

  float subpixNWSWNESE = lumaNWSW + lumaNESE;
  float lengthSign = rcp.x;
  bool horzSpan = edgeHorz >= edgeVert;
  float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;

  if (!horzSpan) lumaN = lumaW;
  if (!horzSpan) lumaS = lumaE;
  if (horzSpan) lengthSign = rcp.y;
  float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;

  float gradientN = lumaN - lumaM;
  float gradientS = lumaS - lumaM;
  float lumaNN = lumaN + lumaM;
  float lumaSS = lumaS + lumaM;
  bool pairN = abs(gradientN) >= abs(gradientS);
  float gradient = max(abs(gradientN), abs(gradientS));
  if (pairN) lengthSign = -lengthSign;
  float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

  vec2 posB = v_uv;
  vec2 offNP = vec2(horzSpan ? rcp.x : 0.0, horzSpan ? 0.0 : rcp.y);
  if (!horzSpan) posB.x += lengthSign * 0.5;
  if (horzSpan) posB.y += lengthSign * 0.5;

  vec2 posN = posB - offNP;
  vec2 posP = posB + offNP;
  float subpixD = ((-2.0) * subpixC) + 3.0;
  float lumaEndN = fxaa_luma(texture(src_tex, posN).rgb);
  float subpixE = subpixC * subpixC;
  float lumaEndP = fxaa_luma(texture(src_tex, posP).rgb);

  if (!pairN) lumaNN = lumaSS;
  float gradientScaled = gradient * (1.0 / 4.0);
  float lumaMM = lumaM - lumaNN * 0.5;
  float subpixF = subpixD * subpixE;
  bool lumaMLTZero = lumaMM < 0.0;

  lumaEndN -= lumaNN * 0.5;
  lumaEndP -= lumaNN * 0.5;
  bool doneN = abs(lumaEndN) >= gradientScaled;
  bool doneP = abs(lumaEndP) >= gradientScaled;
  if (!doneN) posN -= offNP * 1.5;
  if (!doneP) posP += offNP * 1.5;

  const float kSteps[4] = float[](2.0, 2.0, 4.0, 8.0);
  for (int s = 0; s < 4; ++s) {
    if (doneN && doneP) break;
    if (!doneN) lumaEndN = fxaa_luma(texture(src_tex, posN).rgb) - lumaNN * 0.5;
    if (!doneP) lumaEndP = fxaa_luma(texture(src_tex, posP).rgb) - lumaNN * 0.5;
    doneN = abs(lumaEndN) >= gradientScaled;
    doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kSteps[s];
    if (!doneP) posP += offNP * kSteps[s];
  }

  float dstN = horzSpan ? (v_uv.x - posN.x) : (v_uv.y - posN.y);
  float dstP = horzSpan ? (posP.x - v_uv.x) : (posP.y - v_uv.y);
  bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
  float spanLength = dstP + dstN;
  bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
  float spanLengthRcp = 1.0 / max(spanLength, 1e-6);

  bool directionN = dstN < dstP;
  float dst = min(dstN, dstP);
  bool goodSpan = directionN ? goodSpanN : goodSpanP;
  float subpixG = subpixF * subpixF;
  float pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
  float subpixH = subpixG * (pc.rcp_frame.z > 0.0 ? pc.rcp_frame.z : 0.75);

  float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
  float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
  vec2 posM = v_uv;
  if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
  if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;

  out_color = vec4(texture(src_tex, posM).rgb, center.a);
}
