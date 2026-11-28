#include "render_modern.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace carview::modern {

// ---- Render resolution & Retina HiDPI scaling ----------------------------------
int clamp_scale(int scale) { return std::clamp(scale, kMinScale, kMaxScale); }

uint32_t parse_resolution_height(std::string_view size_str) {
  if (size_str.empty()) return 0u;
  const size_t x = size_str.find_first_of("xX");
  if (x == std::string_view::npos || x == 0 || x + 1 >= size_str.size()) return 0u;
  unsigned w = 0, h = 0;
  std::string tmp(size_str);
  if (std::sscanf(tmp.c_str(), "%u%*1[xX]%u", &w, &h) != 2) return 0u;
  if (w < 320u || h < 240u || w > 16384u || h > 8192u) return 0u;
  return h;
}

Extent logical_window_size_for_resolution(uint32_t target_w, uint32_t target_h, float pixel_density) {
  if (target_w == 0 || target_h == 0) return {0, 0};
  const float pd = (std::isfinite(pixel_density) && pixel_density > 1.0f) ? std::clamp(pixel_density, 1.0f, 4.0f) : 1.0f;
  const uint32_t lw = std::max<uint32_t>(320u, uint32_t(std::lround(double(target_w) / double(pd))));
  const uint32_t lh = std::max<uint32_t>(240u, uint32_t(std::lround(double(target_h) / double(pd))));
  return {lw, lh};
}

Extent scene_extent(Extent window, int scale, bool base_540p, float pixel_density, uint32_t target_height) {
  scale = clamp_scale(scale);
  if (window.w == 0 || window.h == 0) return {0, 0};
  const double aspect = double(window.w) / double(window.h);
  if (target_height > 0u) {
    uint32_t h = std::clamp(target_height, 240u, 4320u);
    uint32_t w = uint32_t(std::lround(double(h) * aspect));
    w = (w + 1u) & ~1u;
    w = std::clamp(w, 64u, 16384u);
    return {w, h};
  }
  if (scale == 0) {
    const float pd =
        (std::isfinite(pixel_density) && pixel_density > 1.0f) ? std::clamp(pixel_density, 1.0f, 4.0f) : 1.0f;
    if (pd <= 1.001f) return window;
    uint32_t lw = uint32_t(std::lround(double(window.w) / double(pd)));
    uint32_t lh = uint32_t(std::lround(double(window.h) / double(pd)));
    lw = std::clamp((lw + 1u) & ~1u, 64u, 16384u);
    lh = std::clamp((lh + 1u) & ~1u, 64u, 8192u);
    return {lw, lh};
  }
  const uint32_t base_h = base_540p ? 540u : 720u;
  uint32_t h = std::min(base_h * uint32_t(scale), 8192u);
  uint32_t w = uint32_t(std::lround(double(h) * aspect));
  w = (w + 1u) & ~1u;  // even, so 2x2 resolves and chroma-style passes line up
  w = std::clamp(w, 64u, 16384u);
  h = std::max(h, 64u);
  return {w, h};
}

std::string scale_label(int scale, bool base_540p, Extent window) {
  scale = clamp_scale(scale);
  if (scale == 0) return "NATIVE";
  const Extent e = scene_extent(window.w ? window : Extent{16, 9}, scale, base_540p);
  const Extent ref = base_540p ? Extent{960, 540} : Extent{1280, 720};
  std::string s = (base_540p ? "540P " : "") + std::to_string(scale) + "X";
  const Extent shown = window.w ? e : Extent{ref.w * uint32_t(scale), ref.h * uint32_t(scale)};
  s += " (" + std::to_string(shown.w) + "x" + std::to_string(shown.h) + ")";
  return s;
}

// ---- Ultrawide ----------------------------------------------------------------------
float hfov_from_vfov(float vfov_deg, float aspect) {
  const float v = vfov_deg * 3.14159265f / 180.0f;
  return 2.0f * std::atan(std::tan(v * 0.5f) * aspect) * 180.0f / 3.14159265f;
}

float effective_vfov(float vfov_deg, float aspect, float max_hfov_deg) {
  if (aspect <= 0.0f) return vfov_deg;
  if (hfov_from_vfov(vfov_deg, aspect) <= max_hfov_deg) return vfov_deg;
  const float h = max_hfov_deg * 3.14159265f / 180.0f;
  return 2.0f * std::atan(std::tan(h * 0.5f) / aspect) * 180.0f / 3.14159265f;
}

// ---- HUD ------------------------------------------------------------------------------
HudLayout hud_layout(float window_w, float window_h, bool lock_16_9, float hud_scale_percent, float safe_zone_percent) {
  HudLayout l;
  if (window_w <= 0 || window_h <= 0) return l;
  const float hs = std::clamp(hud_scale_percent, kMinHudScale, kMaxHudScale) / 100.0f;
  const float safe = std::clamp(safe_zone_percent, 0.0f, kMaxSafeZone) / 100.0f;
  float vw = window_w, vh = window_h;
  if (lock_16_9 && window_w / window_h > kHudAspect) vw = window_h * kHudAspect;
  l.virtual_w = vw;
  l.virtual_h = vh;
  l.scale = hs * (1.0f - 2.0f * safe);
  l.origin_x = window_w * 0.5f - vw * 0.5f * l.scale;
  l.origin_y = window_h * 0.5f - vh * 0.5f * l.scale;
  return l;
}

// ---- AA, AO, contact shadows, tonemapper, velocity blur, textures -------------------------
int ssao_taps(SsaoQuality q) {
  switch (q) {
    case SsaoQuality::kLow: return 6;
    case SsaoQuality::kMedium: return 10;
    case SsaoQuality::kHigh: return 16;
    case SsaoQuality::kUltra: return 24;
    default: return 0;
  }
}

float ssao_radius_m(SsaoQuality q) {
  switch (q) {
    case SsaoQuality::kLow: return 0.65f;
    case SsaoQuality::kMedium: return 0.95f;
    case SsaoQuality::kHigh: return 1.25f;
    case SsaoQuality::kUltra: return 1.50f;
    default: return 0.0f;
  }
}

