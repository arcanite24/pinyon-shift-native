// Slice 11: the render-scale / HUD / ultrawide maths, the .cube reader, the
// HDR encoding, the upscaler fallbacks and the frame statistics (no Vulkan).
#include <cmath>
#include <cstdio>
#include <string>

#include "frame_inspector.h"
#include "perf_overlay.h"
#include "platform_support.h"
#include "render_modern.h"
#include "upscaler.h"

using namespace carview::modern;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void test_scene_extent() {
  // 1x is the console's 1280 x 720 at a 16:9 window, whatever its size.
  CHECK(scene_extent({1920, 1080}, 1, false) == (Extent{1280, 720}));
  CHECK(scene_extent({3840, 2160}, 2, false) == (Extent{2560, 1440}));
  CHECK(scene_extent({1920, 1080}, 3, false) == (Extent{3840, 2160}));
  CHECK(scene_extent({1920, 1080}, 4, false) == (Extent{5120, 2880}));
  // 540p base: 960 x 540 times the scale.
  CHECK(scene_extent({1920, 1080}, 1, true) == (Extent{960, 540}));
  CHECK(scene_extent({1920, 1080}, 2, true) == (Extent{1920, 1080}));
  // Ultrawide keeps the height and widens: 21:9 at 1x -> 720 lines, 1680 wide.
  const Extent uw = scene_extent({3440, 1440}, 1, false);
  CHECK(uw.h == 720 && uw.w == 1720);
  CHECK(uw.w % 2 == 0);
  // 4:3 narrows.
  CHECK(scene_extent({1024, 768}, 1, false) == (Extent{960, 720}));
  // Scale 0 is the window; out-of-range clamps.
  CHECK(scene_extent({1600, 900}, 0, false) == (Extent{1600, 900}));
  CHECK(scene_extent({1600, 900}, 9, false) == scene_extent({1600, 900}, 4, false));
  CHECK(scene_extent({1600, 900}, -3, false) == (Extent{1600, 900}));
  CHECK(scene_extent({0, 0}, 1, false) == (Extent{0, 0}));
  CHECK(scale_label(2, false, {1920, 1080}) == "2X (2560x1440)");
  CHECK(scale_label(1, true, {1920, 1080}) == "540P 1X (960x540)");
  CHECK(scale_label(0, false, {1920, 1080}) == "NATIVE");
}

void test_fov_and_hud() {
  CHECK(near(hfov_from_vfov(60.0f, 16.0f / 9.0f), 91.49f, 0.1f));
  // Hor+: the vertical field is kept on an ultrawide.
  CHECK(near(effective_vfov(60.0f, 21.0f / 9.0f), 60.0f));
  // Capped when the horizontal field would pass the limit (32:9 from 80 degrees).
  const float v = effective_vfov(80.0f, 32.0f / 9.0f, 130.0f);
  CHECK(v < 80.0f);
  CHECK(near(hfov_from_vfov(v, 32.0f / 9.0f), 130.0f, 0.2f));

  // A 21:9 window: the HUD is a centred 16:9 box.
  HudLayout l = hud_layout(3440, 1440, true, 100, 0);
  CHECK(near(l.virtual_h, 1440) && near(l.virtual_w, 2560));
  CHECK(near(l.scale, 1.0f));
  CHECK(near(l.origin_x, 440) && near(l.origin_y, 0));
  // Unlocked, the HUD spans the screen.
  l = hud_layout(3440, 1440, false, 100, 0);
  CHECK(near(l.virtual_w, 3440) && near(l.origin_x, 0));
  // 16:9 and narrower windows are unchanged by the lock.
  l = hud_layout(1920, 1080, true, 100, 0);
  CHECK(near(l.virtual_w, 1920) && near(l.origin_x, 0));
  l = hud_layout(1024, 768, true, 100, 0);
  CHECK(near(l.virtual_w, 1024));
  // HUD scale and safe zone shrink about the centre: 90 % scale with a 5 % zone.
  l = hud_layout(1920, 1080, true, 90, 5);
  CHECK(near(l.scale, 0.9f * 0.9f));
  const auto centre = l.to_screen(960, 540);
  CHECK(near(centre[0], 960, 0.01f) && near(centre[1], 540, 0.01f));
  const auto back = l.to_virtual(centre[0], centre[1]);
  CHECK(near(back[0], 960, 0.01f));
  // Limits: 80..120 % and 0..10 %.
  l = hud_layout(1920, 1080, true, 500, 90);
  CHECK(near(l.scale, 1.2f * 0.8f));
  l = hud_layout(1920, 1080, true, 10, -4);
  CHECK(near(l.scale, 0.8f));
  l = hud_layout(0, 0, true, 100, 0);
  CHECK(l.virtual_w == 0 && l.scale == 1);
}

