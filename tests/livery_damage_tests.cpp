// Unit tests for Slice 7 Milestone 4: Paint Shop finishes (ShaderSettings XML,
// List_SpecialColors 102 two-tone paints, HSV & per-region painting), Livery.zip
// (1,372 vinyl/decal .carbin shapes) 1024^2 LiveryMasks compositor & .liv I/O,
// Cosmetic & Simulation Car Damage + dirt/mud, and SettingsPage Damage row.
// When SHIFT_CAPTURE_DIR is set, also renders the 4 headless Vulkan 1280x720
// verification screenshots for Slice 7.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "autoshow.h"
#include "cars/livery.h"
#include "cars/physics.h"
#include "cars/shader_settings.h"
#include "cars/sources.h"
#include "cars/visual_setup.h"
#include "drive/damage.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "hub_scene.h"
#include "livery_editor.h"
#include "renderer.h"
#include "settings_page.h"
#include "showroom.h"
#include "ui.h"
#include "upgrade_shop.h"

namespace {

int g_failed = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++g_failed;                                                                        \
    }                                                                                    \
  } while (0)

namespace fs = std::filesystem;

fs::path find_game_root() {
  for (const char* p : {R"(../horizon1-recomp/.local\game\base)",
                        R"(.local\game\base)"}) {
    if (fs::exists(fs::path(p) / "media" / "db" / "gamedb.slt")) return p;
  }
  return {};
}

fs::path find_bake_dir() {
  for (const char* p : {R"(out\bake)"}) {
    if (fs::exists(fs::path(p) / "shared.shiftbake")) return p;
  }
  return {};
}

void test_shader_settings_and_hsv() {
  const std::string sample_xml = R"(
    <ShaderSettings>
      <body>
        <SpecularCubeScalar value="1.000000"/>
        <PaintScale value="0.500000"/>
        <SpecularAmount value="1.250000"/>
        <SpecularPower value="48.000000"/>
        <SpecularAmount2 value="7.000000"/>
        <SpecularPower2 value="6.000000"/>
        <SpecularAmount3 value="6.000000"/>
        <SpecularPower3 value="4.000000"/>
        <FresnelIndex value="1.350000"/>
        <FresnelScalar value="0.450000"/>
        <MetalColor r="0.200000" g="0.400000" b="0.800000" a="1.000000"/>
        <MetalPower value="2.500000"/>
        <MetalBias value="0.150000"/>
        <MetalScale value="0.350000"/>
      </body>
      <gauge_emissive>
        <GaugeEmissiveScale value="3.500000"/>
      </gauge_emissive>
    </ShaderSettings>)";
  cars::ShaderFinishParams fp;
  CHECK(cars::parse_shader_settings_xml(sample_xml, fp));
  CHECK(std::fabs(fp.paint_scale - 0.5f) < 1e-4f);
  CHECK(std::fabs(fp.spec_amount - 1.25f) < 1e-4f);
  CHECK(std::fabs(fp.spec_power - 48.0f) < 1e-4f);
  CHECK(std::fabs(fp.spec_amount2 - 7.0f) < 1e-4f);
  CHECK(std::fabs(fp.spec_power2 - 6.0f) < 1e-4f);
  CHECK(std::fabs(fp.spec_amount3 - 6.0f) < 1e-4f);
  CHECK(std::fabs(fp.spec_power3 - 4.0f) < 1e-4f);
  CHECK(std::fabs(fp.fresnel_index - 1.35f) < 1e-4f);
  CHECK(std::fabs(fp.fresnel_scalar - 0.45f) < 1e-4f);
  CHECK(std::fabs(fp.metal_color[0] - 0.2f) < 1e-4f && std::fabs(fp.metal_color[1] - 0.4f) < 1e-4f &&
        std::fabs(fp.metal_color[2] - 0.8f) < 1e-4f);
  CHECK(std::fabs(fp.metal_power - 2.5f) < 1e-4f);
  CHECK(std::fabs(fp.metal_bias - 0.15f) < 1e-4f);
  CHECK(std::fabs(fp.metal_scale - 0.35f) < 1e-4f);
  CHECK(std::fabs(fp.gauge_emissive_scale - 3.5f) < 1e-4f);

  // Absent tags preserve existing defaults on overlay.
  cars::ShaderFinishParams partial = fp;
  CHECK(cars::parse_shader_settings_xml("<body><PaintScale value=\"0.9\"/></body>", partial));
  CHECK(std::fabs(partial.paint_scale - 0.9f) < 1e-4f);
  CHECK(std::fabs(partial.fresnel_index - 1.35f) < 1e-4f);
  CHECK(std::fabs(partial.metal_scale - 0.35f) < 1e-4f);

  // RimShaderSettings XML parsing.
  cars::RimShaderParams rim_p;
  CHECK(cars::parse_rim_shader_settings_xml(
      R"(<ShaderSettings><rim_V2><FresnelIndex value="1.75"/><FresnelScalar value="0.60"/><CubeScale value="2.0"/><TintColor r="0.4" g="0.5" b="0.6" a="1.0"/></rim_V2></ShaderSettings>)",
      rim_p));
  CHECK(rim_p.has_override);
  CHECK(std::fabs(rim_p.fresnel_index - 1.75f) < 1e-4f && std::fabs(rim_p.fresnel_scalar - 0.60f) < 1e-4f);
  CHECK(std::fabs(rim_p.cube_scale - 2.0f) < 1e-4f && std::fabs(rim_p.tint_color[2] - 0.6f) < 1e-4f);

  // HSV round-trip.
  for (uint32_t col : {0xFF0000u, 0x00FF00u, 0x0044FFu, 0xE89420u}) {
    float h = 0, s = 0, v = 0;
    cars::rgb_to_hsv(col, h, s, v);
    const uint32_t rt = cars::hsv_to_rgb(h, s, v);
    const int dr = std::abs(int((col >> 16) & 0xFF) - int((rt >> 16) & 0xFF));
    const int dg = std::abs(int((col >> 8) & 0xFF) - int((rt >> 8) & 0xFF));
    const int db = std::abs(int(col & 0xFF) - int(rt & 0xFF));
    CHECK(dr <= 2 && dg <= 2 && db <= 2);
  }
}