int gtao_slices(SsaoQuality q) {
  switch (q) {
    case SsaoQuality::kLow: return 2;
    case SsaoQuality::kMedium: return 2;
    case SsaoQuality::kHigh: return 3;
    case SsaoQuality::kUltra: return 3;
    default: return 0;
  }
}

int gtao_steps(SsaoQuality q) {
  switch (q) {
    case SsaoQuality::kLow: return 2;
    case SsaoQuality::kMedium: return 3;
    case SsaoQuality::kHigh: return 3;
    case SsaoQuality::kUltra: return 4;
    default: return 0;
  }
}

bool gtao_full_res(SsaoQuality q) { return q == SsaoQuality::kUltra; }

int contact_shadow_steps(ContactShadows q) {
  switch (q) {
    case ContactShadows::kLow: return 8;
    case ContactShadows::kHigh: return 10;
    case ContactShadows::kUltra: return 12;
    default: return 0;
  }
}

bool contact_shadow_full_res(ContactShadows q) { return q == ContactShadows::kUltra; }

const char* contact_shadows_name(ContactShadows q) {
  switch (q) {
    case ContactShadows::kLow: return "low";
    case ContactShadows::kHigh: return "high";
    case ContactShadows::kUltra: return "ultra";
    default: return "off";
  }
}

ContactShadows parse_contact_shadows(std::string_view name) {
  if (name == "low") return ContactShadows::kLow;
  if (name == "high" || name == "on") return ContactShadows::kHigh;
  if (name == "ultra") return ContactShadows::kUltra;
  return ContactShadows::kOff;
}

const char* tonemapper_name(Tonemapper t) {
  switch (t) {
    case Tonemapper::kAgX: return "agx";
    case Tonemapper::kGtFilmic: return "gt";
    case Tonemapper::kPbrNeutral: return "pbr-neutral";
    default: return "hable";
  }
}

Tonemapper parse_tonemapper(std::string_view name) {
  if (name == "agx") return Tonemapper::kAgX;
  if (name == "gt" || name == "gt-filmic" || name == "uchimura") return Tonemapper::kGtFilmic;
  if (name == "pbr-neutral" || name == "pbr" || name == "neutral") return Tonemapper::kPbrNeutral;
  return Tonemapper::kHable;
}

std::array<float, 3> tonemap_agx(const std::array<float, 3>& rgb) {
  const float r0 = std::max(rgb[0], 0.0f), g0 = std::max(rgb[1], 0.0f), b0 = std::max(rgb[2], 0.0f);
  float v[3] = {
      0.842479062253094f * r0 + 0.0784335999999992f * g0 + 0.0792237451477643f * b0,
      0.0423282422610123f * r0 + 0.878468636469772f * g0 + 0.0791661274605434f * b0,
      0.0423756549057051f * r0 + 0.0784336f * g0 + 0.879142973793104f * b0,
  };
  constexpr float kMinEv = -12.47393f;
  constexpr float kMaxEv = 4.026069f;
  for (int i = 0; i < 3; ++i) {
    float x = std::clamp(std::log2(std::max(v[i], 1e-10f)), kMinEv, kMaxEv);
    x = (x - kMinEv) / (kMaxEv - kMinEv);
    const float x2 = x * x;
    const float x4 = x2 * x2;
    v[i] = +15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x -
           0.00232f;
  }
  const float out[3] = {
      1.19687900512017f * v[0] - 0.0980208811401368f * v[1] - 0.0990297440797205f * v[2],
      -0.0528968517574562f * v[0] + 1.15190312990417f * v[1] - 0.0989611768448433f * v[2],
      -0.0529716355144438f * v[0] - 0.0980434501171241f * v[1] + 1.15107367264116f * v[2],
  };
  return {std::pow(std::clamp(out[0], 0.0f, 1.0f), 2.2f), std::pow(std::clamp(out[1], 0.0f, 1.0f), 2.2f),
          std::pow(std::clamp(out[2], 0.0f, 1.0f), 2.2f)};
}