void test_textures_and_ao() {
  CHECK(texture_levels_to_skip(2048, 2048, 12, TextureCap::kFull) == 0);
  CHECK(texture_levels_to_skip(2048, 1024, 12, TextureCap::k1024) == 1);
  CHECK(texture_levels_to_skip(2048, 2048, 12, TextureCap::k256) == 3);
  CHECK(texture_levels_to_skip(512, 512, 10, TextureCap::k1024) == 0);
  // Never past the last level.
  CHECK(texture_levels_to_skip(2048, 2048, 2, TextureCap::k256) == 1);
  CHECK(texture_levels_to_skip(2048, 2048, 1, TextureCap::k256) == 0);
  CHECK(ssao_taps(SsaoQuality::kOff) == 0);
  CHECK(ssao_taps(SsaoQuality::kLow) < ssao_taps(SsaoQuality::kMedium));
  CHECK(ssao_taps(SsaoQuality::kMedium) < ssao_taps(SsaoQuality::kHigh));
  CHECK(ssao_taps(SsaoQuality::kHigh) < ssao_taps(SsaoQuality::kUltra));
  CHECK(ssao_radius_m(SsaoQuality::kHigh) > ssao_radius_m(SsaoQuality::kLow));
  CHECK(ssao_radius_m(SsaoQuality::kUltra) > ssao_radius_m(SsaoQuality::kHigh));
  // XeGTAO slice/step/resolution scaling across tiers.
  CHECK(gtao_slices(SsaoQuality::kOff) == 0 && gtao_steps(SsaoQuality::kOff) == 0);
  CHECK(gtao_slices(SsaoQuality::kLow) == 2 && gtao_steps(SsaoQuality::kLow) == 2 && !gtao_full_res(SsaoQuality::kLow));
  CHECK(gtao_slices(SsaoQuality::kHigh) == 3 && gtao_steps(SsaoQuality::kHigh) == 3 && !gtao_full_res(SsaoQuality::kHigh));
  CHECK(gtao_slices(SsaoQuality::kUltra) == 3 && gtao_steps(SsaoQuality::kUltra) == 4 && gtao_full_res(SsaoQuality::kUltra));
  // Screen-Space Contact Shadows (SSCS) scaling.
  CHECK(contact_shadow_steps(ContactShadows::kOff) == 0);
  CHECK(contact_shadow_steps(ContactShadows::kLow) == 8 && !contact_shadow_full_res(ContactShadows::kLow));
  CHECK(contact_shadow_steps(ContactShadows::kHigh) == 10 && !contact_shadow_full_res(ContactShadows::kHigh));
  CHECK(contact_shadow_steps(ContactShadows::kUltra) == 12 && contact_shadow_full_res(ContactShadows::kUltra));
  CHECK(parse_contact_shadows("ultra") == ContactShadows::kUltra);
  CHECK(std::string(contact_shadows_name(ContactShadows::kHigh)) == "high");
  // McGuire Velocity Motion Blur taps.
  CHECK(velocity_blur_taps(VelocityBlur::kOff) == 0);
  CHECK(velocity_blur_taps(VelocityBlur::kLow) == 9);
  CHECK(velocity_blur_taps(VelocityBlur::kHigh) == 13);
  CHECK(parse_velocity_blur("high") == VelocityBlur::kHigh);
}

void test_sota_tonemappers() {
  // Default tonemapper preserves authentic FH1 Hable.
  GraphicsSettings def;
  CHECK(def.tonemapper == Tonemapper::kHable);
  CHECK(std::string(tonemapper_name(Tonemapper::kHable)) == "hable");
  CHECK(parse_tonemapper("agx") == Tonemapper::kAgX);
  CHECK(parse_tonemapper("gt") == Tonemapper::kGtFilmic);
  CHECK(parse_tonemapper("pbr-neutral") == Tonemapper::kPbrNeutral);
  CHECK(parse_tonemapper("unknown") == Tonemapper::kHable);

  // All 3 SOTA curves must map 0 -> ~0, be strictly monotonic on grey ramp, stay bounded <= 1.0 at HDR 50.0,
  // and desaturate/compress bright highlights smoothly without negative values.
  auto verify_curve = [](auto fn, float mid_lo, float mid_hi) {
    const auto z = fn({0.0f, 0.0f, 0.0f});
    CHECK(z[0] >= 0.0f && z[0] < 0.01f);
    float prev = -1.0f;
    for (float x : {0.0f, 0.05f, 0.18f, 0.5f, 1.0f, 2.5f, 8.0f, 32.0f, 50.0f}) {
      const auto y = fn({x, x, x});
      CHECK(y[0] >= prev - 1e-4f && y[0] <= 1.0f);
      CHECK(near(y[0], y[1], 2e-3f) && near(y[1], y[2], 2e-3f));
      prev = y[0];
    }
    const auto mid = fn({0.18f, 0.18f, 0.18f});
    CHECK(mid[0] >= mid_lo && mid[0] <= mid_hi);
    // Bright saturated red headlight/taillight stays valid and hue-ordered (R >= G, R >= B).
    const auto red_hdr = fn({20.0f, 0.4f, 0.1f});
    CHECK(red_hdr[0] > 0.75f && red_hdr[0] <= 1.0f && red_hdr[0] >= red_hdr[1] && red_hdr[0] >= red_hdr[2]);
  };
  verify_curve(tonemap_agx, 0.12f, 0.25f);
  verify_curve(tonemap_gt_filmic, 0.12f, 0.24f);
  verify_curve(tonemap_pbr_neutral, 0.10f, 0.20f);
  // AgX highlight desaturation pulls extreme red gently toward white (G/B rise above 0.2 at HDR 20.0).
  const auto agx_red = tonemap_agx({20.0f, 0.2f, 0.1f});
  CHECK(agx_red[1] > 0.20f && agx_red[2] > 0.20f);
}

void test_presentation() {
  const uint32_t all = vsync_bit(VSync::kFifo) | vsync_bit(VSync::kMailbox) | vsync_bit(VSync::kImmediate);
  CHECK(resolve_vsync(VSync::kImmediate, all) == VSync::kImmediate);
  CHECK(resolve_vsync(VSync::kMailbox, all) == VSync::kMailbox);
  CHECK(resolve_vsync(VSync::kFifo, all) == VSync::kFifo);
  // Fallbacks: no immediate -> mailbox -> fifo.
  CHECK(resolve_vsync(VSync::kImmediate, vsync_bit(VSync::kFifo) | vsync_bit(VSync::kMailbox)) == VSync::kMailbox);
  CHECK(resolve_vsync(VSync::kImmediate, vsync_bit(VSync::kFifo)) == VSync::kFifo);
  CHECK(resolve_vsync(VSync::kMailbox, vsync_bit(VSync::kFifo)) == VSync::kFifo);
  CHECK(resolve_vsync(VSync::kMailbox, 0) == VSync::kFifo);

  CHECK(near(float(fps_cap_interval(60, 144)), 1.0f / 60.0f, 1e-6f));
  CHECK(near(float(fps_cap_interval(0, 144)), 1.0f / 144.0f, 1e-6f));
  CHECK(near(float(fps_cap_interval(0, 0)), 1.0f / 60.0f, 1e-6f));
  CHECK(kFpsCaps.size() == 6 && kFpsCaps[1] == 40);

  FramePacer p;
  p.set_interval(0.01);
  CHECK(p.wait_for(1.000) == 0.0);          // the first frame goes at once
  CHECK(near(float(p.wait_for(1.002)), 0.008f, 1e-4f));  // then waits out the slot
  // A hitch (50 ms) does not make the following frames run fast.
  CHECK(p.wait_for(1.050) == 0.0);
  const double w = p.wait_for(1.052);
  CHECK(w > 0.005 && w <= 0.01);
  p.set_interval(0);
  CHECK(p.wait_for(2.0) == 0.0);
}