void test_damage_and_settings() {
  // SettingsPage Damage row (cosmetic / none / simulation).
  carview::SettingsPage sp;
  sp.load({});
  CHECK(sp.value("damage") == "cosmetic");
  CHECK(sp.live_settings().damage_mode == 1);
  sp.load({"--damage", "simulation"});
  CHECK(sp.value("damage") == "simulation");
  CHECK(sp.live_settings().damage_mode == 2);
  sp.load({"--damage", "none"});
  CHECK(sp.value("damage") == "none");
  CHECK(sp.live_settings().damage_mode == 0);

  // None mode ignores impacts & mechanical wear, but STILL accumulates environmental dirt/mud.
  drive::CarDamageState d_none;
  d_none.mode = drive::DamageMode::kNone;
  d_none.record_impact(0.0f, 0.5f, 1.8f, 25.0f);
  d_none.step(5.0, 20.0, true, 1.0, 0.8, {0.2, 0.2, 0.2, 0.2});
  CHECK(d_none.zone_front == 0.0f && d_none.corner_fl == 0.0f && !d_none.front_bumper_detached);
  CHECK(d_none.body_dirt > 0.1f && d_none.tyre_dirt > 0.3f);
  CHECK(d_none.tyre_wear[0] == 0.0f && d_none.fuel_fraction == 1.0f);

  // 4-corner independent lamp breakage:
  // (a) Off-center front-left hit breaks ONLY FL headlight (FR stays intact).
  {
    drive::CarDamageState d_fl;
    d_fl.mode = drive::DamageMode::kCosmetic;
    d_fl.record_impact(-0.65f, 0.5f, 1.85f, 16.0f);
    CHECK(d_fl.headlight_fl_broken());
    CHECK(!d_fl.headlight_fr_broken());
    CHECK(d_fl.corner_fr == 0.0f);
    CHECK(!d_fl.taillight_rl_broken() && !d_fl.taillight_rr_broken());
  }
  // (b) Moderate front-center hit breaks neither; heavy center hit breaks both headlights.
  {
    drive::CarDamageState d_c;
    d_c.mode = drive::DamageMode::kCosmetic;
    d_c.record_impact(0.0f, 0.5f, 1.85f, 16.0f);
    CHECK(!d_c.headlight_fl_broken() && !d_c.headlight_fr_broken());
    CHECK(std::fabs(d_c.corner_fl - d_c.corner_fr) < 1e-4f);
    d_c.record_impact(0.0f, 0.5f, 1.85f, 18.0f);
    CHECK(d_c.headlight_fl_broken() && d_c.headlight_fr_broken());
  }
  // (c) Off-center rear-right hit breaks ONLY RR taillight.
  {
    drive::CarDamageState d_rr;
    d_rr.mode = drive::DamageMode::kCosmetic;
    d_rr.record_impact(0.60f, 0.5f, -1.90f, 16.5f);
    CHECK(d_rr.taillight_rr_broken() && !d_rr.taillight_rl_broken());
  }
  // (d) Deterministic part detachment, hood_ajar, and clean reset().
  {
    drive::CarDamageState d_det;
    d_det.mode = drive::DamageMode::kCosmetic;
    d_det.record_impact(0.0f, 0.5f, 1.9f, 24.0f);
    CHECK(d_det.hood_ajar > 0.2f);
    d_det.record_impact(0.0f, 0.5f, 1.9f, 15.0f);
    CHECK(d_det.front_bumper_detached && d_det.hood_ajar > 0.8f);
    d_det.record_impact(0.0f, 0.5f, -1.9f, 28.0f);
    d_det.record_impact(0.0f, 0.5f, -1.9f, 10.0f);
    CHECK(d_det.rear_bumper_detached && d_det.wing_detached);
    d_det.record_impact(-0.9f, 0.5f, 0.0f, 22.0f);
    CHECK(d_det.left_mirror_detached && !d_det.right_mirror_detached);
    d_det.record_impact(0.9f, 0.5f, 0.0f, 22.0f);
    CHECK(d_det.right_mirror_detached);
    d_det.reset();
    CHECK(d_det.mode == drive::DamageMode::kCosmetic);
    CHECK(!d_det.front_bumper_detached && !d_det.rear_bumper_detached && !d_det.wing_detached &&
          !d_det.left_mirror_detached && !d_det.right_mirror_detached && d_det.hood_ajar == 0.0f &&
          d_det.corner_fl == 0.0f && d_det.corner_fr == 0.0f && d_det.corner_rl == 0.0f && d_det.corner_rr == 0.0f);
  }

  // Cosmetic mode dents zones, cracks glass, accumulates off-road mud, leaves physics untouched.
  drive::CarDamageState d_cos;
  d_cos.mode = drive::DamageMode::kCosmetic;
  d_cos.record_impact(0.0f, 0.5f, 1.8f, 22.0f);
  d_cos.record_impact(-0.85f, 0.4f, 0.1f, 14.0f);
  CHECK(d_cos.zone_front > 0.5f);
  CHECK(d_cos.zone_left > 0.3f);
  CHECK(d_cos.glass_crack > 0.4f);
  CHECK(d_cos.scratch_amount > 0.4f);
  d_cos.step(6.0, 22.0, true, 0.8, 0.7, {0.1, 0.1, 0.1, 0.1});
  CHECK(d_cos.body_dirt > 0.2f && d_cos.tyre_dirt > 0.5f);
  const float dirt_after_mud = d_cos.tyre_dirt;
  d_cos.step(6.0, 28.0, false, 0.5, 0.5, {0.0, 0.0, 0.0, 0.0});
  CHECK(d_cos.tyre_dirt < dirt_after_mud);  // tarmac sheds tyre mud
  CHECK(d_cos.multipliers().engine_torque == 1.0);

  // Simulation mode degrades engine, aero, brakes, suspension, tyre wear, and fuel.
  drive::CarDamageState d_sim;
  d_sim.mode = drive::DamageMode::kSimulation;
  d_sim.record_impact(0.5f, 0.5f, 1.9f, 24.0f);
  CHECK(d_sim.engine_damage > 0.4f && d_sim.aero_damage > 0.4f);
  CHECK(d_sim.suspension_damage[1] > 0.3f);
  d_sim.step(600.0, 35.0, false, 1.0, 0.9, {0.4, 0.4, 0.6, 0.6});
  CHECK(d_sim.tyre_wear[2] > 0.2f);
  CHECK(d_sim.fuel_fraction < 0.7f);
  const auto mul = d_sim.multipliers();
  CHECK(mul.engine_torque < 0.85);
  CHECK(mul.aero_drag > 1.10 && mul.aero_downforce < 0.85);
  CHECK(mul.tyre_grip[2] < 0.92);

  drive::CarParams cp;
  const double base_tq = cp.torque_scale;
  d_sim.apply_to_params(cp);
  CHECK(cp.torque_scale < base_tq);
}