std::array<float, 3> tonemap_gt_filmic(const std::array<float, 3>& rgb) {
  constexpr float P = 1.0f, a = 1.0f, m = 0.22f, l = 0.4f, c = 1.33f, b = 0.0f;
  constexpr float l0 = ((P - m) * l) / a;
  constexpr float S0 = m + l0;
  constexpr float S1 = m + a * l0;
  constexpr float C2 = (a * P) / (P - S1);
  constexpr float CP = -C2 / P;
  auto smoothstep = [](float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  auto curve = [&](float x) {
    x = std::max(x, 0.0f);
    const float w0 = 1.0f - smoothstep(0.0f, m, x);
    const float w2 = (x >= m + l0) ? 1.0f : 0.0f;
    const float w1 = 1.0f - w0 - w2;
    const float T = m * std::pow(x / m, c) + b;
    const float S = P - (P - S1) * std::exp(CP * (x - S0));
    const float L = m + a * (x - m);
    return std::clamp(T * w0 + L * w1 + S * w2, 0.0f, 1.0f);
  };
  return {curve(rgb[0]), curve(rgb[1]), curve(rgb[2])};
}

std::array<float, 3> tonemap_pbr_neutral(const std::array<float, 3>& rgb) {
  constexpr float startCompression = 0.8f - 0.04f;
  constexpr float desaturation = 0.15f;
  float r = std::max(rgb[0], 0.0f), g = std::max(rgb[1], 0.0f), b = std::max(rgb[2], 0.0f);
  const float x = std::min(r, std::min(g, b));
  const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
  r -= offset;
  g -= offset;
  b -= offset;
  const float peak = std::max(r, std::max(g, b));
  if (peak < startCompression) {
    return {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
  }
  const float d = 1.0f - startCompression;
  const float newPeak = 1.0f - d * d / (peak + d - startCompression);
  const float scale = newPeak / std::max(peak, 1e-6f);
  r *= scale;
  g *= scale;
  b *= scale;
  const float mix_g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
  return {std::clamp(r + (newPeak - r) * mix_g, 0.0f, 1.0f), std::clamp(g + (newPeak - g) * mix_g, 0.0f, 1.0f),
          std::clamp(b + (newPeak - b) * mix_g, 0.0f, 1.0f)};
}

int velocity_blur_taps(VelocityBlur q) {
  switch (q) {
    case VelocityBlur::kLow: return 9;
    case VelocityBlur::kHigh: return 13;
    default: return 0;
  }
}

const char* velocity_blur_name(VelocityBlur q) {
  switch (q) {
    case VelocityBlur::kLow: return "low";
    case VelocityBlur::kHigh: return "high";
    default: return "off";
  }
}

VelocityBlur parse_velocity_blur(std::string_view name) {
  if (name == "low" || name == "on" || name == "medium") return VelocityBlur::kLow;
  if (name == "high" || name == "ultra") return VelocityBlur::kHigh;
  return VelocityBlur::kOff;
}

int shadow_cascade_count(ShadowPcss q) {
  return (q == ShadowPcss::kHigh || q == ShadowPcss::kUltra) ? 3 : 2;
}

bool shadow_pcss_enabled(ShadowPcss q) {
  return q == ShadowPcss::kHigh || q == ShadowPcss::kUltra;
}

int shadow_pcss_taps(ShadowPcss q) {
  switch (q) {
    case ShadowPcss::kHigh: return 12;
    case ShadowPcss::kUltra: return 16;
    default: return 4;
  }
}

const char* shadow_pcss_name(ShadowPcss q) {
  switch (q) {
    case ShadowPcss::kLow: return "low";
    case ShadowPcss::kHigh: return "high";
    case ShadowPcss::kUltra: return "ultra";
    default: return "off";
  }
}

ShadowPcss parse_shadow_pcss(std::string_view name) {
  if (name == "low") return ShadowPcss::kLow;
  if (name == "high" || name == "on") return ShadowPcss::kHigh;
  if (name == "ultra") return ShadowPcss::kUltra;
  return ShadowPcss::kOff;
}

int ssr_steps(SsrQuality q) {
  switch (q) {
    case SsrQuality::kHalf: return 24;
    case SsrQuality::kFull: return 36;
    default: return 0;
  }
}

bool ssr_full_res(SsrQuality q) { return q == SsrQuality::kFull; }

const char* ssr_name(SsrQuality q) {
  switch (q) {
    case SsrQuality::kHalf: return "half";
    case SsrQuality::kFull: return "full";
    default: return "off";
  }
}

SsrQuality parse_ssr(std::string_view name) {
  if (name == "half" || name == "high" || name == "on") return SsrQuality::kHalf;
  if (name == "full" || name == "ultra") return SsrQuality::kFull;
  return SsrQuality::kOff;
}

int volumetric_steps(VolumetricFog q) {
  switch (q) {
    case VolumetricFog::kQuarter: return 16;
    case VolumetricFog::kHalf: return 24;
    default: return 0;
  }
}

uint32_t volumetric_scale_div(VolumetricFog q) {
  switch (q) {
    case VolumetricFog::kQuarter: return 4u;
    case VolumetricFog::kHalf: return 2u;
    default: return 0u;
  }
}

const char* volumetric_fog_name(VolumetricFog q) {
  switch (q) {
    case VolumetricFog::kQuarter: return "quarter";
    case VolumetricFog::kHalf: return "half";
    default: return "off";
  }
}

VolumetricFog parse_volumetric_fog(std::string_view name) {
  if (name == "quarter" || name == "high" || name == "on") return VolumetricFog::kQuarter;
  if (name == "half" || name == "ultra") return VolumetricFog::kHalf;
  return VolumetricFog::kOff;
}

std::array<float, 3> blackbody_rotor_rgb(float heat_0_1) {
  const float h = std::clamp(heat_0_1, 0.0f, 1.0f);
  if (h <= 0.02f) return {0.0f, 0.0f, 0.0f};
  // Planckian thermal ramp 900 K (deep cherry red) -> 1450 K (bright incandescent orange-yellow).
  const float t = (h - 0.02f) / 0.98f;
  const float radiance = t * t * 11.5f;
  const float r = std::clamp(0.85f + 0.15f * t, 0.0f, 1.0f);
  const float g = std::clamp(0.06f + 0.54f * t * t, 0.0f, 1.0f);
  const float b = std::clamp(0.01f + 0.22f * t * t * t, 0.0f, 1.0f);
  return {r * radiance, g * radiance, b * radiance};
}

uint32_t texture_levels_to_skip(uint32_t width, uint32_t height, uint32_t level_count, TextureCap cap) {
  if (cap == TextureCap::kFull || level_count <= 1) return 0;
  uint32_t skip = 0;
  uint32_t side = std::max(width, height);
  while (side > uint32_t(cap) && skip + 1 < level_count) {
    side = std::max(side / 2, 1u);
    ++skip;
  }
  return skip;
}

// ---- Presentation --------------------------------------------------------------------------
VSync resolve_vsync(VSync requested, uint32_t available_mask) {
  auto has = [&](VSync v) { return (available_mask & vsync_bit(v)) != 0; };
  if (requested == VSync::kImmediate && has(VSync::kImmediate)) return VSync::kImmediate;
  if (requested == VSync::kMailbox && has(VSync::kMailbox)) return VSync::kMailbox;
  if (requested == VSync::kImmediate && has(VSync::kMailbox)) return VSync::kMailbox;
  return VSync::kFifo;
}

double fps_cap_interval(int cap, double display_hz) {
  if (cap > 0) return 1.0 / double(cap);
  return display_hz > 1.0 ? 1.0 / display_hz : 1.0 / 60.0;
}

double FramePacer::wait_for(double now) {
  if (interval_ <= 0) {
    next_ = -1;
    return 0;
  }
  if (next_ < 0) next_ = now;
  const double wait = next_ - now;
  if (wait > 0) {
    next_ += interval_;
    return wait;
  }
  // Late: go at once; skip the missed slots so the frames after do not burst.
  next_ += interval_;
  if (next_ < now) next_ = now + interval_;
  return 0;
}

// ---- HDR ------------------------------------------------------------------------------------
HdrMode resolve_hdr(bool wanted, bool has_hdr10, bool has_scrgb) {
  if (!wanted) return HdrMode::kOff;
  if (has_scrgb) return HdrMode::kScRgb;
  if (has_hdr10) return HdrMode::kHdr10;
  return HdrMode::kOff;
}

float clamp_paper_white(float nits) { return std::clamp(nits, 80.0f, 400.0f); }

namespace {
constexpr float kM1 = 2610.0f / 16384.0f, kM2 = 2523.0f / 4096.0f * 128.0f, kC1 = 3424.0f / 4096.0f,
                kC2 = 2413.0f / 4096.0f * 32.0f, kC3 = 2392.0f / 4096.0f * 32.0f;
}  // namespace

float pq_encode(float nits) {
  const float y = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
  const float yp = std::pow(y, kM1);
  return std::pow((kC1 + kC2 * yp) / (1.0f + kC3 * yp), kM2);
}

float pq_decode(float code) {
  const float c = std::clamp(code, 0.0f, 1.0f);
  const float p = std::pow(c, 1.0f / kM2);
  const float num = std::max(p - kC1, 0.0f);
  const float den = kC2 - kC3 * p;
  return 10000.0f * std::pow(num / den, 1.0f / kM1);
}

std::array<float, 3> rec709_to_rec2020(const std::array<float, 3>& c) {
  return {0.6274040f * c[0] + 0.3292820f * c[1] + 0.0433136f * c[2],
          0.0690970f * c[0] + 0.9195400f * c[1] + 0.0113612f * c[2],
          0.0163916f * c[0] + 0.0880132f * c[1] + 0.8955950f * c[2]};
}

float expand_highlight(float t, float highlights_percent, float paper_white_nits, float peak_nits) {
  const float h = std::clamp(highlights_percent, 0.0f, 100.0f) / 100.0f;
  const float ratio = std::max(peak_nits, paper_white_nits) / std::max(paper_white_nits, 1.0f);
  const float max_out = 1.0f + (ratio - 1.0f) * h;
  const float u = std::max(t - 0.5f, 0.0f) / 0.5f;
  return t + (max_out - 1.0f) * u * u;
}

// ---- Upscaling --------------------------------------------------------------------------------
const char* upscaler_name(Upscaler u) {
  switch (u) {
    case Upscaler::kBilinear: return "BILINEAR";
    case Upscaler::kCas: return "CAS";
    case Upscaler::kFsr1: return "FSR 1";
    case Upscaler::kFsr2: return "FSR 2";
    case Upscaler::kFsr3: return "FSR 3";
    case Upscaler::kDlss: return "DLSS";
  }
  return "?";
}

const char* frame_gen_name(FrameGen f) {
  switch (f) {
    case FrameGen::kOff: return "OFF";
    case FrameGen::kFsr3: return "FSR 3";
    case FrameGen::kDlss: return "DLSS";
  }
  return "?";
}

UpscalerChoice resolve_upscaler(Upscaler requested, const UpscalerSupport& s, Extent render, Extent output) {
  UpscalerChoice c;
  c.requested = c.active = requested;
  c.native_aa = render.w == output.w && render.h == output.h;
  c.label = upscaler_name(requested);
  switch (requested) {
    case Upscaler::kBilinear:
    case Upscaler::kCas:
    case Upscaler::kFsr1: break;
    case Upscaler::kFsr2:
    case Upscaler::kFsr3:
      if (s.temporal) {
        c.temporal = true;
        if (c.native_aa) c.label += " (NATIVE AA)";
      } else {
        c.unavailable = true;
        c.active = Upscaler::kFsr1;
        c.label += " (UNAVAILABLE)";
      }
      break;
    case Upscaler::kDlss:
      if (s.rtx && s.dlss_runtime && s.temporal) {
        c.temporal = true;
        c.label = c.native_aa ? "DLAA" : "DLSS";
      } else {
        c.unavailable = true;
        c.active = Upscaler::kFsr1;
        c.label += " (UNAVAILABLE)";
      }
      break;
  }
  return c;
}

FrameGen resolve_frame_gen(FrameGen requested, const UpscalerSupport& s, bool hdr_ok) {
  if (!hdr_ok) return FrameGen::kOff;
  if (!s.frame_gen || !s.temporal) return FrameGen::kOff;
  if (requested == FrameGen::kDlss && !(s.rtx && s.dlss_runtime)) return FrameGen::kOff;
  return requested;
}

static float halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (index > 0) {
    f /= float(base);
    r += f * float(index % base);
    index /= base;
  }
  return r;
}

std::array<float, 2> halton_jitter(uint32_t index, uint32_t phase_count) {
  const uint32_t n = std::max(phase_count, 1u);
  const uint32_t i = index % n + 1;
  return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

uint32_t jitter_phase_count(Extent render, Extent output) {
  if (render.w == 0 || output.w == 0) return 8;
  const double ratio = double(output.w) / double(render.w);
  return std::max(8u, uint32_t(std::ceil(8.0 * ratio * ratio)));
}

// ---- .cube -------------------------------------------------------------------------------------
namespace {
bool parse_floats(const char* p, float* out, int count) {
  char* end = nullptr;
  for (int i = 0; i < count; ++i) {
    out[i] = std::strtof(p, &end);
    if (end == p) return false;
    p = end;
  }
  while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
  return *p == 0;  // nothing after the numbers
}
}  // namespace

bool parse_cube(std::string_view text, CubeLut& out, std::string& error) {
  out = CubeLut{};
  uint32_t declared = 0;
  size_t line_no = 0;
  size_t pos = 0;
  bool saw_data = false;
  while (pos <= text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    std::string line(text.substr(pos, eol - pos));
    pos = eol + 1;
    ++line_no;
    // Trim.
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t b = 0;
    while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
    line.erase(0, b);
    if (line.empty() || line[0] == '#') continue;
    auto fail = [&](const std::string& why) {
      error = "line " + std::to_string(line_no) + ": " + why;
      return false;
    };
    if (std::isalpha(static_cast<unsigned char>(line[0]))) {
      if (saw_data) return fail("keyword after table data");
      auto starts = [&](const char* k) { return line.rfind(k, 0) == 0; };
      if (starts("TITLE")) {
        const size_t q1 = line.find('"'), q2 = line.rfind('"');
        out.title = q1 != std::string::npos && q2 > q1 ? line.substr(q1 + 1, q2 - q1 - 1) : line.substr(5);
      } else if (starts("LUT_3D_SIZE")) {
        const long n = std::strtol(line.c_str() + 11, nullptr, 10);
        if (n < 2 || n > 65) return fail("LUT_3D_SIZE must be 2..65");
        declared = uint32_t(n);
      } else if (starts("LUT_1D_SIZE")) {
        return fail("1D LUTs are not supported (use a 3D .cube)");
      } else if (starts("DOMAIN_MIN")) {
        float v[3];
        if (!parse_floats(line.c_str() + 10, v, 3)) return fail("bad DOMAIN_MIN");
        out.domain_min = {v[0], v[1], v[2]};
      } else if (starts("DOMAIN_MAX")) {
        float v[3];
        if (!parse_floats(line.c_str() + 10, v, 3)) return fail("bad DOMAIN_MAX");
        out.domain_max = {v[0], v[1], v[2]};
      } else if (starts("LUT_1D_INPUT_RANGE") || starts("LUT_3D_INPUT_RANGE")) {
        // ignored: the domain lines carry the range
      } else {
        return fail("unknown keyword");
      }
      continue;
    }
    if (declared == 0) return fail("table data before LUT_3D_SIZE");
    float v[3];
    if (!parse_floats(line.c_str(), v, 3)) return fail("expected three numbers");
    saw_data = true;
    out.rgb.push_back(v[0]);
    out.rgb.push_back(v[1]);
    out.rgb.push_back(v[2]);
  }
  if (declared == 0) {
    error = "no LUT_3D_SIZE";
    return false;
  }
  out.size = declared;
  const size_t want = size_t(declared) * declared * declared * 3;
  if (out.rgb.size() != want) {
    error = "expected " + std::to_string(want / 3) + " entries, found " + std::to_string(out.rgb.size() / 3);
    out = CubeLut{};
    return false;
  }
  for (int k = 0; k < 3; ++k)
    if (!(out.domain_max[size_t(k)] > out.domain_min[size_t(k)])) {
      error = "DOMAIN_MAX must exceed DOMAIN_MIN";
      out = CubeLut{};
      return false;
    }
  return true;
}

std::vector<uint8_t> cube_to_rgba8(const CubeLut& lut) {
  std::vector<uint8_t> px;
  if (!lut.valid()) return px;
  const size_t n = size_t(lut.size) * lut.size * lut.size;
  px.resize(n * 4);
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c) {
      const float lo = lut.domain_min[size_t(c)], hi = lut.domain_max[size_t(c)];
      const float v = (lut.rgb[i * 3 + size_t(c)] - lo) / (hi - lo);
      px[i * 4 + size_t(c)] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }
    px[i * 4 + 3] = 255;
  }
  return px;
}

