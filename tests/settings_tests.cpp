// The pause menu's settings page: rows read from the file's tokens, left
// and right step a row, key rebinds capture a key, live_settings() reflects
// immediate state, and tokens() keeps the file's other options.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "settings.h"
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
             "--assist-tcs", "0", "--assist-stm", "1", "--assist-shifting", "2", "--assist-line", "1",
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
  CHECK(page.value("traction control") == "off");
  CHECK(page.value("stability control") == "on");
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
    CHECK(!ls.controls.assists.traction_control);
    CHECK(ls.controls.assists.stability_control);
    CHECK(ls.controls.assists.shifting == 2);
    CHECK(ls.controls.assists.driving_line == 1);
    CHECK(!ls.controls.assists.rewind);
  }

  // Unchanged: the same options come back.
  auto t = page.tokens();
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));
  CHECK(has(t, {"--preset", "low"}) && has(t, {"--no-traffic"}) && has(t, {"--difficulty", "hard"}));
  CHECK(has(t, {"--no-rumble"}) && has(t, {"--controller-layout", "2"}) && has(t, {"--no-camera-effects"}));

  // Tab-scoped navigation: Display tab starts on preset, Gameplay tab holds difficulty & traffic.
  page.open();
  page.step(1);  // preset low -> high
  CHECK(page.value("preset") == "high");
  page.select_tab(int(carview::SettingsPage::Tab::kGameplay));
  CHECK(page.active_tab() == carview::SettingsPage::Tab::kGameplay);
  page.move(3);  // difficulty preset -> difficulty -> rubber-banding -> traffic
  page.step(1);  // off -> on
  CHECK(page.value("traffic") == "on");
  page.move(-2);  // back to difficulty
  page.step(1);   // hard -> pro
  CHECK(page.value("difficulty") == "pro");
  t = page.tokens();
  CHECK(has(t, {"--preset", "high"}) && !has(t, {"--no-traffic"}) && has(t, {"--difficulty", "pro"}));
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));

  // Rebind row capture and commit (Controls tab, one before save line -> last rebind row "key telemetry").
  page.open();
  page.select_tab(int(carview::SettingsPage::Tab::kControls));
  page.move(-2);  // one before save line -> last rebind row ("key telemetry")
  CHECK(page.active_tab() == carview::SettingsPage::Tab::kControls);
  CHECK(page.on_rebind_row());
  page.begin_rebind();
  CHECK(page.capturing_key());
  CHECK(page.commit_rebind("F4"));
  CHECK(!page.capturing_key());
  CHECK(page.value("key telemetry") == "F4");
  CHECK(page.live_settings().controls.keys.matches(drive::ControlAction::kTelemetry, "F4"));

  // The save line follows the last row of each tab and wraps within that tab.
  page.open();
  CHECK(page.active_tab() == carview::SettingsPage::Tab::kDisplay);
  page.move(-1);
  CHECK(page.on_save_line());
  CHECK(page.active_tab() == carview::SettingsPage::Tab::kDisplay);
  page.move(1);
  CHECK(!page.on_save_line());
  CHECK(page.active_tab() == carview::SettingsPage::Tab::kDisplay);
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

  // Driving line defaults to Full (2), and stepping to Off persists "--assist-line 0" -> 0.
  {
    carview::SettingsPage dl_def;
    dl_def.load({});
    CHECK(dl_def.value("driving line") == "full");
    CHECK(dl_def.live_settings().controls.assists.driving_line == 2);

    carview::SettingsPage dl_off;
    dl_off.load({"--assist-line", "0"});
    CHECK(dl_off.value("driving line") == "off");
    CHECK(dl_off.live_settings().controls.assists.driving_line == 0);
    CHECK(has(dl_off.tokens(), {"--assist-line", "0"}));

    carview::SettingsPage dl_legacy_full;
    dl_legacy_full.load({"--assist-line", "0", "--assist-line", "2"});
    CHECK(dl_legacy_full.value("driving line") == "full");
    CHECK(dl_legacy_full.live_settings().controls.assists.driving_line == 2);
  }

  // Value-taking flags (--damage, --shadow-pcss, --ssr, --volumetric-fog, --units)
  // must never leak their argument string into unowned tokens on reload.
  {
    carview::SettingsPage leak;
    leak.load({"--damage", "simulation", "--shadow-pcss", "ultra", "--ssr", "full",
               "--volumetric-fog", "half", "--units", "imperial"});
    CHECK(leak.value("damage") == "simulation");
    CHECK(leak.value("shadow softness (pcss)") == "ultra");
    CHECK(leak.value("screen-space reflections") == "full-res");
    CHECK(leak.value("volumetric fog & beams") == "half-res");
    CHECK(leak.value("units") == "imperial (mph)");
    const auto toks = leak.tokens();
    CHECK(toks.size() == 10);
    // Resetting all of them back to default via reload must yield zero tokens.
    carview::SettingsPage cleared;
    cleared.load({"--damage", "simulation", "--damage", "cosmetic", "--no-car-detail", "--no-head-tracking"});
    CHECK(cleared.value("damage") == "cosmetic");
    CHECK(cleared.value("car clearcoat detail") == "off");
    CHECK(cleared.value("head tracking (opentrack)") == "off");
  }

  // Exhaustive settings.txt write_settings -> read_settings -> load round-trip
  // for EVERY row and EVERY choice across ALL 5 Settings tabs + ALL 10 key rebinds.
  {
    const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_engine_settings_tests";
    std::error_code ec;
    std::filesystem::create_directories(tmp_dir, ec);
    const auto tmp_file = tmp_dir / "settings_roundtrip.txt";

    carview::SettingsPage base;
    base.set_available_luts({"warm_desert.cube", "midnight_neon.cube"});
    base.load({});
    const auto all_rows = base.display_rows();

    int tab_counts[int(carview::SettingsPage::Tab::kCount)] = {};
    int rows_verified = 0;
    int choices_verified = 0;
    int rebinds_verified = 0;

    // 1) Verify every single choice of every single row survives settings.txt disk I/O
    //    and every emitted option is accepted by is_settings_option() (CLI startup safe).
    //    Batch by wave (step index k=1..20) across non-preset rows so all ~350 choices
    //    hit real settings.txt disk I/O in ~20 file writes instead of 350 individual writes,
    //    plus test the "preset" row individually across all 5 presets.
    std::map<std::string, std::vector<std::string>> seen_choices;
    for (const auto& dr : all_rows) {
      if (dr.is_save) continue;
      ++tab_counts[int(dr.tab)];
      if (!dr.is_rebind) ++rows_verified;
    }

    // 1a) Preset row individually (since stepping preset also updates SOTA defaults).
    for (const auto& dr : all_rows) {
      if (dr.name != "preset") continue;
      carview::SettingsPage p;
      p.load({});
      p.open();
      p.select_row(dr.flat_index);
      const std::string initial_val = p.value(dr.name);
      for (int s = 0; s < 10; ++s) {
        p.step(1);
        CHECK(p.dirty());
        const std::string picked = p.value(dr.name);
        const auto toks = p.tokens();
        for (size_t ti = 0; ti < toks.size(); ++ti) {
          bool takes_val = false;
          CHECK(carview::is_settings_option(toks[ti], &takes_val));
          if (takes_val) {
            CHECK(ti + 1 < toks.size());
            ++ti;
          }
        }
        std::string werr;
        CHECK(carview::write_settings(tmp_file, toks, &werr));
        std::vector<std::string> notes;
        const auto reread = carview::read_settings(tmp_file, &notes);
        CHECK(notes.empty());
        carview::SettingsPage reloaded;
        reloaded.load(reread);
        CHECK(reloaded.value(dr.name) == picked);
        ++choices_verified;
        if (picked == initial_val) break;
      }
      break;
    }

    // 1b) All remaining rows stepped in lockstep waves through real settings.txt I/O.
    {
      carview::SettingsPage wave;
      wave.set_available_luts({"warm_desert.cube", "midnight_neon.cube"});
      wave.load({});
      wave.open();
      std::map<std::string, std::string> initial_vals;
      std::map<std::string, bool> wrapped;
      for (const auto& dr : all_rows) {
        if (dr.is_save || dr.is_rebind || dr.name == "preset" || dr.name == "difficulty preset") continue;
        initial_vals[dr.name] = wave.value(dr.name);
        wrapped[dr.name] = false;
      }
      for (int w = 0; w < 24; ++w) {
        bool any_remaining = false;
        std::map<std::string, std::string> wave_expected;
        for (const auto& dr : all_rows) {
          if (dr.is_save || dr.is_rebind || dr.name == "preset" || dr.name == "difficulty preset") continue;
          if (!wrapped[dr.name]) {
            any_remaining = true;
            wave.select_row(dr.flat_index);
            wave.step(1);
            CHECK(wave.dirty());
            const std::string picked = wave.value(dr.name);
            ++choices_verified;
            if (picked == initial_vals[dr.name]) wrapped[dr.name] = true;
          }
          wave_expected[dr.name] = wave.value(dr.name);
        }
        if (!any_remaining) break;
        const auto toks = wave.tokens();
        for (size_t ti = 0; ti < toks.size(); ++ti) {
          bool takes_val = false;
          CHECK(carview::is_settings_option(toks[ti], &takes_val));
          if (takes_val) {
            CHECK(ti + 1 < toks.size());
            ++ti;
          }
        }
        std::string werr;
        CHECK(carview::write_settings(tmp_file, toks, &werr));
        std::vector<std::string> notes;
        const auto reread = carview::read_settings(tmp_file, &notes);
        if (!notes.empty()) {
          std::printf("wave %d read_settings note: %s\n", w, notes[0].c_str());
        }
        CHECK(notes.empty());
        carview::SettingsPage reloaded;
        reloaded.set_available_luts({"warm_desert.cube", "midnight_neon.cube"});
        reloaded.load(reread);
        for (const auto& [rname, rval] : wave_expected) {
          if (reloaded.value(rname) != rval) {
            std::printf("WAVE %d MISMATCH row='%s': picked='%s' reloaded='%s'\n",
                        w, rname.c_str(), rval.c_str(), reloaded.value(rname).c_str());
          }
          CHECK(reloaded.value(rname) == rval);
        }
      }
    }

    // All 5 tabs must have rows tested.
    for (int t_idx = 0; t_idx < int(carview::SettingsPage::Tab::kCount); ++t_idx) {
      CHECK(tab_counts[t_idx] > 0);
    }
    CHECK(rows_verified >= 85);
    CHECK(choices_verified >= 200);

    // 2) Simultaneously modify EVERY row across ALL 5 tabs + ALL 10 key rebinds,
    //    write to settings.txt, reopen in a fresh SettingsPage, and verify 100% match.
    carview::SettingsPage full;
    full.set_available_luts({"warm_desert.cube"});
    full.load({});
    full.open();
    std::map<std::string, std::string> expected;
    const std::vector<std::string> custom_keys = {"I", "K", "J", "L", "N", "F1", "B", "U", "O", "Backspace",
                                                  "Z", "X", "T", "G", "Y", "F2", "F4", "F5", "F6", "Tab"};
    size_t key_idx = 0;
    for (const auto& dr : full.display_rows()) {
      if (dr.is_save) continue;
      full.select_row(dr.flat_index);
      if (dr.is_rebind) {
        full.begin_rebind();
        CHECK(full.commit_rebind(custom_keys[key_idx % custom_keys.size()]));
        expected[dr.name] = custom_keys[key_idx % custom_keys.size()];
        ++key_idx;
        ++rebinds_verified;
      } else {
        full.step(1);
        expected[dr.name] = full.value(dr.name);
      }
    }
    CHECK(rebinds_verified == 20);
    // Note: stepping "preset" first set SOTA rows, then stepping each SOTA row adjusted them;
    // record final values right before writing so expected reflects the exact page state.
    for (const auto& dr : full.display_rows()) {
      if (!dr.is_save) expected[dr.name] = full.value(dr.name);
    }

    std::string werr;
    CHECK(carview::write_settings(tmp_file, full.tokens(), &werr));
    std::vector<std::string> notes;
    const auto full_reread = carview::read_settings(tmp_file, &notes);
    CHECK(notes.empty());
    carview::SettingsPage reopened;
    reopened.set_available_luts({"warm_desert.cube"});
    reopened.load(full_reread);
    CHECK(reopened.has_explicit_control_tokens());
    for (const auto& [rname, rval] : expected) {
      if (reopened.value(rname) != rval) {
        std::printf("FULL REOPEN MISMATCH row='%s': expected='%s' got='%s'\n",
                    rname.c_str(), rval.c_str(), reopened.value(rname).c_str());
      }
      CHECK(reopened.value(rname) == rval);
    }

    // 3) Preset SOTA preservation + explicit SOTA off-override persistence.
    carview::SettingsPage preset_p;
    preset_p.load({"--preset", "ultra"});
    CHECK(preset_p.live_settings().graphics.ssao == carview::modern::SsaoQuality::kUltra);
    CHECK(preset_p.live_settings().graphics.ssr == carview::modern::SsrQuality::kFull);
    // Stepping SSR to off under ultra preset must emit "--ssr off" and persist off across reload.
    preset_p.open();
    for (const auto& dr : preset_p.display_rows()) {
      if (dr.name == "screen-space reflections") {
        preset_p.select_row(dr.flat_index);
        // step 1 -> half-res, step 2 -> full-res, step 3 -> off (sets explicit_default)
        preset_p.step(1);
        preset_p.step(1);
        preset_p.step(1);
        break;
      }
    }
    CHECK(preset_p.value("screen-space reflections") == "off");
    CHECK(has(preset_p.tokens(), {"--ssr", "off"}));
    CHECK(preset_p.live_settings().graphics.ssr == carview::modern::SsrQuality::kOff);
    CHECK(preset_p.live_settings().graphics.ssao == carview::modern::SsaoQuality::kUltra);
    CHECK(carview::write_settings(tmp_file, preset_p.tokens(), &werr));
    carview::SettingsPage preset_re;
    preset_re.load(carview::read_settings(tmp_file, nullptr));
    CHECK(preset_re.value("preset") == "ultra");
    CHECK(preset_re.value("screen-space reflections") == "off");
    CHECK(preset_re.live_settings().graphics.ssr == carview::modern::SsrQuality::kOff);
    CHECK(preset_re.live_settings().graphics.ssao == carview::modern::SsaoQuality::kUltra);

    // 4) sync_from_live() accurately mirrors live runtime values changed outside settings.txt.
    carview::LiveSettings live_in;
    live_in.difficulty = 3;  // pro
    live_in.ai_mode = 2;     // hard
    live_in.damage_mode = 2; // simulation
    live_in.metric_units = false;
    live_in.language = "JP";
    live_in.treasure_map = true;
    live_in.motion_blur = false;
    live_in.dof = false;
    live_in.sound = false;
    live_in.radio = false;
    live_in.controls.assists.driving_line = 0;  // off
    live_in.controls.assists.traction_control = true;
    live_in.controls.assists.shifting = 2;
    live_in.controls.rumble_enabled = false;
    live_in.controls.rumble_scale = 0.50;
    live_in.graphics_backend = "dx12";
    live_in.graphics.tonemapper = carview::modern::Tonemapper::kAgX;
    live_in.graphics.fps_cap = 144;
    live_in.graphics.vsync = carview::modern::VSync::kMailbox;
    live_in.graphics.hdr = true;

    carview::SettingsPage synced;
    synced.load({});
    synced.sync_from_live(live_in);
    CHECK(synced.value("difficulty") == "pro");
    CHECK(synced.value("ai opponents") == "hard (+1 tier)");
    CHECK(synced.value("damage") == "simulation");
    CHECK(synced.value("units") == "imperial (mph)");
    CHECK(synced.value("language") == "japanese");
    CHECK(synced.value("treasure map") == "on");
    CHECK(synced.value("motion blur") == "off");
    CHECK(synced.value("depth of field") == "off");
    CHECK(synced.value("sound") == "off");
    CHECK(synced.value("radio") == "off");
    CHECK(synced.value("driving line") == "off");
    CHECK(synced.value("traction control") == "on");
    CHECK(synced.value("shifting") == "manual+clutch");
    CHECK(synced.value("rumble") == "off");
    CHECK(synced.value("rumble scale") == "50%");
    CHECK(synced.value("graphics backend") == "directx 12");
    CHECK(synced.value("tonemapper") == "agx");
    CHECK(synced.value("frame rate") == "144 fps");
    CHECK(synced.value("vsync") == "mailbox");
    CHECK(synced.value("hdr") == "on");

    // Phase 6A: Speedometer mode, North-Up minimap, Switch Handbrake/Clutch, and cumulative Difficulty CR % bonus.
    carview::SettingsPage p6;
    p6.load({"--hud-speedo", "digital", "--minimap-mode", "north-up", "--switch-handbrake-clutch",
             "--difficulty", "pro", "--assist-braking", "2", "--assist-steering", "2", "--assist-tcs", "0",
             "--assist-shifting", "2", "--assist-line", "0", "--assist-rewind", "0", "--damage", "simulation"});
    const auto p6_ls = p6.live_settings();
    CHECK(p6_ls.hud_speedo_mode == 1);
    CHECK(p6_ls.minimap_mode == 1);
    CHECK(p6_ls.controls.switch_handbrake_clutch == true);
    // The original's HUD options: on by default, off by their profile names,
    // and back from the live state.
    CHECK(p6_ls.hud == carview::HudToggles{});
    {
      carview::SettingsPage hp;
      hp.load({"--no-hud-timers", "--no-hud-names-over-cars"});
      const auto hl = hp.live_settings();
      CHECK(!hl.hud.timers && hl.hud.wrong_way && !hl.hud.names_over_cars && hl.hud.popularity_skills);
      carview::LiveSettings off;
      off.hud.wrong_way = off.hud.popularity_skills = false;
      carview::SettingsPage hs;
      hs.load({});
      hs.sync_from_live(off);
      CHECK(hs.value("wrong way warning") == "off" && hs.value("popularity and skills") == "off");
      CHECK(hs.value("lap time and position") == "on" && hs.value("names over cars") == "on");
    }
    // Pro (+25) + ABS Off (+20) + Sim Steering (+20) + TCS Off (+15) + M+C (+20) + Line Off (+20) + Rewind Off (+20) = 140% (the ceiling; STM and damage are not counted)
    CHECK(p6.difficulty_credit_bonus_pct() == 140);
    const auto base_lay = drive::default_controller_layout(0);
    const auto swap_lay = drive::apply_handbrake_clutch_swap(base_lay, true);
    CHECK(base_lay.control_for("Handbrake") == swap_lay.control_for("Clutch"));
    CHECK(base_lay.control_for("Clutch") == swap_lay.control_for("Handbrake"));

    // R3-UI-15: Top Difficulty Preset macro row (EASY/MEDIUM/HARD/INSANE/CUSTOM with live +XX% CR preview)
    // and clean tab separation (HUD rows moved onto Gameplay/HUD tab).
    {
      carview::SettingsPage dp;
      dp.load({});
      dp.open();
      dp.select_tab(int(carview::SettingsPage::Tab::kGameplay));
      CHECK(dp.value("difficulty preset") == "MEDIUM (+25% CR)");
      // FEEL-12: a new profile (Medium) has TCS on and STM off.
      CHECK(dp.value("traction control") == "on");
      CHECK(dp.value("stability control") == "off");
      dp.step(1);  // MEDIUM -> HARD (+75% CR)
      CHECK(dp.value("difficulty preset") == "HARD (+75% CR)");
      CHECK(dp.difficulty_credit_bonus_pct() == 75);
      CHECK(dp.value("difficulty") == "hard");
      CHECK(dp.value("shifting") == "manual");
      dp.step(1);  // HARD -> INSANE (+140% CR)
      CHECK(dp.value("difficulty preset") == "INSANE (+140% CR)");
      CHECK(dp.difficulty_credit_bonus_pct() == 140);
      CHECK(dp.value("rewind") == "off");
      CHECK(dp.value("damage") == "simulation");
      dp.step(1);  // INSANE -> EASY (+0% CR)
      CHECK(dp.value("difficulty preset") == "EASY (+0% CR)");
      CHECK(dp.difficulty_credit_bonus_pct() == 0);
      CHECK(dp.value("braking") == "assisted");
      // Changing an individual assist flips preset cleanly to CUSTOM (+XX% CR).
      dp.move(1);  // difficulty
      dp.step(1);  // easy -> medium
      CHECK(dp.value("difficulty preset").rfind("CUSTOM (+", 0) == 0);

      bool hud_scale_on_gameplay = false;
      bool preset_on_display = false;
      for (const auto& dr : dp.display_rows()) {
        if (dr.name == "hud scale" && dr.tab == carview::SettingsPage::Tab::kGameplay) hud_scale_on_gameplay = true;
        if (dr.name == "preset" && dr.tab == carview::SettingsPage::Tab::kDisplay) preset_on_display = true;
      }
      CHECK(hud_scale_on_gameplay && preset_on_display);
    }

    // R3-UI-07: Metric vs Imperial formatting for chase height & chase distance camera offsets.
    {
      carview::SettingsPage cm;
      cm.load({"--chase-height", "-0.50", "--chase-distance", "1.00"});
      CHECK(cm.value("chase height") == "-0.50 m");
      CHECK(cm.value("chase distance") == "+1.00 m");
      carview::SettingsPage ci;
      ci.load({"--units", "imperial", "--chase-height", "-0.50", "--chase-distance", "1.00"});
      CHECK(ci.value("chase height") == "-1.64 ft");
      CHECK(ci.value("chase distance") == "+3.28 ft");
      carview::SettingsPage cdef;
      cdef.load({"--units", "imperial"});
      CHECK(cdef.value("chase height") == "default");
      CHECK(cdef.value("chase distance") == "default");
    }

    // R3-UI-06 & R3-UI-13: Explicit ControllerDiagramSpec on Controls tab + optional text resolver.
    {
      carview::SettingsPage ctrl_p;
      ctrl_p.load({"--controller-layout", "2", "--switch-handbrake-clutch"});
      ctrl_p.open();
      CHECK(!ctrl_p.controller_diagram_spec().enabled);  // Display tab
      ctrl_p.select_tab(int(carview::SettingsPage::Tab::kControls));
      const auto dspec = ctrl_p.controller_diagram_spec();
      CHECK(dspec.enabled && dspec.layout_id == 2 && dspec.swap_handbrake_clutch);
      carview::SettingsPage::set_text_resolver(
          [](std::string_view, std::string_view id, const char* fb) -> std::string {
            if (id == "IDS_Audio") return "AUDIO_LOC";
            return fb;
          });
      CHECK(carview::SettingsPage::tab_label(carview::SettingsPage::Tab::kAudio) == "AUDIO_LOC");
      carview::SettingsPage::set_text_resolver(nullptr);
    }

    std::filesystem::remove(tmp_file, ec);
    std::filesystem::remove(tmp_dir, ec);
  }

  // VIS-31: display rows carry the row's own name, so the help card can pick
  // the difficulty preset's text whatever the row's localized label is.
  {
    carview::SettingsPage p;
    p.load({});
    bool found = false;
    for (const auto& r : p.display_rows()) found = found || r.id == "difficulty preset";
    CHECK(found);
  }

  // VIS-29: --no-settings means no settings file is read or written for the
  // whole run (the settings page, the pause menu's difficulty and the camera
  // editor all write through write_settings). Temp paths only.
  {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "shift_engine_settings_guard_tests";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "settings.txt";
    {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out << "preset high\n";
    }
    auto content = [&] {
      std::ifstream in(file, std::ios::binary);
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    CHECK(!carview::settings_file_disabled());
    CHECK(has(carview::read_settings(file, nullptr), {"--preset", "high"}));

    carview::set_settings_file_disabled(true);
    CHECK(carview::settings_file_disabled());
    CHECK(carview::read_settings(file, nullptr).empty());
    std::string werr;
    CHECK(!carview::write_settings(file, {"--preset", "low", "--no-traffic"}, &werr));
    CHECK(!werr.empty());
    CHECK(content() == "preset high\n");
    const auto other = dir / "new_settings.txt";
    CHECK(!carview::write_settings(other, {"--preset", "low"}, &werr));
    CHECK(!std::filesystem::exists(other));
    carview::SettingsPage guarded;
    guarded.load(carview::read_settings(file, nullptr));
    CHECK(!carview::write_settings(file, guarded.tokens(), &werr));
    CHECK(content() == "preset high\n");

    carview::set_settings_file_disabled(false);
    CHECK(carview::write_settings(file, {"--preset", "low"}, &werr));
    CHECK(has(carview::read_settings(file, nullptr), {"--preset", "low"}));
    std::filesystem::remove_all(dir, ec);
  }

  std::printf("settings_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
