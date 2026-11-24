#include "showroom_internal.h"

#include "cars/mods.h"
#include "fh1data/archive.h"
#include "fh1data/xds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace carview {

bool Showroom::load_showcase_texture(const std::filesystem::path& game_root, int aircraft_kind, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (showcase_tex_kind_ == aircraft_kind && showcase_tex_set_ != VK_NULL_HANDLE) return true;

  bake::Texture tex;
  tex.format = bake::TextureFormat::kRgba8;
  bool decoded = false;
  if (!game_root.empty()) {
    const auto zip_path = cars::mods::resolve(game_root / "media" / "animatedobjects.zip");
    std::error_code ec;
    if (std::filesystem::exists(zip_path, ec)) {
      std::string aerr;
      if (auto arc = fh1::Archive::open(zip_path, &aerr)) {
        std::vector<const char*> candidates;
        switch (aircraft_kind) {
          case 1:  // Hot-Air Balloon
            candidates = {"Textures/FEST_AUTOSHOW_HotAirBalloon_DIFF.xds",
                          "Textures/FEST_AUTOSHOW_HotAirBalloon_2_DIFF.xds"};
            break;
          case 2:  // Stunt Jet
            candidates = {"Textures/ANIM_P-51_Mustang2_DIFF.xds", "Textures/ANIM_P-51_Mustang_DIFF.xds"};
            break;
          case 3:  // Biplane
            candidates = {"Textures/ANIM_BiplaneStearmanHRZN_Diff.xds",
                          "Textures/ANIM_BiplaneStearmanCropduster_Diff.xds"};
            break;
          case 4:  // Attack Helicopter
            candidates = {"Textures/GR_Heli_Blade_DIFF.xds", "Textures/ANIM_P-51_Mustang2_DIFF.xds"};
            break;
          default:  // P-51 Mustang
            candidates = {"Textures/ANIM_P-51_Mustang_DIFF.xds", "Textures/ANIM_P-51_Mustang2_DIFF.xds"};
            break;
        }
        for (const char* member : candidates) {
          const auto rd = arc->read(member);
          if (!rd.ok() || rd.data.empty()) continue;
          auto xr = fh1::decode_xds(rd.data, false);
          if (!xr.levels.empty() && xr.levels[0].width > 0 && xr.levels[0].height > 0 &&
              xr.levels[0].rgba.size() == size_t(xr.levels[0].width) * xr.levels[0].height * 4) {
            tex.levels.push_back({xr.levels[0].width, xr.levels[0].height, std::move(xr.levels[0].rgba)});
            decoded = true;
            break;
          }
        }
      }
    }
  }
  if (!decoded) {
    // Crisp procedural livery fallback (riveted metallic panels / festival gores / tactical steel).
    constexpr uint32_t kS = 128;
    std::vector<uint8_t> px(kS * kS * 4, 255);
    for (uint32_t y = 0; y < kS; ++y) {
      for (uint32_t x = 0; x < kS; ++x) {
        const size_t idx = (size_t(y) * kS + x) * 4;
        const bool seam = (x % 32 == 0) || (y % 32 == 0);
        const bool stripe = (y >= 54 && y <= 74);
        uint8_t r = 210, g = 216, b = 224;
        if (aircraft_kind == 1) {
          const int gore = int(x / 16) % 4;
          static constexpr uint8_t kPal[4][3] = {{238, 28, 108}, {252, 198, 26}, {252, 108, 22}, {244, 246, 252}};
          r = kPal[gore][0], g = kPal[gore][1], b = kPal[gore][2];
        } else if (aircraft_kind == 4) {
          r = stripe ? 215 : 58;
          g = stripe ? 64 : 66;
          b = stripe ? 38 : 72;
        } else if (aircraft_kind == 3) {
          r = stripe ? 245 : 206;
          g = stripe ? 236 : 36;
          b = stripe ? 210 : 34;
        } else if (aircraft_kind == 2) {
          r = stripe ? 242 : 226;
          g = stripe ? 20 : 230;
          b = stripe ? 98 : 238;
        } else {
          r = stripe ? 215 : 212;
          g = stripe ? 32 : 218;
          b = stripe ? 34 : 226;
        }
        const float shade = seam ? 0.78f : 1.0f;
        px[idx + 0] = uint8_t(float(r) * shade);
        px[idx + 1] = uint8_t(float(g) * shade);
        px[idx + 2] = uint8_t(float(b) * shade);
        px[idx + 3] = 255;
      }
    }
    tex.levels.push_back({kS, kS, std::move(px)});
  }

  r_.wait_idle();
  r_.destroy_image(showcase_tex_);
  if (!upload_texture(&tex, true, showcase_tex_, error)) return false;
  if (showcase_tex_set_ == VK_NULL_HANDLE) {
    if (!create_world_single_set(showcase_tex_.view, showcase_tex_set_, error)) return false;
  } else {
    VkDescriptorImageInfo di{sampler_, showcase_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wd.dstSet = showcase_tex_set_;
    wd.dstBinding = 0;
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &di;
    vkUpdateDescriptorSets(r_.device_, 1, &wd, 0, nullptr);
  }
  showcase_tex_kind_ = aircraft_kind;
  return true;
}

void Showroom::set_showcase_mesh(const std::vector<ShowcaseVertex>& tris) {
  showcase_count_ = 0;
  if (!vfx_ready_ || !showcase_buf_.ready() || tris.empty()) return;
  const uint32_t n = uint32_t(std::min(tris.size(), size_t(kMaxShowcaseVerts)));
  auto* dst = static_cast<WorldVertex*>(showcase_buf_.data());
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };
  for (uint32_t i = 0; i < n; ++i) {
    const auto& v = tris[i];
    WorldVertex& o = dst[i];
    o = WorldVertex{};
    o.position[0] = float(double(v.pos[0]) - world_origin_[0]);
    o.position[1] = v.pos[1];
    o.position[2] = float(double(v.pos[2]) - world_origin_[1]);
    o.normal[0] = snorm8(v.normal[0]);
    o.normal[1] = snorm8(v.normal[1]);
    o.normal[2] = snorm8(v.normal[2]);
    o.normal[3] = 0;
    o.uv0[0] = v.uv[0];
    o.uv0[1] = v.uv[1];
    for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
  }
  showcase_count_ = n;
  ++showcase_buf_.ver;
}