std::array<float, 3> sample_cube(const CubeLut& lut, const std::array<float, 3>& rgb) {
  if (!lut.valid()) return rgb;
  const uint32_t N = lut.size;
  float f[3];
  uint32_t i0[3], i1[3];
  for (int c = 0; c < 3; ++c) {
    const float x = std::clamp(rgb[size_t(c)], 0.0f, 1.0f) * float(N - 1);
    i0[c] = std::min(uint32_t(x), N - 1);
    i1[c] = std::min(i0[c] + 1, N - 1);
    f[c] = x - float(i0[c]);
  }
  auto at = [&](uint32_t r, uint32_t g, uint32_t b, int ch) {
    const float raw = lut.rgb[((size_t(b) * N + g) * N + r) * 3 + size_t(ch)];
    const float lo = lut.domain_min[size_t(ch)], hi = lut.domain_max[size_t(ch)];
    return std::clamp((raw - lo) / (hi - lo), 0.0f, 1.0f);
  };
  std::array<float, 3> out{};
  for (int ch = 0; ch < 3; ++ch) {
    float acc = 0;
    for (int dz = 0; dz < 2; ++dz)
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const float w = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
          acc += w * at(dx ? i1[0] : i0[0], dy ? i1[1] : i0[1], dz ? i1[2] : i0[2], ch);
        }
    out[size_t(ch)] = acc;
  }
  return out;
}