void test_disc_livery_and_paint() {
  const fs::path game_root = find_game_root();
  if (game_root.empty()) {
    std::printf("  (skipping disc livery/paint checks: base game_root not found)\n");
    return;
  }
  std::string err;
  cars::Install install;
  CHECK(cars::discover_install(game_root, {}, {}, install, &err));
  cars::PartsDb db;
  CHECK(db.open(install, &err));

  // 1. ShaderSettings XML from Shared.zip + per-car overrides (ColorShaderSettings, Rim, Brake).
  cars::ShaderSettingsSet ss;
  CHECK(cars::load_shader_settings(install.media / "cars" / "Shared.zip", ss, &err));
  CHECK(ss.loaded_from_xml);
  CHECK(std::fabs(ss.normal.fresnel_index - 1.55f) < 1e-3f);
  CHECK(std::fabs(ss.normal.fresnel_scalar - 0.90f) < 1e-3f);
  CHECK(std::fabs(ss.gauge_emissive_scale - 3.0f) < 1e-3f);
  CHECK(std::fabs(ss.metallic.paint_scale - 0.5f) < 1e-4f);
  CHECK(std::fabs(ss.metallic.fresnel_index - 1.55f) < 1e-3f);  // inherited from Normal.xml
  CHECK(std::fabs(ss.matte.fresnel_scalar - 0.0f) < 1e-4f);
  CHECK(std::fabs(ss.matte.metal_scale - 0.06f) < 1e-4f);

  // Per-car ColorShaderSettings1..N.xml, RimShaderSettings.xml, BrakeShaderSettings.xml.
  {
    const fs::path r8gt = install.media / "cars" / "AUD_R8GT_11.zip";
    if (fs::exists(r8gt)) {
      cars::CarShaderOverrides ov;
      CHECK(cars::load_car_shader_overrides(r8gt, ov, &ss.normal, &err));
      CHECK(ov.color_overrides.count(1) == 1);
      CHECK(std::fabs(ov.color_overrides.at(1).fresnel_scalar - 0.20f) < 1e-3f);
      CHECK(std::fabs(ov.color_overrides.at(1).metal_scale - 0.10f) < 1e-3f);
    }
    const fs::path m6 = install.media / "cars" / "BMW_M6_13.zip";
    if (fs::exists(m6)) {
      cars::CarShaderOverrides ov;
      CHECK(cars::load_car_shader_overrides(m6, ov, &ss.normal, &err));
      CHECK(ov.rim.has_override);
      CHECK(std::fabs(ov.rim.fresnel_index - 1.75f) < 1e-3f);
      CHECK(std::fabs(ov.rim.cube_scale - 2.0f) < 1e-3f);
    }
    const fs::path evo6 = install.media / "cars" / "MIT_LancerEVOVIGSR_99.zip";
    if (fs::exists(evo6)) {
      cars::CarShaderOverrides ov;
      CHECK(cars::load_car_shader_overrides(evo6, ov, &ss.normal, &err));
      CHECK(ov.brake.has_override);
    }
  }

  // 2. List_SpecialColors (102 two-tone paints).
  std::vector<cars::SpecialColorRow> specials;
  CHECK(cars::query_special_colors(db, specials, &err));
  std::printf("  List_SpecialColors rows: %zu\n", specials.size());
  CHECK(specials.size() == 102);

  // 3. Livery.zip (1,372 vinyl & decal .carbin shapes — test both kind-3 and kind-1!).
  cars::LiveryLibrary lib;
  CHECK(lib.open(install.media / "Livery.zip", &err));
  std::printf("  Livery.zip shapes: %zu (%zu vinyls + %zu decals)\n", lib.total_shapes(), lib.vinyl_names().size(),
              lib.decal_names().size());
  CHECK(lib.total_shapes() == 1372);
  CHECK(lib.vinyl_names().size() == 880);
  CHECK(lib.decal_names().size() == 492);

  const auto* v_a01 = lib.load_shape("vinyls/a_01.carbin", &err);  // kind 3
  CHECK(v_a01 != nullptr && v_a01->kind == 3 && !v_a01->indices.empty());
  const auto* v_c01 = lib.load_shape("vinyls/c_01.carbin", &err);  // kind 1
  CHECK(v_c01 != nullptr && v_c01->kind == 1 && !v_c01->indices.empty());
  const auto* d_001 = lib.load_shape("decals/decal001.carbin", &err);
  CHECK(d_001 != nullptr && !d_001->indices.empty());

  // Verify broad parse success over a sample of 40 shapes (20 vinyls + 20 decals).
  int parsed_ok = 0;
  for (size_t i = 0; i < 20 && i < lib.vinyl_names().size(); ++i)
    if (lib.load_shape(lib.vinyl_names()[i * 40 % lib.vinyl_names().size()], nullptr)) ++parsed_ok;
  for (size_t i = 0; i < 20 && i < lib.decal_names().size(); ++i)
    if (lib.load_shape(lib.decal_names()[i * 20 % lib.decal_names().size()], nullptr)) ++parsed_ok;
  std::printf("  Livery.zip parse sample: %d/40\n", parsed_ok);
  CHECK(parsed_ok >= 36);

  // 4. 1024x1024 LiveryMasks compositor + .liv save/load round-trip + LiveryEditor UI.
  carview::LiveryEditor editor;
  std::string msg;
  CHECK(editor.open(&db, game_root, 445, {}, msg));
  CHECK(editor.special_colors().size() == 102);
  editor.move_special_color(2);
  editor.apply_special_color();
  editor.add_vinyl_layer("vinyls/a_01.carbin", 0xFFD428u);
  editor.transform_layer(-0.1f, 0.0f, 0.3f, 0.08f, 15.0f);
  editor.add_vinyl_layer("vinyls/c_01.carbin", 0xFFFFFFu);
  editor.add_vinyl_layer("decals/decal001.carbin", 0xFFFFFFu);

  std::vector<uint8_t> rgba;
  CHECK(editor.composite_1024(nullptr, rgba, 1024));
  CHECK(rgba.size() == 1024u * 1024u * 4u);
  size_t non_empty = 0;
  for (size_t i = 3; i < rgba.size(); i += 4)
    if (rgba[i] > 16) ++non_empty;
  std::printf("  1024x1024 livery composited opaque pixels: %zu\n", non_empty);
  CHECK(non_empty > 20000);

  // .liv round-trip persistence.
  const fs::path tmp_liv = fs::temp_directory_path() / "shift_test_livery.liv";
  CHECK(editor.save(tmp_liv, &err));
  cars::LiveryDocument loaded;
  CHECK(cars::load_livery_file(tmp_liv, loaded, &err));
  CHECK(loaded.car_id == 445);
  CHECK(loaded.layers.size() == 3);
  CHECK(loaded.paint.body.finish == cars::PaintFinish::kTwoTone);
  std::error_code ec;
  fs::remove(tmp_liv, ec);

  // Verify chrome rim paint protection.
  editor.set_wheel_is_chrome(true);
  editor.move_region(int(carview::PaintRegion::kRims));
  editor.step_hsv(0.3f, 0.0f, 0.0f);
  CHECK(!editor.document().paint.rims.active);

  // Rasterize Paint Shop / Vinyl Editor HUD.
  carview::ui::List hud;
  carview::draw_livery_editor_hud(hud, 1280.0f, 720.0f, editor, "2002 Nissan Skyline GT-R");
  CHECK(!hud.empty());
}