void Showroom::update_driver_mesh(float wheel_rad) {
  driver_count_ = 0;
  if (!driver_buf_.ready()) return;

  // Steering wheel centre & axis in car model space (+X right, +Y up, -Z forward, +Z rearward).
  std::array<float, 3> pivot{-0.36f, 0.72f, 0.08f};
  std::array<float, 3> axis{0.0f, -0.25f, -0.968f};
  if (cockpit_rigs_[0].valid) {
    pivot = cockpit_rigs_[0].pivot;
    axis = cockpit_rigs_[0].axis;
  }

  auto vadd = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
  };
  auto vsub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  };
  auto vscale = [](const std::array<float, 3>& a, float s) -> std::array<float, 3> {
    return {a[0] * s, a[1] * s, a[2] * s};
  };
  auto vcross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  };
  auto vnorm = [](const std::array<float, 3>& a) -> std::array<float, 3> {
    const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 1e-6f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : std::array<float, 3>{0, 1, 0};
  };
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };

  auto* dst = static_cast<WorldVertex*>(driver_buf_.data());
  uint32_t count = 0;
  auto push_tri = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
                      const std::array<uint8_t, 4>& col) {
    if (count + 3 > kMaxDriverVerts) return;
    // world_opaque_ uses VK_FRONT_FACE_CLOCKWISE (for det=-1 world_transform_); emit CW order
    // (a, c, b) while keeping the outward CCW geometric normal so car-space det=+1 faces draw.
    const auto n = vnorm(vcross(vsub(b, a), vsub(c, a)));
    const std::array<float, 3>* pts[3] = {&a, &c, &b};
    for (int k = 0; k < 3; ++k) {
      WorldVertex& o = dst[count++];
      o = WorldVertex{};
      o.position[0] = (*pts[k])[0];
      o.position[1] = (*pts[k])[1];
      o.position[2] = (*pts[k])[2];
      o.normal[0] = snorm8(n[0]);
      o.normal[1] = snorm8(n[1]);
      o.normal[2] = snorm8(n[2]);
      for (int j = 0; j < 4; ++j) o.colour[j] = col[j];
    }
  };
  auto push_quad = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
                       const std::array<float, 3>& d, const std::array<uint8_t, 4>& col) {
    push_tri(a, b, c, col);
    push_tri(a, c, d, col);
  };
  auto push_box = [&](const std::array<float, 3>& c, const std::array<float, 3>& rx, const std::array<float, 3>& ry,
                      const std::array<float, 3>& rz, const std::array<uint8_t, 4>& col) {
    auto pt = [&](float sx, float sy, float sz) -> std::array<float, 3> {
      return {c[0] + rx[0] * sx + ry[0] * sy + rz[0] * sz, c[1] + rx[1] * sx + ry[1] * sy + rz[1] * sz,
              c[2] + rx[2] * sx + ry[2] * sy + rz[2] * sz};
    };
    const auto p000 = pt(-1, -1, -1), p100 = pt(1, -1, -1), p110 = pt(1, 1, -1), p010 = pt(-1, 1, -1);
    const auto p001 = pt(-1, -1, 1), p101 = pt(1, -1, 1), p111 = pt(1, 1, 1), p011 = pt(-1, 1, 1);
    push_quad(p001, p101, p111, p011, col);
    push_quad(p100, p000, p010, p110, col);
    push_quad(p011, p111, p110, p010, col);
    push_quad(p000, p100, p101, p001, col);
    push_quad(p101, p100, p110, p111, col);
    push_quad(p000, p001, p011, p010, col);
  };
  auto push_limb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float rad,
                       const std::array<uint8_t, 4>& col) {
    const auto dir = vsub(b, a);
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-4f) return;
    const auto f = vscale(dir, 0.5f);
    const auto mid = vadd(a, f);
    const auto u = vnorm(dir);
    const std::array<float, 3> ref = std::fabs(u[1]) < 0.9f ? std::array<float, 3>{0, 1, 0} : std::array<float, 3>{1, 0, 0};
    const auto r1 = vscale(vnorm(vcross(ref, u)), rad);
    const auto r2 = vscale(vnorm(vcross(u, r1)), rad);
    push_box(mid, r1, r2, f, col);
  };

  // Orthonormal wheel plane basis perpendicular to steering column axis.
  const auto ax = vnorm(axis);
  const auto wheel_right = vnorm(vcross({0.0f, 1.0f, 0.0f}, vscale(ax, -1.0f)));
  const auto wheel_up = vnorm(vcross(vscale(ax, -1.0f), wheel_right));

  // Clamp hand animation angle to realistic single-grip range (~ +/- 135 deg) so arms never twist through chest.
  const float grip_ang = std::clamp(wheel_rad, -2.35f, 2.35f);
  const float ca = std::cos(grip_ang), sa = std::sin(grip_ang);
  const float rim_r = 0.165f;
  // 9 o'clock (-wheel_right) and 3 o'clock (+wheel_right) rotated by grip_ang.
  const auto l_dir = vadd(vscale(wheel_right, -ca), vscale(wheel_up, sa));
  const auto r_dir = vadd(vscale(wheel_right, ca), vscale(wheel_up, -sa));
  const auto l_hand = vadd(pivot, vscale(l_dir, rim_r));
  const auto r_hand = vadd(pivot, vscale(r_dir, rim_r));

  // Rounded ellipsoid helper for smooth racing helmet & shoulders (no blocky cube helmet!).
  auto push_ellipsoid = [&](const std::array<float, 3>& c, float rx, float ry, float rz,
                            const std::array<uint8_t, 4>& col, int stacks = 5, int slices = 10) {
    constexpr float kPi = 3.14159265358979f;
    auto pt = [&](int st, int sl) -> std::array<float, 3> {
      const float phi = -0.5f * kPi + kPi * float(st) / float(stacks);
      const float th = 2.0f * kPi * float(sl) / float(slices);
      const float cp = std::cos(phi);
      return {c[0] + rx * cp * std::cos(th), c[1] + ry * std::sin(phi), c[2] + rz * cp * std::sin(th)};
    };
    for (int st = 0; st < stacks; ++st) {
      for (int sl = 0; sl < slices; ++sl) {
        const auto p00 = pt(st, sl);
        const auto p10 = pt(st + 1, sl);
        const auto p11 = pt(st + 1, sl + 1);
        const auto p01 = pt(st, sl + 1);
        if (st > 0) push_tri(p00, p10, p11, col);
        if (st + 1 < stacks) push_tri(p00, p11, p01, col);
      }
    }
  };

  // Seated festival driver behind steering wheel (+Z rearward in model space, visible through
  // rear and side glass in chase/external cameras, hidden in cockpit/hood/bumper views).
  const float lean = std::clamp(wheel_rad * 0.03f, -0.045f, 0.045f);
  const std::array<float, 3> chest{pivot[0] + lean * 0.4f, pivot[1] - 0.06f, pivot[2] + 0.36f};
  const std::array<float, 3> head{pivot[0] + lean, pivot[1] + 0.21f, pivot[2] + 0.38f};

  const std::array<uint8_t, 4> jacket{56, 66, 82, 255};
  const std::array<uint8_t, 4> collar{168, 52, 68, 255};
  const std::array<uint8_t, 4> skin{184, 140, 114, 255};
  const std::array<uint8_t, 4> hair{48, 38, 32, 255};
  const std::array<uint8_t, 4> shades{22, 24, 30, 255};
  const std::array<uint8_t, 4> glove{42, 44, 50, 255};

  // Emit forearms & gloved steering hands FIRST so cockpit camera can draw [0..driver_hands_count_)
  // cleanly gripping the rotating wheel without head/torso obstructing the windshield!
  const std::array<float, 3> l_sh{chest[0] - 0.21f, chest[1] + 0.14f, chest[2] - 0.02f};
  const std::array<float, 3> r_sh{chest[0] + 0.21f, chest[1] + 0.14f, chest[2] - 0.02f};
  const std::array<float, 3> l_el{(l_sh[0] + l_hand[0]) * 0.5f - 0.06f, (l_sh[1] + l_hand[1]) * 0.5f - 0.08f,
                                  (l_sh[2] + l_hand[2]) * 0.5f + 0.04f};
  const std::array<float, 3> r_el{(r_sh[0] + r_hand[0]) * 0.5f + 0.06f, (r_sh[1] + r_hand[1]) * 0.5f - 0.08f,
                                  (r_sh[2] + r_hand[2]) * 0.5f + 0.04f};
  push_limb(l_el, l_hand, 0.040f, jacket);
  push_limb(r_el, r_hand, 0.040f, jacket);
  push_box(l_hand, {0.042f, 0, 0}, {0, 0.042f, 0}, {0, 0, 0.046f}, glove);
  push_box(r_hand, {0.042f, 0, 0}, {0, 0.042f, 0}, {0, 0, 0.046f}, glove);
  driver_hands_count_ = count;

  // Sculpted festival driver upper arms, torso, collar, neck, head, hair cap & sunglasses (exterior views).
  push_limb(l_sh, l_el, 0.048f, jacket);
  push_limb(r_sh, r_el, 0.048f, jacket);
  push_box(chest, {0.19f, 0, 0}, {0, 0.21f, 0}, {0, 0, 0.105f}, jacket);
  push_box({chest[0], chest[1] + 0.16f, chest[2] - 0.02f}, {0.205f, 0, 0}, {0, 0.048f, 0}, {0, 0, 0.11f}, collar);
  push_box({head[0], head[1] - 0.11f, head[2]}, {0.052f, 0, 0}, {0, 0.060f, 0}, {0, 0, 0.052f}, skin);
  push_ellipsoid(head, 0.098f, 0.115f, 0.112f, skin, 5, 10);
  push_ellipsoid({head[0], head[1] + 0.042f, head[2] + 0.015f}, 0.104f, 0.088f, 0.114f, hair, 5, 10);
  push_ellipsoid({head[0], head[1] + 0.008f, head[2] - 0.084f}, 0.078f, 0.028f, 0.032f, shades, 3, 8);

  driver_count_ = count;
  ++driver_buf_.ver;
}