// ---- Settings tokens ------------------------------------------------------------------------------
namespace {
const char* aa_name(AaMode m) {
  return m == AaMode::kFxaaWorld ? "fxaa" : m == AaMode::kFxaaAll ? "fxaa-hud" : "off";
}
const char* ssao_name(SsaoQuality q) {
  return q == SsaoQuality::kLow      ? "low"
         : q == SsaoQuality::kMedium ? "medium"
         : q == SsaoQuality::kHigh   ? "high"
         : q == SsaoQuality::kUltra  ? "ultra"
                                     : "off";
}
const char* vsync_name(VSync v) { return v == VSync::kMailbox ? "mailbox" : v == VSync::kImmediate ? "immediate" : "fifo"; }
const char* output_name(Upscaler u) {
  switch (u) {
    case Upscaler::kCas: return "cas";
    case Upscaler::kFsr1: return "fsr1";
    case Upscaler::kFsr2: return "fsr2";
    case Upscaler::kFsr3: return "fsr3";
    case Upscaler::kDlss: return "dlss";
    default: return "bilinear";
  }
}
std::string num(float v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", double(v));
  return b;
}
}  // namespace

const std::vector<std::string>& graphics_option_names() {
  static const std::vector<std::string> names = {
      "--render-scale", "--scene-540p", "--no-scene-540p", "--aa", "--ssao", "--contact-shadows", "--tonemapper",
      "--hex-tiling", "--no-hex-tiling", "--velocity-blur", "--shadow-pcss", "--ssr", "--volumetric-fog",
      "--car-detail", "--no-car-detail", "--lut", "--lut-strength", "--vrs", "--no-vrs",
      "--texture-cap", "--vsync", "--fps-cap", "--output", "--sharpening", "--frame-gen", "--hdr", "--no-hdr",
      "--auto-hdr", "--no-auto-hdr", "--hdr-paper-white", "--hdr-highlights", "--hdr-peak", "--hdr-compare",
      "--no-hdr-compare", "--hud-scale", "--safe-zone", "--hud-16-9", "--no-hud-16-9",
      "--rt", "--rt-bounces", "--rt-spp", "--rt-denoise", "--no-rt-denoise"};
  return names;
}