void test_hdr() {
  CHECK(resolve_hdr(false, true, true) == HdrMode::kOff);
  CHECK(resolve_hdr(true, true, true) == HdrMode::kScRgb);
  CHECK(resolve_hdr(true, true, false) == HdrMode::kHdr10);
  CHECK(resolve_hdr(true, false, false) == HdrMode::kOff);
  CHECK(near(clamp_paper_white(10), 80) && near(clamp_paper_white(900), 400));
  CHECK(near(scrgb_scale(203), 203.0f / 80.0f));
  // ST 2084 reference points.
  CHECK(near(pq_encode(0.0f), 0.0f, 1e-4f));
  CHECK(near(pq_encode(10000.0f), 1.0f, 1e-4f));
  CHECK(near(pq_encode(100.0f), 0.5081f, 2e-3f));
  CHECK(near(pq_encode(203.0f), 0.5807f, 2e-3f));
  for (float nits : {1.0f, 80.0f, 203.0f, 1000.0f, 4000.0f}) CHECK(near(pq_decode(pq_encode(nits)) / nits, 1.0f, 0.01f));
  // BT.709 white stays white in BT.2020.
  const auto w = rec709_to_rec2020({1, 1, 1});
  CHECK(near(w[0], 1.0f, 2e-3f) && near(w[1], 1.0f, 2e-3f) && near(w[2], 1.0f, 2e-3f));
  const auto red = rec709_to_rec2020({1, 0, 0});
  CHECK(red[0] < 1.0f && red[1] > 0.0f);
  // Highlight expansion: nothing below the knee, peak/paper at white with full strength.
  CHECK(near(expand_highlight(0.4f, 100, 203, 1000), 0.4f));
  CHECK(near(expand_highlight(1.0f, 100, 203, 1000), 1000.0f / 203.0f, 1e-3f));
  CHECK(near(expand_highlight(1.0f, 0, 203, 1000), 1.0f));
  CHECK(near(expand_highlight(1.0f, 50, 203, 1000), 1.0f + (1000.0f / 203.0f - 1.0f) * 0.5f, 1e-3f));
  float prev = -1;
  for (int i = 0; i <= 20; ++i) {
    const float e = expand_highlight(float(i) / 20.0f, 70, 203, 1000);
    CHECK(e >= prev);
    prev = e;
  }
}

void test_upscaler_fallbacks() {
  UpscalerSupport none;  // temporal path only: no runtimes
  const Extent render{1280, 720}, out{1920, 1080};
  auto c = resolve_upscaler(Upscaler::kFsr1, none, render, out);
  CHECK(c.active == Upscaler::kFsr1 && !c.unavailable && c.label == "FSR 1");
  c = resolve_upscaler(Upscaler::kBilinear, none, render, out);
  CHECK(c.active == Upscaler::kBilinear && !c.temporal);
  c = resolve_upscaler(Upscaler::kFsr2, none, render, out);
  CHECK(c.active == Upscaler::kFsr2 && c.temporal && !c.unavailable);
  // Equal sizes: FSR 2/3 as native AA, DLSS as DLAA.
  c = resolve_upscaler(Upscaler::kFsr3, none, out, out);
  CHECK(c.native_aa && c.label == "FSR 3 (NATIVE AA)");
  // DLSS without an RTX GPU or its runtime: "(UNAVAILABLE)", FSR 1 runs.
  c = resolve_upscaler(Upscaler::kDlss, none, render, out);
  CHECK(c.unavailable && c.active == Upscaler::kFsr1 && c.label == "DLSS (UNAVAILABLE)");
  UpscalerSupport rtx_no_runtime;
  rtx_no_runtime.rtx = true;
  c = resolve_upscaler(Upscaler::kDlss, rtx_no_runtime, render, out);
  CHECK(c.unavailable && c.active == Upscaler::kFsr1);
  UpscalerSupport full;
  full.rtx = full.dlss_runtime = full.fsr_runtime = true;
  c = resolve_upscaler(Upscaler::kDlss, full, render, out);
  CHECK(!c.unavailable && c.active == Upscaler::kDlss && c.temporal && c.label == "DLSS");
  c = resolve_upscaler(Upscaler::kDlss, full, out, out);
  CHECK(c.label == "DLAA");
  // No temporal path: FSR 2 falls back too.
  UpscalerSupport no_temporal;
  no_temporal.temporal = false;
  c = resolve_upscaler(Upscaler::kFsr2, no_temporal, render, out);
  CHECK(c.unavailable && c.active == Upscaler::kFsr1);

  // Frame generation needs its hook, the temporal path and (DLSS) an RTX runtime.
  CHECK(resolve_frame_gen(FrameGen::kFsr3, none) == FrameGen::kOff);
  UpscalerSupport fg = none;
  fg.frame_gen = true;
  CHECK(resolve_frame_gen(FrameGen::kFsr3, fg) == FrameGen::kFsr3);
  CHECK(resolve_frame_gen(FrameGen::kDlss, fg) == FrameGen::kOff);
  CHECK(resolve_frame_gen(FrameGen::kDlss, full) == FrameGen::kOff);  // full has no frame_gen
  full.frame_gen = true;
  CHECK(resolve_frame_gen(FrameGen::kDlss, full) == FrameGen::kDlss);
  CHECK(resolve_frame_gen(FrameGen::kFsr3, fg, false) == FrameGen::kOff);

  // Jitter: FSR's phase count and a low-discrepancy sequence inside +-0.5 pixel.
  CHECK(jitter_phase_count({1280, 720}, {1280, 720}) == 8);
  CHECK(jitter_phase_count({1280, 720}, {1920, 1080}) == 18);  // 8 * 1.5^2
  CHECK(jitter_phase_count({640, 360}, {1920, 1080}) == 72);   // 8 * 3^2
  bool all_inside = true, distinct = true;
  std::array<float, 2> first = halton_jitter(0, 8);
  for (uint32_t i = 0; i < 16; ++i) {
    const auto j = halton_jitter(i, 8);
    if (std::fabs(j[0]) > 0.5f || std::fabs(j[1]) > 0.5f) all_inside = false;
    if (i > 0 && i < 8 && j == first) distinct = false;
    CHECK(halton_jitter(i, 8) == halton_jitter(i + 8, 8));  // it cycles
  }
  CHECK(all_inside && distinct);
}