void Showroom::record_driver_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || driver_count_ == 0 || !world_opaque_ || !world_white_set_) return;
  const uint32_t player_verts = cockpit_camera_active_ ? driver_hands_count_ : driver_count_;
  const bool draw_player = draw_car_ && player_verts > 0;
  if (!draw_player && instances_.empty()) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_opaque_);
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  r_.apply_vrs(cmd);
  const VkDescriptorSet sets[2] = {frame_set_, world_white_set_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
  VkDeviceSize at = dyn_sync(driver_buf_, VkDeviceSize(driver_count_) * sizeof(WorldVertex));
  vkCmdBindVertexBuffers(cmd, 0, 1, &driver_buf_.buf.buffer, &at);

  ShowroomPush pc{};
  for (int i = 0; i < 3; ++i) {
    pc.uv0[i] = world_sun_[i];
    pc.uv1[i] = world_up_[i];
  }
  pc.uv0[3] = 2.0f;  // swap = none
  pc.uv1[3] = world_up_[3] * 0.78f;  // balanced cabin-shaded lighting visible through rear/side glass
  pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;
  pc.params[0] = 0.0f;      // kOpaque
  pc.params[1] = 65536.0f;  // kFlagVertexLit
  pc.params[2] = 0.0f;
  pc.params[3] = 1.0f;

  if (draw_player) {
    identity(pc.model);  // player driver vertices are in car model space
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdDraw(cmd, player_verts, 1, 0, 0);
  }
  // Also render seated drivers inside nearby AI race / festival / traffic cars.
  for (const auto& in : instances_) {
    if (!in.car || in.fade_alpha < 0.5f) continue;
    const float dx = in.model[12] - frame_.eye[0];
    const float dy = in.model[13] - frame_.eye[1];
    const float dz = in.model[14] - frame_.eye[2];
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) continue;
    if (dx * dx + dy * dy + dz * dz > 95.0f * 95.0f) continue;
    std::memcpy(pc.model, in.model, sizeof pc.model);
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdDraw(cmd, driver_count_, 1, 0, 0);
  }
}