std::vector<std::string> graphics_tokens(const GraphicsSettings& s) {
  const GraphicsSettings d;
  std::vector<std::string> t;
  auto add = [&](const char* k, const std::string& v) {
    t.push_back(k);
    t.push_back(v);
  };
  if (s.scale != d.scale) add("--render-scale", std::to_string(s.scale));
  if (s.base_540p) t.push_back("--scene-540p");
  if (s.aa != d.aa) add("--aa", aa_name(s.aa));
  if (s.ssao != d.ssao) add("--ssao", ssao_name(s.ssao));
  if (s.contact_shadows != d.contact_shadows) add("--contact-shadows", contact_shadows_name(s.contact_shadows));
  if (s.tonemapper != d.tonemapper) add("--tonemapper", tonemapper_name(s.tonemapper));
  if (s.hex_tiling) t.push_back("--hex-tiling");
  if (s.velocity_blur != d.velocity_blur) add("--velocity-blur", velocity_blur_name(s.velocity_blur));
  if (s.shadow_pcss != d.shadow_pcss) add("--shadow-pcss", shadow_pcss_name(s.shadow_pcss));
  if (s.ssr != d.ssr) add("--ssr", ssr_name(s.ssr));
  if (s.volumetric_fog != d.volumetric_fog) add("--volumetric-fog", volumetric_fog_name(s.volumetric_fog));
  if (s.car_detail) t.push_back("--car-detail");
  if (!s.lut.empty()) add("--lut", s.lut);
  if (s.lut_strength != d.lut_strength) add("--lut-strength", num(s.lut_strength));
  if (s.vrs) t.push_back("--vrs");
  if (s.texture_cap != d.texture_cap) add("--texture-cap", std::to_string(int(s.texture_cap)));
  if (s.vsync != d.vsync) add("--vsync", vsync_name(s.vsync));
  if (s.fps_cap != d.fps_cap) add("--fps-cap", s.fps_cap == 0 ? std::string("display") : std::to_string(s.fps_cap));
  if (s.upscaler != d.upscaler) add("--output", output_name(s.upscaler));
  if (s.sharpening != d.sharpening) add("--sharpening", num(s.sharpening));
  if (s.frame_gen != d.frame_gen) add("--frame-gen", s.frame_gen == FrameGen::kFsr3 ? "fsr3" : s.frame_gen == FrameGen::kDlss ? "dlss" : "off");
  if (s.hdr) t.push_back("--hdr");
  if (s.auto_hdr) t.push_back("--auto-hdr");
  if (s.paper_white != d.paper_white) add("--hdr-paper-white", num(s.paper_white));
  if (s.hdr_highlights != d.hdr_highlights) add("--hdr-highlights", num(s.hdr_highlights));
  if (s.peak_nits != d.peak_nits) add("--hdr-peak", num(s.peak_nits));
  if (s.hdr_compare) t.push_back("--hdr-compare");
  if (s.hud_scale != d.hud_scale) add("--hud-scale", num(s.hud_scale));
  if (s.safe_zone != d.safe_zone) add("--safe-zone", num(s.safe_zone));
  if (!s.hud_lock_16_9) t.push_back("--no-hud-16-9");
  if (s.rt_mode != d.rt_mode) add("--rt", rt::rt_mode_name(s.rt_mode));
  if (s.rt_bounces != d.rt_bounces) add("--rt-bounces", std::to_string(rt::clamp_rt_bounces(s.rt_bounces)));
  if (s.rt_spp != d.rt_spp) add("--rt-spp", std::to_string(rt::clamp_rt_spp(s.rt_spp)));
  if (!s.rt_denoise) t.push_back("--no-rt-denoise");
  return t;
}

