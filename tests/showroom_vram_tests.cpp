#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cars/bake_format.h"
#include "renderer.h"
#include "showroom.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const char* name) {
    path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path / "cars", ec);
    std::filesystem::create_directories(path / "wheels", ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

bool write_synthetic_bake(const std::filesystem::path& dir, std::string& error) {
  using namespace cars::bake;
  Bake car;
  car.kind = Kind::kCar;
  car.car.media_name = "testcar";
  car.car.display_name = "Test Car";
  car.car.wheel_media = "testwheel";
  car.car.tyre_media = "testtyre";
  car.car.tyre_texture_media = "tirea";
  car.car.colours.push_back({1, 0, false, 0xFFFFFFu, "", "", false, 0});

  Model m;
  m.name = "body";
  m.role = Role::kExterior;
  Part p;
  p.name = "chassis";
  // > 24 vertices so stub_pool() does not classify it as a stand-in box.
  for (int i = 0; i < 27; ++i) {
    Vertex v{};
    v.position = {float(i % 3) * 0.1f, float(i / 9) * 0.1f, float((i / 3) % 3) * 0.1f};
    v.frame = {32767, 0, 0, 0};
    v.lighting = {100, 0, 0, 0};
    p.lod_vertices.push_back(v);
  }
  Subsection sub;
  sub.technique = "opaque";
  sub.material = MaterialClass::kOpaque;
  sub.lod = 1;
  sub.uv_transform = {0, 0, 1, 1, 0, 0, 1, 1};
  sub.indices = {0, 1, 2};
  p.subsections.push_back(std::move(sub));
  m.parts.push_back(std::move(p));
  car.models.push_back(std::move(m));

  Bake wheel;
  wheel.kind = Kind::kWheel;
  Bake tyre;
  tyre.kind = Kind::kWheel;
  Bake shared;
  shared.kind = Kind::kShared;

  return write_file(dir / "cars" / "testcar.shiftbake", encode(car), &error) &&
         write_file(dir / "wheels" / "testwheel.shiftbake", encode(wheel), &error) &&
         write_file(dir / "wheels" / "testtyre.shiftbake", encode(tyre), &error) &&
         write_file(dir / "wheels" / "tirea.shiftbake", encode(shared), &error) &&
         write_file(dir / "shared.shiftbake", encode(shared), &error);
}

std::shared_ptr<carview::WorldTextureData> make_world_texture(uint32_t record, uint8_t shade) {
  auto d = std::make_shared<carview::WorldTextureData>();
  d->record = record;
  d->srgb = true;
  d->texture.name = "tex_" + std::to_string(record);
  d->texture.format = cars::bake::TextureFormat::kRgba8;
  // 64x64 RGBA8 = 16 KiB texels (+ alignment).
  constexpr uint32_t kSize = 64;
  std::vector<uint8_t> rgba(size_t(kSize) * kSize * 4, shade);
  for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
  d->texture.levels.push_back({kSize, kSize, std::move(rgba)});
  return d;
}

carview::WorldTriangles make_gather(uint32_t a, uint32_t b, uint32_t c, uint32_t single) {
  carview::WorldTriangles w;
  w.colours.push_back({0.5f, 0.5f, 0.5f});
  // Triangle 0: Layers::kSplat (a, b, c) in front of the camera at z = -4.
  w.positions.push_back({-2.0f, -1.0f, -4.0f});
  w.positions.push_back({0.0f, -1.0f, -4.0f});
  w.positions.push_back({-1.0f, 1.0f, -4.0f});
  // Triangle 1: single texture (d) in front of the camera at z = -4.
  w.positions.push_back({0.5f, -1.0f, -4.0f});
  w.positions.push_back({2.5f, -1.0f, -4.0f});
  w.positions.push_back({1.5f, 1.0f, -4.0f});

  for (int i = 0; i < 6; ++i) {
    w.normals.push_back({0.0f, 0.0f, 1.0f});
    w.uv0.push_back({0.5f, 0.5f});
    w.uv1.push_back({0.5f, 0.5f});
    w.vertex_colours.push_back({255, 255, 255, 255});
  }
  w.group = {0, 0};
  w.material = {0, 1};

  carview::WorldTriangles::Material splat_mat;
  splat_mat.texture = a;
  splat_mat.blend = 0;
  splat_mat.layers = 2;  // Layers::kSplat
  splat_mat.layer_b = b;
  splat_mat.splat = c;
  w.materials.push_back(splat_mat);

  carview::WorldTriangles::Material single_mat;
  single_mat.texture = single;
  single_mat.blend = 0;
  single_mat.layers = 0;
  w.materials.push_back(single_mat);

  w.textures.push_back(make_world_texture(a, uint8_t(40 + a * 10)));
  w.textures.push_back(make_world_texture(b, uint8_t(40 + b * 10)));
  w.textures.push_back(make_world_texture(c, uint8_t(40 + c * 10)));
  w.textures.push_back(make_world_texture(single, uint8_t(40 + single * 10)));
  return w;
}

carview::ShowroomFrame test_frame() {
  carview::ShowroomFrame f{};
  // Simple perspective-like column-major matrix looking down -Z from (0, 0, 0).
  const float n = 0.5f, far_p = 100.0f;
  f.view_projection[0] = 1.0f;
  f.view_projection[5] = -1.0f;
  f.view_projection[10] = far_p / (n - far_p);
  f.view_projection[11] = -1.0f;
  f.view_projection[14] = (n * far_p) / (n - far_p);
  std::memcpy(f.transparent_view_projection, f.view_projection, sizeof f.view_projection);
  f.eye[3] = 1.0f;
  f.key_light[1] = 1.0f;
  f.key_light[3] = 1.0f;
  f.options[0] = 1.0f;
  f.tint[0] = f.tint[1] = f.tint[2] = f.tint[3] = 1.0f;
  return f;
}

}  // namespace

