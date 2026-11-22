#include "photo_mode.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include "drive/save.h"
#include "settings_page.h"
#include "ui4_chrome.h"

namespace {

int g_fails = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

void test_limits_and_settings() {
  const auto stock = carview::PhotoLimits::from_settings(false, false);
  const auto wider = carview::PhotoLimits::from_settings(true, true);
  check(std::fabs(stock.max_height - 2.0) < 1e-6, "stock max_height = 2m");
  check(std::fabs(stock.cylinder_radius - 100.0) < 1e-6, "stock cylinder_radius = 100m");
  check(!stock.through_walls, "stock through_walls = false");
  check(std::fabs(wider.max_height - 30.0) < 1e-6, "wider max_height = 30m");
  check(std::fabs(wider.cylinder_radius - 300.0) < 1e-6, "wider cylinder_radius = 300m");
  check(std::fabs(wider.min_fov_deg - 1.0) < 1e-6, "wider min_fov = 1 deg");
  check(wider.through_walls, "wider through_walls = true");

  carview::SettingsPage sp;
  sp.load({"--photo-through-walls", "--photo-wider-limits"});
  const auto ls = sp.live_settings();
  check(ls.photo_through_walls, "SettingsPage parses --photo-through-walls");
  check(ls.photo_wider_limits, "SettingsPage parses --photo-wider-limits");
  check(sp.value("photo camera through walls") == "on", "SettingsPage row value through walls");
  check(sp.value("wider photo limits") == "on", "SettingsPage row value wider limits");
}

void test_camera_clamping_and_autofocus() {
  carview::PhotoCamera cam;
  const std::array<double, 3> anchor = {100.0, 10.0, 200.0};
  auto flat_ground = [](double, double, double, double& gy) {
    gy = 10.0;
    return true;
  };
  cam.reset(anchor, 0.0, carview::PhotoLimits::from_settings(false, false));

  // Push straight up for 5 seconds under stock limits -> capped at 10 + 2 = 12 m.
  carview::PhotoCameraInput up{};
  up.move_up = 1.0f;
  for (int i = 0; i < 60; ++i) cam.step(0.1, up, flat_ground);
  check(cam.eye()[1] <= 12.001, "stock camera clamped to max_height 2m above ground");

  // Push straight down -> clamped above ground clearance (10.25 m).
  carview::PhotoCameraInput down{};
  down.move_up = -1.0f;
  for (int i = 0; i < 60; ++i) cam.step(0.1, down, flat_ground);
  check(cam.eye()[1] >= 10.249, "stock camera clamped above ground clearance");

  // Enable wider limits + through walls -> can climb to 30m+ and dip below ground.
  cam.set_limits(carview::PhotoLimits::from_settings(true, true));
  for (int i = 0; i < 120; ++i) cam.step(0.1, up, flat_ground);
  check(cam.eye()[1] > 25.0, "wider limits camera climbs past 25m");
  for (int i = 0; i < 200; ++i) cam.step(0.1, down, flat_ground);
  check(cam.eye()[1] < 9.5, "through_walls camera passes below terrain");

  const float dist = cam.autofocus_distance(anchor);
  check(dist >= 1.5f && dist <= 120.0f, "autofocus distance in valid range");
}

void test_effects_and_presets() {
  carview::PhotoEffects fx;
  carview::PostSettings base;
  base.enabled = true;
  base.exposure = 1.0f;
  base.contrast = 1.0f;

  fx.apply_preset(carview::PhotoPreset::kSepiaVintage);
  auto post = fx.apply_to_post(base);
  check(post.sepia > 0.7f, "Sepia Vintage sets sepia > 0.7");
  check(post.vignette_amount > 0.4f, "Sepia Vintage sets vignette > 0.4");
  check(post.dof && post.dof_max_blur > 0.3f, "Sepia Vintage enables DoF");

  fx.apply_preset(carview::PhotoPreset::kBlackAndWhite);
  post = fx.apply_to_post(base);
  check(post.desaturate < 0.01f, "B&W Classic sets desaturate to 0");

  fx.apply_preset(carview::PhotoPreset::kMotorsportAction);
  post = fx.apply_to_post(base);
  check(post.motion_blur && post.motion_blur_strength > 0.5f, "Motorsport Action enables motion blur");
  check(fx.wheel_blur_omega_boost() > 40.0f, "Motorsport Action boosts wheel blur omega");
}

void test_supersample_accumulator() {
  carview::PhotoAccumulator acc;
  acc.begin(4, 4, 4);
  check(acc.active() && !acc.complete(), "accumulator begins active");

  float jx0 = 0, jy0 = 0, jx1 = 0, jy1 = 0;
  acc.jitter_ndc(jx0, jy0);
  std::vector<uint8_t> f0(4 * 4 * 4, 255);
  std::vector<uint8_t> f1(4 * 4 * 4, 0);
  for (size_t i = 3; i < f1.size(); i += 4) f1[i] = 255;

  check(acc.add_frame(f0, 4, 4), "add frame 0");
  acc.jitter_ndc(jx1, jy1);
  check(std::fabs(jx0 - jx1) > 1e-4f || std::fabs(jy0 - jy1) > 1e-4f, "Halton jitter varies per sample");
  check(acc.add_frame(f1, 4, 4), "add frame 1");
  check(acc.add_frame(f0, 4, 4), "add frame 2");
  check(acc.add_frame(f1, 4, 4), "add frame 3");
  check(acc.complete(), "accumulator complete after 4 samples");

  std::vector<uint8_t> out;
  uint32_t ow = 0, oh = 0;
  check(acc.resolve(out, ow, oh), "accumulator resolves");
  check(ow == 4 && oh == 4 && out.size() == 64, "resolved dimensions match");
  // Linear average of 0.0 and 1.0 is 0.5 -> sRGB ~188.
  check(out[0] >= 180 && out[0] <= 195 && out[3] == 255, "linear-space average of black+white is ~188 sRGB");
}

void test_horizon_promo_and_save_roundtrip() {
  carview::HorizonPromoTracker promo;
  promo.load({101, 102, 103, 104, 105, 106, 107, 108, 109}, 240);
  check(promo.spotted_count() == 9, "promo loaded 9 cars");

  // Simple orthographic-like identity clip matrix where (0, 0, 5) maps to (0, 0, 0.5, 1).
  float m[16] = {
      0.1f, 0, 0, 0,
      0, 0.1f, 0, 0,
      0, 0, 0.05f, 0,
      0, 0, 0, 1.0f,
  };
  const std::array<double, 3> eye = {0.0, 1.0, 0.0};
  std::vector<carview::PromoCarCandidate> cands = {
      {1219, "2002 Nissan Skyline GT-R", {0.0, 0.3, 10.0}, 2.2, true},
      {999, "Out of frame car", {50.0, 0.3, 10.0}, 2.2, false},
      {101, "Already spotted car", {1.0, 0.3, 12.0}, 2.2, false},
  };
  const auto res = promo.evaluate(m, eye, cands, true);
  check(res.visible_car_ids.size() == 2, "2 cars visible in frustum");
  check(res.newly_spotted_ids.size() == 1 && res.newly_spotted_ids[0] == 1219, "1 new car spotted (1219)");
  check(res.total_spotted == 10, "total spotted reached 10");
  check(res.milestone_reached, "10-car milestone reached");
  check(res.credits_awarded == carview::HorizonPromoTracker::kCreditsPerNewCar +
                                   carview::HorizonPromoTracker::kMilestoneBonus,
        "awarded new-car + milestone bonus credits");

  // Round-trip through DriveSave.
  drive::DriveSave sv;
  sv.car = "NIS_SkylineGTRVSpecII_02";
  sv.promo_cars = promo.save_ids();
  const std::string text = drive::save_to_text(sv);
  drive::DriveSave loaded;
  std::string err;
  check(drive::save_from_text(text, loaded, &err), "DriveSave roundtrip succeeds");
  check(loaded.promo_cars.size() == 10, "DriveSave preserved 10 promo_cars");
}

void test_png_write_and_hud_build() {
  const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_photo_mode_test";
  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);