void maybe_capture_headless_screenshots() {
  const char* cap_env = std::getenv("SHIFT_CAPTURE_DIR");
  if (!cap_env || !*cap_env) return;
  const fs::path out_dir(cap_env);
  const fs::path game_root = find_game_root();
  const fs::path bake_dir = find_bake_dir();
  if (game_root.empty() || bake_dir.empty()) {
    std::fprintf(stderr, "capture skipped: game_root or bake_dir missing\n");
    return;
  }
  fs::create_directories(out_dir);

  std::string err;
  cars::Install install;
  CHECK(cars::discover_install(game_root, {}, {}, install, &err));
  cars::PartsDb db;
  CHECK(db.open(install, &err));
  cars::PhysicsSettings phys_settings;
  CHECK(cars::load_physics_settings(install.media / "physics.zip", phys_settings, &err));
  garage::PiConfig pi_cfg;
  CHECK(garage::load_pi_config(install.media / "physics.zip", pi_cfg, &err));
  std::vector<garage::ClassBand> bands;
  CHECK(garage::load_class_bands(game_root, bands, &err));
  const fs::path xpr_path = game_root / "media" / "tracks" / "uiautoshow" / "staticcarcubemap.xpr";

  constexpr uint32_t kW = 1280, kH = 720;

  auto render_shot = [&](const std::string& filename, const std::string& car_media, carview::HubRoom room,
                         const carview::ShowroomOptions& opts, const carview::HubCamera& cam,
                         const std::function<void(carview::Showroom&)>& after_upload,
                         const carview::ui::List& overlay) {
    carview::Renderer renderer;
    carview::Showroom showroom(renderer);
    if (fs::exists(xpr_path)) showroom.set_environment_xpr(xpr_path);
    CHECK(showroom.load(bake_dir, car_media, opts, err));
    carview::Renderer::Options ro;
    ro.window = nullptr;
    ro.msaa = 4;
    CHECK(renderer.init(ro, err));
    CHECK(showroom.upload(err));

    const auto bg = carview::hub_background_color(room);
    showroom.set_background(bg[0], bg[1], bg[2]);
    const float floor_y = showroom.bounds_min()[1];
    const float hw = 0.5f * (showroom.bounds_max()[0] - showroom.bounds_min()[0]);
    const float hl = 0.5f * (showroom.bounds_max()[2] - showroom.bounds_min()[2]);
    CHECK(showroom.set_world(carview::build_hub_stage(room, hw, hl, floor_y), err));
    if (after_upload) after_upload(showroom);

    carview::ShowroomFrame frame;
    carview::build_hub_frame(room, cam, kW, kH, frame, false);
    showroom.set_frame(frame);

    std::vector<uint8_t> rgba;
    CHECK(renderer.render_offscreen(carview::FrameConstants{}, kW, kH, rgba, err));
    if (!overlay.empty()) carview::ui::rasterize(overlay.items(), rgba, kW, kH);

    const fs::path dst = out_dir / filename;
    CHECK(stbi_write_png(dst.string().c_str(), int(kW), int(kH), 4, rgba.data(), int(kW * 4)) != 0);
    std::printf("  captured %s\n", dst.string().c_str());
  };

  // (a) 3D Autoshow turntable + make carousel + factory colour picker (Skyline GT-R R34).
  {
    carview::Autoshow autoshow;
    CHECK(autoshow.open(db, {}, err, 250000, false));
    // Jump to Nissan in Make Carousel and select Skyline GT-R (445).
    for (size_t i = 0; i < autoshow.makes().size(); ++i) {
      if (autoshow.makes()[i].name == "Nissan") {
        autoshow.select_make(i, 250000);
        break;
      }
    }
    for (size_t i = 0; i < autoshow.visible_cars().size(); ++i) {
      if (autoshow.visible_cars()[i].dealer.id == 445) break;
      autoshow.move(1);
    }
    autoshow.move_colour(1);

    carview::ShowroomOptions opts;
    opts.car_id = 445;
    opts.match_sequence = autoshow.selected_colour_sequence();
    opts.game_root = game_root;
    carview::HubCamera cam;
    cam.yaw = 0.68f;
    cam.pitch = 0.18f;
    cam.distance = 4.65f;
    cam.target[0] = -0.35f;
    cam.target[1] = 0.55f;
    cam.target[2] = 0.0f;

    carview::ui::List ui_list;
    carview::draw_autoshow_hud(ui_list, float(kW), float(kH), autoshow, bands, 250000);
    render_shot("slice7_a_autoshow.png", "nis_skylinegtrvspecii_02", carview::HubRoom::kAutoshow, opts, cam, {}, ui_list);
  }

  // (b) Upgraded Corrado (309) with aftermarket ATS_Classic rims, Race Rear Wing,
  //     Race Front Bumper, Race Roll Cage, lowered suspension, 13" carbon brakes + PI/stat diff HUD.
  {
    garage::Garage g;
    std::map<std::string, int64_t> stock;
    CHECK(db.stock_slots(309, stock, &err));
    int64_t credits = 500000, gid = 0;
    CHECK(garage::buy_car(g, 309, 10000, 0.524381, 2, 1, stock, credits, &gid));
    auto* c = g.find(gid);
    c->slots["RearWing"] = 309002;           // WingRace
    c->slots["FrontBumper"] = 309005;        // BumperFRace
    c->slots["ChassisStiffness"] = 309003;   // CageRace
    c->slots["SpringDamper"] = 309003;       // Race lowered springs
    c->slots["Brakes"] = 309003;             // 13" carbon 6-piston brakes
    c->slots["TireWidthFront"] = 309002;
    c->slots["TireWidthRear"] = 309002;
    c->slots["RimSizeFront"] = 309001;
    c->slots["RimSizeRear"] = 309001;
    c->slots["WheelStyle"] = 640;            // ATS_Classic aftermarket rims
    garage::refresh_car_pi(*c, db, phys_settings, pi_cfg, bands);

    carview::UpgradeShop shop;
    CHECK(shop.open(db, g, gid, err, &phys_settings, &pi_cfg, &bands));
    shop.set_category(carview::ShopCategory::kEngine);
    shop.move_part(2);  // preview engine upgrade for live green PI/5-stat diff bars!

    carview::ShowroomOptions opts;
    opts.car_id = 309;
    opts.game_root = game_root;
    CHECK(cars::resolve_visual_setup(db, 309, garage::fitted(*c), opts.visual, &err, 0.15f, 0.15f, -2.5f, -1.5f));

    carview::HubCamera cam;
    cam.yaw = 2.18f;   // rear-3/4 angle showing Race Wing, ATS_Classic rims, lowered stance & carbon brakes!
    cam.pitch = 0.21f;
    cam.distance = 4.95f;
    cam.target[0] = -0.10f;
    cam.target[1] = 0.52f;
    cam.target[2] = 0.0f;

    carview::ui::List ui_list;
    carview::draw_upgrade_shop_hud(ui_list, float(kW), float(kH), shop, bands, 25, credits, "1995 VW Corrado VR6");
    render_shot("slice7_b_upgrades.png", "vw_corrado_95", carview::HubRoom::kWorkshop, opts, cam, {}, ui_list);
  }

  // (c) Two-tone metallic pearlescent paint + custom Livery.zip 1024^2 vinyl layers on Skyline GT-R.
  {
    carview::LiveryEditor editor;
    std::string msg;
    CHECK(editor.open(&db, game_root, 445, {}, msg));
    auto& body = editor.document().paint.body;
    body.active = true;
    body.finish = cars::PaintFinish::kTwoTone;
    body.rgb = 0x1642B8u;       // deep cobalt blue
    body.sec_rgb = 0x18E8C8u;   // electric turquoise pearlescent highlight
    body.two_tone_scale = 0.45f;
    body.two_tone_bias = 0.05f;
    body.two_tone_power = 6.0f;
    editor.document().paint.calipers.active = true;
    editor.document().paint.calipers.rgb = 0xFFD018u;
    editor.document().paint.rims.active = true;
    editor.document().paint.rims.rgb = 0xD8A430u;  // gold rims
    editor.document().paint.window_tint = 0.65f;

    // Bold racing stripes + real Livery.zip vinyls & manufacturer decals across the 1024^2 livery atlas!
    editor.add_vinyl_layer("@stripe", 0xFFFFFFu);
    editor.document().layers.back().x = 0.50f;
    editor.document().layers.back().y = 0.50f;
    editor.document().layers.back().scale_x = 0.95f;
    editor.document().layers.back().scale_y = 0.14f;
    editor.document().layers.back().rot_deg = -12.0f;

    editor.add_vinyl_layer("@stripe", 0xFF6A10u);
    editor.document().layers.back().x = 0.50f;
    editor.document().layers.back().y = 0.59f;
    editor.document().layers.back().scale_x = 0.95f;
    editor.document().layers.back().scale_y = 0.05f;
    editor.document().layers.back().rot_deg = -12.0f;

    editor.add_vinyl_layer("vinyls/a_01.carbin", 0xFFD820u);
    editor.document().layers.back().x = 0.30f;
    editor.document().layers.back().y = 0.32f;
    editor.document().layers.back().scale_x = 0.24f;
    editor.document().layers.back().scale_y = 0.24f;
    editor.document().layers.back().rot_deg = 22.0f;

    editor.add_vinyl_layer("decals/decal001.carbin", 0xFFFFFFu);
    editor.document().layers.back().x = 0.68f;
    editor.document().layers.back().y = 0.48f;
    editor.document().layers.back().scale_x = 0.34f;
    editor.document().layers.back().scale_y = 0.18f;

    editor.add_vinyl_layer("@star", 0xFF3830u);
    editor.document().layers.back().x = 0.72f;
    editor.document().layers.back().y = 0.72f;
    editor.document().layers.back().scale_x = 0.20f;
    editor.document().layers.back().scale_y = 0.20f;

    editor.set_tab(carview::EditorTab::kVinylLayers);

    std::vector<uint8_t> liv_rgba;
    CHECK(editor.composite_1024(nullptr, liv_rgba, 1024));

    carview::ShowroomOptions opts;
    opts.car_id = 445;
    opts.game_root = game_root;
    carview::HubCamera cam;
    cam.yaw = 0.85f;
    cam.pitch = 0.26f;
    cam.distance = 4.45f;
    cam.target[0] = 0.35f;
    cam.target[1] = 0.55f;
    cam.target[2] = 0.0f;

    carview::ui::List ui_list;
    carview::draw_livery_editor_hud(ui_list, float(kW), float(kH), editor, "2002 Nissan Skyline GT-R");
    render_shot("slice7_c_paint_vinyls.png", "nis_skylinegtrvspecii_02", carview::HubRoom::kPaintShop, opts, cam,
                [&](carview::Showroom& sr) {
                  sr.set_paint_spec(editor.document().paint);
                  std::string lerr;
                  CHECK(sr.set_custom_livery(liv_rgba.data(), 1024, 1024, lerr));
                },
                ui_list);
  }

  // (d) Cosmetic damage (front/left impact crumple, scratches, cracked windshield) + off-road mud/dirt.
  {
    drive::CarDamageState dmg;
    dmg.mode = drive::DamageMode::kCosmetic;
    dmg.record_impact(-0.25f, 0.55f, 1.95f, 26.0f);  // heavy front impact -> dent + cracked glass
    dmg.record_impact(-0.85f, 0.45f, 0.20f, 18.0f);  // left side scrape
    dmg.step(8.5, 24.0, true, 0.9, 0.8, {0.2, 0.2, 0.2, 0.2});  // mud & dirt accumulation

    carview::ShowroomOptions opts;
    opts.car_id = 309;
    opts.game_root = game_root;
    carview::HubCamera cam;
    cam.yaw = -0.58f;  // front-left 3/4 view showing front crumple, cracked windshield, left scuffs & mud!
    cam.pitch = 0.18f;
    cam.distance = 4.10f;
    cam.target[0] = 0.0f;
    cam.target[1] = 0.50f;
    cam.target[2] = -0.15f;

    carview::ui::List ui_list;
    // Compact diagnostic telemetry banner showing active cosmetic damage & dirt values.
    ui_list.rect(24.0f, 24.0f, 640.0f, 128.0f, carview::ui::rgba(14, 16, 22, 224), 8.0f);
    ui_list.frame(24.0f, 24.0f, 640.0f, 128.0f, carview::ui::rgba(236, 98, 42, 220), 1.5f, 8.0f);
    carview::ui::TextStyle ts;
    ts.size = 15.0f;
    ts.color = carview::ui::rgba(255, 255, 255);
    ts.font = carview::ui::Font::kE;
    ui_list.text(40.0f, 36.0f, "DAMAGE & DIRT  //  COSMETIC MODE", ts);
    ts.size = 11.5f;
    ts.font = carview::ui::Font::kB;
    ts.color = carview::ui::rgba(225, 232, 242);
    char b1[96], b2[96];
    std::snprintf(b1, sizeof b1, "FRONT DENT: %.0f%%   LEFT SCRAPE: %.0f%%   GLASS CRACK: %.0f%%",
                  dmg.zone_front * 100.0f, dmg.zone_left * 100.0f, dmg.glass_crack * 100.0f);
    std::snprintf(b2, sizeof b2, "BODY DIRT/MUD: %.0f%%   TYRE MUD: %.0f%%", dmg.body_dirt * 100.0f,
                  dmg.tyre_dirt * 100.0f);
    ui_list.text(40.0f, 68.0f, b1, ts);
    ui_list.text(40.0f, 92.0f, b2, ts);

    render_shot("slice7_d_damage_dirt.png", "vw_corrado_95", carview::HubRoom::kBarnFind, opts, cam,
                [&](carview::Showroom& sr) { sr.set_damage_state(dmg); }, ui_list);
  }
}