int main() {
  TempDir temp("shift_showroom_vram_test");
  std::string err;
  if (!write_synthetic_bake(temp.path, err)) {
    std::printf("bake write failed: %s\n", err.c_str());
    return 1;
  }

  {
    carview::Renderer renderer;
    carview::Renderer::Options opts;
    opts.validation = true;
    opts.msaa = 1;
    if (!renderer.init(opts, err)) {
      std::printf("skipping showroom_vram_tests: %s\n", err.c_str());
      return 77;
    }

    carview::Showroom showroom(renderer);
    CHECK(showroom.load(temp.path, "testcar", {}, err));
    CHECK(showroom.upload(err));

    // VMA sub-allocation check: multiple allocations share fewer device blocks.
    const auto vram0 = renderer.vram_stats();
    CHECK(vram0.allocation_count > 0);
    CHECK(vram0.block_count > 0);
    CHECK(vram0.block_count < vram0.allocation_count);

    showroom.set_world_texture_budget({256 * 1024, 6});
    showroom.set_world_swap_fade(0.05);
    showroom.set_draw_car(false);
    showroom.set_world_culling(false);
    showroom.set_frame(test_frame());

    auto stream_and_render = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t single, bool fade_wait) {
      carview::Showroom::StagedWorld staged;
      CHECK(showroom.stage_world(carview::build_world(make_gather(a, b, c, single)), staged, err));
      CHECK(showroom.begin_world(std::move(staged), err));
      bool swapped = false;
      CHECK(showroom.poll_world(true, &swapped));
      CHECK(swapped);
      std::vector<uint8_t> rgba;
      CHECK(renderer.render_offscreen(carview::FrameConstants{}, 64, 64, rgba, err));
      if (fade_wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(65));
        CHECK(renderer.render_offscreen(carview::FrameConstants{}, 64, 64, rgba, err));
        CHECK(renderer.render_offscreen(carview::FrameConstants{}, 64, 64, rgba, err));
      }
      const auto ds = showroom.world_draw_stats();
      CHECK(ds.white == 0);
      CHECK(ds.layer_fallback == 0);
    };

    // 4-gather cycle: A -> B -> C -> A (re-entry after eviction).
    stream_and_render(1, 2, 3, 4, false);
    stream_and_render(5, 6, 7, 8, true);
    auto st = showroom.world_texture_stats();
    CHECK(st.evictions > 0);
    CHECK(st.resident_count <= 6);
    CHECK(st.resident_bytes <= 256 * 1024);

    stream_and_render(9, 10, 11, 12, true);
    st = showroom.world_texture_stats();
    CHECK(st.resident_count <= 6);
    CHECK(st.resident_bytes <= 256 * 1024);

    // Re-enter A: evicted textures 1..4 must cleanly re-upload without white/layer fallback.
    stream_and_render(1, 2, 3, 4, true);
    st = showroom.world_texture_stats();
    CHECK(st.reuploads >= 4);
    CHECK(st.resident_count <= 6);
    CHECK(st.resident_bytes <= 256 * 1024);
    CHECK(st.descriptor_pools <= 1);
    CHECK(st.retired_pending == 0);
    CHECK(st.descriptor_sets_free > 0);

    // Prepare-pin protection: reserve_world_texture protects an unreferenced resident
    // texture from eviction until release_prepare_pins() is called.
    const bool reserved = showroom.reserve_world_texture(1);
    CHECK(reserved);
    // Stream D (13..16) without A in world_draws_: texture 1 must stay pinned!
    stream_and_render(13, 14, 15, 16, true);
    CHECK(showroom.reserve_world_texture(1));  // still resident (now pin count 2)
    showroom.release_prepare_pins({1, 1});
    // Now lower count to 4 and trigger eviction via set_world_texture_budget.
    showroom.set_world_texture_budget({256 * 1024, 4});
    CHECK(!showroom.reserve_world_texture(1));
    CHECK(showroom.world_texture_stats().resident_count <= 4);

    // Empty-levels WorldTextureData regression check: both stage_world+begin_world
    // and set_world must handle empty texture levels gracefully without crashing,
    // leave the texture unuploaded (reserve_world_texture == false), and fall back
    // to white draws cleanly.
    auto make_empty_levels_gather = [](uint32_t record) {
      carview::WorldTriangles w;
      w.colours.push_back({0.5f, 0.5f, 0.5f});
      w.positions.push_back({-1.0f, -1.0f, -4.0f});
      w.positions.push_back({1.0f, -1.0f, -4.0f});
      w.positions.push_back({0.0f, 1.0f, -4.0f});
      for (int i = 0; i < 3; ++i) {
        w.normals.push_back({0.0f, 0.0f, 1.0f});
        w.uv0.push_back({0.5f, 0.5f});
        w.uv1.push_back({0.5f, 0.5f});
        w.vertex_colours.push_back({255, 255, 255, 255});
      }
      w.group = {0};
      w.material = {0};
      carview::WorldTriangles::Material mat;
      mat.texture = record;
      w.materials.push_back(mat);
      auto empty_tex = std::make_shared<carview::WorldTextureData>();
      empty_tex->record = record;
      empty_tex->srgb = true;
      empty_tex->texture.name = "empty_" + std::to_string(record);
      empty_tex->texture.format = cars::bake::TextureFormat::kRgba8;
      w.textures.push_back(std::move(empty_tex));
      return w;
    };

    constexpr uint32_t kEmptyAsyncRecord = 9001;
    {
      carview::Showroom::StagedWorld staged_empty;
      CHECK(showroom.stage_world(carview::build_world(make_empty_levels_gather(kEmptyAsyncRecord)),
                                 staged_empty, err));
      CHECK(showroom.begin_world(std::move(staged_empty), err));
      bool swapped_empty = false;
      CHECK(showroom.poll_world(true, &swapped_empty));
      CHECK(swapped_empty);
      CHECK(!showroom.reserve_world_texture(kEmptyAsyncRecord));
      std::vector<uint8_t> rgba_empty;
      CHECK(renderer.render_offscreen(carview::FrameConstants{}, 64, 64, rgba_empty, err));
      CHECK(showroom.world_draw_stats().white == 1);
    }

    constexpr uint32_t kEmptySyncRecord = 9002;
    {
      // Also verify set_world drops an active pending_world_ cleanly before replacing draws.
      carview::Showroom::StagedWorld staged_pending;
      CHECK(showroom.stage_world(carview::build_world(make_gather(30, 31, 32, 33)), staged_pending, err));
      CHECK(showroom.begin_world(std::move(staged_pending), err));
      CHECK(showroom.set_world(make_empty_levels_gather(kEmptySyncRecord), err));
      bool swapped_pending = true;
      CHECK(showroom.poll_world(true, &swapped_pending));
      CHECK(!swapped_pending);
      CHECK(!showroom.reserve_world_texture(kEmptySyncRecord));
      std::vector<uint8_t> rgba_empty;
      CHECK(renderer.render_offscreen(carview::FrameConstants{}, 64, 64, rgba_empty, err));
      CHECK(showroom.world_draw_stats().white == 1);
    }

    if (opts.validation) CHECK(renderer.validation_messages() == 0);
  }

  // Pending-world teardown check: destroy Showroom while pending_world_ has not been polled.
  {
    carview::Renderer renderer;
    carview::Renderer::Options opts;
    opts.validation = true;
    opts.msaa = 1;
    if (renderer.init(opts, err)) {
      carview::Showroom showroom(renderer);
      CHECK(showroom.load(temp.path, "testcar", {}, err));
      CHECK(showroom.upload(err));
      carview::Showroom::StagedWorld staged;
      CHECK(showroom.stage_world(carview::build_world(make_gather(20, 21, 22, 23)), staged, err));
      CHECK(showroom.begin_world(std::move(staged), err));
      // Intentionally destroy Showroom + Renderer without calling poll_world().
      CHECK(renderer.validation_messages() == 0);
    }
  }

  std::printf("showroom_vram_tests: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