bool apply_graphics_option(GraphicsSettings& s, const std::vector<std::string>& args, size_t& i) {
  const std::string& a = args[i];
  auto value = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
  auto number = [&](float fallback) {
    const std::string v = value();
    return v.empty() ? fallback : float(std::atof(v.c_str()));
  };
  if (a == "--render-scale") s.scale = clamp_scale(int(number(1)));
  else if (a == "--scene-540p") s.base_540p = true;
  else if (a == "--no-scene-540p") s.base_540p = false;
  else if (a == "--aa") {
    const std::string v = value();
    s.aa = v == "fxaa" ? AaMode::kFxaaWorld : v == "fxaa-hud" ? AaMode::kFxaaAll : AaMode::kOff;
  } else if (a == "--ssao") {
    const std::string v = value();
    s.ssao = v == "low"      ? SsaoQuality::kLow
             : v == "medium" ? SsaoQuality::kMedium
             : v == "high"   ? SsaoQuality::kHigh
             : v == "ultra"  ? SsaoQuality::kUltra
                             : SsaoQuality::kOff;
  } else if (a == "--contact-shadows") {
    s.contact_shadows = parse_contact_shadows(value());
  } else if (a == "--tonemapper") {
    s.tonemapper = parse_tonemapper(value());
  } else if (a == "--hex-tiling") {
    s.hex_tiling = true;
  } else if (a == "--no-hex-tiling") {
    s.hex_tiling = false;
  } else if (a == "--velocity-blur") {
    s.velocity_blur = parse_velocity_blur(value());
  } else if (a == "--shadow-pcss") {
    s.shadow_pcss = parse_shadow_pcss(value());
  } else if (a == "--ssr") {
    s.ssr = parse_ssr(value());
  } else if (a == "--volumetric-fog") {
    s.volumetric_fog = parse_volumetric_fog(value());
  } else if (a == "--car-detail") {
    s.car_detail = true;
  } else if (a == "--no-car-detail") {
    s.car_detail = false;
  } else if (a == "--lut") s.lut = value();
  else if (a == "--lut-strength") s.lut_strength = std::clamp(number(100), 0.0f, 100.0f);
  else if (a == "--vrs") s.vrs = true;
  else if (a == "--no-vrs") s.vrs = false;
  else if (a == "--texture-cap") {
    const int v = std::atoi(value().c_str());
    s.texture_cap = v == 1024 ? TextureCap::k1024 : v == 512 ? TextureCap::k512 : v == 256 ? TextureCap::k256 : TextureCap::kFull;
  } else if (a == "--vsync") {
    const std::string v = value();
    s.vsync = v == "mailbox" ? VSync::kMailbox : v == "immediate" ? VSync::kImmediate : VSync::kFifo;
  } else if (a == "--fps-cap") {
    const std::string v = value();
    s.fps_cap = v == "display" ? 0 : std::atoi(v.c_str());
    if (v != "display" && std::find(kFpsCaps.begin(), kFpsCaps.end(), s.fps_cap) == kFpsCaps.end()) s.fps_cap = 60;
  } else if (a == "--output") {
    const std::string v = value();
    s.upscaler = v == "cas" ? Upscaler::kCas : v == "fsr1" ? Upscaler::kFsr1 : v == "fsr2" ? Upscaler::kFsr2
                 : v == "fsr3" ? Upscaler::kFsr3 : v == "dlss" ? Upscaler::kDlss : Upscaler::kBilinear;
  } else if (a == "--sharpening") s.sharpening = std::clamp(number(50), 0.0f, 100.0f);
  else if (a == "--frame-gen") {
    const std::string v = value();
    s.frame_gen = v == "fsr3" ? FrameGen::kFsr3 : v == "dlss" ? FrameGen::kDlss : FrameGen::kOff;
  } else if (a == "--hdr") s.hdr = true;
  else if (a == "--no-hdr") s.hdr = false;
  else if (a == "--auto-hdr") s.auto_hdr = true;
  else if (a == "--no-auto-hdr") s.auto_hdr = false;
  else if (a == "--hdr-paper-white") s.paper_white = clamp_paper_white(number(203));
  else if (a == "--hdr-highlights") s.hdr_highlights = std::clamp(number(50), 0.0f, 100.0f);
  else if (a == "--hdr-peak") s.peak_nits = std::clamp(number(1000), 200.0f, 10000.0f);
  else if (a == "--hdr-compare") s.hdr_compare = true;
  else if (a == "--no-hdr-compare") s.hdr_compare = false;
  else if (a == "--hud-scale") s.hud_scale = std::clamp(number(100), kMinHudScale, kMaxHudScale);
  else if (a == "--safe-zone") s.safe_zone = std::clamp(number(0), 0.0f, kMaxSafeZone);
  else if (a == "--hud-16-9") s.hud_lock_16_9 = true;
  else if (a == "--no-hud-16-9") s.hud_lock_16_9 = false;
  else if (a == "--rt") s.rt_mode = rt::parse_rt_mode(value());
  else if (a == "--rt-bounces") s.rt_bounces = rt::clamp_rt_bounces(int(number(2)));
  else if (a == "--rt-spp") s.rt_spp = rt::clamp_rt_spp(int(number(1)));
  else if (a == "--rt-denoise") s.rt_denoise = true;
  else if (a == "--no-rt-denoise") s.rt_denoise = false;
  else return false;
  return true;
}

GraphicsSettings clamp_graphics(GraphicsSettings s) {
  s.scale = clamp_scale(s.scale);
  if (s.render_height > 0u) s.render_height = std::clamp(s.render_height, 240u, 4320u);
  s.lut_strength = std::clamp(s.lut_strength, 0.0f, 100.0f);
  s.sharpening = std::clamp(s.sharpening, 0.0f, 100.0f);
  s.paper_white = clamp_paper_white(s.paper_white);
  s.hdr_highlights = std::clamp(s.hdr_highlights, 0.0f, 100.0f);
  s.peak_nits = std::clamp(s.peak_nits, 200.0f, 10000.0f);
  s.hud_scale = std::clamp(s.hud_scale, kMinHudScale, kMaxHudScale);
  s.safe_zone = std::clamp(s.safe_zone, 0.0f, kMaxSafeZone);
  if (s.fps_cap != 0 && std::find(kFpsCaps.begin(), kFpsCaps.end(), s.fps_cap) == kFpsCaps.end()) s.fps_cap = 60;
  s.rt_bounces = rt::clamp_rt_bounces(s.rt_bounces);
  s.rt_spp = rt::clamp_rt_spp(s.rt_spp);
  return s;
}

namespace {
struct PipelineCacheHeader {
  char magic[4];
  uint32_t version;
  uint32_t vendor_id;
  uint32_t device_id;
  uint8_t uuid[16];
  uint64_t payload_size;
};
static_assert(sizeof(PipelineCacheHeader) == 40);
}  // namespace

std::vector<uint8_t> encode_pipeline_cache_file(const PipelineCacheKey& key, const uint8_t* payload, size_t size) {
  PipelineCacheHeader hdr{};
  hdr.magic[0] = 'S', hdr.magic[1] = 'H', hdr.magic[2] = 'P', hdr.magic[3] = 'C';
  hdr.version = 1u;
  hdr.vendor_id = key.vendor_id;
  hdr.device_id = key.device_id;
  std::memcpy(hdr.uuid, key.uuid.data(), sizeof(hdr.uuid));
  hdr.payload_size = uint64_t(size);
  std::vector<uint8_t> out(sizeof(hdr) + size);
  std::memcpy(out.data(), &hdr, sizeof(hdr));
  if (size > 0 && payload) std::memcpy(out.data() + sizeof(hdr), payload, size);
  return out;
}

bool decode_pipeline_cache_file(const uint8_t* file_data, size_t file_size, const PipelineCacheKey& expected,
                                std::vector<uint8_t>& payload_out) {
  payload_out.clear();
  if (!file_data || file_size < sizeof(PipelineCacheHeader)) return false;
  PipelineCacheHeader hdr{};
  std::memcpy(&hdr, file_data, sizeof(hdr));
  if (hdr.magic[0] != 'S' || hdr.magic[1] != 'H' || hdr.magic[2] != 'P' || hdr.magic[3] != 'C') return false;
  if (hdr.version != 1u) return false;
  if (hdr.vendor_id != expected.vendor_id || hdr.device_id != expected.device_id) return false;
  if (std::memcmp(hdr.uuid, expected.uuid.data(), sizeof(hdr.uuid)) != 0) return false;
  if (hdr.payload_size != uint64_t(file_size - sizeof(hdr))) return false;
  if (hdr.payload_size == 0) return false;
  payload_out.resize(size_t(hdr.payload_size));
  std::memcpy(payload_out.data(), file_data + sizeof(hdr), size_t(hdr.payload_size));
  return true;
}

