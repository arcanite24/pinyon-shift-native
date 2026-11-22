// Unit tests for Slice 7 Milestone 4: Paint Shop finishes (ShaderSettings XML,
// List_SpecialColors 102 two-tone paints, HSV & per-region painting), Livery.zip
// (1,372 vinyl/decal .carbin shapes) 1024^2 LiveryMasks compositor & .liv I/O,
// Cosmetic & Simulation Car Damage + dirt/mud, and SettingsPage Damage row.
// When SHIFT_CAPTURE_DIR is set, also renders the 4 headless Vulkan 1280x720
// verification screenshots for Slice 7.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
  for (const char* p : {R"(C:\Users\neri\code\horizon1-recomp\.local\game\base)",
                        R"(C:\Users\neri\code\shift-engine\.local\game\base)"}) {
    if (fs::exists(fs::path(p) / "media" / "db" / "gamedb.slt")) return p;
  }
  return {};
}

fs::path find_bake_dir() {
  for (const char* p : {R"(C:\Users\neri\code\shift-engine\out\bake)"}) {
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
        <SpecularAmount2 value="7.000000"/>
        <SpecularPower2 value="6.000000"/>
        <SpecularAmount3 value="6.000000"/>
        <SpecularPower3 value="4.000000"/>
      </body>
    </ShaderSettings>)";
  cars::ShaderFinishParams fp;
  CHECK(cars::parse_shader_settings_xml(sample_xml, fp));
  CHECK(std::fabs(fp.paint_scale - 0.5f) < 1e-4f);
  CHECK(std::fabs(fp.spec_amount2 - 7.0f) < 1e-4f);

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

  // None mode ignores impacts and dirt.
  drive::CarDamageState d_none;
  d_none.mode = drive::DamageMode::kNone;
  d_none.record_impact(0.0f, 0.5f, 1.8f, 25.0f);
  d_none.step(5.0, 20.0, true, 1.0, 0.8, {0.2, 0.2, 0.2, 0.2});
  CHECK(d_none.zone_front == 0.0f && d_none.body_dirt == 0.0f);

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

  // 1. ShaderSettings XML from Shared.zip.
  cars::ShaderSettingsSet ss;
  CHECK(cars::load_shader_settings(install.media / "cars" / "Shared.zip", ss, &err));
  CHECK(ss.loaded_from_xml);
  CHECK(std::fabs(ss.metallic.paint_scale - 0.5f) < 1e-4f);

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

}  // namespace

int main() {
  test_shader_settings_and_hsv();
  test_damage_and_settings();
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