  std::vector<uint8_t> rgba(16 * 12 * 4);
  for (size_t i = 0; i < 16 * 12; ++i) {
    rgba[i * 4 + 0] = uint8_t((i * 13) & 255);
    rgba[i * 4 + 1] = uint8_t((i * 29) & 255);
    rgba[i * 4 + 2] = 200;
    rgba[i * 4 + 3] = 255;
  }
  std::filesystem::path saved;
  std::string err;
  check(carview::write_photo_png(tmp_dir, rgba, 16, 12, "f8test", saved, &err), "write_photo_png succeeds");
  check(std::filesystem::exists(saved), "PNG file exists on disk");

  int rw = 0, rh = 0, rc = 0;
  unsigned char* decoded = stbi_load(saved.string().c_str(), &rw, &rh, &rc, 4);
  check(decoded != nullptr && rw == 16 && rh == 12, "written PNG decodes back at 16x12");
  if (decoded) {
    check(decoded[0] == rgba[0] && decoded[1] == rgba[1] && decoded[2] == 200, "PNG pixel data lossless");
    stbi_image_free(decoded);
  }
  std::filesystem::remove_all(tmp_dir, ec);

  // Verify 516_I_EFFECTSMODE Photo Mode HUD anchors effects on right edge (x>=1000) with real slider/arrow textures.
  std::set<std::string> requested_tex;
  carview::ui4::set_texture_loader([&](const std::string& path, std::array<float, 4>& uv) {
    requested_tex.insert(path);
    uv = {0.0f, 0.0f, 0.1f, 0.1f};
    return true;
  });
  carview::PhotoModeController ctrl;
  ctrl.enter({0, 0, 0}, 0.0, carview::PhotoLimits::from_settings(false, true));
  carview::ui::List list;
  ctrl.build_hud(list, 1280.0f, 720.0f);
  carview::ui4::set_texture_loader({});
  check(!list.empty(), "PhotoModeController::build_hud emits UI shapes");
  check(requested_tex.count("HUD/PHOTOMODEFOCUSRETICLE") > 0, "Photo Mode loads PHOTOMODEFOCUSRETICLE");
  check(requested_tex.count("OPTIONS/OPT_SLIDER_FRAME") > 0, "Photo Mode loads OPT_SLIDER_FRAME");
  check(requested_tex.count("DIRTTEXTURES/STATBARFILL") > 0, "Photo Mode loads STATBARFILL");
  check(requested_tex.count("OPTIONS/OPT_BAR_ARROW") > 0, "Photo Mode loads OPT_BAR_ARROW");
  // Confirm right-edge effects column exists (x >= 1000) and left-middle drawer area is open.
  int right_items = 0;
  for (const auto& it : list.items()) {
    if (it.p[0] >= 1000.0f && it.p[1] >= 60.0f && it.p[1] <= 620.0f) ++right_items;
  }
  check(right_items >= 20, "516_I_EFFECTSMODE effects column is anchored on the right edge (x>=1000)");
}

}  // namespace

int main() {
  test_limits_and_settings();
  test_camera_clamping_and_autofocus();
  test_effects_and_presets();
  test_supersample_accumulator();
  test_horizon_promo_and_save_roundtrip();
  test_png_write_and_hud_build();
  if (g_fails == 0) {
    std::printf("photo_mode_tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "photo_mode_tests: %d failed\n", g_fails);
  return 1;
}