bool split_bindless_supported(const SplitBindlessLimits& limits, uint32_t texture_slots) {
  constexpr uint32_t kMinSamplers = 16u;
  constexpr uint32_t kHeadroom = 64u;
  const uint64_t need_images = uint64_t(texture_slots) + kHeadroom;
  return limits.max_per_stage_samplers >= kMinSamplers &&
         limits.max_per_stage_sampled_images >= need_images &&
         limits.max_set_sampled_images >= need_images;
}

ModernTargetPlan plan_modern_targets(const GraphicsSettings& g, Extent render, Extent output, bool post_bloom,
                                     HdrMode hdr_mode, uint32_t hdr_pattern, bool inspector_active,
                                     bool inspector_needs_motion, const UpscalerChoice& up_choice,
                                     bool post_motion_blur, bool need_brightness) {
  ModernTargetPlan p{};
  p.bloom = post_bloom;
  p.ssao = (g.ssao != SsaoQuality::kOff);
  p.sscs = (g.contact_shadows != ContactShadows::kOff);
  p.ssr = (g.ssr != SsrQuality::kOff);
  p.volumetric = (g.volumetric_fog != VolumetricFog::kOff);
  p.mblur_tiles = post_motion_blur && (g.velocity_blur != VelocityBlur::kOff);
  const bool need_scale = (render.w != output.w || render.h != output.h);
  const bool need_upscaler = need_scale || g.upscaler != Upscaler::kBilinear;
  const bool need_hdr_stage = (hdr_mode != HdrMode::kOff) || g.hdr_compare || (hdr_pattern != 0) || need_brightness;
  p.fxaa_scene = (g.aa == AaMode::kFxaaWorld);
  p.fxaa_standin = (g.aa == AaMode::kFxaaAll);
  p.modern_chain = need_upscaler || need_hdr_stage || p.fxaa_scene || p.fxaa_standin || inspector_active;
  p.upscale_tmp = p.modern_chain && !up_choice.temporal && up_choice.active == Upscaler::kFsr1;
  p.temporal_hist = p.modern_chain && up_choice.temporal;
  p.rt_active = (g.rt_mode != rt::RtMode::kOff);
  p.motion_vec = p.temporal_hist || p.mblur_tiles || p.rt_active || (inspector_active && inspector_needs_motion);
  p.inspector = inspector_active;
  return p;
}

ReflectionStep schedule_reflections(uint64_t frame_index, float reflection_rate, bool cubemap_initialized,
                                    bool mirror_active, bool mirror_initialized) {
  ReflectionStep s{};
  if (reflection_rate > 0.0f) {
    if (!cubemap_initialized) {
      s.render_cubemap = true;
      s.first_face = 0;
      s.face_count = 6;
    } else {
      // rate >= 1.0 -> every frame; 0.5 -> every 2; 0.25 -> every 4; 0.125 -> every 8; <= 0.08 -> every 16.
      const uint64_t interval =
          reflection_rate >= 0.99f ? 1ull
          : (reflection_rate >= 0.49f ? 2ull
             : (reflection_rate >= 0.24f ? 4ull
                : (reflection_rate >= 0.12f ? 8ull : 16ull)));
      const uint64_t f = frame_index > 0 ? frame_index - 1ull : 0ull;
      if ((f % interval) == 0ull) {
        s.render_cubemap = true;
        s.first_face = uint32_t((f / interval) % 6ull);
        s.face_count = 1;
      }
    }
  }
  if (mirror_active) {
    s.render_mirror = true;
    if (!mirror_initialized) {
      s.mirror_band_begin = 0;
      s.mirror_band_count = 3;
    } else {
      // Stagger 1 mirror band per frame (L / Center / R) to slash cockpit CPU/GPU cost 3x.
      const uint64_t f = frame_index > 0 ? frame_index - 1ull : 0ull;
      s.mirror_band_begin = uint32_t(f % 3ull);
      s.mirror_band_count = 1;
    }
  }
  return s;
}

WorldMemoryBudget choose_world_memory_budget(uint64_t device_local_bytes, bool low_preset, float reflection_rate) {
  WorldMemoryBudget b{};
  if (low_preset) {
    // 192-384 MiB vertex arena: scales cleanly with VRAM so dense 1.3M+ tri Horizon Festival
    // gathers and 240 km/h highway regathers never overflow or trigger blocking arena resets,
    // while still staying below the High/Ultra 512 MiB budget.
    b.vertex_arena_bytes = std::clamp<uint64_t>(device_local_bytes / 12, 192ULL << 20, 384ULL << 20);
    b.texture_lru_bytes = std::clamp<uint64_t>(device_local_bytes / 8, 128ULL << 20, 256ULL << 20);
    b.texture_lru_count = (device_local_bytes >= (4ULL << 30)) ? 1536u : 1024u;
  } else {
    // 256-512 MiB vertex (scales with VRAM, e.g. 256 MiB on 2 GB, 512 MiB on 6+ GB):
    // generous headroom for dense Horizon Festival gathers and 240 km/h streaming without resets.
    b.vertex_arena_bytes = std::clamp<uint64_t>(device_local_bytes / 10, 256ULL << 20, 512ULL << 20);
    // Keep desktop LRU budget generous (up to 512 MiB on >= 8 GB GPUs and 3584 textures,
    // staying comfortably within WorldGpu::kMaxTextures = 4096) so highway drives never thrash.
    const uint64_t max_tex_bytes = (device_local_bytes >= (8ULL << 30)) ? (512ULL << 20) : (320ULL << 20);
    b.texture_lru_bytes = std::clamp<uint64_t>(device_local_bytes / 6, 192ULL << 20, max_tex_bytes);
    b.texture_lru_count = 3584;
  }
  if (low_preset || reflection_rate <= 0.25f) {
    b.dyn_env_size = 128;
    b.dyn_env_mips = 8;  // 128 -> 1 (8 levels)
  } else {
    b.dyn_env_size = 256;
    b.dyn_env_mips = 9;  // 256 -> 1 (9 levels)
  }
  return b;
}

}  // namespace carview::modern
