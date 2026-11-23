// The pause menu's settings page: rows read from the file's tokens, left
// and right step a row, key rebinds capture a key, live_settings() reflects
// immediate state, and tokens() keeps the file's other options.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "settings_page.h"

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static bool has(const std::vector<std::string>& v, const std::vector<std::string>& seq) {
  for (size_t i = 0; i + seq.size() <= v.size(); ++i) {
    bool ok = true;
    for (size_t k = 0; k < seq.size(); ++k) ok = ok && v[i + k] == seq[k];
    if (ok) return true;
  }
  return false;
}

int main() {
  carview::SettingsPage page;
  page.load({"--size", "1280x720", "--preset", "low", "--no-traffic", "--difficulty", "hard", "--hour", "18",
             "--language", "FR", "--no-rumble", "--rumble-scale", "0.50", "--controller-layout", "2",
             "--steer-dz-in", "0.10", "--no-camera-effects", "--assist-braking", "2", "--assist-steering", "2",
             "--assist-tcs", "1", "--assist-stm", "0", "--assist-shifting", "2", "--assist-line", "1",
             "--assist-rewind", "0", "--keys", "throttle=I,camera=F1"});
  CHECK(page.value("preset") == "low");
  CHECK(page.value("traffic") == "off");
  CHECK(page.value("difficulty") == "hard");
  CHECK(page.value("radio") == "on");
  CHECK(page.value("camera") == "default");
  CHECK(page.value("camera effects") == "off");
  CHECK(page.value("rumble") == "off");
  CHECK(page.value("rumble scale") == "50%");
  CHECK(page.value("controller layout") == "layout 3");
  CHECK(page.value("steer deadzone in") == "10%");
  CHECK(page.value("braking") == "abs off");
  CHECK(page.value("steering") == "simulation");
  CHECK(page.value("traction control") == "on");
  CHECK(page.value("stability control") == "off");
  CHECK(page.value("shifting") == "manual+clutch");
  CHECK(page.value("driving line") == "braking");
  CHECK(page.value("rewind") == "off");
  CHECK(page.value("key throttle") == "I");
  CHECK(page.value("key camera") == "F1");

  // Live settings reflect all loaded controls and assists immediately.
  {
    const auto ls = page.live_settings();
    CHECK(ls.difficulty == 2);
    CHECK(ls.radio && ls.sound && ls.rubberband);
    CHECK(!ls.controls.camera_effects);
    CHECK(!ls.controls.rumble_enabled);
    CHECK(std::fabs(ls.controls.rumble_scale - 0.50) < 1e-6);
    CHECK(ls.controls.layout_id == 2);
    CHECK(std::fabs(ls.controls.deadzones.steering_inside - 0.10) < 1e-6);
    CHECK(ls.controls.keys.matches(drive::ControlAction::kThrottle, "I"));
    CHECK(ls.controls.keys.matches(drive::ControlAction::kSwitchCamera, "F1"));
    CHECK(ls.controls.assists.braking == 2);
    CHECK(ls.controls.assists.steering == 2);
    CHECK(ls.controls.assists.traction_control);
    CHECK(!ls.controls.assists.stability_control);
    CHECK(ls.controls.assists.shifting == 2);
    CHECK(ls.controls.assists.driving_line == 1);
    CHECK(!ls.controls.assists.rewind);
  }

  // Unchanged: the same options come back.
  auto t = page.tokens();
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));
  CHECK(has(t, {"--preset", "low"}) && has(t, {"--no-traffic"}) && has(t, {"--difficulty", "hard"}));
  CHECK(has(t, {"--no-rumble"}) && has(t, {"--controller-layout", "2"}) && has(t, {"--no-camera-effects"}));

  // Rows: preset, shadows, bloom, difficulty, rubber-banding, traffic, ...
  page.open();
  page.step(1);  // preset low -> high
  CHECK(page.value("preset") == "high");
  page.move(5);  // traffic
  page.step(1);  // off -> on
  CHECK(page.value("traffic") == "on");
  page.move(-2);  // difficulty
  page.step(1);   // hard -> pro
  CHECK(page.value("difficulty") == "pro");
  t = page.tokens();
  CHECK(has(t, {"--preset", "high"}) && !has(t, {"--no-traffic"}) && has(t, {"--difficulty", "pro"}));
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));

  // Rebind row capture and commit.
  page.open();
  page.move(-2);  // one before save line -> last rebind row ("key rewind")
  CHECK(page.on_rebind_row());
  page.begin_rebind();
  CHECK(page.capturing_key());
  CHECK(page.commit_rebind("Backspace"));
  CHECK(!page.capturing_key());
  CHECK(page.value("key rewind") == "Backspace");
  CHECK(page.live_settings().controls.keys.matches(drive::ControlAction::kRewind, "Backspace"));

  // The save line follows the last row and wraps.
  page.open();
  page.move(-1);
  CHECK(page.on_save_line());
  page.move(1);
  CHECK(!page.on_save_line());
  std::string title;
  std::vector<std::string> lines;
  int selected = -1;
  page.fill(title, lines, selected);
  CHECK(selected == 0 && lines[0] == "preset  < high >" && lines.back() == "save & apply");

  // Audio Sound Mix (0-200%), master volume/mute, Full Dynamic Range, radio
  // DJ/volume, and Your Music rows round-trip through tokens().
  carview::SettingsPage audio_page;
  audio_page.load({"--master-volume", "75", "--mute", "--full-dynamic-range", "--no-radio-dj", "--radio-volume", "50",
                   "--mix-engine", "175", "--mix-music", "25", "--mix-vo", "200", "--your-music",
                   "--your-music-shuffle", "--your-music-volume", "75"});
  CHECK(audio_page.value("master volume") == "75%");
  CHECK(audio_page.value("mute") == "on");
  CHECK(audio_page.value("full dynamic range") == "on");
  CHECK(audio_page.value("radio dj") == "off");
  CHECK(audio_page.value("radio volume") == "50%");
  CHECK(audio_page.value("mix engine") == "175%");
  CHECK(audio_page.value("mix music") == "25%");
  CHECK(audio_page.value("mix vo") == "200%");
  CHECK(audio_page.value("your music") == "on");
  CHECK(audio_page.value("your music shuffle") == "on");
  CHECK(audio_page.value("your music volume") == "75%");
  auto at = audio_page.tokens();
  CHECK(has(at, {"--master-volume", "75"}) && has(at, {"--mute"}) && has(at, {"--full-dynamic-range"}));
  CHECK(has(at, {"--no-radio-dj"}) && has(at, {"--radio-volume", "50"}));
  CHECK(has(at, {"--mix-engine", "175"}) && has(at, {"--mix-music", "25"}) && has(at, {"--mix-vo", "200"}));
  CHECK(has(at, {"--your-music"}) && has(at, {"--your-music-shuffle"}) && has(at, {"--your-music-volume", "75"}));

  // Slice 6: HARD+ (Pinyon) difficulty and AI opponents mode round-trip.
  carview::SettingsPage ai_page;
  ai_page.load({"--difficulty", "hard+", "--ai-mode", "hard"});
  CHECK(ai_page.value("difficulty") == "hard+ (pinyon)");
  CHECK(ai_page.value("ai opponents") == "hard (+1 tier)");
  const auto als = ai_page.live_settings();
  CHECK(als.difficulty == 4 && als.ai_mode == 2 && !als.rubberband);
  const auto ait = ai_page.tokens();
  CHECK(has(ait, {"--difficulty", "hard+"}) && has(ait, {"--ai-mode", "hard"}));

  carview::SettingsPage fair_page;
  fair_page.load({"--ai-mode", "fair"});
  CHECK(fair_page.value("ai opponents") == "no rubber-banding");
  CHECK(fair_page.live_settings().ai_mode == 1 && !fair_page.live_settings().rubberband);

  // Slice 11: Modern Display settings (render scale, SSAO, FXAA, .cube LUT, VRS, texture cap, VSync, HDR, HUD).
  carview::SettingsPage gfx_page;
  gfx_page.set_available_luts({"warm_desert.cube"});
  gfx_page.load({"--render-scale", "2", "--output", "fsr1", "--sharpening", "75", "--aa", "fxaa",
                 "--ssao", "high", "--lut", "warm_desert.cube", "--lut-strength", "50", "--vrs",
                 "--texture-cap", "512", "--vsync", "mailbox", "--fps-cap", "120", "--hdr",
                 "--hdr-paper-white", "250", "--hdr-highlights", "75", "--hdr-peak", "800",
                 "--hdr-compare", "--no-hud-16-9", "--hud-scale", "90", "--safe-zone", "5"});
  CHECK(gfx_page.value("render scale") == "2x (1440p)");
  CHECK(gfx_page.value("output scaling") == "fsr 1");
  CHECK(gfx_page.value("anti-aliasing") == "fxaa");
  CHECK(gfx_page.value("ambient occlusion") == "high");
  CHECK(gfx_page.value("colour lut") == "warm_desert.cube");
  CHECK(gfx_page.value("lut strength") == "50%");
  CHECK(gfx_page.value("vrs shading") == "on");
  CHECK(gfx_page.value("texture size") == "512");
  CHECK(gfx_page.value("vsync") == "mailbox");
  CHECK(gfx_page.value("frame rate") == "120 fps");
  CHECK(gfx_page.value("hdr") == "on");
  CHECK(gfx_page.value("hdr compare") == "on");
  CHECK(gfx_page.value("hud 16:9 lock") == "off");
  const auto gls = gfx_page.live_settings();
  CHECK(gls.graphics.scale == 2 && gls.graphics.upscaler == carview::modern::Upscaler::kFsr1);
  CHECK(gls.graphics.ssao == carview::modern::SsaoQuality::kHigh && gls.graphics.vrs);
  CHECK(gls.graphics.lut == "warm_desert.cube" && gls.graphics.lut_strength == 50.0f);
  CHECK(gls.graphics.hdr && gls.graphics.hdr_compare && !gls.graphics.hud_lock_16_9);
  const auto gt = gfx_page.tokens();
  CHECK(has(gt, {"--render-scale", "2"}) && has(gt, {"--output", "fsr1"}) && has(gt, {"--ssao", "high"}));
  CHECK(has(gt, {"--lut", "warm_desert.cube"}) && has(gt, {"--vrs"}) && has(gt, {"--hdr-compare"}));

  // Phase A SOTA Display settings (XeGTAO Ultra, SSCS, AgX/GT/PBR Neutral tonemappers, Hex-Tiling, Velocity Blur).
  carview::SettingsPage sota_page;
  sota_page.load({"--preset", "ultra", "--ssao", "ultra", "--contact-shadows", "ultra",
                  "--tonemapper", "agx", "--hex-tiling", "--velocity-blur", "high"});
  CHECK(sota_page.value("preset") == "ultra");
  CHECK(sota_page.value("ambient occlusion") == "ultra");
  CHECK(sota_page.value("contact shadows") == "ultra");
  CHECK(sota_page.value("tonemapper") == "agx");
  CHECK(sota_page.value("terrain hex-tiling") == "on");
  CHECK(sota_page.value("velocity motion blur") == "high");
  const auto sls = sota_page.live_settings();
  CHECK(sls.graphics.ssao == carview::modern::SsaoQuality::kUltra);
  CHECK(sls.graphics.contact_shadows == carview::modern::ContactShadows::kUltra);
  CHECK(sls.graphics.tonemapper == carview::modern::Tonemapper::kAgX);
  CHECK(sls.graphics.hex_tiling);
  CHECK(sls.graphics.velocity_blur == carview::modern::VelocityBlur::kHigh);
  const auto st = sota_page.tokens();
  CHECK(has(st, {"--preset", "ultra"}) && has(st, {"--ssao", "ultra"}) && has(st, {"--contact-shadows", "ultra"}));
  CHECK(has(st, {"--tonemapper", "agx"}) && has(st, {"--hex-tiling"}) && has(st, {"--velocity-blur", "high"}));

  carview::SettingsPage gt_page;
  gt_page.load({"--tonemapper", "gt"});
  CHECK(gt_page.value("tonemapper") == "gt filmic");
  CHECK(gt_page.live_settings().graphics.tonemapper == carview::modern::Tonemapper::kGtFilmic);

  carview::SettingsPage pbr_page;
  pbr_page.load({"--tonemapper", "pbr-neutral"});
  CHECK(pbr_page.value("tonemapper") == "pbr neutral");
  CHECK(pbr_page.live_settings().graphics.tonemapper == carview::modern::Tonemapper::kPbrNeutral);

  // Phase B SOTA Display settings (3-cascade PCSS, Hi-Z SSR, Volumetric Fog & Headlight Beams, Car Clearcoat Detail).
  carview::SettingsPage pb_page;
  pb_page.load({"--shadow-pcss", "ultra", "--ssr", "full", "--volumetric-fog", "half", "--car-detail"});
  CHECK(pb_page.value("shadow softness (pcss)") == "ultra");
  CHECK(pb_page.value("screen-space reflections") == "full-res");
  CHECK(pb_page.value("volumetric fog & beams") == "half-res");
  CHECK(pb_page.value("car clearcoat detail") == "on");
  const auto pbls = pb_page.live_settings();
  CHECK(pbls.graphics.shadow_pcss == carview::modern::ShadowPcss::kUltra);
  CHECK(pbls.graphics.ssr == carview::modern::SsrQuality::kFull);
  CHECK(pbls.graphics.volumetric_fog == carview::modern::VolumetricFog::kHalf);
  CHECK(pbls.graphics.car_detail);
  const auto pbt = pb_page.tokens();
  CHECK(has(pbt, {"--shadow-pcss", "ultra"}) && has(pbt, {"--ssr", "full"}) &&
        has(pbt, {"--volumetric-fog", "half"}) && has(pbt, {"--car-detail"}));

  carview::SettingsPage pb_high;
  pb_high.load({"--shadow-pcss", "high", "--ssr", "half", "--volumetric-fog", "quarter"});
  CHECK(pb_high.value("shadow softness (pcss)") == "high");
  CHECK(pb_high.value("screen-space reflections") == "half-res");
  CHECK(pb_high.value("volumetric fog & beams") == "quarter-res");
  CHECK(pb_high.live_settings().graphics.shadow_pcss == carview::modern::ShadowPcss::kHigh);
  CHECK(pb_high.live_settings().graphics.ssr == carview::modern::SsrQuality::kHalf);
  CHECK(pb_high.live_settings().graphics.volumetric_fog == carview::modern::VolumetricFog::kQuarter);

  // Pillar 6: Global Units setting (Metric km/h vs Imperial MPH) & mouse select_tab / select_row.
  carview::SettingsPage units_def;
  units_def.load({});
  CHECK(units_def.value("units") == "metric (km/h)");
  CHECK(units_def.live_settings().metric_units == true);

  carview::SettingsPage units_imp;
  units_imp.load({"--units", "imperial"});
  CHECK(units_imp.value("units") == "imperial (mph)");
  CHECK(units_imp.live_settings().metric_units == false);
  const auto ut = units_imp.tokens();
  CHECK(has(ut, {"--units", "imperial"}));
  units_imp.select_tab(2);  // HUD & Gameplay tab
  CHECK(int(units_imp.active_tab()) == 2);
  const auto tab_rows = units_imp.display_rows();
  CHECK(!tab_rows.empty());
  if (!tab_rows.empty()) {
    units_imp.select_row(tab_rows.back().flat_index);
    CHECK(units_imp.selected() == tab_rows.back().flat_index);
  }

  // Multi-backend RHI Display setting (--backend / --graphics_backend).
  {
    carview::SettingsPage be_def;
    be_def.load({});
    CHECK(be_def.value("graphics backend") == "auto");
    CHECK(be_def.live_settings().graphics_backend == "auto");

    carview::SettingsPage be_dx12;
    be_dx12.load({"--backend", "dx12"});
    CHECK(be_dx12.value("graphics backend") == "directx 12");
    CHECK(be_dx12.live_settings().graphics_backend == "dx12");
    CHECK(has(be_dx12.tokens(), {"--backend", "dx12"}));

    carview::SettingsPage be_mtl;
    be_mtl.load({"--graphics_backend", "metal"});
    CHECK(be_mtl.value("graphics backend") == "metal 3");
    CHECK(be_mtl.live_settings().graphics_backend == "metal");
    CHECK(has(be_mtl.tokens(), {"--backend", "metal"}));

    carview::SettingsPage be_vk;
    be_vk.load({"--graphics_backend", "vulkan"});
    CHECK(be_vk.value("graphics backend") == "vulkan 1.3");
    CHECK(be_vk.live_settings().graphics_backend == "vulkan");
    const auto drows = be_vk.display_rows();
    bool found_display = false;
    for (const auto& dr : drows) {
      if (dr.name == "graphics backend" && dr.tab == carview::SettingsPage::Tab::kDisplay) found_display = true;
    }
    CHECK(found_display);
  }

  std::printf("settings_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