void maybe_capture_gfx_parity_screenshots() {
  const char* cap = std::getenv("SHIFT_CAPTURE_GFX_PARITY_DIR");
  if (!cap || !*cap) return;
  const fs::path out_dir(cap);
  std::error_code ec;
  fs::create_directories(out_dir, ec);

  const fs::path game_root = find_game_root();
  const fs::path bake_dir = find_bake_dir();
  if (game_root.empty() || bake_dir.empty()) return;
  const fs::path xpr_path = game_root / "media" / "tracks" / "uiautoshow" / "staticcarcubemap.xpr";
  constexpr uint32_t kW = 1280, kH = 720;
  std::string err;

  auto render_studio = [&](const std::string& filename, const std::string& car_media,
                           const carview::ShowroomOptions& opts, const carview::HubCamera& cam,
                           const std::function<void(carview::Showroom&)>& after_upload) {
    carview::Renderer renderer;
    carview::Showroom showroom(renderer);
    if (fs::exists(xpr_path)) showroom.set_environment_xpr(xpr_path);
    CHECK(showroom.load(bake_dir, car_media, opts, err));
    carview::Renderer::Options ro;
    ro.window = nullptr;
    ro.msaa = 4;
    CHECK(renderer.init(ro, err));
    CHECK(showroom.upload(err));

    const auto bg = carview::hub_background_color(carview::HubRoom::kAutoshow);
    showroom.set_background(bg[0], bg[1], bg[2]);
    const float floor_y = showroom.bounds_min()[1];
    const float hw = 0.5f * (showroom.bounds_max()[0] - showroom.bounds_min()[0]);
    const float hl = 0.5f * (showroom.bounds_max()[2] - showroom.bounds_min()[2]);
    CHECK(showroom.set_world(carview::build_hub_stage(carview::HubRoom::kAutoshow, hw, hl, floor_y), err));
    if (after_upload) after_upload(showroom);

    carview::ShowroomFrame frame;
    carview::build_hub_frame(carview::HubRoom::kAutoshow, cam, kW, kH, frame, false);
    showroom.set_frame(frame);

    std::vector<uint8_t> rgba;
    CHECK(renderer.render_offscreen(carview::FrameConstants{}, kW, kH, rgba, err));
    const fs::path dst = out_dir / filename;
    CHECK(stbi_write_png(dst.string().c_str(), int(kW), int(kH), 4, rgba.data(), int(kW * 4)) != 0);
    std::printf("  captured %s\n", dst.string().c_str());
  };

  // 1. Autoshow Ferrari FXX (front 3/4 view, staticcarcubemap.xpr HDR studio light strips).
  {
    carview::ShowroomOptions opts;
    opts.colour = 0;
    opts.game_root = game_root;
    carview::HubCamera cam;
    cam.yaw = 0.62f;
    cam.pitch = 0.16f;
    cam.distance = 4.45f;
    cam.target[0] = 0.0f;
    cam.target[1] = 0.48f;
    cam.target[2] = 0.0f;
    render_studio("gfx_parity_autoshow_fxx.png", "fer_fxx_05", opts, cam, {});
  }
  // 2. Autoshow White R34 GT-R (non-clipped white midtones + crisp studio highlights & creases).
  {
    carview::ShowroomOptions opts;
    opts.car_id = 445;
    opts.game_root = game_root;
    carview::HubCamera cam;
    cam.yaw = 0.65f;
    cam.pitch = 0.17f;
    cam.distance = 4.55f;
    cam.target[0] = 0.0f;
    cam.target[1] = 0.52f;
    cam.target[2] = 0.0f;
    cars::CarPaintSpec white_spec;
    white_spec.body.active = true;
    white_spec.body.rgb = 0xF2F3F5u;  // FH1 OEM QM1 Cloud White
    white_spec.body.finish = cars::PaintFinish::kNormal;
    render_studio("gfx_parity_autoshow_white_r34.png", "nis_skylinegtrvspecii_02", opts, cam,
                  [&](carview::Showroom& sr) { sr.set_paint_spec(white_spec); });
  }
}

