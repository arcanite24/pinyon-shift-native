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

  // Phase 6B: Watermark stamping, Photo Gallery list/delete, wall collision raycast, and Car Club save persistence.
  {
    std::vector<uint8_t> img(640u * 360u * 4u, 32u);
    carview::stamp_photo_watermark(img, 640, 360);
    int changed_px = 0;
    for (size_t i = 0; i < img.size(); i += 4) {
      if (img[i] != 32u || img[i + 1] != 32u || img[i + 2] != 32u) ++changed_px;
    }
    check(changed_px > 200, "stamp_photo_watermark draws visible bottom-right watermark pixels");

    const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_photo_gallery_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
    std::filesystem::path saved_p;
    std::string werr;
    check(carview::write_photo_png(tmp_dir, img, 640, 360, "test", saved_p, &werr, true),
          "write_photo_png with watermark succeeds");
    ctrl.open_gallery(tmp_dir);
    check(ctrl.gallery_open() && ctrl.gallery_entries().size() == 1, "Photo Gallery lists saved PNG");
    carview::ui::List gal_list;
    ctrl.build_hud(gal_list, 1280.0f, 720.0f);
    check(!gal_list.empty(), "Photo Gallery HUD renders entries");
    check(ctrl.gallery_delete_selected() && ctrl.gallery_entries().empty(), "Photo Gallery deletes selected photo");
    ctrl.close_gallery();
    std::filesystem::remove_all(tmp_dir, ec);

    // Wall collision raycast stops camera before wall when through_walls == false.
    carview::PhotoCamera wall_cam;
    wall_cam.reset({0.0, 0.0, 0.0}, 0.0, carview::PhotoLimits::from_settings(false, false));
    carview::PhotoCameraInput pin{};
    wall_cam.step(0.016, pin, {}, [](const std::array<double, 3>&, const std::array<double, 3>&, double max_d,
                                     double& hit_d) {
      if (max_d > 2.0) {
        hit_d = 2.0;
        return true;
      }
      return false;
    });
    const double d_after = std::hypot(wall_cam.eye()[0], wall_cam.eye()[2]);
    check(d_after <= 2.05, "wall_raycast clamps PhotoCamera inside wall clearance");

    // Tracking vs Panning shutter blur boost differs cleanly.
    carview::PhotoEffects pe;
    pe.shutter_speed = 80.0f;
    pe.tracking_panning = false;
    const float pan_w = pe.wheel_blur_omega_boost();
    pe.tracking_panning = true;
    const float trk_w = pe.wheel_blur_omega_boost();
    check(trk_w > pan_w, "Tracking mode gives stronger wheel blur boost than Panning");

    // Car Club DriveSave round-trip.
    drive::DriveSave cs;
    cs.car = "NIS_SkylineGTRVSpecII_02";
    cs.car_club_name = "COLORADO APEX";
    cs.car_club_tag = "APEX";
    cs.car_club_shared = {1219, 1031};
    drive::DriveSave cs_out;
    std::string serr;
    check(drive::save_from_text(drive::save_to_text(cs), cs_out, &serr), "DriveSave with Car Club round-trips");
    check(cs_out.car_club_name == "COLORADO APEX" && cs_out.car_club_tag == "APEX" &&
              cs_out.car_club_shared.size() == 2 && cs_out.car_club_shared[0] == 1219,
          "Car Club name, tag, and shared car IDs survive save round-trip");
  }
}

