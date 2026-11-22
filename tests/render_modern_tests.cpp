// Slice 11: the render-scale / HUD / ultrawide maths, the .cube reader, the
// HDR encoding, the upscaler fallbacks and the frame statistics (no Vulkan).
#include <cmath>
#include <cstdio>
#include <string>

#include "perf_overlay.h"
#include "render_modern.h"

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
  CHECK(ssao_radius_m(SsaoQuality::kHigh) > ssao_radius_m(SsaoQuality::kLow));
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
  s.scale = 2, s.base_540p = true, s.aa = AaMode::kFxaaAll, s.ssao = SsaoQuality::kHigh;
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
}

}  // namespace

int main() {
  test_scene_extent();
  test_fov_and_hud();
  test_textures_and_ao();
  test_presentation();
  test_hdr();
  test_upscaler_fallbacks();
  test_cube();
  test_settings_tokens();
  test_frame_stats();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("render_modern_tests: ok\n");
  return 0;
}