void test_polish_tint_and_livery_regions() {
  // P0-1: ShowroomPush default tint MUST be {1,1,1,1} so untinted rims/interiors never black-multiply.
  carview::ShowroomPush push{};
  CHECK(push.tint[0] == 1.0f && push.tint[1] == 1.0f && push.tint[2] == 1.0f && push.tint[3] == 1.0f);

  // P0-2: LiveryEditor::current_region_spec() reflects active paint region independently of body.
  carview::LiveryEditor ed;
  std::string msg;
  ed.open(nullptr, {}, 445, {}, msg);
  const uint32_t body_before = ed.document().paint.body.rgb;
  ed.move_region(4);  // kRims
  CHECK(ed.region() == carview::PaintRegion::kRims);
  ed.step_hsv(0.15f, 0.05f, 0.05f);
  CHECK(ed.current_region_spec().active);
  CHECK(ed.document().paint.rims.active);
  CHECK(ed.current_region_spec().rgb == ed.document().paint.rims.rgb);
  CHECK(ed.document().paint.body.rgb == body_before);

  // Verify LiveryEditor HUD renders cleanly across paint regions without crashing or clipping.
  carview::ui::List l;
  carview::draw_livery_editor_hud(l, 1280.0f, 720.0f, ed, "2002 Nissan Skyline GT-R V-Spec II");
  CHECK(!l.items().empty());
  ed.move_region(2);  // kWindowTint
  CHECK(ed.region() == carview::PaintRegion::kWindowTint);
  ed.step_window_tint(0.25f);
  carview::ui::List l2;
  carview::draw_livery_editor_hud(l2, 1280.0f, 720.0f, ed, "2002 Nissan Skyline GT-R V-Spec II");
  CHECK(!l2.items().empty());
}

