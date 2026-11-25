#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "world/road_follow.h"

namespace carview {

void set_driver_wristband_color(uint32_t rgba);
void spawn_detached_car_part(const void* owner_key, int part_kind, const std::array<float, 3>& world_pos,
                             const std::array<float, 3>& car_vel, const std::array<float, 3>& car_fwd,
                             const std::array<float, 3>& car_right, const std::array<float, 3>& car_up,
                             const std::array<uint8_t, 4>& paint_rgba);
void step_and_append_detached_car_parts(const void* owner_key, float dt, const drive::Ground* ground,
                                        std::vector<world::DebrisVertex>& out_tris);
void clear_detached_car_parts(const void* owner_key);

namespace {

// The world clock at the race's countdown start (the gantry's 0:05 row) and,
// outside a race, the offset that keeps it past the gantry's count.
constexpr double kGantryStart = 6.0, kGantryIdle = 11.0;

uint32_t resolve_wristband_rgba(const DriveApp& app) {
  // Canonical Horizon Festival wristband tiers: Yellow, Green, Blue, Pink, Orange, Purple, Gold.
  static constexpr uint32_t kTierRgba[7] = {
      0xFFD628FFu,  // 0 Yellow
      0x4BDC6EFFu,  // 1 Green
      0x0A9BFAFFu,  // 2 Blue
      0xE81C6EFFu,  // 3 Pink
      0xFF9224FFu,  // 4 Orange
      0xC678FFFFu,  // 5 Purple
      0xF6CE4EFFu,  // 6 Gold
  };
  const int tier = std::clamp(app.collected_wristband, 0, 6);
  if (app.have_career_catalog && size_t(tier) < app.career_catalog.wristbands.size()) {
    const uint32_t argb = app.career_catalog.wristbands[size_t(tier)].argb;
    if ((argb & 0x00FFFFFFu) != 0u) {
      const uint32_t r = (argb >> 16) & 0xFFu;
      const uint32_t g = (argb >> 8) & 0xFFu;
      const uint32_t b = argb & 0xFFu;
      return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
    }
  }
  return kTierRgba[size_t(tier)];
}

// Per-car overhead roof probe (35 m upward BVH raycast) so AI/traffic/replay
// vehicles toggle tunnel headlamps based on their own roof occlusion rather
// than mirroring the player's audio tunnel flag across Colorado.
bool is_under_tunnel_roof(const drive::Ground* ground, const drive::Vec3& pos) {
  if (!ground) return false;
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;
  drive::Hit hit{};
  if (!ground->raycast({pos.x, pos.y + 1.6, pos.z}, {0.0, 1.0, 0.0}, 35.0, hit)) return false;
  return hit.distance >= 0.5 && std::fabs(hit.normal.y) > 0.40;
}

}  // namespace

void DriveSession::poll_async_guest_loads() {
  for (auto it = festival_guest_loads.begin(); it != festival_guest_loads.end();) {
    if (!it->second.valid()) {
      it = festival_guest_loads.erase(it);
      continue;
    }
    if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++it;
      continue;
    }
    std::unique_ptr<Showroom> gs = it->second.get();
    festival_guests[it->first] = std::move(gs);
    it = festival_guest_loads.erase(it);
  }
}

void DriveSession::request_guest_load_async(const std::string& key, const std::string& media,
                                            const ShowroomOptions& opt, size_t max_cache) {
  if (key.empty() || media.empty()) return;
  if (festival_guests.count(key) || festival_guest_loads.count(key)) return;
#ifdef __ANDROID__
  // Cap guest car models on unified-RAM mobile devices so roaming AI and
  // Autoshow browsing never grow unbounded into lowmemorykiller OOM.
  // Never erase existing entries here: callers may hold raw Showroom* pointers
  // in an in-progress Instance list for the current frame.
  max_cache = std::min<size_t>(max_cache, 6u);
#endif
  if (festival_guests.size() + festival_guest_loads.size() >= max_cache) return;
  if (festival_guest_loads.size() >= 2) return;  // cap concurrent background uploads to prevent CPU contention
  Renderer* r = &host.renderer;
  const std::filesystem::path bake = app.bake_dir;
  const std::filesystem::path root = app.game_root_of();
  const VkImageView env_view = host.showroom.dynamic_env_view();
  const VkSampler env_sampler = host.showroom.dynamic_env_sampler();
  ShowroomOptions local_opt = opt;
  if (local_opt.game_root.empty()) local_opt.game_root = root;
  festival_guest_loads.emplace(
      key, std::async(std::launch::async, [r, bake, root, media, local_opt, env_view, env_sampler]() {
        auto gs = std::make_unique<Showroom>(*r);
        gs->set_guest();
        if (env_view && env_sampler) gs->set_shared_dynamic_env(env_view, env_sampler);
        std::string ge;
        if (!gs->load(bake, media, local_opt, ge)) return std::unique_ptr<Showroom>{};
        gs->load_car_lamps(root, media);
        if (!gs->upload(ge)) return std::unique_ptr<Showroom>{};
        return gs;
      }));
}

void DriveSession::open_replay_viewer(bool browser_if_empty) {
        if (replay_rec.frame_count() >= 2) {
          if (app.race) {
            const auto rh = app.race->hud_info();
            replay_rec.update_summary(rh.place, rh.cars, rh.best_lap, app.tod_clock.tod_sec);
          }
          replay_viewer.open(replay_rec.snapshot(), true);
        } else if (browser_if_empty) {
          replay_viewer.open_browser(replays_dir);
        } else {
          notice = "NO REPLAY RECORDED YET";
          notice_until = vehicle.time() + 2.5;
        }
}

std::vector<PromoCarCandidate> DriveSession::gather_promo_candidates() {
        std::vector<carview::PromoCarCandidate> cands;
        auto resolve_car_id = [&](int64_t raw_id, const std::string& media) -> int {
          if (raw_id > 0) return int(raw_id);
          if (!media.empty()) {
            if (app.have_career_catalog) {
              if (const auto* spec = app.career_catalog.find_car_by_media(media)) return int(spec->car_id);
            }
            const std::string want = lower_ascii(media);
            for (const auto& [cid, bm] : app.baked_ids) {
              if (lower_ascii(bm) == want) return int(cid);
            }
          }
          return 0;
        };
        auto resolve_display_name = [&](int64_t cid, const std::string& media, const std::string& fallback) -> std::string {
          if (!media.empty()) return carview::car_name(media);
          if (cid > 0) {
            if (auto it = app.baked_ids.find(cid); it != app.baked_ids.end()) return carview::car_name(it->second);
            if (app.have_career_catalog) {
              if (const auto* spec = app.career_catalog.find_car(cid)) return carview::car_name(spec->media_name);
            }
          }
          return fallback;
        };

        const drive::Vec3 pat = vehicle.position();
        const int player_cid = resolve_car_id(info.car_id, app.car);
        if (player_cid > 0) {
          cands.push_back({player_cid, carview::car_name(app.car), {pat.x, pat.y, pat.z}, 2.2, true});
        }
        if (app.race) {
          std::vector<carview::RaceView::OpponentTag> otags;
          app.race->opponent_tags(otags);
          for (const auto& ot : otags) {
            const int cid = resolve_car_id(ot.car_id, ot.car_media);
            if (cid <= 0) continue;
            const std::string cname =
                !ot.car_name.empty() ? ot.car_name : resolve_display_name(cid, ot.car_media, ot.name);
            cands.push_back({cid, cname, {ot.x, ot.y, ot.z}, 2.2, false});
          }
        }
        if (traffic && traffic->cars()) {
          for (const auto& tc : *traffic->cars()) {
            if (tc.model <= 0) continue;
            const std::string tname = resolve_display_name(tc.model, {}, "TRAFFIC CAR");
            cands.push_back({int(tc.model), tname, {tc.position.x, tc.position.y, tc.position.z}, 2.2, false});
          }
        }
        if (festival_mgr) {
          for (const auto& fc : festival_mgr->cars()) {
            const int cid = resolve_car_id(fc.spec.car_id, fc.spec.car_media);
            if (cid <= 0) continue;
            const std::string fname = resolve_display_name(cid, fc.spec.car_media, fc.spec.driver_name);
            cands.push_back({cid, fname, {fc.position.x, fc.position.y, fc.position.z}, 2.2, false});
          }
        }
        return cands;
}