std::string identity_cube(int n, const char* extra = "") {
  std::string s = std::string("# test\nTITLE \"id\"\n") + extra + "LUT_3D_SIZE " + std::to_string(n) + "\n";
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r) {
        char line[96];
        std::snprintf(line, sizeof line, "%.6f %.6f %.6f\n", double(r) / (n - 1), double(g) / (n - 1), double(b) / (n - 1));
        s += line;
      }
  return s;
}

void test_cube() {
  CubeLut lut;
  std::string err;
  for (int n : {17, 33}) {
    CHECK(parse_cube(identity_cube(n), lut, err));
    CHECK(lut.valid() && lut.size == uint32_t(n) && lut.title == "id");
    CHECK(lut.rgb.size() == size_t(n) * n * n * 3);
    // The identity table samples to its input.
    const auto s = sample_cube(lut, {0.25f, 0.6f, 0.9f});
    CHECK(near(s[0], 0.25f, 2e-3f) && near(s[1], 0.6f, 2e-3f) && near(s[2], 0.9f, 2e-3f));
    const auto px = cube_to_rgba8(lut);
    CHECK(px.size() == size_t(n) * n * n * 4);
    CHECK(px[3] == 255 && px[px.size() - 4] == 255 && px[px.size() - 3] == 255);  // last entry white
    // Red runs fastest: entry 1 is (1/(n-1), 0, 0).
    CHECK(px[4] == uint8_t(std::lround(255.0 / (n - 1))) && px[5] == 0 && px[6] == 0);
  }
  // CRLF files, comments, a domain.
  std::string crlf = identity_cube(2, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n");
  for (size_t i = crlf.find('\n'); i != std::string::npos; i = crlf.find('\n', i + 2)) crlf.insert(i, "\r");
  CHECK(parse_cube(crlf, lut, err));
  CHECK(lut.size == 2);
  // A domain wider than 0..1 is mapped when converting.
  CHECK(parse_cube("LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n0 0 0\n2 0 0\n0 2 0\n2 2 0\n0 0 2\n2 0 2\n0 2 2\n2 2 2\n", lut, err));
  CHECK(cube_to_rgba8(lut)[4] == 255);
  CHECK(near(sample_cube(lut, {1, 1, 1})[0], 1.0f));

  // Rejections, each with a reason.
  CHECK(!parse_cube("LUT_1D_SIZE 16\n0 0 0\n", lut, err) && err.find("1D") != std::string::npos);
  CHECK(!parse_cube("0 0 0\n", lut, err));
  CHECK(!parse_cube("LUT_3D_SIZE 1\n0 0 0\n", lut, err));
  CHECK(!parse_cube("LUT_3D_SIZE 2\n0 0 0\n1 1 1\n", lut, err) && err.find("expected 8") != std::string::npos);
  CHECK(!parse_cube("LUT_3D_SIZE 2\nBOGUS 1\n", lut, err));
  CHECK(!parse_cube("LUT_3D_SIZE 2\n0 0\n", lut, err));
  CHECK(!parse_cube("LUT_3D_SIZE 2\n0 0 0 0\n", lut, err));
  CHECK(!parse_cube("", lut, err));
  CHECK(!lut.valid());
  CHECK(!parse_cube("LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n", lut, err));

  // 17^3 and 33^3 .cube LUTs parse and pack to exact RGBA8 byte counts.
  for (uint32_t sz : {17u, 33u}) {
    std::string txt = "TITLE \"Identity\"\nLUT_3D_SIZE " + std::to_string(sz) + "\n";
    for (uint32_t b = 0; b < sz; ++b)
      for (uint32_t g = 0; g < sz; ++g)
        for (uint32_t r = 0; r < sz; ++r) {
          char row[64];
          std::snprintf(row, sizeof row, "%.6f %.6f %.6f\n", double(r) / (sz - 1), double(g) / (sz - 1),
                        double(b) / (sz - 1));
          txt += row;
        }
    CubeLut big;
    CHECK(parse_cube(txt, big, err));
    CHECK(big.size == sz);
    const auto rgba = cube_to_rgba8(big);
    CHECK(rgba.size() == size_t(sz) * sz * sz * 4u);
    CHECK(near(sample_cube(big, {0.5f, 0.25f, 0.75f})[1], 0.25f, 0.01f));
  }
}

void test_settings_tokens() {
  GraphicsSettings d;
  CHECK(graphics_tokens(d).empty());
  GraphicsSettings s;
  s.scale = 2, s.base_540p = true, s.aa = AaMode::kFxaaAll, s.ssao = SsaoQuality::kUltra;
  s.contact_shadows = ContactShadows::kHigh, s.tonemapper = Tonemapper::kAgX, s.hex_tiling = true;
  s.velocity_blur = VelocityBlur::kHigh;
  s.lut = "film.cube", s.lut_strength = 40, s.vrs = true, s.texture_cap = TextureCap::k512;
  s.vsync = VSync::kMailbox, s.fps_cap = 0, s.upscaler = Upscaler::kFsr3, s.sharpening = 70;
  s.frame_gen = FrameGen::kFsr3, s.hdr = true, s.auto_hdr = true, s.paper_white = 250, s.hdr_highlights = 80;
  s.peak_nits = 800, s.hdr_compare = true, s.hud_scale = 110, s.safe_zone = 4, s.hud_lock_16_9 = false;
  const auto tokens = graphics_tokens(s);
  GraphicsSettings back;
  for (size_t i = 0; i < tokens.size(); ++i) CHECK(apply_graphics_option(back, tokens, i));
  CHECK(back == s);
  // Unknown options are left alone; bad values clamp.
  std::vector<std::string> other = {"--preset", "low"};
  size_t i = 0;
  CHECK(!apply_graphics_option(back, other, i));
  std::vector<std::string> bad = {"--render-scale", "9", "--hud-scale", "500", "--fps-cap", "75", "--texture-cap", "4096"};
  GraphicsSettings b;
  for (i = 0; i < bad.size(); ++i) CHECK(apply_graphics_option(b, bad, i));
  CHECK(b.scale == 4 && near(b.hud_scale, 120) && b.fps_cap == 60 && b.texture_cap == TextureCap::kFull);
  CHECK(graphics_option_names().size() > 20);
  GraphicsSettings wild;
  wild.scale = 99, wild.paper_white = 1, wild.fps_cap = 7, wild.safe_zone = 99;
  wild = clamp_graphics(wild);
  CHECK(wild.scale == 4 && near(wild.paper_white, 80) && wild.fps_cap == 60 && near(wild.safe_zone, 10));
}

void test_frame_stats() {
  carview::FrameStats st(1.0);
  for (int i = 0; i < 120; ++i) st.frame(10.0 + i / 60.0);  // 60 fps for two seconds
  CHECK(near(float(st.fps()), 60.0f, 0.5f));
  CHECK(near(float(st.ms_avg()), 16.667f, 0.1f));
  CHECK(near(float(st.ms_percentile(0.99)), 16.667f, 0.1f));
  CHECK(st.frames() == 120);
  st.frame(10.0 + 120 / 60.0 + 0.1);  // one 100 ms hitch
  CHECK(st.ms_max() > 99.0 && st.ms_percentile(0.5) < 17.0);
  st.generated(30);
  CHECK(st.generated_total() == 30);
  CHECK(st.generated_fps() > 0.0);
  CHECK(!st.summary().empty());
  st.reset();
  CHECK(st.frames() == 0 && st.fps() == 0.0);

  carview::PerfInfo info;
  info.fps = 58, info.generated_fps = 58, info.render_w = 1280, info.render_h = 720, info.output_w = 1920,
  info.output_h = 1080, info.cpu_ms = 1.5, info.gpu_ms = 2.25, info.hdr = true, info.vsync = "FIFO";
  info.vram_used = 512ull << 20, info.vram_budget = 8ull << 30, info.resident_cells = 40;
  info.gpu_stages = {{"world", 1.0}, {"post", 0.5}};
  info.upscaler = "FSR 1";
  info.build = "shift-engine abc1234";
  const auto lines = carview::perf_overlay_lines(info);
  CHECK(lines.size() >= 6);
  CHECK(lines[0].find("real 58 + generated 58") != std::string::npos);
  CHECK(lines[1].find("RENDER 1280x720") != std::string::npos && lines[1].find("OUTPUT 1920x1080") != std::string::npos);
  CHECK(lines[2].find("CPU 1.50 ms") != std::string::npos && lines[2].find("GPU 2.25 ms") != std::string::npos);
  CHECK(lines.back() == "shift-engine abc1234");
  bool vram = false, hdr = false;
  for (const auto& l : lines) {
    if (l.find("VRAM 512.0 MiB / 8.00 GiB") != std::string::npos) vram = true;
    if (l.find("HDR ON") != std::string::npos && l.find("VSYNC FIFO") != std::string::npos) hdr = true;
  }
  CHECK(vram && hdr);
  CHECK(carview::format_bytes(1536) == "1.5 KiB");
  carview::ui::List list;
  carview::build_perf_overlay(list, info, 1920, 1080);
  CHECK(!list.empty());

  // Rolling 2-second / 120-frame 1% low window: an early 500ms load spike must age out!
  {
    carview::FrameStats rfs(1.0, 4096);
    double t = 0.0;
    rfs.frame(t);
    t += 0.500;  // 500 ms startup hitch (2 FPS)
    rfs.frame(t);
    for (int i = 0; i < 150; ++i) {
      t += 1.0 / 60.0;  // 2.5 seconds of rock-steady 60 FPS
      rfs.frame(t);
    }
    CHECK(rfs.ms_low_fps(0.01) > 55.0);
  }

  // Compact F3 overlay bounds: even with 16 GPU passes (many 0.00ms), width stays <= 420 px at 1080p!
  {
    carview::PerfInfo big = info;
    big.gpu_stages = {
        {"shadows", 0.57},      {"setup", 1.28},      {"world opaque", 0.0}, {"world cut-outs", 0.0},
        {"contact_shadow", 0.0},{"car_opaque", 0.0},  {"sky", 0.0},          {"world blended", 0.0},
        {"car_glass", 0.0},     {"hiz_build", 0.01},  {"particles", 0.0},    {"post", 0.13},
        {"upscale", 0.08},      {"ui", 0.18},         {"rest", 0.0},         {"env_cubemap", 0.24},
    };
    const auto rect = carview::perf_overlay_rect(big, 1920, 1080, 1.0f);
    const float panel_w = rect[2] - rect[0];
    CHECK(panel_w >= 300.0f && panel_w <= 420.0f);
    const auto blines = carview::perf_overlay_lines(big);
    for (const auto& ln : blines) {
      CHECK(ln.size() < 80);  // no unwrapped 200-char GPU stage line!
      CHECK(ln.find("0.00") == std::string::npos);
    }
  }
}

void test_upscaler_runtime_and_inspector() {
  // Missing DLLs under non-existent path must cleanly yield unavailable fallback without crashing.
  const std::filesystem::path bogus = std::filesystem::temp_directory_path() / "shift_no_upscalers_dir_xyz";
  const auto rt_amd = carview::probe_upscaler_runtime(0x1002u, bogus, bogus);
  CHECK(!rt_amd.support.rtx && !rt_amd.support.dlss_runtime && !rt_amd.support.temporal);
  const auto rt_nv = carview::probe_upscaler_runtime(0x10DEu, bogus, bogus);
  CHECK(rt_nv.support.rtx && !rt_nv.support.dlss_runtime && !rt_nv.support.temporal);
  CHECK(rt_nv.ffx_dll_path.empty() && rt_nv.dlss_dll_path.empty() && rt_nv.dlssg_dll_path.empty());

  const auto dlss_choice = resolve_upscaler(Upscaler::kDlss, rt_amd.support, {1280, 720}, {1920, 1080});
  CHECK(dlss_choice.unavailable && dlss_choice.active == Upscaler::kFsr1);
  const auto fsr3_choice = resolve_upscaler(Upscaler::kFsr3, rt_amd.support, {1280, 720}, {1920, 1080});
  CHECK(fsr3_choice.unavailable && fsr3_choice.active == Upscaler::kFsr1);

  // Frame Inspector view parsing & layout generation.
  CHECK(carview::parse_inspector_view("color") == carview::InspectorView::kColor);
  CHECK(carview::parse_inspector_view("depth") == carview::InspectorView::kDepth);
  CHECK(carview::parse_inspector_view("ao") == carview::InspectorView::kAo);
  CHECK(carview::parse_inspector_view("ssao") == carview::InspectorView::kAo);
  CHECK(carview::parse_inspector_view("shadow") == carview::InspectorView::kShadow);
  CHECK(carview::parse_inspector_view("motion") == carview::InspectorView::kMotion);
  CHECK(carview::parse_inspector_view("hiz") == carview::InspectorView::kHiZ);
  CHECK(carview::parse_inspector_view("hi-z") == carview::InspectorView::kHiZ);

  carview::FrameInspectorSnapshot snap;
  snap.open = true;
  snap.view = carview::InspectorView::kDepth;
  snap.render_w = 1280, snap.render_h = 720, snap.output_w = 1920, snap.output_h = 1080;
  snap.cpu_ms = 2.1, snap.gpu_ms = 3.4;
  snap.pass_timings = {{"zpre", 0.3}, {"world", 1.2}, {"post", 0.8}};
  snap.world_draws = 412, snap.world_culled = 98, snap.world_triangles = 125000;
  snap.shadow_draws = 64, snap.shadow_cars = 4;
  snap.resident_cells = 18, snap.resident_draws = 520;
  snap.vram_used = 256ull << 20, snap.vram_budget = 8ull << 30;
  snap.vrs_active = true, snap.reverse_z = true, snap.z_prepass = true;
  snap.hdr_mode = "HDR10", snap.upscaler = "FSR 1";
  snap.has_preview = true, snap.preview_uv = {0.1f, 0.1f, 0.3f, 0.3f};

  const auto ilines = carview::frame_inspector_lines(snap);
  CHECK(ilines.size() >= 8);
  CHECK(ilines[0].find("DEPTH") != std::string::npos);
  bool has_draws = false, has_pass = false;
  for (const auto& l : ilines) {
    if (l.find("WORLD 412") != std::string::npos && l.find("98 CULLED") != std::string::npos) has_draws = true;
    if (l.find("world") != std::string::npos && l.find("1.20") != std::string::npos) has_pass = true;
  }
  CHECK(has_draws && has_pass);
  carview::ui::List ilist;
  carview::build_frame_inspector(ilist, snap, 1920, 1080);
  CHECK(!ilist.empty());
}

void test_split_bindless_and_pipeline_cache() {
  // Split bindless: 1 immutable sampler + SAMPLED_IMAGE[4096] unlocks Apple Silicon (16 samplers)
  // and Android Turnip/Adreno (16 samplers, >=16k sampled images).
  SplitBindlessLimits apple_silicon{16, 500000, 500000};
  CHECK(split_bindless_supported(apple_silicon));
  SplitBindlessLimits turnip{16, 16384, 16384};
  CHECK(split_bindless_supported(turnip));
  SplitBindlessLimits too_few_samplers{8, 16384, 16384};
  CHECK(!split_bindless_supported(too_few_samplers));
  SplitBindlessLimits too_few_images{64, 1024, 16384};
  CHECK(!split_bindless_supported(too_few_images));

  // Persistent VkPipelineCache header validation.
  PipelineCacheKey key{};
  key.vendor_id = 0x10DEu;
  key.device_id = 0x2704u;
  for (size_t i = 0; i < 16; ++i) key.uuid[i] = uint8_t(i + 1);
  const std::vector<uint8_t> payload = {0x11, 0x22, 0x33, 0x44, 0x55};
  const auto encoded = encode_pipeline_cache_file(key, payload.data(), payload.size());
  std::vector<uint8_t> decoded;
  CHECK(decode_pipeline_cache_file(encoded.data(), encoded.size(), key, decoded));
  CHECK(decoded == payload);

  // Mismatched vendor / device / UUID / truncated blob must be rejected.
  PipelineCacheKey wrong_vendor = key;
  wrong_vendor.vendor_id = 0x1002u;
  CHECK(!decode_pipeline_cache_file(encoded.data(), encoded.size(), wrong_vendor, decoded));
  PipelineCacheKey wrong_uuid = key;
  wrong_uuid.uuid[3] ^= 0xFFu;
  CHECK(!decode_pipeline_cache_file(encoded.data(), encoded.size(), wrong_uuid, decoded));
  CHECK(!decode_pipeline_cache_file(encoded.data(), 12, key, decoded));
}

void test_lazy_targets_and_reflection_schedule() {
  // Low-power default: no bloom, no SSAO, no FXAA, no upscale/HDR -> zero optional targets.
  GraphicsSettings g;
  UpscalerChoice bil{};
  const auto low_plan =
      plan_modern_targets(g, {1920, 1080}, {1920, 1080}, false, HdrMode::kOff, 0, false, false, bil);
  CHECK(!low_plan.bloom && !low_plan.ssao && !low_plan.fxaa_scene && !low_plan.fxaa_standin &&
        !low_plan.modern_chain && !low_plan.upscale_tmp && !low_plan.temporal_hist && !low_plan.motion_vec &&
        !low_plan.inspector);

  // 4K Ultra SSAO + FXAA: allocates bloom, SSAO, FXAA scene, and modern chain, NOT temporal/motion.
  GraphicsSettings ultra;
  ultra.ssao = SsaoQuality::kHigh;
  ultra.aa = AaMode::kFxaaWorld;
  const auto ultra_plan =
      plan_modern_targets(ultra, {3840, 2160}, {3840, 2160}, true, HdrMode::kOff, 0, false, false, bil);
  CHECK(ultra_plan.bloom && ultra_plan.ssao && ultra_plan.fxaa_scene && ultra_plan.modern_chain &&
        !ultra_plan.temporal_hist && !ultra_plan.motion_vec);

  // Temporal upscaler (FSR2): enables temporal_hist and motion_vec.
  UpscalerChoice fsr2{};
  fsr2.active = Upscaler::kFsr2;
  fsr2.temporal = true;
  GraphicsSettings ts;
  ts.upscaler = Upscaler::kFsr2;
  const auto fsr2_plan =
      plan_modern_targets(ts, {1280, 720}, {1920, 1080}, true, HdrMode::kOff, 0, false, false, fsr2);
  CHECK(fsr2_plan.modern_chain && fsr2_plan.temporal_hist && fsr2_plan.motion_vec && !fsr2_plan.upscale_tmp);

  // Phase A SOTA: SSCS + McGuire Velocity Motion Blur allocate sscs, mblur_tiles, and motion_vec lazily.
  GraphicsSettings sota;
  sota.ssao = SsaoQuality::kUltra;
  sota.contact_shadows = ContactShadows::kUltra;
  sota.velocity_blur = VelocityBlur::kHigh;
  const auto sota_plan =
      plan_modern_targets(sota, {1920, 1080}, {1920, 1080}, true, HdrMode::kOff, 0, false, false, bil, true);
  CHECK(sota_plan.bloom && sota_plan.ssao && sota_plan.sscs && sota_plan.mblur_tiles && sota_plan.motion_vec);
  // Without post_motion_blur active, mblur_tiles and motion_vec stay unallocated.
  const auto sota_no_mb =
      plan_modern_targets(sota, {1920, 1080}, {1920, 1080}, true, HdrMode::kOff, 0, false, false, bil, false);
  CHECK(sota_no_mb.sscs && !sota_no_mb.mblur_tiles && !sota_no_mb.motion_vec);

  // Phase B SOTA: Hi-Z SSR + Volumetric Scattering allocate ssr and volumetric lazily (off on low-power).
  CHECK(!low_plan.ssr && !low_plan.volumetric);
  GraphicsSettings phase_b;
  phase_b.shadow_pcss = ShadowPcss::kUltra;
  phase_b.ssr = SsrQuality::kFull;
  phase_b.volumetric_fog = VolumetricFog::kHalf;
  phase_b.car_detail = true;
  const auto pb_plan =
      plan_modern_targets(phase_b, {3840, 2160}, {3840, 2160}, true, HdrMode::kOff, 0, false, false, bil, true);
  CHECK(pb_plan.ssr && pb_plan.volumetric);
  // 3-cascade CSM & PCSS tap scaling across tiers.
  CHECK(shadow_cascade_count(ShadowPcss::kOff) == 2 && !shadow_pcss_enabled(ShadowPcss::kOff));
  CHECK(shadow_cascade_count(ShadowPcss::kLow) == 2 && shadow_pcss_taps(ShadowPcss::kLow) == 4);
  CHECK(shadow_cascade_count(ShadowPcss::kHigh) == 3 && shadow_pcss_enabled(ShadowPcss::kHigh) &&
        shadow_pcss_taps(ShadowPcss::kHigh) == 12);
  CHECK(shadow_cascade_count(ShadowPcss::kUltra) == 3 && shadow_pcss_enabled(ShadowPcss::kUltra) &&
        shadow_pcss_taps(ShadowPcss::kUltra) == 16);
  CHECK(parse_shadow_pcss("high") == ShadowPcss::kHigh && parse_shadow_pcss("ultra") == ShadowPcss::kUltra);
  CHECK(std::string(shadow_pcss_name(ShadowPcss::kHigh)) == "high");
  // Hi-Z SSR & Volumetric Fog scaling across tiers.
  CHECK(ssr_steps(SsrQuality::kOff) == 0 && !ssr_full_res(SsrQuality::kOff));
  CHECK(ssr_steps(SsrQuality::kHalf) == 24 && !ssr_full_res(SsrQuality::kHalf));
  CHECK(ssr_steps(SsrQuality::kFull) == 36 && ssr_full_res(SsrQuality::kFull));
  CHECK(parse_ssr("half") == SsrQuality::kHalf && parse_ssr("full") == SsrQuality::kFull);
  CHECK(volumetric_steps(VolumetricFog::kOff) == 0 && volumetric_scale_div(VolumetricFog::kOff) == 0u);
  CHECK(volumetric_steps(VolumetricFog::kQuarter) == 16 && volumetric_scale_div(VolumetricFog::kQuarter) == 4u);
  CHECK(volumetric_steps(VolumetricFog::kHalf) == 24 && volumetric_scale_div(VolumetricFog::kHalf) == 2u);
  CHECK(parse_volumetric_fog("quarter") == VolumetricFog::kQuarter &&
        parse_volumetric_fog("half") == VolumetricFog::kHalf);
  // Planckian blackbody brake rotor thermal ramp (900 K -> 1450 K): cold = 0, hot = HDR bloom > 8.0, R > G > B.
  const auto cold = blackbody_rotor_rgb(0.0f);
  const auto warm = blackbody_rotor_rgb(0.45f);
  const auto hot = blackbody_rotor_rgb(1.0f);
  CHECK(cold[0] == 0.0f && cold[1] == 0.0f && cold[2] == 0.0f);
  CHECK(warm[0] > warm[1] && warm[1] > warm[2] && warm[0] > 0.5f);
  CHECK(hot[0] > 9.0f && hot[0] > hot[1] && hot[1] > hot[2] && hot[1] > 4.0f);
  // Graphics presets map all 4 hardware tiers cleanly for Phase B.
  {
    carview::GraphicsPresetSpec lp, lo, hi, ul;
    CHECK(carview::resolve_graphics_preset("low-power", lp));
    CHECK(carview::resolve_graphics_preset("low", lo));
    CHECK(carview::resolve_graphics_preset("high", hi));
    CHECK(carview::resolve_graphics_preset("ultra", ul));
    CHECK(lp.shadow_pcss == ShadowPcss::kOff && lp.ssr == SsrQuality::kOff &&
          lp.volumetric_fog == VolumetricFog::kOff && !lp.car_detail);
    CHECK(lo.shadow_pcss == ShadowPcss::kLow && lo.ssr == SsrQuality::kOff &&
          lo.volumetric_fog == VolumetricFog::kOff && !lo.car_detail);
    CHECK(hi.shadow_pcss == ShadowPcss::kHigh && hi.ssr == SsrQuality::kHalf &&
          hi.volumetric_fog == VolumetricFog::kQuarter && hi.car_detail);
    CHECK(ul.shadow_pcss == ShadowPcss::kUltra && ul.ssr == SsrQuality::kFull &&
          ul.volumetric_fog == VolumetricFog::kHalf && ul.car_detail);
  }
  // Phase B token round-trip.
  {
    const auto toks = graphics_tokens(phase_b);
    GraphicsSettings parsed;
    for (size_t i = 0; i < toks.size(); ++i) CHECK(apply_graphics_option(parsed, toks, i));
    CHECK(parsed == phase_b);
  }

  // Reflection rate staggering & mirror gating.
  const auto init_step = schedule_reflections(0, 0.25f, false, false, false);
  CHECK(init_step.render_cubemap && init_step.face_count == 6 && !init_step.render_mirror);
  int low_hits = 0;
  for (uint64_t f = 1; f <= 8; ++f) {
    const auto st = schedule_reflections(f, 0.25f, true, false, false);
    if (st.render_cubemap) ++low_hits;
    CHECK(!st.render_mirror);
  }
  CHECK(low_hits == 2);  // quarter-rate: 1 face every 4 frames

  const auto zero_step = schedule_reflections(1, 0.0f, true, false, false);
  CHECK(!zero_step.render_cubemap);

  // Cockpit mirror: full 3 bands on first frame, then staggered 1 band/frame cycling 0, 1, 2.
  const auto m0 = schedule_reflections(1, 1.0f, true, true, false);
  CHECK(m0.render_mirror && m0.mirror_band_begin == 0 && m0.mirror_band_count == 3);
  const auto m1 = schedule_reflections(1, 1.0f, true, true, true);
  const auto m2 = schedule_reflections(2, 1.0f, true, true, true);
  const auto m3 = schedule_reflections(3, 1.0f, true, true, true);
  CHECK(m1.mirror_band_count == 1 && m2.mirror_band_count == 1 && m3.mirror_band_count == 1);
  CHECK(m1.mirror_band_begin == 0 && m2.mirror_band_begin == 1 && m3.mirror_band_begin == 2);

  // World memory budgets per preset.
  const auto mb_low = choose_world_memory_budget(16ULL << 30, true, 0.25f);
  const auto mb_high = choose_world_memory_budget(16ULL << 30, false, 1.0f);
  CHECK(mb_low.vertex_arena_bytes < mb_high.vertex_arena_bytes);
  CHECK(mb_low.dyn_env_size == 128 && mb_low.dyn_env_mips == 8);
  CHECK(mb_high.dyn_env_size == 256 && mb_high.dyn_env_mips == 9);
}

}  // namespace

int main() {
  test_scene_extent();
  test_fov_and_hud();
  test_textures_and_ao();
  test_sota_tonemappers();
  test_presentation();
  test_hdr();
  test_upscaler_fallbacks();
  test_cube();
  test_settings_tokens();
  test_frame_stats();
  test_upscaler_runtime_and_inspector();
  test_split_bindless_and_pipeline_cache();
  test_lazy_targets_and_reflection_schedule();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("render_modern_tests: ok\n");
  return 0;
}