void test_phase3_livery_and_paint_parity() {
  carview::LiveryEditor ed;
  std::string msg;
  CHECK(ed.open(nullptr, {}, 445, {}, msg));

  // Manufacturer & Previous Colors tabs.
  std::vector<cars::bake::Colour> oem;
  cars::bake::Colour c1{};
  c1.sequence = 0;
  c1.rgb = 0x1144CCu;
  c1.metallic = true;
  cars::bake::Colour c2{};
  c2.sequence = 1;
  c2.rgb = 0xEE2211u;
  c2.metallic = false;
  oem.push_back(c1);
  oem.push_back(c2);
  ed.set_manufacturer_colors(oem);
  ed.set_tab(carview::EditorTab::kManufacturerColors);
  ed.move_manufacturer_color(1);
  ed.apply_manufacturer_color();
  CHECK(ed.document().paint.body.rgb == 0xEE2211u);
  CHECK(ed.document().paint.body.finish == cars::PaintFinish::kNormal);
  CHECK(ed.previous_colors().size() >= 2);

  // Stock rim refusal alongside chrome rim refusal.
  ed.set_wheel_is_stock(true);
  ed.set_region(carview::PaintRegion::kRims);
  CHECK(ed.rims_paint_blocked());
  const uint32_t rim_rgb_before = ed.document().paint.rims.rgb;
  ed.step_hsv(0.2f, 0.0f, 0.0f);
  CHECK(ed.document().paint.rims.rgb == rim_rgb_before);
  ed.set_wheel_is_stock(false);
  CHECK(!ed.rims_paint_blocked());
  ed.step_hsv(0.2f, 0.0f, 0.0f);
  CHECK(ed.document().paint.rims.active);

  // Vinyl tools: Mirror L<->R, Copy/Paste, Mask/Cutout, Group, Flip, Skew, Alpha, Wing plane.
  ed.set_tab(carview::EditorTab::kVinylLayers);
  ed.add_vinyl_layer("@rect", 0xFF0000u);
  ed.document().layers[0].plane = cars::LiveryPlaneKind::kLeft;
  ed.document().layers[0].x = 0.25f;
  ed.document().layers[0].rot_deg = 30.0f;
  CHECK(ed.mirror_selected_layer());
  CHECK(ed.document().layers.size() == 2);
  CHECK(ed.document().layers[1].plane == cars::LiveryPlaneKind::kRight);
  CHECK(std::fabs(ed.document().layers[1].x - 0.75f) < 1e-4f);
  CHECK(ed.document().layers[1].flip_x);
  CHECK(ed.copy_selected_layer());
  CHECK(ed.has_clipboard());
  CHECK(ed.paste_layer());
  CHECK(ed.document().layers.size() == 3);
  ed.toggle_layer_mask();
  CHECK(ed.document().layers[ed.selected_layer()].is_mask);
  ed.toggle_layer_group();
  CHECK(ed.document().layers[ed.selected_layer()].group_id != 0);
  ed.adjust_layer_skew(0.25f);
  ed.adjust_layer_alpha(-0.25f);
  ed.document().layers[ed.selected_layer()].plane = cars::LiveryPlaneKind::kWing;

  // Mask/Cutout compositor + plane sub-rect clipping (no bleed outside plane bounds).
  cars::LiveryDocument doc;
  cars::bake::LiveryPlane top_plane{};
  top_plane.name = "Top";
  top_plane.valid = true;
  top_plane.left = -512.0f;
  top_plane.right = 0.0f;
  top_plane.top = -512.0f;
  top_plane.bottom = 0.0f;
  std::vector<cars::bake::LiveryPlane> planes{top_plane};

  cars::VinylLayer base_rect;
  base_rect.shape_path = "@rect";
  base_rect.plane = cars::LiveryPlaneKind::kTop;
  base_rect.x = 0.5f;
  base_rect.y = 0.5f;
  base_rect.scale_x = 1.5f;  // Oversized intentionally to verify sub-rect clipping.
  base_rect.scale_y = 1.5f;
  base_rect.rgb = 0x00FF00u;
  doc.layers.push_back(base_rect);

  cars::VinylLayer cutout;
  cutout.shape_path = "@rect";
  cutout.plane = cars::LiveryPlaneKind::kTop;
  cutout.x = 0.5f;
  cutout.y = 0.5f;
  cutout.scale_x = 0.3f;
  cutout.scale_y = 0.3f;
  cutout.is_mask = true;
  doc.layers.push_back(cutout);

  std::vector<uint8_t> rgba;
  CHECK(cars::composite_livery(doc, nullptr, &planes, rgba, 128));
  // Bottom-up TGA Y-flip maps top_plane (top=-512, bottom=0) to py in [64..127], px in [0..63].
  // Center of top_plane (32, 96) was punched out by mask layer -> alpha 0.
  CHECK(rgba[(96 * 128 + 32) * 4 + 3] == 0);
  // Corner inside top_plane (8, 72) remains green opaque -> alpha 255.
  CHECK(rgba[(72 * 128 + 8) * 4 + 3] == 255);
  // Outside top_plane (96, 96) and (32, 32) must not bleed -> alpha 0.
  CHECK(rgba[(96 * 128 + 96) * 4 + 3] == 0);
  CHECK(rgba[(32 * 128 + 32) * 4 + 3] == 0);

  // Round-trip .liv preserves is_mask, group_id, and kWing.
  const fs::path tmp_liv = fs::temp_directory_path() / "shift_phase3_livery_test.liv";
  CHECK(ed.save(tmp_liv, nullptr));
  carview::LiveryEditor ed2;
  CHECK(ed2.open(nullptr, {}, 445, tmp_liv, msg));
  CHECK(ed2.document().layers.size() == 3);
  CHECK(ed2.document().layers[2].is_mask);
  CHECK(ed2.document().layers[2].group_id != 0);
  CHECK(ed2.document().layers[2].plane == cars::LiveryPlaneKind::kWing);
  std::error_code ec;
  fs::remove(tmp_liv, ec);

  // Phase R2-3A: Binary .liv import preserves caller base paint instead of defaulting to flat red,
  // and parses structured FLIV header + layer records cleanly.
  {
    const fs::path bin_liv = fs::temp_directory_path() / "shift_r2_opaque_test.liv";
    {
      std::ofstream bf(bin_liv, std::ios::binary);
      const uint8_t opaque[32] = {0x9A, 0x31, 0xF4, 0x02, 0x55, 0x77, 0xAA, 0xEE};
      bf.write(reinterpret_cast<const char*>(opaque), sizeof(opaque));
    }
    cars::LiveryDocument opaque_doc;
    opaque_doc.car_id = 309;
    opaque_doc.paint.body.active = true;
    opaque_doc.paint.body.rgb = 0x1A55CCu;
    opaque_doc.paint.body.finish = cars::PaintFinish::kMetallic;
    CHECK(cars::load_livery_file(bin_liv, opaque_doc, nullptr));
    CHECK(opaque_doc.imported_binary);
    CHECK(opaque_doc.binary_size_bytes == 32u);
    CHECK(opaque_doc.car_id == 309);
    CHECK(opaque_doc.paint.body.active);
    CHECK(opaque_doc.paint.body.rgb == 0x1A55CCu);
    CHECK(opaque_doc.paint.body.finish == cars::PaintFinish::kMetallic);
    fs::remove(bin_liv, ec);

    // Structured FLIV binary payload with body paint + 1 vinyl layer.
    std::vector<uint8_t> fliv;
    auto push_be32 = [&](uint32_t v) {
      fliv.push_back(uint8_t((v >> 24) & 0xFF));
      fliv.push_back(uint8_t((v >> 16) & 0xFF));
      fliv.push_back(uint8_t((v >> 8) & 0xFF));
      fliv.push_back(uint8_t(v & 0xFF));
    };
    auto push_be16 = [&](uint16_t v) {
      fliv.push_back(uint8_t((v >> 8) & 0xFF));
      fliv.push_back(uint8_t(v & 0xFF));
    };
    auto push_bef32 = [&](float f) {
      uint32_t u = 0;
      std::memcpy(&u, &f, 4);
      push_be32(u);
    };
    fliv.insert(fliv.end(), {'F', 'L', 'I', 'V'});
    push_be32(1u);            // version
    push_be32(445u);          // car_id
    push_be32(0x001188EEu);   // body_rgb
    fliv.push_back(uint8_t(cars::PaintFinish::kMatte));
    fliv.push_back(0x07u);    // flags: body active + sec_rgb + window_tint
    push_be16(1u);            // 1 layer
    push_be32(0x00FFD020u);   // sec_rgb
    push_bef32(0.45f);        // window_tint
    // Layer 0
    fliv.push_back(uint8_t(cars::LiveryPlaneKind::kTop));
    fliv.push_back(0x05u);    // flip_x | visible
    push_be16(7u);            // group_id
    push_bef32(0.40f);        // x
    push_bef32(0.60f);        // y
    push_bef32(0.30f);        // scale_x
    push_bef32(0.15f);        // scale_y
    push_bef32(45.0f);        // rot_deg
    push_bef32(0.10f);        // skew
    push_be32(0x00EEAA11u);   // rgb
    push_bef32(0.85f);        // alpha
    const std::string spath = "@stripe";
    push_be16(uint16_t(spath.size()));
    fliv.insert(fliv.end(), spath.begin(), spath.end());

    cars::LiveryDocument fliv_doc;
    CHECK(cars::import_binary_livery_bytes(fliv, "r34_custom", fliv_doc, nullptr));
    CHECK(fliv_doc.imported_binary && fliv_doc.car_id == 445);
    CHECK(fliv_doc.paint.body.active && fliv_doc.paint.body.rgb == 0x1188EEu);
    CHECK(fliv_doc.paint.body.finish == cars::PaintFinish::kMatte);
    CHECK(fliv_doc.paint.body.sec_rgb == 0xFFD020u);
    CHECK(std::fabs(fliv_doc.paint.window_tint - 0.45f) < 1e-4f);
    CHECK(fliv_doc.layers.size() == 1);
    CHECK(fliv_doc.layers[0].shape_path == "@stripe");
    CHECK(fliv_doc.layers[0].plane == cars::LiveryPlaneKind::kTop);
    CHECK(fliv_doc.layers[0].flip_x && fliv_doc.layers[0].visible && !fliv_doc.layers[0].is_mask);
    CHECK(fliv_doc.layers[0].group_id == 7 && fliv_doc.layers[0].rgb == 0xEEAA11u);
    CHECK(std::fabs(fliv_doc.layers[0].rot_deg - 45.0f) < 1e-4f);
  }
}

}  // namespace

int main() {
  test_shader_settings_and_hsv();
  test_damage_and_settings();
  test_polish_tint_and_livery_regions();
  test_phase3_livery_and_paint_parity();
  test_disc_livery_and_paint();
  maybe_capture_headless_screenshots();
  maybe_capture_gfx_parity_screenshots();
  if (g_failed) {
    std::fprintf(stderr, "livery_damage_tests: %d FAILED\n", g_failed);
    return 1;
  }
  std::printf("livery_damage_tests: all passed\n");
  return 0;
}