void test_gallery_thumbnails_and_smooth_watermark() {
  // 1. High-resolution antialiased "FORZA HORIZON" watermark produces smooth edge luminance gradients.
  constexpr uint32_t W = 1280, H = 720;
  std::vector<uint8_t> canvas(size_t(W) * H * 4, 24u);
  for (size_t i = 3; i < canvas.size(); i += 4) canvas[i] = 255u;
  carview::stamp_photo_watermark(canvas, W, H);

  // Top-left corner must remain untouched.
  check(canvas[0] == 24u && canvas[1] == 24u && canvas[2] == 24u, "smooth watermark leaves top-left photo untouched");
  std::set<uint8_t> distinct_r;
  int changed = 0;
  for (uint32_t y = H / 2; y < H; ++y) {
    for (uint32_t x = W / 2; x < W; ++x) {
      const size_t idx = (size_t(y) * W + x) * 4;
      if (canvas[idx] != 24u || canvas[idx + 1] != 24u || canvas[idx + 2] != 24u) {
        ++changed;
        distinct_r.insert(canvas[idx]);
      }
    }
  }
  check(changed > 2000, "smooth 720p watermark draws crisp high-res bottom-right badge");
  check(distinct_r.size() >= 12,
        "antialiased watermark produces smooth intermediate luminance shades (not 2-level 5x5 bitmap)");

  // Custom retail/user watermark image override composites cleanly and resets cleanly.
  std::vector<uint8_t> custom_wm(16u * 8u * 4u, 0u);
  for (size_t i = 0; i < 16u * 8u; ++i) {
    custom_wm[i * 4 + 0] = 249u;
    custom_wm[i * 4 + 1] = 0u;
    custom_wm[i * 4 + 2] = 100u;
    custom_wm[i * 4 + 3] = 220u;
  }
  carview::set_photo_watermark_image(16u, 8u, custom_wm);
  std::vector<uint8_t> custom_canvas(640u * 360u * 4u, 40u);
  for (size_t i = 3; i < custom_canvas.size(); i += 4) custom_canvas[i] = 255u;
  carview::stamp_photo_watermark(custom_canvas, 640u, 360u);
  int pink_px = 0;
  for (size_t i = 0; i < custom_canvas.size(); i += 4) {
    if (custom_canvas[i] > 150u && custom_canvas[i + 1] < 40u) ++pink_px;
  }
  check(pink_px > 200, "custom watermark RGBA image composites cleanly");
  carview::set_photo_watermark_image(0, 0, {});

  // 2. Photo Gallery thumbnail decoding (.png + .bmp), preview texture upload, and UI kImage emission.
  const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_photo_thumbs_test";
  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
  std::filesystem::create_directories(tmp_dir, ec);

  // Create a 320x180 gradient PNG and a 160x120 BMP in the gallery folder.
  std::vector<uint8_t> img_png(320u * 180u * 4u, 255u);
  for (uint32_t y = 0; y < 180u; ++y) {
    for (uint32_t x = 0; x < 320u; ++x) {
      const size_t i = (size_t(y) * 320u + x) * 4u;
      img_png[i + 0] = uint8_t((x * 255u) / 319u);
      img_png[i + 1] = uint8_t((y * 255u) / 179u);
      img_png[i + 2] = 180u;
      img_png[i + 3] = 255u;
    }
  }
  std::filesystem::path saved_png;
  std::string werr;
  check(carview::write_photo_png(tmp_dir, img_png, 320u, 180u, "gallery_a", saved_png, &werr, true),
        "write_photo_png creates gallery PNG");

  const auto bmp_path = tmp_dir / "gallery_b.bmp";
  std::vector<uint8_t> img_bmp(160u * 120u * 4u, 200u);
  check(stbi_write_bmp(bmp_path.string().c_str(), 160, 120, 4, img_bmp.data()) != 0, "wrote gallery BMP");

  const auto listed = carview::list_photo_gallery(tmp_dir);
  check(listed.size() == 2, "list_photo_gallery discovers both .png and .bmp photos");

  std::vector<uint8_t> thumb_rgba;
  uint32_t tw = 0, th = 0, sw = 0, sh = 0;
  check(carview::load_photo_thumbnail(saved_png, 160u, 90u, thumb_rgba, tw, th, &sw, &sh),
        "load_photo_thumbnail decodes and downsamples PNG");
  check(sw == 320u && sh == 180u && tw == 160u && th == 90u && thumb_rgba.size() == 160u * 90u * 4u,
        "load_photo_thumbnail preserves 16:9 aspect ratio and original dimensions");

  carview::PhotoModeController ctrl;
  ctrl.enter({0, 0, 0}, 0.0, carview::PhotoLimits::from_settings(false, false));
  ctrl.open_gallery(tmp_dir);
  check(ctrl.gallery_open() && ctrl.gallery_entries().size() == 2, "Photo Gallery opens with 2 photos");
  check(ctrl.gallery_has_preview() && ctrl.gallery_preview_width() > 0 && ctrl.gallery_preview_height() > 0,
        "Photo Gallery loads decoded preview texture for selected photo");

  carview::ui::List gal_list;
  ctrl.build_hud(gal_list, 1280.0f, 720.0f);
  int image_quads = 0;
  for (const auto& it : gal_list.items()) {
    if (it.kind == carview::ui::Kind::kImage) ++image_quads;
  }
  // 2 thumbnail quads + 1 large right-pane preview quad = at least 3 kImage quads!
  check(image_quads >= 3, "Photo Gallery HUD emits kImage quads for decoded thumbnails and live preview");

  ctrl.gallery_move(+1);
  check(ctrl.gallery_selected() == 1 && ctrl.gallery_has_preview(),
        "Moving gallery selection updates decoded preview texture cleanly");
  ctrl.close_gallery();
  std::filesystem::remove_all(tmp_dir, ec);
}

}  // namespace

int main() {
  test_limits_and_settings();
  test_camera_clamping_and_autofocus();
  test_effects_and_presets();
  test_supersample_accumulator();
  test_horizon_promo_and_save_roundtrip();
  test_png_write_and_hud_build();
  test_gallery_thumbnails_and_smooth_watermark();
  if (g_fails == 0) {
    std::printf("photo_mode_tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "photo_mode_tests: %d failed\n", g_fails);
  return 1;
}