void Showroom::record_showcase_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || showcase_count_ == 0 || !world_opaque_) return;
  VkDescriptorSet mat_set = showcase_tex_set_ ? showcase_tex_set_ : world_white_set_;
  if (!mat_set) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_opaque_);
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  r_.apply_vrs(cmd);
  const VkDescriptorSet sets[2] = {frame_set_, mat_set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
  ShowroomPush pc{};
  std::memcpy(pc.model, world_transform_, sizeof pc.model);
  for (int i = 0; i < 3; ++i) {
    pc.uv0[i] = world_sun_[i];
    pc.uv1[i] = world_up_[i];
  }
  pc.uv0[3] = 2.0f;  // swap = none
  pc.uv1[3] = world_up_[3];
  pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;  // UV tiling 1x1
  pc.params[0] = 0.0f;  // kOpaque
  pc.params[1] = showcase_tex_set_ ? 1.0f : 65536.0f;  // kFlagTextured when showcase texture loaded
  pc.params[2] = 0.0f;
  pc.params[3] = 1.0f;
  vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof pc, &pc);
  VkDeviceSize at = dyn_sync(showcase_buf_, VkDeviceSize(showcase_count_) * sizeof(WorldVertex));
  vkCmdBindVertexBuffers(cmd, 0, 1, &showcase_buf_.buf.buffer, &at);
  vkCmdDraw(cmd, showcase_count_, 1, 0, 0);
}


}  // namespace carview