void DriveSession::place_world() {
        if (!host.streamer) return;
        float m[16];
        scenery_transform(info, vehicle, host.showroom.world_origin_x(), host.showroom.world_origin_z(), m);
        host.showroom.set_world_transform(m);
        poll_async_guest_loads();
        if (traffic) traffic->place(info, vehicle);
        else host.showroom.set_instances({});
        if (replay_viewer.active() && replay_viewer.current_cars().size() > 1) {
          // Multi-car Replay: player (slot 0) stays on host.showroom; place every recorded
          // opponent/traffic car (slot > 0) onto its own guest Showroom in player-local space.
          host.showroom.set_instances({});
          float pm[16];
          drive::model_to_world(info, vehicle, pm);
          double pmd[16];
          for (int i = 0; i < 16; ++i) pmd[i] = pm[i];
          const double a = pmd[0], b = pmd[4], c = pmd[8], d = pmd[1], e = pmd[5], f = pmd[9], g = pmd[2], h = pmd[6], k = pmd[10];
          const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
          const double id = det != 0.0 ? 1.0 / det : 0.0;
          const double inv[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                                 (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                                 (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};
          double world_to_player[16]{};
          for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) world_to_player[col * 4 + r] = inv[r * 3 + col];
          for (int r = 0; r < 3; ++r)
            world_to_player[12 + r] = -(inv[r * 3 + 0] * pmd[12] + inv[r * 3 + 1] * pmd[13] + inv[r * 3 + 2] * pmd[14]);
          world_to_player[15] = 1.0;

          std::vector<Showroom::Instance> rmore;
          const uint32_t focus_slot = replay_viewer.focus_slot();
          const bool hide_focus_bumper = replay_viewer.camera_mode() == ReplayCameraMode::kBumper;
          const bool tod_night_lamps = drive::CarLampState::tod_requests_headlamps(app.tod_clock.tod_sec);
          const auto& all_rcars = replay_viewer.current_cars();
          // Process the focused car first so it gets priority for the 1-per-frame guest upload budget.
          std::vector<size_t> order;
          order.reserve(all_rcars.size());
          for (size_t idx = 0; idx < all_rcars.size(); ++idx) {
            if (all_rcars[idx].slot == focus_slot) order.push_back(idx);
          }
          for (size_t idx = 0; idx < all_rcars.size(); ++idx) {
            if (all_rcars[idx].slot != focus_slot) order.push_back(idx);
          }
          int uploads_this_frame = 0;
          constexpr float kTwoPi = 6.28318530717958647692f;
          std::unordered_set<Showroom*> claimed_guests;
          for (size_t idx : order) {
            const auto& rc = all_rcars[idx];
            // Slot 0 is always rendered by host.showroom (player car).
            if (rc.slot == 0u) continue;
            if (hide_focus_bumper && rc.slot == focus_slot) continue;
            const auto* cinfo = replay_viewer.recording().find_car_info(rc.slot);
            std::string media;
            if (cinfo && !cinfo->car_media.empty()) {
              media = cinfo->car_media;
            } else if (cinfo && cinfo->car_id > 0) {
              if (auto bit = app.baked_ids.find(cinfo->car_id); bit != app.baked_ids.end()) {
                media = bit->second;
              } else if (app.have_career_catalog) {
                if (const auto* sp = app.career_catalog.find_car(cinfo->car_id)) media = sp->media_name;
              }
            }
            if (media.empty()) {
              // Last-resort fallback: prefer a baked non-player model if available, else player model.
              for (const auto& bc : app.baked_cars) {
                if (lower_ascii(bc) != lower_ascii(app.car)) {
                  media = bc;
                  break;
                }
              }
              if (media.empty()) media = app.car;
            }
            const int64_t paint_seq = cinfo ? int64_t(cinfo->paint_index) : -1;
            // Claim a distinct guest Showroom per replay car so identical-model cars never
            // overwrite each other's wheel steer, spin, omega blur, or suspension Y.
            Showroom* guest_sr = app.race ? app.race->find_loaded_model(media, paint_seq, &claimed_guests) : nullptr;
            if (!guest_sr) {
              const std::string base_key = paint_seq >= 0 ? (media + "#" + std::to_string(paint_seq)) : media;
              const std::string slot_key = base_key + "@slot" + std::to_string(rc.slot);
              auto it = festival_guests.find(slot_key);
              if (it == festival_guests.end()) {
                auto it_base = festival_guests.find(base_key);
                if (it_base != festival_guests.end() && it_base->second &&
                    !claimed_guests.count(it_base->second.get())) {
                  it = it_base;
                } else if (paint_seq >= 0) {
                  auto it_plain = festival_guests.find(media);
                  if (it_plain != festival_guests.end() && it_plain->second &&
                      !claimed_guests.count(it_plain->second.get())) {
                    it = it_plain;
                  }
                }
              }
              if (it == festival_guests.end() && festival_guests.size() < 36 && uploads_this_frame < 1) {
                ++uploads_this_frame;
                ShowroomOptions opt{};
                opt.game_root = app.game_root_of();
                if (paint_seq > 0) opt.match_sequence = paint_seq;
                request_guest_load_async(slot_key, media, opt, 36);
              }
              if (it != festival_guests.end() && it->second) guest_sr = it->second.get();
              // Temporary fallback while an extra slot clone awaits the 1-per-frame upload budget,
              // or if a recorded model is absent from the local bake directory.
              if (!guest_sr) {
                if (app.race) guest_sr = app.race->find_loaded_model(media, paint_seq);
                if (!guest_sr) {
                  if (auto it_b = festival_guests.find(base_key); it_b != festival_guests.end() && it_b->second) {
                    guest_sr = it_b->second.get();
                  } else if (auto it_m = festival_guests.find(media); it_m != festival_guests.end() && it_m->second) {
                    guest_sr = it_m->second.get();
                  } else {
                    for (const auto& [k, ptr] : festival_guests) {
                      if (ptr) {
                        guest_sr = ptr.get();
                        break;
                      }
                    }
                  }
                }
              }
            }
            if (!guest_sr) continue;
            // Forward recorded wheel steer, spin, angular speed, and suspension Y to the guest car.
            if (claimed_guests.insert(guest_sr).second) {
              const auto& rest = guest_sr->car().wheel_centres_body;
              bool have_rec_wheels = false;
              for (int wi = 0; wi < 4; ++wi) {
                if (std::fabs(rc.wheel_omega[wi]) > 1e-4f || std::fabs(rc.wheel_spin[wi]) > 1e-4f ||
                    std::fabs(rc.wheel_steer[wi]) > 1e-4f || std::fabs(rc.wheel_y[wi]) > 1e-4f) {
                  have_rec_wheels = true;
                  break;
                }
              }
              const float radius = std::clamp(rest[0][1], 0.26f, 0.55f);
              const float fallback_omega = rc.speed_mps / radius;
              float fallback_spin = std::fmod(float(replay_viewer.time()) * fallback_omega, kTwoPi);
              if (fallback_spin < 0.0f) fallback_spin += kTwoPi;
              WheelPose wposes[4]{};
              for (int wi = 0; wi < 4; ++wi) {
                wposes[wi].centre[0] = rest[size_t(wi)][0];
                wposes[wi].centre[1] = (std::fabs(rc.wheel_y[wi]) > 1e-4f) ? rc.wheel_y[wi] : rest[size_t(wi)][1];
                wposes[wi].centre[2] = rest[size_t(wi)][2];
                wposes[wi].steer = rc.wheel_steer[wi];
                wposes[wi].spin = have_rec_wheels ? rc.wheel_spin[wi] : fallback_spin;
                wposes[wi].omega = have_rec_wheels ? rc.wheel_omega[wi] : fallback_omega;
              }
              guest_sr->set_wheel_poses(wposes);
            }
            const drive::Quat q{double(rc.quat[1]), double(rc.quat[2]), double(rc.quat[3]), double(rc.quat[0])};
            float gm[16];
            drive::model_to_world(guest_sr->car(), q, {0, 0, 0}, gm);
            double gmd[16];
            for (int i = 0; i < 16; ++i) gmd[i] = gm[i];
            gmd[12] += double(rc.pos[0]);
            gmd[13] += double(rc.pos[1]);
            gmd[14] += double(rc.pos[2]);
            Showroom::Instance inst;
            inst.car = guest_sr;
            for (int col = 0; col < 4; ++col)
              for (int row = 0; row < 4; ++row) {
                double sum = 0.0;
                for (int kk = 0; kk < 4; ++kk) sum += world_to_player[kk * 4 + row] * gmd[col * 4 + kk];
                inst.model[col * 4 + row] = float(sum);
              }
            drive::CarLampState ls = (rc.lamp_bits != 0u) ? drive::unpack_replay_lamps(rc.lamp_bits) : drive::CarLampState{};
            if (rc.lamp_bits == 0u) {
              const bool rc_in_tunnel =
                  !tod_night_lamps &&
                  is_under_tunnel_roof(host.ground, {double(rc.pos[0]), double(rc.pos[1]), double(rc.pos[2])});
              ls.headlamps_on = tod_night_lamps || rc_in_tunnel;
              ls.brake_on = rc.brake_u8 > 20;
              ls.reverse_on = rc.gear < 0;
            }
            inst.lamps = ls.vectors();
            inst.has_lamps = true;
            if (cinfo && cinfo->has_body_color) {
              const uint32_t rgb = cinfo->body_color;
              auto to_lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
              inst.paint_rgb[0] = to_lin(float((rgb >> 16) & 0xFFu) / 255.0f);
              inst.paint_rgb[1] = to_lin(float((rgb >> 8) & 0xFFu) / 255.0f);
              inst.paint_rgb[2] = to_lin(float(rgb & 0xFFu) / 255.0f);
              inst.has_paint = true;
            }
            rmore.push_back(inst);
          }
          host.showroom.set_instances(std::move(rmore));
        } else if (app.race) {
          app.race->set_tod_sec(float(app.tod_clock.tod_sec));
          std::vector<Showroom::Instance> more;
          app.race->instances(info, vehicle, more);
          host.showroom.add_instances(more);
        } else if (festival_mgr && !festival_mgr->cars().empty()) {
          // Render roaming Festival AI racers & active H2H opponent as guest Showrooms.
          // Compute world_to_player in double precision so kilometre-scale Colorado coordinates never jitter.
          float pm[16];
          drive::model_to_world(info, vehicle, pm);
          double pmd[16];
          for (int i = 0; i < 16; ++i) pmd[i] = pm[i];
          const double a = pmd[0], b = pmd[4], c = pmd[8], d = pmd[1], e = pmd[5], f = pmd[9], g = pmd[2], h = pmd[6], k = pmd[10];
          const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
          const double id = det != 0.0 ? 1.0 / det : 0.0;
          const double inv[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                                 (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                                 (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};
          double world_to_player[16]{};
          for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) world_to_player[col * 4 + r] = inv[r * 3 + col];
          for (int r = 0; r < 3; ++r)
            world_to_player[12 + r] = -(inv[r * 3 + 0] * pmd[12] + inv[r * 3 + 1] * pmd[13] + inv[r * 3 + 2] * pmd[14]);
          world_to_player[15] = 1.0;

          std::vector<Showroom::Instance> fmore;
          const bool fest_tod_lamps = drive::CarLampState::tod_requests_headlamps(app.tod_clock.tod_sec);
          const float fdt = float(tod_frame_dt > 0.0 ? std::clamp(tod_frame_dt, 0.0, 0.25) : 1.0 / 60.0);
          static const freeroam::FestivalManager* s_fest_lamp_owner = nullptr;
          static std::map<uint32_t, drive::CarLampState> s_fest_lamps;
          static std::map<uint32_t, float> s_fest_wheel_spin;
          if (s_fest_lamp_owner != festival_mgr.get()) {
            s_fest_lamp_owner = festival_mgr.get();
            s_fest_lamps.clear();
            s_fest_wheel_spin.clear();
          }
          {
            std::unordered_set<uint32_t> alive_fids;
            for (const auto& fc : festival_mgr->cars()) alive_fids.insert(fc.id);
            for (auto it = s_fest_lamps.begin(); it != s_fest_lamps.end();) {
              it = alive_fids.count(it->first) ? std::next(it) : s_fest_lamps.erase(it);
            }
            for (auto it = s_fest_wheel_spin.begin(); it != s_fest_wheel_spin.end();) {
              it = alive_fids.count(it->first) ? std::next(it) : s_fest_wheel_spin.erase(it);
            }
          }
          const drive::Vec3 ppos_f = vehicle.position();
          // Process nearest roaming Festival AI cars first so they get priority for dedicated guest slots.
          std::vector<size_t> fest_order;
          fest_order.reserve(festival_mgr->cars().size());
          for (size_t fi = 0; fi < festival_mgr->cars().size(); ++fi) fest_order.push_back(fi);
          std::sort(fest_order.begin(), fest_order.end(), [&](size_t ia, size_t ib) {
            const auto& a = festival_mgr->cars()[ia];
            const auto& b = festival_mgr->cars()[ib];
            const double da = (a.position.x - ppos_f.x) * (a.position.x - ppos_f.x) +
                              (a.position.z - ppos_f.z) * (a.position.z - ppos_f.z);
            const double db = (b.position.x - ppos_f.x) * (b.position.x - ppos_f.x) +
                              (b.position.z - ppos_f.z) * (b.position.z - ppos_f.z);
            return da < db;
          });
          Showroom* fest_fallback = nullptr;
          for (const auto& [kname, gptr] : festival_guests) {
            if (gptr) {
              fest_fallback = gptr.get();
              break;
            }
          }
          constexpr float kTwoPi = 6.28318530717958647692f;
          std::map<std::string, size_t> fest_media_occ;
          std::unordered_set<Showroom*> claimed_fest_guests;
          for (size_t fi : fest_order) {
            const auto& fc = festival_mgr->cars()[fi];
            if (!std::isfinite(fc.position.x) || !std::isfinite(fc.position.y) || !std::isfinite(fc.position.z)) continue;
            // Give each simultaneous Festival AI car of the same car_media its own guest Showroom
            // slot so identical models never overwrite each other's wheel steer & spin poses.
            const size_t occ = fest_media_occ[fc.spec.car_media]++;
            const std::string gkey =
                (occ == 0u) ? fc.spec.car_media : (fc.spec.car_media + "@dup" + std::to_string(occ));
            auto it = festival_guests.find(gkey);
            if (it == festival_guests.end() && festival_guests.size() < 24) {
              ShowroomOptions opt{};
              opt.game_root = app.game_root_of();
              request_guest_load_async(gkey, fc.spec.car_media, opt, 24);
            }
            Showroom* guest_sr = (it != festival_guests.end() && it->second) ? it->second.get() : nullptr;
            if (!guest_sr && occ > 0u) {
              if (auto it0 = festival_guests.find(fc.spec.car_media); it0 != festival_guests.end() && it0->second) {
                guest_sr = it0->second.get();
              }
            }
            if (!guest_sr) guest_sr = fest_fallback;
            if (!guest_sr) continue;
            const drive::Quat yq{0, std::sin(fc.heading * 0.5), 0, std::cos(fc.heading * 0.5)};
            const drive::Quat pq{std::sin(-fc.pitch * 0.5), 0, 0, std::cos(-fc.pitch * 0.5)};
            const drive::Quat q{
                yq.w * pq.x + yq.x * pq.w + yq.y * pq.z - yq.z * pq.y,
                yq.w * pq.y - yq.x * pq.z + yq.y * pq.w + yq.z * pq.x,
                yq.w * pq.z + yq.x * pq.y - yq.y * pq.x + yq.z * pq.w,
                yq.w * pq.w - yq.x * pq.x - yq.y * pq.y - yq.z * pq.z,
            };
            float gm[16];
            drive::model_to_world(guest_sr->car(), q, {0, 0, 0}, gm);
            double gmd[16];
            for (int i = 0; i < 16; ++i) gmd[i] = gm[i];
            gmd[12] += fc.position.x;
            gmd[13] += fc.position.y;
            gmd[14] += fc.position.z;
            Showroom::Instance inst;
            inst.car = guest_sr;
            for (int col = 0; col < 4; ++col)
              for (int row = 0; row < 4; ++row) {
                double sum = 0.0;
                for (int kk = 0; kk < 4; ++kk) sum += world_to_player[kk * 4 + row] * gmd[col * 4 + kk];
                inst.model[col * 4 + row] = float(sum);
              }
            const double d2 = (fc.position.x - ppos_f.x) * (fc.position.x - ppos_f.x) +
                              (fc.position.z - ppos_f.z) * (fc.position.z - ppos_f.z);
            // Persistent per-car CarLampState: 15-tick AI brake anti-flicker, staggered
            // dusk/dawn headlamps, per-car overhead tunnel roof probe, turn indicators, and reverse lights.
            const bool is_new_lamp = !s_fest_lamps.count(fc.id);
            drive::CarLampState& lstate = s_fest_lamps[fc.id];
            if (is_new_lamp) lstate.rng_state = 0x9E3779B9u ^ (fc.id * 2654435761u);
            const bool car_in_tunnel =
                !fest_tod_lamps && d2 < 220.0 * 220.0 &&
                is_under_tunnel_roof(host.ground, {fc.position.x, fc.position.y, fc.position.z});
            drive::LampInputs lin{};
            lin.brake_cmd = (fc.braking || fc.speed < 0.25) ? 1.0f : 0.0f;
            lin.brake_pedal = lin.brake_cmd;
            lin.gear = fc.reversing ? 0 : 1;
            lin.tod_sec = app.tod_clock.tod_sec;
            lin.dt = fdt;
            lin.indicator_mode = fc.turn_left ? 1 : (fc.turn_right ? 2 : 0);
            lin.light_override = car_in_tunnel ? 1 : 0;
            lin.is_ai = true;
            lin.snap_headlamps = is_new_lamp || app.headless;
            lstate.update(lin);
            inst.lamps = lstate.vectors();
            inst.has_lamps = true;
            if (const auto& cols = guest_sr->car().colours; !cols.empty()) {
              const uint32_t rgb = cols[size_t(fc.id) % cols.size()].rgb;
              auto to_lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
              inst.paint_rgb[0] = to_lin(float((rgb >> 16) & 0xFF) / 255.0f);
              inst.paint_rgb[1] = to_lin(float((rgb >> 8) & 0xFF) / 255.0f);
              inst.paint_rgb[2] = to_lin(float(rgb & 0xFF) / 255.0f);
              inst.has_paint = true;
            }
            fmore.push_back(inst);

            // Integrate per-car wheel spin and pose this car's dedicated guest Showroom.
            const auto& rest = guest_sr->car().wheel_centres_body;
            const float radius = std::clamp(rest[0][1], 0.26f, 0.55f);
            const float omega = float(fc.speed) / radius;
            float& spin = s_fest_wheel_spin[fc.id];
            spin = std::fmod(spin + omega * fdt, kTwoPi);
            if (spin < 0.0f) spin += kTwoPi;
            if (claimed_fest_guests.insert(guest_sr).second) {
              WheelPose wposes[4]{};
              for (int wi = 0; wi < 4; ++wi) {
                wposes[wi].centre[0] = rest[size_t(wi)][0];
                wposes[wi].centre[1] = rest[size_t(wi)][1];
                wposes[wi].centre[2] = rest[size_t(wi)][2];
                wposes[wi].steer = (wi < 2) ? float(fc.steer_angle) : 0.0f;
                wposes[wi].spin = spin;
                wposes[wi].omega = omega;
              }
              guest_sr->set_wheel_poses(wposes);
            }
          }
          if (!fmore.empty()) host.showroom.add_instances(fmore);
        }
        const bool player_in_tunnel = app.audio_host.in_tunnel();
        static const DriveWorldHost* s_tunnel_host = nullptr;
        static float s_tunnel_blend = 0.0f;
        if (s_tunnel_host != &host || app.headless || tod_frame_dt <= 0.0) {
          s_tunnel_host = &host;
          s_tunnel_blend = player_in_tunnel ? 1.0f : 0.0f;
        } else {
          const float t_alpha = std::clamp(1.0f - std::exp(-float(tod_frame_dt) / 0.35f), 0.0f, 1.0f);
          s_tunnel_blend += ((player_in_tunnel ? 1.0f : 0.0f) - s_tunnel_blend) * t_alpha;
        }
        const float tunnel_t = std::clamp(s_tunnel_blend, 0.0f, 1.0f);
        const float active_near =
            (host.cam_proj.near_z > 0.01f && std::isfinite(host.cam_proj.near_z)) ? host.cam_proj.near_z : 0.25f;
        {
          drive::LampInputs lin{};
          const float bg = brake_glow(vehicle);
          lin.brake_cmd = bg;
          lin.brake_pedal = bg;
          lin.gear = vehicle.gear();
          lin.tod_sec = app.tod_clock.tod_sec;
          lin.dt = float(tod_frame_dt > 0.0 ? tod_frame_dt : 1.0 / 60.0);
          // Player turn-signal / hazard indicator controls (',' left, '.' right, '/' hazard)
          // with steering-unwind self-cancel and automatic hazard flashers on rollover.
          static const DriveSession* s_ind_session = nullptr;
          static int s_ind_mode = 0;  // 0 off, 1 left, 2 right, 3 hazard
          static bool s_prev_comma = false, s_prev_period = false, s_prev_slash = false;
          static bool s_ind_turned = false;
          if (s_ind_session != this) {
            s_ind_session = this;
            s_ind_mode = 0;
            s_prev_comma = s_prev_period = s_prev_slash = false;
            s_ind_turned = false;
          }
          const bool ui_blocking = menu.open || map_open || picking || paint_open || garage_open ||
                                   outpost_hub_open || club_open || car_club_open || livery_editor.is_open() ||
                                   shop.is_open() || autoshow.is_open() || photo_mode.active() ||
                                   replay_viewer.active() || settings_live_overlay || app.show_title;
          if (!app.headless && !ui_blocking) {
            const SDL_Keymod mods = SDL_GetModState();
            const bool no_ctrl = (mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0;
            if (const bool* kb = SDL_GetKeyboardState(nullptr); kb && no_ctrl) {
              const bool c_now = kb[SDL_SCANCODE_COMMA];
              const bool p_now = kb[SDL_SCANCODE_PERIOD];
              const bool s_now = kb[SDL_SCANCODE_SLASH];
              if (c_now && !s_prev_comma) {
                s_ind_mode = (s_ind_mode == 1) ? 0 : 1;
                s_ind_turned = false;
              }
              if (p_now && !s_prev_period) {
                s_ind_mode = (s_ind_mode == 2) ? 0 : 2;
                s_ind_turned = false;
              }
              if (s_now && !s_prev_slash) {
                s_ind_mode = (s_ind_mode == 3) ? 0 : 3;
                s_ind_turned = false;
              }
              s_prev_comma = c_now;
              s_prev_period = p_now;
              s_prev_slash = s_now;
            }
          } else {
            s_prev_comma = s_prev_period = s_prev_slash = false;
          }
          const float road_steer = float(vehicle.wheels()[0].steer);
          const float spd = float(std::max(0.0, vehicle.speed()));
          // Self-cancel left/right turn indicator after completing a turn while moving.
          if ((s_ind_mode == 1 || s_ind_mode == 2) && spd > 2.5f) {
            const float signed_turn = (s_ind_mode == 1) ? road_steer : -road_steer;
            if (signed_turn > 0.16f) s_ind_turned = true;
            if (s_ind_turned && std::fabs(road_steer) < 0.05f) {
              s_ind_mode = 0;
              s_ind_turned = false;
            }
          }
          lin.indicator_mode = vehicle.is_rolled_over() ? 3 : s_ind_mode;
          // Entering a tunnel automatically triggers daytime headlamps on the player car when in auto lamp mode.
          lin.light_override = (light_override == 0 && player_in_tunnel) ? 1 : light_override;
          lin.popup_open = app.headless ? 1.0f : host.showroom.popup_open();
          lin.snap_headlamps = app.headless;
          player_lamps.update(lin);
          host.showroom.set_lamp_state(player_lamps);

          cockpit_anim.calibration = host.showroom.gauge_calibration();
          cockpit_anim.update_steering(road_steer, spd, lin.dt, cockpit_steer_kick_rad);
          const float redline = float(std::max(vehicle.params().redline_rpm, 6000.0));
          const float boost_gauge = drive::CockpitAnimState::gauge_boost_psi(vehicle.boost_psi(), cockpit_has_fi);
          constexpr double kRpmToRadS = 6.28318530717958647692 / 60.0;
          const double cur_rpm = std::max(0.0, vehicle.rpm());
          const float power_w =
              float(std::max(0.0, vehicle.engine_torque(cur_rpm, vehicle.engine_throttle()) * cur_rpm * kRpmToRadS));
          float max_power_w = 0.0f;
          {
            const double r_min = std::max(500.0, vehicle.params().idle_rpm);
            const double r_max = std::max(r_min + 500.0, double(redline));
            constexpr int kSteps = 24;
            for (int s = 0; s <= kSteps; ++s) {
              const double r = r_min + (r_max - r_min) * (double(s) / double(kSteps));
              const double p = vehicle.engine_torque(r, 1.0) * r * kRpmToRadS;
              if (p > double(max_power_w)) max_power_w = float(p);
            }
          }
          cockpit_anim.update_gauges(spd, cockpit_dial_max_speed, float(cur_rpm), redline, boost_gauge,
                                     cockpit_dial_max_boost, car_damage.fuel_fraction,
                                     power_w, max_power_w, 0.0f);
          Showroom::CockpitTelemetry ctel{};
          ctel.gear = vehicle.gear();
          ctel.speed_mps = spd;
          ctel.dial_max_speed_mps = cockpit_dial_max_speed;
          ctel.metric_units = app.metric_units;
          ctel.abs_active = vehicle.abs_active();
          ctel.tcs_active = vehicle.tcs_active() || vehicle.stm_active();
          ctel.handbrake_active = vehicle.shaped_input().handbrake;
          ctel.high_beam = player_lamps.fullbeam_on;
          ctel.turn_left = player_lamps.ind_left > 0.5f;
          ctel.turn_right = player_lamps.ind_right > 0.5f;
          ctel.shifting = vehicle.shifting();
          ctel.dt = lin.dt;
          set_driver_wristband_color(resolve_wristband_rgba(app));
          host.showroom.set_driver_visible(!autoshow.is_open() && !shop.is_open() && !paint_open && !garage_open);
          host.showroom.set_cockpit_telemetry(ctel);
          host.showroom.set_cockpit_anim(cockpit_anim, tod_cockpit_cam);
        }
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(m, up_game, up);
        if (app.tod_system.loaded_tod) {
          const world::TodState raw_tod = app.tod_system.evaluate(app.tod_clock);
          static const DriveWorldHost* s_tod_smooth_host = nullptr;
          static world::TodSmoother s_tod_smoother;
          const bool snap_tod = (s_tod_smooth_host != &host || app.headless);
          if (s_tod_smooth_host != &host) s_tod_smooth_host = &host;
          world::TodState tod =
              (!snap_tod && tod_frame_dt <= 0.0 && s_tod_smoother.initialized)
                  ? s_tod_smoother.state
                  : s_tod_smoother.update(raw_tod, float(std::clamp(tod_frame_dt, 0.0, 0.25)), snap_tod);
          rotate_direction(m, tod.sun_dir.data(), sun);
          host.showroom.set_world_light(sun, up, 1.0f);
          host.showroom.set_shadow_direction(sun);
          const drive::Vec3 vpos = vehicle.position();
          const auto& cfg = app.tod_system.post_config;
          const int zone_idx = app.tod_system.zones.find_zone(float(vpos.x), float(vpos.z));
          const float step_dt = float(tod_frame_dt > 0.0 ? tod_frame_dt : (app.headless ? 10.0 : 1.0 / 60.0));
          zone_blender.update(app.tod_system.zones, cfg, float(vpos.x), float(vpos.z), step_dt);
          // Apply blended PostZone FogTemplate multipliers (Fog_Templates.xml) before uploading ToD UBO.
          {
            const auto& ft = zone_blender.fog;
            for (int k = 0; k < 3; ++k) {
              tod.fog_color[k] *= ft.color[k];
              tod.fog_sun_scatter_color[k] *= ft.sun_scatter_color[k];
            }
            tod.fog_density *= ft.density;
            tod.fog_start *= ft.start_distance;
            tod.fog_sun_scatter_power *= ft.sun_scatter_power;
            tod.fog_sun_scatter_start = std::max(0.0f, ft.sun_scatter_start_distance);
            tod.fog_sun_scatter_rampup = std::max(1.0f, ft.sun_scatter_rampup_distance);
            tod.fog_height_fade_amount *= ft.height_fade_amount;
            tod.fog_height_fade_start *= ft.height_fade_start;
            tod.fog_height_fade_end *= ft.height_fade_end;
          }
          // Localized tunnel occlusion: preserve global outdoor Colorado ToD (sun_color, hl_top,
          // hl_bottom, ambient_color, ibl_ambient_scale, shadow_intensity, switch_on_lights, and
          // car_sun_color) intact so sunlit terrain outside tunnel portals stays bright and the
          // shadow map occludes direct sunlight under the tunnel roof. Apply only subtle car
          // sky-hemisphere ambient darkening (unused by world shaders) while the player is inside.
          if (tunnel_t > 0.001f) {
            const float car_sky_keep = 1.0f - 0.35f * tunnel_t;
            const float car_gnd_keep = 1.0f - 0.20f * tunnel_t;
            for (int k = 0; k < 3; ++k) {
              tod.car_hl_top[k] *= car_sky_keep;
              tod.car_hl_bottom[k] *= car_gnd_keep;
            }
          }
          // The world's animation clock, which the start gantry's LEDs also
          // read (world.frag: 0..6 s the 0:05..0:01 rows, then GO! for 4.5 s,
          // then the festival screen): in a race it runs on the race clock so
          // the count meets the start signal; elsewhere it stays past the
          // count. It moves on monotonically within either.
          const double world_clock = app.race ? kGantryStart + app.race->session().clock()
                                              : vehicle.time() + kGantryIdle;
          host.showroom.set_tod(tod, app.tod_clock.moon_phase(), float(world_clock), float(vpos.y));
          if (app.post_on) {
            PostSettings ps = host.renderer.post_settings();
            const float dn = tod.day_night_post;
            for (int k = 0; k < 6; ++k) ps.hable[k] = cfg.hable_day[k] + dn * (cfg.hable_night[k] - cfg.hable_day[k]);
            ps.white = cfg.white_day + dn * (cfg.white_night - cfg.white_day);
            const auto& bt = zone_blender.bloom;
            const auto& cs = zone_blender.color;
            const auto& vt = zone_blender.vig_top;
            const auto& vb = zone_blender.vig_bottom;
            // Forward blended per-zone ColorSettings_Templates.xml (desat_*, contrast, brightness, exposure_bias).
            ps.desaturate = std::clamp(cs.desat_amount * tod.desat_amount_mult, 0.0f, 1.5f);
            for (int k = 0; k < 3; ++k) {
              ps.luminance[k] = cs.desat_luminosity[k] * tod.desat_luminosity_mult[k];
              ps.balance[k] = cs.desat_post_mod[k] * tod.desat_post_mod_mult[k];
            }
            if (const char* b = std::getenv("SHIFT_BALANCE"))
              std::sscanf(b, "%f,%f,%f", &ps.balance[0], &ps.balance[1], &ps.balance[2]);
            ps.bloom = (app.bloom_flag >= 0 ? app.bloom_flag == 1 : !app.low_preset) && slice12.trainer.config.debug_bloom;
            ps.bloom_cutoff = std::max(0.05f, bt.cutoff * tod.bloom_cutoff_mult);
            ps.bloom_scale = std::max(0.0f, bt.scale * tod.bloom_scale_mult);
            ps.bloom_persist = std::clamp(bt.persist * tod.bloom_persist_mult, 0.0f, 0.35f);
            ps.bloom_lower_grad = std::max(0.0f, bt.lower_grad * tod.bloom_lower_grad_mult);
            ps.bloom_upper_grad = std::max(0.05f, bt.upper_grad * tod.bloom_upper_grad_mult);
            for (int k = 0; k < 5; ++k) ps.bloom_weights[k] = bt.weights[k];
            ps.vignette_amount = std::clamp(tod.vignette_amount, 0.0f, 1.0f);
            ps.contrast = std::clamp((1.0f + cs.contrast) * tod.contrast_mult, 0.5f, 1.8f);
            ps.brightness = std::clamp(cs.brightness, -0.35f, 0.35f);
            ps.paired_vignette = !app.tod_system.zones.vignette_templates.empty() && !app.low_preset;
            const float lr_int = std::clamp(tod.light_rays_intensity, 0.0f, 1.5f);
            const float lr_w = std::clamp(lr_int, 0.0f, 1.0f);
            const float sc_dist_rays =
                std::clamp((3500.0f - tod.fog_sun_scatter_start) / std::max(tod.fog_sun_scatter_rampup, 1.0f),
                           0.15f, 1.0f);
            for (int k = 0; k < 3; ++k) {
              ps.vig_top_color[k] = vt.color[k];
              ps.vig_bot_color[k] = vb.color[k];
              const float fog_sc =
                  tod.fog_color[k] + (tod.fog_sun_scatter_color[k] - tod.fog_color[k]) * sc_dist_rays;
              const float base_scatter = fog_sc * 1.45f + tod.sun_color[k] * 0.45f;
              ps.sun_scatter_color[k] =
                  std::clamp(base_scatter * (1.0f - lr_w) + tod.light_rays_color[k] * lr_w, 0.05f, 2.0f);
            }
            constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
            const float vt_span = std::clamp(vt.max_intensity - vt.min_intensity, 0.0f, 1.0f);
            const float vb_span = std::clamp(vb.max_intensity - vb.min_intensity, 0.0f, 1.0f);
            ps.vig_top_opacity = std::clamp(vt.color[3], 0.0f, 1.0f) * vt_span;
            ps.vig_top_center[0] = 0.5f + vt.pos_x * 0.5f;
            ps.vig_top_center[1] = 0.5f - vt.pos_y * 0.5f;
            ps.vig_top_radius[0] = std::max(vt.scale_x, 0.005f);
            ps.vig_top_radius[1] = std::max(vt.scale_y, 0.005f);
            ps.vig_top_rot = vt.angle_deg * kDegToRad;
            ps.vig_top_feather = std::clamp(vt.power, 0.25f, 8.0f);
            ps.vig_bot_opacity = std::clamp(vb.color[3], 0.0f, 1.0f) * vb_span;
            ps.vig_bot_center[0] = 0.5f + vb.pos_x * 0.5f;
            ps.vig_bot_center[1] = 0.5f - vb.pos_y * 0.5f;
            ps.vig_bot_radius[0] = std::max(vb.scale_x, 0.005f);
            ps.vig_bot_radius[1] = std::max(vb.scale_y, 0.005f);
            ps.vig_bot_rot = vb.angle_deg * kDegToRad;
            ps.vig_bot_feather = std::clamp(vb.power, 0.25f, 8.0f);
            // Optical post-FX: camera motion blur, DoF, god rays & lens flare/dirt.
            // Motion blur follows the camera's screen motion with the track's
            // motionBlurAmount (0 in free roam) and no floor (FEEL-31).
            ps.motion_blur = live_motion_blur && slice12.trainer.config.debug_motion_blur && (!app.bench || app.bench_full_fx);
            ps.motion_blur_strength = cfg.motion_blur_strength(vehicle.speed());
            ps.motion_blur_directional = true;
            ps.motion_blur_car_protect = tod_cockpit_cam ? 2.2f : 6.8f;
            ps.dof = live_dof && slice12.trainer.config.debug_dof && (!app.bench || app.bench_full_fx);
            ps.dof_focal_distance = std::clamp(cfg.dof_focal_distance, 5.0f, 40.0f);
            ps.dof_focal_range = 42.0f;
            ps.dof_max_blur = std::clamp(cfg.dof_focal_max_blur > 0.05f ? cfg.dof_focal_max_blur * 0.55f : 0.35f,
                                         0.15f, 0.65f);
            ps.near_plane = active_near;
            ps.god_rays = !app.low_preset && (!app.bench || app.bench_full_fx);
            ps.lens_flare = !app.low_preset && (!app.bench || app.bench_full_fx);
            ps.god_ray_strength = lr_int * (1.0f - tunnel_t);
            ps.lens_flare_strength = 0.36f * (1.0f - tunnel_t);
            ps.time_sec = float(vehicle.time());
            ps.lut_enabled = true;
            if (app.exposure_given > 0) {
              ps.auto_exposure = false;
              ps.exposure = float(app.exposure_given);
            } else {
              ps.auto_exposure = true;
              const bool cockpit = tod_cockpit_cam;
              const float* keys = cockpit ? cfg.cockpit_keys : cfg.exterior_keys;
              const float* evs = cockpit ? cfg.cockpit_ev : cfg.exterior_ev;
              // Apply TrackSettings filmicTone.exposure + blended zone ExposureBias + tunnel adaptation,
              // respecting parsed CameraExposureRange [exposure_min, exposure_max] (SKY_AND_TOD §5.2).
              const float ev_min = std::min(cfg.exposure_min, cfg.exposure_max);
              const float ev_max = std::max(cfg.exposure_min, cfg.exposure_max);
              const float tunnel_ev = 0.35f * tunnel_t;
              ps.exposure = std::clamp(cfg.filmic_exposure + cs.exposure_bias + tunnel_ev, ev_min, ev_max);
              for (int k = 0; k < 4; ++k) {
                ps.exp_keys[k] = keys[k];
                ps.exp_ev[k] = std::clamp(evs[k] + ps.exposure, ev_min, ev_max) - ps.exposure;
              }
              const float tau = std::max(cfg.tonemap_delay * 10.0f * (1.0f - 0.35f * tunnel_t), 0.05f);
              ps.adapt_alpha = (app.headless || tod_frame_dt <= 0.0)
                                   ? 1.0f
                                   : float(1.0 - std::exp(-tod_frame_dt / double(tau)));
            }
            if (photo_mode.active()) {
              ps = photo_mode.effects().apply_to_post(ps);
            }
            host.renderer.set_post_settings(ps);
            if (!host.user_lut_active) {
              static const DriveWorldHost* s_lut_host = nullptr;
              static world::ColorLutBlender s_lut_blender;
              const bool reset_lut = (s_lut_host != &host || !have_post_lut || app.headless || tod_frame_dt <= 0.0);
              if (s_lut_host != &host) s_lut_host = &host;
              if (!have_post_lut || zone_idx != last_lut_zone ||
                  std::fabs(tod.day_color_grade - last_lut_day) > 0.005f ||
                  std::fabs(tod.night_color_grade - last_lut_night) > 0.005f) {
                last_lut_zone = zone_idx;
                last_lut_day = tod.day_color_grade;
                last_lut_night = tod.night_color_grade;
                target_post_lut = world::ColorLut16::blend(app.tod_system.zones.day_lut_for(zone_idx),
                                                           app.tod_system.zones.night_lut_for(zone_idx),
                                                           tod.day_color_grade, tod.night_color_grade);
                if (reset_lut) {
                  s_lut_blender.snap(target_post_lut);
                  cur_post_lut = s_lut_blender.current;
                  have_post_lut = true;
                  post_lut_blending = false;
                  host.renderer.set_post_lut(cur_post_lut.rgba);
                } else {
                  post_lut_blending = true;
                }
              }
              if (post_lut_blending && have_post_lut && tod_frame_dt > 0.0) {
                const float zone_lerp_s =
                    (zone_idx >= 0 && size_t(zone_idx) < app.tod_system.zones.zones.size())
                        ? std::max(app.tod_system.zones.zones[size_t(zone_idx)].lerp_time, 1.0f)
                        : 15.0f;
                const float lut_tau = std::clamp(zone_lerp_s * 0.20f, 2.0f, 4.0f);
                const float lut_dt = float(std::clamp(tod_frame_dt, 0.0, 0.25));
                if (s_lut_blender.update(target_post_lut, lut_dt, lut_tau, false)) {
                  cur_post_lut = s_lut_blender.current;
                  host.renderer.set_post_lut(cur_post_lut.rgba);
                }
                if (s_lut_blender.settled(target_post_lut, 0.25f)) {
                  s_lut_blender.snap(target_post_lut);
                  cur_post_lut = target_post_lut;
                  host.renderer.set_post_lut(cur_post_lut.rgba);
                  post_lut_blending = false;
                }
              }
            }
          }
        } else {
          rotate_direction(m, kSunGame, sun);
          host.showroom.set_world_light(sun, up, 1.0f);
          if (host.have_shadow_light) {
            float towards[3];
            rotate_direction(m, host.shadow_light_game, towards);
            host.showroom.set_shadow_direction(towards);
          }
        }
        if (!app.bench || app.bench_full_fx) {
          std::vector<world::SkidVertex> asph_tris, rut_tris;
          host.skid_sys.build_geometry(asph_tris, rut_tris);
          host.showroom.set_skidmarks(asph_tris, rut_tris);
          std::vector<world::DebrisVertex> debris_tris;
          app.smashables.build_debris_mesh(debris_tris);
          if (!debris_tris.empty()) {
            // Ensure every real FH1 smashable debris texture record is immediately resident in
            // Showroom before the first flying debris frame draws. Only remember permanent decode
            // failures so textures evicted by the world LRU during long sessions are cleanly
            // re-uploaded instead of falling back to flat grey untextured vertex colour.
            static std::unordered_set<uint32_t> s_debris_tex_failed;
            std::unordered_set<uint32_t> seen_this_frame;
            std::string derr;
            for (const auto& dv : debris_tris) {
              const uint32_t rec = dv.texture_record;
              if (rec == world::kNoTexture || !seen_this_frame.insert(rec).second) continue;
              if (host.showroom.has_world_texture(rec) || s_debris_tex_failed.count(rec)) continue;
              fh1::WorldTexture wt;
              if (app.smashables.read_texture(rec, wt)) {
                auto td = std::make_shared<WorldTextureData>();
                td->record = rec;
                td->srgb = wt.format.gamma;
                char tname[40];
                std::snprintf(tname, sizeof tname, "debris:_rec_%u", rec);
                td->texture.name = tname;
                td->texture.format = cars::bake::TextureFormat(uint32_t(wt.blocks.format));
                td->texture.swizzle = wt.blocks.swizzle;
                for (auto& l : wt.blocks.levels) td->texture.levels.push_back({l.width, l.height, std::move(l.blocks)});
                (void)host.showroom.ensure_world_texture(td, derr);
              } else {
                s_debris_tex_failed.insert(rec);
              }
            }
          }
          std::vector<carview::Showroom::ShowcaseVertex> showcase_tris;
          if (app.race && app.race->is_showcase()) {
            std::string sc_err;
            host.showroom.load_showcase_texture(app.game_root_of(), int(app.race->showcase().spec().kind), sc_err);
            app.race->build_showcase_mesh(showcase_tris);
          }
          host.showroom.set_showcase_mesh(showcase_tris);
          const drive::Vec3 vpos = vehicle.position();
          // Spawn tumbling rigid-body debris whenever a bumper, side mirror, or rear wing detaches on impact.
          {
            struct DetachedTracker {
              std::string car_name;
              uint8_t prev_mask = 0u;
              bool initialized = false;
            };
            static std::unordered_map<const DriveSession*, DetachedTracker> s_detached_trackers;
            auto& trk = s_detached_trackers[this];
            const bool damage_on = (car_damage.mode != drive::DamageMode::kNone);
            const uint8_t cur_mask = damage_on
                ? uint8_t((car_damage.front_bumper_detached ? (1u << 0) : 0u) |
                          (car_damage.rear_bumper_detached ? (1u << 1) : 0u) |
                          (car_damage.left_mirror_detached ? (1u << 2) : 0u) |
                          (car_damage.right_mirror_detached ? (1u << 3) : 0u) |
                          (car_damage.wing_detached ? (1u << 4) : 0u))
                : 0u;
            if (!trk.initialized || trk.car_name != app.car) {
              trk.car_name = app.car;
              trk.prev_mask = cur_mask;
              trk.initialized = true;
              clear_detached_car_parts(this);
            } else {
              const uint8_t newly_detached = uint8_t(cur_mask & ~trk.prev_mask);
              if (newly_detached != 0u && !rewind_buf.active() && !replay_viewer.active()) {
                const drive::Quat vrot = vehicle.orientation();
                const drive::Vec3 vvel = vehicle.velocity();
                const drive::Vec3 vfwd = drive::rotate(vrot, {0, 0, 1});
                const drive::Vec3 vright = drive::rotate(vrot, {1, 0, 0});
                const drive::Vec3 vup = drive::rotate(vrot, {0, 1, 0});
                const float half_w = float(std::clamp(vehicle.params().size.x * 0.5, 0.72, 1.15));
                const float half_l = float(std::clamp(vehicle.params().size.z * 0.5, 1.65, 2.70));

                std::array<uint8_t, 4> paint_rgba{185, 44, 44, 255};
                const auto& cols = host.showroom.colours();
                const size_t cidx = host.showroom.colour_index();
                if (!cols.empty() && cidx < cols.size()) {
                  const uint32_t rgb = cols[cidx].rgb;
                  paint_rgba = {uint8_t((rgb >> 16) & 0xFFu), uint8_t((rgb >> 8) & 0xFFu), uint8_t(rgb & 0xFFu), 255u};
                }

                const std::array<float, 3> cf{float(vfwd.x), float(vfwd.y), float(vfwd.z)};
                const std::array<float, 3> cr{float(vright.x), float(vright.y), float(vright.z)};
                const std::array<float, 3> cu{float(vup.x), float(vup.y), float(vup.z)};
                const std::array<float, 3> cv{float(vvel.x), float(vvel.y), float(vvel.z)};
                const std::array<drive::Vec3, 5> local_offsets = {
                    drive::Vec3{0.0, 0.28, double(half_l)},
                    drive::Vec3{0.0, 0.30, -double(half_l)},
                    drive::Vec3{-double(half_w) - 0.08, 0.86, 0.36},
                    drive::Vec3{double(half_w) + 0.08, 0.86, 0.36},
                    drive::Vec3{0.0, 0.96, -double(half_l) + 0.18},
                };
                for (int pk = 0; pk < 5; ++pk) {
                  if ((newly_detached & (1u << pk)) == 0u) continue;
                  const drive::Vec3 wp = vpos + drive::rotate(vrot, local_offsets[size_t(pk)]);
                  spawn_detached_car_part(this, pk, {float(wp.x), float(wp.y), float(wp.z)}, cv, cf, cr, cu, paint_rgba);
                }
              }
              trk.prev_mask = cur_mask;
            }
            const float debris_dt = (rewind_buf.active() || tod_frame_dt <= 0.0)
                                        ? 0.0f
                                        : float(std::clamp(tod_frame_dt, 0.0, 0.05));
            step_and_append_detached_car_parts(this, debris_dt, host.ground, debris_tris);
          }
          app.world_anim.build_mesh(vpos.x, vpos.y + 1.5, vpos.z, float(vehicle.time()), host.ground, debris_tris, !app.low_preset);
          host.showroom.set_debris_mesh(debris_tris);
          std::vector<world::ParticleSprite> p_alpha, p_add;
          host.particle_sys.build_sprites({float(vpos.x), float(vpos.y + 1.5), float(vpos.z)}, p_alpha, p_add);
          host.showroom.set_particles(p_alpha, p_add, host.particle_sys.used_textures(), active_near, tod_cockpit_cam);
        }
}

audio::View DriveSession::audio_view() {
        return (have_rig && !app.front) ? carview::engine_audio_view(rig.mode())
                                    : (app.front ? audio::View::kFront : audio::View::kFollow);
}

void DriveSession::prune_world_rewind() {
        const auto keep = rewind_buf.active_snapshot_ids();
        for (auto it = world_rewind_snaps.begin(); it != world_rewind_snaps.end();) {
          if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) it = world_rewind_snaps.erase(it);
          else ++it;
        }
}

void DriveSession::capture_world_rewind(uint64_t snap_id) {
        if (snap_id == 0u) return;
        WorldRewindSnapshot ws;
        if (app.race) {
          ws.has_race = true;
          ws.race = app.race->capture_rewind();
        }
        if (traffic) {
          ws.has_traffic = true;
          ws.traffic = traffic->capture_rewind();
        }
        if (festival_mgr) {
          ws.has_festival = true;
          ws.festival = festival_mgr->capture_rewind();
        }
        if (app.skill_tracker) {
          ws.has_skills = true;
          ws.skills = app.skill_tracker->capture_rewind();
        }
        ws.smashables = app.smashables.capture_rewind();
        ws.car_damage = car_damage;
        // On trigger (cancel snapshot), attach exact trigger-frame actor poses to the
        // rewind stream so reverse scrub begins in seamless lockstep with zero gap.
        if (snap_id == rewind_buf.cancel_snapshot_id()) {
          auto lane_quat = [](double heading, double pitch) {
            const drive::Quat yq{0, std::sin(heading * 0.5), 0, std::cos(heading * 0.5)};
            const drive::Quat pq{std::sin(-pitch * 0.5), 0, 0, std::cos(-pitch * 0.5)};
            return drive::Quat{
                yq.w * pq.x + yq.x * pq.w + yq.y * pq.z - yq.z * pq.y,
                yq.w * pq.y - yq.x * pq.z + yq.y * pq.w + yq.z * pq.x,
                yq.w * pq.z + yq.x * pq.y - yq.y * pq.x + yq.z * pq.w,
                yq.w * pq.w - yq.x * pq.x - yq.y * pq.y - yq.z * pq.z,
            };
          };
          std::vector<drive::RewindActorPose> actors;
          actors.reserve(40);
          if (app.race) app.race->collect_rewind_actors(actors);
          if (ws.has_traffic) {
            for (const auto& c : ws.traffic.population.cars) {
              drive::RewindActorPose ap{};
              ap.kind = 1u;
              ap.id = c.id;
              const auto bit = ws.traffic.bodies.bodies.find(c.id);
              if (bit != ws.traffic.bodies.bodies.end()) {
                ap.pos = bit->second.vehicle.position();
                ap.rot = bit->second.vehicle.orientation();
                ap.vel = bit->second.vehicle.velocity();
              } else {
                ap.pos = {c.position.x, c.position.y, c.position.z};
                ap.rot = lane_quat(c.heading, c.pitch);
                ap.vel = {std::sin(c.heading) * c.speed, 0.0, std::cos(c.heading) * c.speed};
              }
              ap.steer = float(c.heading);
              ap.wheel_spin = float(c.speed);
              ap.extra0 = float(c.pitch);
              actors.push_back(ap);
            }
          }
          if (ws.has_festival) {
            for (const auto& fc : ws.festival.cars) {
              drive::RewindActorPose ap{};
              ap.kind = 2u;
              ap.id = fc.id;
              ap.pos = {fc.position.x, fc.position.y, fc.position.z};
              ap.rot = lane_quat(fc.heading, fc.pitch);
              ap.vel = {fc.vx, fc.vy, fc.vz};
              ap.steer = float(fc.heading);
              ap.wheel_spin = float(fc.speed);
              ap.extra0 = float(fc.pitch);
              actors.push_back(ap);
            }
          }
          if (!actors.empty()) rewind_buf.attach_latest_stream_actors(std::move(actors));
        }
        world_rewind_snaps[snap_id] = std::move(ws);
        prune_world_rewind();
}

void DriveSession::apply_world_rewind_scrub() {
        drive::RewindBuffer::RewindBracket br{};
        if (!rewind_buf.sampled_snapshot_bracket(br)) return;
        const auto it_a = world_rewind_snaps.find(br.snap_a);
        const auto it_b = world_rewind_snaps.find(br.snap_b);
        if (it_a == world_rewind_snaps.end() && it_b == world_rewind_snaps.end()) return;
        const WorldRewindSnapshot& ws_a = (it_a != world_rewind_snaps.end()) ? it_a->second : it_b->second;
        const WorldRewindSnapshot& ws_b = (it_b != world_rewind_snaps.end()) ? it_b->second : it_a->second;
        const double alpha = (it_a != world_rewind_snaps.end() && it_b != world_rewind_snaps.end()) ? br.alpha : 0.0;

        auto lane_quat = [](double heading, double pitch) {
          const drive::Quat yq{0, std::sin(heading * 0.5), 0, std::cos(heading * 0.5)};
          const drive::Quat pq{std::sin(-pitch * 0.5), 0, 0, std::cos(-pitch * 0.5)};
          return drive::Quat{
              yq.w * pq.x + yq.x * pq.w + yq.y * pq.z - yq.z * pq.y,
              yq.w * pq.y - yq.x * pq.z + yq.y * pq.w + yq.z * pq.x,
              yq.w * pq.z + yq.x * pq.y - yq.y * pq.x + yq.z * pq.w,
              yq.w * pq.w - yq.x * pq.x - yq.y * pq.y - yq.z * pq.z,
          };
        };
        auto lerp_angle = [](double a0, double a1, double t) {
          double d = a1 - a0;
          constexpr double kPi = 3.14159265358979323846;
          while (d > kPi) d -= 2.0 * kPi;
          while (d < -kPi) d += 2.0 * kPi;
          return a0 + d * t;
        };

        // Sample high-rate (20 Hz) world actor stream when recorded; falls back to
        // velocity-aware cubic Hermite interpolation across keyframe snapshots.
        std::vector<drive::RewindActorPose> stream_actors;
        const bool have_stream_actors = rewind_buf.sampled_actor_poses(stream_actors);
        std::unordered_map<uint32_t, const drive::RewindActorPose*> race_stream;
        std::unordered_map<uint32_t, const drive::RewindActorPose*> traffic_stream;
        std::unordered_map<uint32_t, const drive::RewindActorPose*> festival_stream;
        const drive::RewindActorPose* showcase_stream = nullptr;
        if (have_stream_actors) {
          for (const auto& ap : stream_actors) {
            if (ap.kind == 0u) race_stream.emplace(ap.id, &ap);
            else if (ap.kind == 1u) traffic_stream.emplace(ap.id, &ap);
            else if (ap.kind == 2u) festival_stream.emplace(ap.id, &ap);
            else if (ap.kind == 3u) showcase_stream = &ap;
          }
        }

        // 1. Smoothly scrub AI race rivals and Showcase aircraft.
        if (app.race && ws_a.has_race && ws_b.has_race) {
          RaceView::RewindSnapshot blended = (alpha < 0.5) ? ws_a.race : ws_b.race;
          const size_t n = blended.cars.size();
          for (size_t i = 0; i < n; ++i) {
            if (const auto sit = race_stream.find(uint32_t(i)); sit != race_stream.end()) {
              const auto& ap = *sit->second;
              drive::Vehicle::Pose p = blended.cars[i].body.pose();
              p.pos = ap.pos;
              p.rot = ap.rot;
              p.vel = ap.vel;
              p.wheels[0].steer = ap.steer;
              p.wheels[1].steer = ap.steer;
              for (size_t wi = 0; wi < 4; ++wi) p.wheels[wi].spin = ap.wheel_spin;
              blended.cars[i].body.set_pose(p);
              blended.cars[i].before = p;
            } else if (i < ws_a.race.cars.size() && i < ws_b.race.cars.size()) {
              const drive::Vehicle::Pose p = blend_vehicle_pose_hermite(
                  ws_a.race.cars[i].body.pose(), ws_b.race.cars[i].body.pose(), br.span_s, alpha);
              blended.cars[i].body.set_pose(p);
              blended.cars[i].before = p;
            }
          }
          if (ws_a.race.has_showcase && ws_b.race.has_showcase) {
            const auto& sa = ws_a.race.showcase;
            const auto& sb = ws_b.race.showcase;
            auto& sc = blended.showcase;
            sc.pose.progress_m = sa.pose.progress_m + (sb.pose.progress_m - sa.pose.progress_m) * alpha;
            sc.pose.fraction = sa.pose.fraction + (sb.pose.fraction - sa.pose.fraction) * alpha;
            sc.pose.speed_mps = sa.pose.speed_mps + (sb.pose.speed_mps - sa.pose.speed_mps) * alpha;
            sc.pose.ground_y = sa.pose.ground_y + (sb.pose.ground_y - sa.pose.ground_y) * alpha;
            sc.pose.agl_m = sa.pose.agl_m + (sb.pose.agl_m - sa.pose.agl_m) * alpha;
            sc.anim_time = sa.anim_time + (sb.anim_time - sa.anim_time) * alpha;
            sc.anim_speed_mult = sa.anim_speed_mult + (sb.anim_speed_mult - sa.anim_speed_mult) * alpha;
            sc.altitude_y = sa.altitude_y + (sb.altitude_y - sa.altitude_y) * alpha;
            sc.smoothed_pitch = sa.smoothed_pitch + (sb.smoothed_pitch - sa.smoothed_pitch) * alpha;
            sc.smoothed_roll = sa.smoothed_roll + (sb.smoothed_roll - sa.smoothed_roll) * alpha;
            if (showcase_stream) {
              sc.pose.position = {showcase_stream->pos.x, showcase_stream->pos.y, showcase_stream->pos.z};
              sc.pose.heading = showcase_stream->steer;
              sc.pose.speed_mps = showcase_stream->wheel_spin;
              sc.pose.pitch = showcase_stream->extra0;
              sc.pose.roll = showcase_stream->extra1;
              sc.pose.rotor_angle = showcase_stream->extra2;
              sc.prev_heading = sc.pose.heading;
            } else {
              const drive::Vec3 pa{sa.pose.position[0], sa.pose.position[1], sa.pose.position[2]};
              const drive::Vec3 pb{sb.pose.position[0], sb.pose.position[1], sb.pose.position[2]};
              const double cpa = std::cos(sa.pose.pitch);
              const double cpb = std::cos(sb.pose.pitch);
              const drive::Vec3 va{std::sin(sa.pose.heading) * cpa * sa.pose.speed_mps,
                                   std::sin(sa.pose.pitch) * sa.pose.speed_mps,
                                   std::cos(sa.pose.heading) * cpa * sa.pose.speed_mps};
              const drive::Vec3 vb{std::sin(sb.pose.heading) * cpb * sb.pose.speed_mps,
                                   std::sin(sb.pose.pitch) * sb.pose.speed_mps,
                                   std::cos(sb.pose.heading) * cpb * sb.pose.speed_mps};
              const drive::Vec3 p = hermite_vec3(pa, va, pb, vb, br.span_s, alpha);
              sc.pose.position = {p.x, p.y, p.z};
              sc.pose.heading = lerp_angle(sa.pose.heading, sb.pose.heading, alpha);
              sc.pose.pitch = sa.pose.pitch + (sb.pose.pitch - sa.pose.pitch) * alpha;
              sc.pose.roll = sa.pose.roll + (sb.pose.roll - sa.pose.roll) * alpha;
              sc.pose.rotor_angle = sa.pose.rotor_angle + (sb.pose.rotor_angle - sa.pose.rotor_angle) * alpha;
              sc.prev_heading = sc.pose.heading;
            }
          }
          app.race->restore_rewind(blended, false);
          app.race->set_blend(1.0);
        }

        // 2. Smoothly scrub open-world & street-race traffic.
        if (traffic && ws_a.has_traffic && ws_b.has_traffic) {
          TrafficView::RewindSnapshot blended = (alpha < 0.5) ? ws_a.traffic : ws_b.traffic;
          blended.before.clear();
          struct TrafficSampleA {
            TrafficPose pose{};
            drive::Vec3 vel{};
            double heading = 0.0;
            double pitch = 0.0;
            double speed = 0.0;
          };
          std::unordered_map<uint32_t, TrafficSampleA> map_a, map_b;
          auto fill_map = [&](const TrafficView::RewindSnapshot& src, std::unordered_map<uint32_t, TrafficSampleA>& dst) {
            dst.reserve(src.population.cars.size());
            for (const auto& c : src.population.cars) {
              TrafficSampleA s{};
              const auto bit = src.bodies.bodies.find(c.id);
              if (bit != src.bodies.bodies.end()) {
                s.pose = {bit->second.vehicle.position(), bit->second.vehicle.orientation()};
                s.vel = bit->second.vehicle.velocity();
              } else {
                s.pose = {{c.position.x, c.position.y, c.position.z}, lane_quat(c.heading, c.pitch)};
                s.vel = {std::sin(c.heading) * c.speed, 0.0, std::cos(c.heading) * c.speed};
              }
              s.heading = c.heading;
              s.pitch = c.pitch;
              s.speed = c.speed;
              dst.emplace(c.id, s);
            }
          };
          fill_map(ws_a.traffic, map_a);
          fill_map(ws_b.traffic, map_b);

          auto apply_traffic_body_pose = [&](uint32_t id, const TrafficPose& tp, const drive::Vec3& vel) {
            auto bit = blended.bodies.bodies.find(id);
            if (bit == blended.bodies.bodies.end()) return;
            auto& v = bit->second.vehicle;
            const drive::Quat cur_q = v.orientation();
            const drive::Quat inv_q{-cur_q.x, -cur_q.y, -cur_q.z, cur_q.w};
            const drive::Vec3 com_local = drive::rotate(inv_q, v.centre_of_mass() - v.position());
            drive::Vehicle::Pose p = v.pose();
            p.pos = tp.pos + drive::rotate(tp.rot, com_local);
            p.rot = tp.rot;
            p.vel = vel;
            v.set_pose(p);
          };

          for (auto& c : blended.population.cars) {
            if (const auto sit = traffic_stream.find(c.id); sit != traffic_stream.end()) {
              const auto& ap = *sit->second;
              c.position = {ap.pos.x, ap.pos.y, ap.pos.z};
              c.heading = ap.steer;
              c.pitch = ap.extra0;
              c.speed = ap.wheel_spin;
              const TrafficPose tp{ap.pos, ap.rot};
              apply_traffic_body_pose(c.id, tp, ap.vel);
              blended.before.emplace(c.id, tp);
            } else {
              const auto ait = map_a.find(c.id);
              const auto bit = map_b.find(c.id);
              if (ait != map_a.end() && bit != map_b.end()) {
                const TrafficPose tp = blend_traffic_pose_hermite(
                    ait->second.pose, ait->second.vel, bit->second.pose, bit->second.vel, br.span_s, alpha);
                const drive::Vec3 vel = ait->second.vel + (bit->second.vel - ait->second.vel) * alpha;
                c.position = {tp.pos.x, tp.pos.y, tp.pos.z};
                c.heading = lerp_angle(ait->second.heading, bit->second.heading, alpha);
                c.pitch = ait->second.pitch + (bit->second.pitch - ait->second.pitch) * alpha;
                c.speed = ait->second.speed + (bit->second.speed - ait->second.speed) * alpha;
                apply_traffic_body_pose(c.id, tp, vel);
                blended.before.emplace(c.id, tp);
              }
            }
          }
          traffic->restore_rewind(blended);
          traffic->set_blend(1.0);
        }

        // 3. Smoothly scrub roaming Festival AI & H2H rivals.
        if (festival_mgr && ws_a.has_festival && ws_b.has_festival) {
          freeroam::FestivalManager::RewindSnapshot blended = (alpha < 0.5) ? ws_a.festival : ws_b.festival;
          std::unordered_map<uint32_t, const freeroam::FestivalAiCar*> map_a, map_b;
          map_a.reserve(ws_a.festival.cars.size());
          map_b.reserve(ws_b.festival.cars.size());
          for (const auto& c : ws_a.festival.cars) map_a.emplace(c.id, &c);
          for (const auto& c : ws_b.festival.cars) map_b.emplace(c.id, &c);
          for (auto& c : blended.cars) {
            if (const auto sit = festival_stream.find(c.id); sit != festival_stream.end()) {
              const auto& ap = *sit->second;
              c.position = {ap.pos.x, ap.pos.y, ap.pos.z};
              c.heading = ap.steer;
              c.pitch = ap.extra0;
              c.speed = ap.wheel_spin;
              c.vx = ap.vel.x;
              c.vy = ap.vel.y;
              c.vz = ap.vel.z;
            } else {
              const auto ait = map_a.find(c.id);
              const auto bit = map_b.find(c.id);
              if (ait != map_a.end() && bit != map_b.end()) {
                const auto& ca = *ait->second;
                const auto& cb = *bit->second;
                const drive::Vec3 pa{ca.position.x, ca.position.y, ca.position.z};
                const drive::Vec3 pb{cb.position.x, cb.position.y, cb.position.z};
                const drive::Vec3 d = pb - pa;
                if (drive::dot(d, d) <= 62500.0) {
                  const drive::Vec3 va{ca.vx, ca.vy, ca.vz};
                  const drive::Vec3 vb{cb.vx, cb.vy, cb.vz};
                  const drive::Vec3 p = hermite_vec3(pa, va, pb, vb, br.span_s, alpha);
                  c.position = {p.x, p.y, p.z};
                  c.heading = lerp_angle(ca.heading, cb.heading, alpha);
                  c.pitch = ca.pitch + (cb.pitch - ca.pitch) * alpha;
                  c.speed = ca.speed + (cb.speed - ca.speed) * alpha;
                  c.vx = ca.vx + (cb.vx - ca.vx) * alpha;
                  c.vy = ca.vy + (cb.vy - ca.vy) * alpha;
                  c.vz = ca.vz + (cb.vz - ca.vz) * alpha;
                }
              }
            }
          }
          festival_mgr->restore_rewind(blended);
        }

        // 4. Restore smashable prop states as rewind scrubs past impacts (props pop back intact!).
        const uint64_t smash_snap = (alpha < 0.5) ? br.snap_a : br.snap_b;
        if (smash_snap != 0u && smash_snap != last_rewind_smash_snap) {
          last_rewind_smash_snap = smash_snap;
          const auto& ws_s = (alpha < 0.5) ? ws_a : ws_b;
          const auto unhidden = app.smashables.restore_rewind(ws_s.smashables);
          for (const auto& hs : unhidden) host.showroom.unhide_world_props_near(hs.pos, hs.radius);
          if (host.streamer) host.streamer->set_hidden_positions(app.smashables.broken_hidden_positions());
        }
}

void DriveSession::restore_world_rewind(uint64_t snap_id, bool is_resume) {
        last_rewind_smash_snap = 0u;
        auto it = world_rewind_snaps.find(snap_id);
        if (it != world_rewind_snaps.end()) {
          const auto& ws = it->second;
          if (app.race && ws.has_race) {
            app.race->restore_rewind(ws.race, is_resume && rewind_buf.lap_uncertified());
            app.audio_host.clear_codriver();
          }
          if (traffic && ws.has_traffic) traffic->restore_rewind(ws.traffic);
          if (festival_mgr && ws.has_festival) festival_mgr->restore_rewind(ws.festival);
          if (app.skill_tracker && ws.has_skills) app.skill_tracker->restore_rewind(ws.skills);
          const auto unhidden = app.smashables.restore_rewind(ws.smashables);
          for (const auto& hs : unhidden) host.showroom.unhide_world_props_near(hs.pos, hs.radius);
          if (host.streamer) host.streamer->set_hidden_positions(app.smashables.broken_hidden_positions());
          car_damage = ws.car_damage;
          const drive::DamageMultipliers dm = car_damage.multipliers();
          drive::Vehicle::DamageMods vdm;
          vdm.engine_torque = dm.engine_torque;
          vdm.shift_time_add = dm.shift_time_add;
          vdm.brake_power = dm.brake_power;
          vdm.aero_drag = dm.aero_drag;
          vdm.aero_downforce = dm.aero_downforce;
          vdm.tyre_grip = dm.tyre_grip;
          vdm.suspension_scale = dm.suspension_scale;
          vdm.steer_pull_rad = dm.steer_pull_rad;
          vehicle.set_damage_mods(vdm);
          host.showroom.set_damage_state(car_damage);
          clear_detached_car_parts(this);
        } else {
          app.smashables.clear_debris();
          clear_detached_car_parts(this);
        }
        prune_world_rewind();
}

namespace {

// `authored`: a cutscene's PlacePlayerCar. The original keeps the authored
// x/z and heading and drops the car onto the surface nearest the authored
// height (any surface facing up, not only road), so the cutscene's cameras
// frame it where they were authored; the road search below would move it.
void prime_world_and_snap_impl(DriveSession& s, std::array<double, 3> target_pos, double target_heading,
                               bool authored) {
  double px = target_pos[0];
  double py = target_pos[1];
  double pz = target_pos[2];
  double ph = target_heading;

  if (s.host.streamer) {
    auto sync_prime_at = [&](double cx, double cz) {
      bool any_changed = false;
      for (int wait_i = 0; wait_i < 2000 && s.host.streamer->gathering(); ++wait_i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        bool ch = false;
        s.host.streamer->update(cx, cz, &ch, &s.app.error);
        any_changed = any_changed || ch;
      }
      s.host.streamer->set_async(false);
      const double dist_centre = std::hypot(cx - s.host.streamer->centre_x(), cz - s.host.streamer->centre_z());
      if (s.host.streamer->gathers() == 0 || dist_centre > 100.0) {
        s.host.streamer->set_hidden(s.host.hidden_sent);
        bool ch = false;
        s.host.streamer->update(cx, cz, &ch, &s.app.error);
        any_changed = any_changed || ch;
      }
      s.host.streamer->set_async(s.host.streamer_async);
      if (any_changed && s.host.streamer->prepared().has_value()) {
        s.host.ground = &s.host.streamer->ground();
        s.host.follower.set_ground(&s.host.streamer->ground());
        s.host.set_staged_world();
      }
    };

    auto raycast_ground = [&](double rx, double ry_hint, double rz, double& out_y, drive::Hit& out_hit) -> bool {
      const double y_from = ry_hint + 25.0;
      if (s.host.streamer->ground().raycast({rx, y_from, rz}, {0, -1, 0}, 60.0, out_hit)) {
        out_y = y_from - out_hit.distance;
        return true;
      }
      if (s.host.streamer->ground().raycast({rx, 2000.0, rz}, {0, -1, 0}, 4000.0, out_hit)) {
        out_y = 2000.0 - out_hit.distance;
        return true;
      }
      return false;
    };

    auto is_drivable_road = [&](const drive::Hit& hit) -> bool {
      if (hit.normal.y < 0.7) return false;
      const auto& surfs = s.host.collision.surfaces();
      if (hit.surface >= 0 && size_t(hit.surface) < surfs.size()) {
        return surfs[size_t(hit.surface)].off_roadness < 0.5f;
      }
      return hit.surface <= 1;
    };

    sync_prime_at(px, pz);

    if (authored) {
      // Down from just above the authored height, and from high up: the hit
      // nearest the authored height that faces up wins.
      double best_y = py, best_d = 1e30;
      for (const double from : {py + 3.0, py + 1000.0}) {
        drive::Hit h{};
        if (!s.host.streamer->ground().raycast({px, from, pz}, {0, -1, 0}, 2000.0, h) || h.normal.y < 0.2) continue;
        const double y = from - h.distance;
        if (std::abs(y - py) < best_d) {
          best_d = std::abs(y - py);
          best_y = y;
        }
      }
      py = best_y + 0.03;
    }

    double hit_y = py;
    drive::Hit hit{};
    const bool direct_hit = !authored && raycast_ground(px, py, pz, hit_y, hit);
    if (authored) {
    } else if (direct_hit && is_drivable_road(hit)) {
      py = hit_y + 0.03;
    } else {
      world::Spawn sp;
      if (world::find_spawn(s.host.streamer->mesh(), px, pz, sp, &ph)) {
        px = sp.position[0];
        pz = sp.position[2];
        ph = sp.heading;
        if (std::hypot(px - s.host.streamer->centre_x(), pz - s.host.streamer->centre_z()) > 100.0) {
          sync_prime_at(px, pz);
        }
        double snap_y = sp.position[1];
        drive::Hit snap_hit{};
        if (raycast_ground(px, sp.position[1], pz, snap_y, snap_hit)) {
          py = snap_y + 0.03;
        } else {
          py = sp.position[1] + 0.03;
        }
        const drive::Vec3 p{px, py, pz};
        if (world::wall_between(s.host.streamer->ground(), p, p.x + 10.0 * std::sin(ph),
                                p.z + 10.0 * std::cos(ph))) {
          ph = std::remainder(ph + 3.14159265358979323846, 2.0 * 3.14159265358979323846);
        }
      } else if (direct_hit) {
        py = hit_y + 0.03;
      }
    }
  }

  s.vehicle.reset(drive::Vec3{px, py, pz}, ph);
  s.host.start = {px, py, pz};
  s.host.start_heading = ph;
  s.have_touch = true;
  s.last_touch_y = s.vehicle.position().y;
  s.camera_pull = CameraPull{};
  if (s.have_rig) s.rig.restart();
  s.rig_accel.reset();
  s.drawn_before = s.vehicle.pose();
  s.rewind_buf.reset_history(s.vehicle.time());
  s.world_rewind_snaps.clear();
  s.place_world();
  std::printf("campaign teleport: primed world at (%.2f, %.2f, %.2f) heading %.4f rad%s\n", px, py, pz, ph,
              authored ? " (as authored)" : "");
}

}  // namespace

void prime_world_and_snap_vehicle(DriveSession& s, std::array<double, 3> target_pos, double target_heading) {
  prime_world_and_snap_impl(s, target_pos, target_heading, false);
}

void prime_world_and_place_vehicle_as_authored(DriveSession& s, std::array<double, 3> target_pos,
                                               double target_heading) {
  prime_world_and_snap_impl(s, target_pos, target_heading, true);
}

void prime_world_at(DriveSession& s, double x, double z) {
  // Streams the world around (x, z) without moving the car: a cutscene's
  // places, before it plays (the original's CPrepareCutscene).
  if (!s.host.streamer) return;
  bool any_changed = false;
  for (int wait_i = 0; wait_i < 2000 && s.host.streamer->gathering(); ++wait_i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    bool ch = false;
    s.host.streamer->update(x, z, &ch, &s.app.error);
    any_changed = any_changed || ch;
  }
  s.host.streamer->set_async(false);
  s.host.streamer->set_hidden(s.host.hidden_sent);
  bool ch = false;
  s.host.streamer->update(x, z, &ch, &s.app.error);
  any_changed = any_changed || ch;
  s.host.streamer->set_async(s.host.streamer_async);
  if (any_changed && s.host.streamer->prepared().has_value()) {
    s.host.ground = &s.host.streamer->ground();
    s.host.follower.set_ground(&s.host.streamer->ground());
    s.host.set_staged_world();
  }
  std::printf("cutscene preload: primed world at (%.1f, %.1f)\n", x, z);
}

}  // namespace carview
