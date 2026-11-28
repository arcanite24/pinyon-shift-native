#include "showroom_internal.h"

#include "cars/bake.h"
#include "cars/database.h"
#include "cars/mods.h"
#include "cars/sources.h"
#include "cars/texture_mods.h"
#include "fh1data/xds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <regex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace carview {

namespace {

// Thread-safe shared in-memory disc car/wheel/tyre/shared cache for zero-bake
// direct-from-disc loading. Decodes Shared.zip and TireA/TireC once per session
// and caches recently decoded wheels and cars in bounded LRUs so car swaps,
// traffic spawning, and race opponent loading are fast and parallel-safe.
class DiscCarCache {
 public:
  static DiscCarCache& instance() {
    static DiscCarCache cache;
    return cache;
  }

  void set_root(const std::filesystem::path& game_root, const std::filesystem::path& state_root) {
    std::lock_guard<std::mutex> lock(mu_);
    if (game_root_ == game_root && state_root_ == state_root) return;
    game_root_ = game_root;
    state_root_ = state_root;
    reset_locked();
  }

  std::filesystem::path configured_game_root() const {
    std::lock_guard<std::mutex> lock(mu_);
    return game_root_;
  }

  bool discover_cars(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                     std::map<int64_t, std::string>& out_ids, std::vector<std::string>& out_media,
                     std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_init_locked(game_root, state_root, error)) return false;
    out_ids.clear();
    out_media.clear();
    std::set<std::string> media_set;
    for (const auto& [key, row] : chosen_) {
      if (!install_.find_car(row->media_name)) continue;
      media_set.insert(key);
      for (const cars::CarRow* r : all_rows_[key]) {
        out_ids[r->id] = r->media_name;
      }
    }
    out_media.assign(media_set.begin(), media_set.end());
    return !out_media.empty();
  }

  bool load_car(const std::filesystem::path& game_root, const std::string& car_name,
                cars::bake::Bake& out, std::string* error) {
    const std::string key = lower(car_name);
    cars::FolderRef ref;
    cars::CarRow chosen_row;
    std::vector<cars::bake::StockRow> stock_rows;
    std::string own_tyre;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!ensure_init_locked(game_root, state_root_, error)) return false;
      if (auto hit = lru_get(car_lru_, car_lru_map_, key)) {
        out = *hit;
        return true;
      }
      auto it = chosen_.find(key);
      if (it == chosen_.end() || !it->second) {
        if (error) *error = "car '" + car_name + "' not found in game database";
        return false;
      }
      auto found = install_.find_car(it->second->media_name);
      if (!found) {
        if (error) *error = "car folder '" + it->second->media_name + "' not found on disc";
        return false;
      }
      ref = *found;
      chosen_row = *it->second;
      // Match apps/carbake/main.cpp 1:1: baked row first, then all other Data_Car rows sharing this model.
      const cars::CarRow* primary = it->second;
      stock_rows.push_back({primary->id, primary->stock_parts, primary->removed_parts, primary->race_livery,
                            primary->has_stock_body});
      if (auto ait = all_rows_.find(key); ait != all_rows_.end()) {
        for (const cars::CarRow* r : ait->second) {
          if (r != primary) {
            stock_rows.push_back({r->id, r->stock_parts, r->removed_parts, r->race_livery, r->has_stock_body});
          }
        }
      }
      const std::string candidate_tyre = std::string(cars::kTyreTextures) + key;
      if (install_.find_wheel(candidate_tyre)) own_tyre = candidate_tyre;
    }

    auto decoded = std::make_shared<cars::bake::Bake>();
    std::string err;
    if (!cars::bake_car(ref, chosen_row, *decoded, &err)) {
      if (error) *error = err;
      return false;
    }
    if (!own_tyre.empty()) {
      decoded->car.tyre_media = own_tyre;
      decoded->car.tyre_texture_media = own_tyre;
    }
    decoded->car.stock_rows = std::move(stock_rows);

    {
      std::lock_guard<std::mutex> lock(mu_);
      lru_put(car_lru_, car_lru_map_, key, decoded, kMaxCachedCars);
    }
    out = *decoded;
    return true;
  }

  bool load_wheel(const std::filesystem::path& game_root, const std::string& wheel_media, bool tyre,
                  cars::bake::Bake& out, std::string* error) {
    const std::string key = lower(wheel_media);
    const std::string cache_key = (tyre ? "t:" : "w:") + key;
    cars::FolderRef ref;
    bool have_ref = false;
    std::filesystem::path eff_root;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (ensure_init_locked(game_root, state_root_, nullptr)) {
        if (auto hit = lru_get(wheel_lru_, wheel_lru_map_, cache_key)) {
          out = *hit;
          return true;
        }
        if (auto found = install_.find_wheel(key)) {
          ref = *found;
          have_ref = true;
        }
      }
      eff_root = !game_root.empty() ? game_root : game_root_;
    }

    auto decoded = std::make_shared<cars::bake::Bake>();
    std::string err;
    bool ok = false;
    if (have_ref) {
      ok = cars::bake_wheel(ref, tyre, *decoded, &err);
    } else if (!eff_root.empty()) {
      ok = cars::load_wheel_from_disc(eff_root, wheel_media, tyre, *decoded, &err);
    } else {
      err = "no game root for wheel " + wheel_media;
    }
    if (!ok) {
      if (error) *error = err;
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      lru_put(wheel_lru_, wheel_lru_map_, cache_key, decoded, kMaxCachedWheels);
    }
    out = *decoded;
    return true;
  }

  bool load_shared(const std::filesystem::path& game_root, cars::bake::Bake& out, std::string* error) {
    cars::FolderRef ref;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!ensure_init_locked(game_root, state_root_, error)) return false;
      if (shared_bake_) {
        out = *shared_bake_;
        return true;
      }
      auto it = install_.base_cars.find("shared");
      if (it == install_.base_cars.end()) {
        if (error) *error = "no media/cars/Shared.zip on disc";
        return false;
      }
      ref = it->second;
    }

    auto decoded = std::make_shared<cars::bake::Bake>();
    std::string err;
    if (!cars::bake_shared(ref, *decoded, &err)) {
      if (error) *error = err;
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!shared_bake_) shared_bake_ = decoded;
      out = *shared_bake_;
    }
    return true;
  }

  bool find_car_ref(const std::filesystem::path& game_root, const std::string& car_name,
                    cars::FolderRef& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_init_locked(game_root, state_root_, nullptr)) return false;
    auto ref = install_.find_car(car_name);
    if (!ref) return false;
    out = *ref;
    return true;
  }

 private:
#ifdef __ANDROID__
  // Android unified RAM: keep only the active car + immediate wheel cache on CPU.
  static constexpr size_t kMaxCachedCars = 2;
  static constexpr size_t kMaxCachedWheels = 4;
#else
  static constexpr size_t kMaxCachedCars = 32;
  static constexpr size_t kMaxCachedWheels = 32;
#endif

  using LruList = std::list<std::pair<std::string, std::shared_ptr<const cars::bake::Bake>>>;
  using LruMap = std::unordered_map<std::string, LruList::iterator>;

  static std::shared_ptr<const cars::bake::Bake> lru_get(LruList& lru, LruMap& map, const std::string& key) {
    auto it = map.find(key);
    if (it == map.end()) return nullptr;
    lru.splice(lru.begin(), lru, it->second);
    return it->second->second;
  }

  static void lru_put(LruList& lru, LruMap& map, const std::string& key,
                      std::shared_ptr<const cars::bake::Bake> val, size_t max_entries) {
    auto it = map.find(key);
    if (it != map.end()) {
      it->second->second = std::move(val);
      lru.splice(lru.begin(), lru, it->second);
      return;
    }
    lru.emplace_front(key, std::move(val));
    map[key] = lru.begin();
    while (lru.size() > max_entries) {
      map.erase(lru.back().first);
      lru.pop_back();
    }
  }

  void reset_locked() {
    initialized_ = false;
    install_ = {};
    db_ = {};
    chosen_.clear();
    all_rows_.clear();
    shared_bake_.reset();
    wheel_lru_.clear();
    wheel_lru_map_.clear();
    car_lru_.clear();
    car_lru_map_.clear();
  }

  bool ensure_init_locked(const std::filesystem::path& req_game, const std::filesystem::path& req_state,
                          std::string* error) {
    const std::filesystem::path eff_game = !req_game.empty() ? req_game : game_root_;
    const std::filesystem::path eff_state = !req_state.empty() ? req_state : state_root_;
    if (eff_game.empty()) {
      if (error) *error = "no game root configured for direct-from-disc loading";
      return false;
    }
    if (initialized_ && game_root_ == eff_game && state_root_ == eff_state) return true;
    reset_locked();
    game_root_ = eff_game;
    state_root_ = eff_state;
    std::string err;
    if (!cars::discover_install(game_root_, state_root_, "", install_, &err) ||
        !cars::load_database(install_, db_, &err)) {
      if (error) *error = err;
      return false;
    }
    // One primary row per distinct media name: selectable first, then lowest Id (matches carbake).
    for (const auto& r : db_.cars) {
      const std::string key = cars::lower(r.media_name);
      auto& slot = chosen_[key];
      if (!slot || (r.selectable && !slot->selectable)) slot = &r;
      all_rows_[key].push_back(&r);
    }
    initialized_ = true;
    return true;
  }

  mutable std::mutex mu_;
  std::filesystem::path game_root_;
  std::filesystem::path state_root_;
  bool initialized_ = false;
  cars::Install install_;
  cars::Database db_;
  std::map<std::string, const cars::CarRow*> chosen_;
  std::map<std::string, std::vector<const cars::CarRow*>> all_rows_;
  std::shared_ptr<const cars::bake::Bake> shared_bake_;
  LruList wheel_lru_;
  LruMap wheel_lru_map_;
  LruList car_lru_;
  LruMap car_lru_map_;
};

struct CarAnimEventsFlags {
  bool has_rear_wing = false;
  bool has_headlight = false;
  bool has_engine_vent = false;
};

CarAnimEventsFlags parse_car_anim_events_xml(const std::string& xml) {
  CarAnimEventsFlags out;
  const std::string lx = lower(xml);
  size_t pos = 0;
  while ((pos = lx.find('<', pos)) != std::string::npos) {
    const size_t end = lx.find('>', pos + 1);
    if (end == std::string::npos) break;
    const std::string tag = lx.substr(pos + 1, end - pos - 1);
    pos = end + 1;
    if (tag.rfind("part", 0) != 0 && tag.rfind("physicspart", 0) != 0) continue;
    const size_t np = tag.find("name=\"");
    if (np == std::string::npos) continue;
    const size_t v0 = np + 6;
    const size_t v1 = tag.find('"', v0);
    if (v1 == std::string::npos) continue;
    const std::string name = tag.substr(v0, v1 - v0);
    if (name.find("headlight") != std::string::npos) {
      out.has_headlight = true;
    } else if (name.find("wing") != std::string::npos || name.find("spoiler") != std::string::npos) {
      out.has_rear_wing = true;
    } else if (name.find("vent") != std::string::npos) {
      out.has_engine_vent = true;
    }
  }
  return out;
}

struct ParsedCarAttribs {
  std::array<float, 3> driver_hands_offset{0.0f, 0.0f, 0.0f};
  float driver_hands_scale = 1.0f;
  std::array<float, 3> wheel_pos_offset{0.0f, 0.0f, 0.0f};
  float wheel_rot_offset_deg = 0.0f;
  bool has_wheel_pos = false;
  bool has_wheel_rot = false;
  drive::CockpitGaugeCalibration gauges;
};

ParsedCarAttribs parse_car_attribs_xml(const std::string& xml) {
  ParsedCarAttribs out;
  const std::string lx = lower(xml);

  auto read_attr_f = [](const std::string& tag, const char* key, float& dst) -> bool {
    const std::string k = std::string(key) + "=\"";
    size_t p = 0;
    while ((p = tag.find(k, p)) != std::string::npos) {
      if (p == 0 || std::isspace(static_cast<unsigned char>(tag[p - 1]))) {
        char* stop = nullptr;
        const float v = std::strtof(tag.c_str() + p + k.size(), &stop);
        if (stop != tag.c_str() + p + k.size() && std::isfinite(v)) {
          dst = v;
          return true;
        }
        return false;
      }
      ++p;
    }
    return false;
  };

  auto read_tag_scalar = [&](const char* tag_name, float& dst) -> bool {
    const std::string open = std::string("<") + tag_name;
    size_t p = 0;
    while ((p = lx.find(open, p)) != std::string::npos) {
      const size_t after = p + open.size();
      if (after < lx.size() &&
          (std::isspace(static_cast<unsigned char>(lx[after])) || lx[after] == '/' || lx[after] == '>')) {
        const size_t gt = lx.find('>', after);
        if (gt == std::string::npos) break;
        const std::string attrs = lx.substr(after, gt - after);
        if (read_attr_f(attrs, "value", dst) || read_attr_f(attrs, "val", dst)) return true;
        if (gt > after && lx[gt - 1] != '/') {
          const size_t close = lx.find('<', gt + 1);
          if (close != std::string::npos && close > gt + 1) {
            char* stop = nullptr;
            const std::string body = lx.substr(gt + 1, close - (gt + 1));
            const float v = std::strtof(body.c_str(), &stop);
            if (stop != body.c_str() && std::isfinite(v)) {
              dst = v;
              return true;
            }
          }
        }
      }
      p = after;
    }
    return false;
  };

  auto read_tag_vec3 = [&](const char* tag_name, std::array<float, 3>& dst) -> bool {
    const std::string open = std::string("<") + tag_name;
    size_t p = 0;
    while ((p = lx.find(open, p)) != std::string::npos) {
      const size_t after = p + open.size();
      if (after < lx.size() &&
          (std::isspace(static_cast<unsigned char>(lx[after])) || lx[after] == '/' || lx[after] == '>')) {
        const size_t gt = lx.find('>', after);
        if (gt == std::string::npos) break;
        const std::string attrs = lx.substr(after, gt - after);
        bool got = false;
        got |= read_attr_f(attrs, "x", dst[0]);
        got |= read_attr_f(attrs, "y", dst[1]);
        got |= read_attr_f(attrs, "z", dst[2]);
        if (got) return true;
        const size_t vp = attrs.find("value=\"");
        if (vp != std::string::npos) {
          float vx = 0, vy = 0, vz = 0;
          if (std::sscanf(attrs.c_str() + vp + 7, "%f , %f , %f", &vx, &vy, &vz) == 3 ||
              std::sscanf(attrs.c_str() + vp + 7, "%f %f %f", &vx, &vy, &vz) == 3) {
            dst = {vx, vy, vz};
            return true;
          }
        }
      }
      p = after;
    }
    return false;
  };

  read_tag_vec3("driverhandsoffset", out.driver_hands_offset);
  read_tag_scalar("driverhandsoffsetx", out.driver_hands_offset[0]);
  read_tag_scalar("driverhandsoffsety", out.driver_hands_offset[1]);
  read_tag_scalar("driverhandsoffsetz", out.driver_hands_offset[2]);
  {
    std::array<float, 3> hs{1.0f, 1.0f, 1.0f};
    if (read_tag_scalar("driverhandsscale", out.driver_hands_scale)) {
      out.driver_hands_scale = std::clamp(out.driver_hands_scale, 0.5f, 1.6f);
    } else if (read_tag_vec3("driverhandsscale", hs)) {
      out.driver_hands_scale = std::clamp((hs[0] + hs[1] + hs[2]) / 3.0f, 0.5f, 1.6f);
    }
  }
  out.has_wheel_pos |= read_tag_vec3("cockpitwheelpositionoffset", out.wheel_pos_offset);
  out.has_wheel_pos |= read_tag_scalar("cockpitwheelpositionoffsetx", out.wheel_pos_offset[0]);
  out.has_wheel_pos |= read_tag_scalar("cockpitwheelpositionoffsety", out.wheel_pos_offset[1]);
  out.has_wheel_pos |= read_tag_scalar("cockpitwheelpositionoffsetz", out.wheel_pos_offset[2]);
  out.has_wheel_rot |= read_tag_scalar("cockpitwheelrotationoffset", out.wheel_rot_offset_deg);

  // Parse <Gauges><Speed/Tach/Fuel/Boost/Power> <TickMark> tables (CAR_LAMPS.md §7.2).
  std::string_view gauge_scope = lx;
  if (const size_t g0 = lx.find("<gauges"); g0 != std::string::npos) {
    const size_t g1 = lx.find("</gauges>", g0);
    if (g1 != std::string::npos) gauge_scope = std::string_view(lx).substr(g0, g1 - g0 + 9);
  }
  auto parse_gauge_block = [&](std::initializer_list<const char*> names, drive::GaugeConfig& cfg, bool& has_flag) {
    for (const char* nm : names) {
      const std::string open = std::string("<") + nm;
      const std::string close = std::string("</") + nm + ">";
      size_t p = 0;
      while ((p = gauge_scope.find(open, p)) != std::string_view::npos) {
        const size_t after = p + open.size();
        if (after < gauge_scope.size() &&
            (std::isspace(static_cast<unsigned char>(gauge_scope[after])) || gauge_scope[after] == '>')) {
          const size_t end = gauge_scope.find(close, after);
          if (end == std::string_view::npos) break;
          const std::string_view blk = gauge_scope.substr(p, end - p + close.size());
          if (drive::parse_gauge_tick_marks_xml(blk, cfg)) {
            const size_t gt = blk.find('>');
            const std::string head = gt != std::string_view::npos ? std::string(blk.substr(0, gt)) : std::string();
            float mn = cfg.cal_points.front().second;
            float mx = cfg.cal_points.back().second;
            (void)read_attr_f(head, "min", mn);
            (void)read_attr_f(head, "minval", mn);
            (void)read_attr_f(head, "max", mx);
            (void)read_attr_f(head, "maxval", mx);
            cfg.min_val = mn;
            cfg.max_val = mx;
            has_flag = true;
            return;
          }
          p = end + close.size();
        } else {
          p = after;
        }
      }
    }
  };
  parse_gauge_block({"speed", "speedometer", "speedgauge"}, out.gauges.speed, out.gauges.has_speed);
  parse_gauge_block({"tach", "tachometer", "rpm", "tachgauge"}, out.gauges.tach, out.gauges.has_tach);
  parse_gauge_block({"fuel", "fuelgauge"}, out.gauges.fuel, out.gauges.has_fuel);
  parse_gauge_block({"boost", "boostgauge"}, out.gauges.boost, out.gauges.has_boost);
  parse_gauge_block({"power", "powergauge"}, out.gauges.power, out.gauges.has_power);
  return out;
}

}  // namespace

bool open_disc_car_folder(const std::filesystem::path& game_root, const std::string& car_name,
                          cars::Folder& out) {
  cars::FolderRef ref;
  if (!game_root.empty() && DiscCarCache::instance().find_car_ref(game_root, car_name, ref)) {
    if (out.open(ref, nullptr)) return true;
  }
  if (!game_root.empty() && !car_name.empty()) {
    const std::string car_l = lower(car_name);
    std::filesystem::path zip_path = game_root / "media" / "cars" / (car_l + ".zip");
    if (!std::filesystem::exists(zip_path)) zip_path = game_root / "media" / "cars" / (car_name + ".zip");
    if (std::filesystem::exists(zip_path)) {
      cars::FolderRef fallback;
      fallback.kind = cars::FolderRef::Kind::kArchive;
      fallback.path = zip_path;
      fallback.name = car_name;
      if (out.open(fallback, nullptr)) return true;
    }
  }
  return false;
}

void Showroom::set_disc_root(const std::filesystem::path& game_root, const std::filesystem::path& state_root) {
  DiscCarCache::instance().set_root(game_root, state_root);
}

bool Showroom::discover_disc_cars(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                                  std::map<int64_t, std::string>& out_ids, std::vector<std::string>& out_media,
                                  std::string* error) {
  return DiscCarCache::instance().discover_cars(game_root, state_root, out_ids, out_media, error);
}

// The model's own name for a sheet, e.g. "tyre:sidewall" with tyre textures
// "tirea" (or a car's own "tireatoy_fjcruiser_12") -> "sidewalla_clr.xds",
// and "tyre:sidewall_nrm" -> "sidewalla_nrm.xds".
std::string tyre_texture_member(const std::string& sheet, const std::string& tyre_media) {
  const char letter = tyre_media.size() > 4 ? tyre_media[4] : 'a';
  if (ends_with(sheet, "_nrm")) return sheet.substr(0, sheet.size() - 4) + letter + "_nrm.xds";
  return sheet + letter + "_clr.xds";
}

// Upgrade parts ("bumperFrace", "wingrace", "cagerace") are not drawn, and
// of a variant set ("bumperFa".."bumperFd") only the first real one, where
// `real` holds the lower-case names of parts that are not stand-ins (spec
// question Q4).
bool hidden_variant(const std::string& name, const std::set<std::string>& real) {
  const std::string n = lower(name);
  if (ends_with(n, "race")) return true;
  if (n.size() > 1) {
    const char last = n.back();
    if (last >= 'b' && last <= 'h') {
      const std::string stem = n.substr(0, n.size() - 1);
      for (char c = 'a'; c < last; ++c)
        if (real.count(stem + c)) return true;
    }
  }
  return false;
}

// FORMAT-CARS, Stock parts: the upgrade-family member a part name is or
// extends ("winga_arm1" follows "winga"), or "" when it is in no family
// (bumpers, skirts, hood and wing by letter or race, the race hoods, the
// roll cage, and the exhausts, which no table names).
std::string family_member(const std::string& name) {
  static const std::regex kVariant(
      "(bumperf|bumperr|bumperframef|bumperframer|skirtl|skirtr|hood|wing|wingendl|wingendr|wingcenter|wing[1-3]|"
      "trunk|trunk1|grille|mirrorl|mirrorr)([a-z]|race)|"
      "hood(ps|ci)race|cagerace|exhaust[lr]?[a-z]|(glass|lexan)[a-z0-9]*b");
  const std::string n = lower(name);
  const std::string base = n.substr(0, n.find('_'));
  return std::regex_match(base, kVariant) ? base : std::string();
}

// Whether a family member is drawn for a stock row: exhausts by letter a,
// the rest when a stock row names it.
bool stock_hidden(const std::string& member, const cars::bake::StockRow& stock) {
  static const std::regex kExhaust("exhaust[lr]?([a-z])");
  std::smatch m;
  if (std::regex_match(member, m, kExhaust)) return m[1].str() != "a";
  return !std::binary_search(stock.parts.begin(), stock.parts.end(), member);
}

// Some LOD0, cockpit, brake and rim models hold stand-ins instead of
// geometry: a cube of exactly 17 vertices (0 to 0.2 m across; 506 such
// pools over the 240 cars and their wheels, while every real 17-vertex pool
// is a flat pane), a quad or triangle pair of at most 6 vertices, or a box
// of at most 24 vertices all on its bounding box's corners (one car's whole
// LOD0 body). Such pools are not drawn.
bool stub_pool(const std::vector<bake::Vertex>& pool) {
  if (pool.size() <= 6) return true;
  if (pool.size() > 24) return false;
  std::array<float, 3> lo = pool[0].position, hi = lo;
  for (const auto& v : pool)
    for (size_t k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], v.position[k]);
      hi[k] = std::max(hi[k], v.position[k]);
    }
  const float x = hi[0] - lo[0], y = hi[1] - lo[1], z = hi[2] - lo[2];
  if (pool.size() == 17 && std::max({x, y, z}) - std::min({x, y, z}) <= 0.02f) return true;
  for (const auto& v : pool)
    for (size_t k = 0; k < 3; ++k)
      if (std::fabs(v.position[k] - lo[k]) > 0.001f && std::fabs(v.position[k] - hi[k]) > 0.001f) return false;
  return true;
}

// The part's best drawable LOD: the lowest whose pool is not a stand-in,
// or -1. When min_lod > 0, picks the lowest lod >= min_lod (falling back to
// highest available lod >= 1 before LOD0).
int32_t best_lod(const bake::Part& part, int32_t min_lod) {
  std::set<int32_t> lods;
  for (const auto& s : part.subsections)
    if ((s.material != MaterialClass::kHidden || cars::is_blur_rim_technique(s.technique)) && !s.indices.empty())
      lods.insert(s.lod);
  if (min_lod > 0 && !stub_pool(part.lod_vertices)) {
    for (int32_t lod : lods) {
      if (lod >= min_lod) return lod;
    }
    int32_t highest_ext = -1;
    for (int32_t lod : lods) {
      if (lod >= 1) highest_ext = lod;
    }
    if (highest_ext >= 1) return highest_ext;
  }
  // A LOD0 pool with under a quarter of the LOD1-5 pool's vertices is a
  // stand-in too (some rims: 74 against about 2,300).
  const bool lod0_coarse = lods.count(0) && lods.size() > 1 &&
                           part.lod0_vertices.size() * 4 < part.lod_vertices.size();
  for (int32_t lod : lods) {
    if (lod == 0 && lod0_coarse) continue;
    if (!stub_pool(lod == 0 ? part.lod0_vertices : part.lod_vertices)) return lod;
  }
  return -1;
}

int32_t far_lod_for(const bake::Part& part, int32_t near_lod) {
  if (near_lod < 1 || stub_pool(part.lod_vertices)) return -1;
  std::set<int32_t> lods;
  for (const auto& s : part.subsections)
    if (s.material != MaterialClass::kHidden && !s.indices.empty() && s.lod > near_lod)
      lods.insert(s.lod);
  // Prefer LOD2 or LOD3 for distant AI/shadow silhouettes without collapsing to ultra-blocky LOD5.
  for (int32_t lod : lods) {
    if (lod >= 2 && lod <= 3) return lod;
  }
  return lods.empty() ? -1 : *lods.begin();
}

bool placeholder(const bake::Part& part) { return best_lod(part) < 0; }

Showroom::~Showroom() {
  VkDevice d = r_.device_;
  if (!d) return;
  if (!skip_wait_idle_on_destroy_) r_.wait_idle();
  rt_pipeline_.reset();
  rt_scene_.reset();
  drop_pending_world();
  drop_fading_world(true);
  stop_reaper();
  gpu_world_.reset();
  if (!guest_) r_.set_scene({});
  for (auto& t : gpu_textures_) r_.destroy_image(t);
  r_.destroy_image(custom_livery_img_);
  r_.destroy_image(environment_);
  r_.destroy_image(shared_dirt_pattern_);
  r_.destroy_image(shared_dirt_mask_);
  r_.destroy_image(car_dirt_mask_);
  r_.destroy_image(shared_glass_crack_);
  r_.destroy_image(driver_tex_);
  r_.destroy_buffer(vertex_buffer_);
  r_.destroy_buffer(index_buffer_);
  destroy_ubo_ring(frame_buffer_);
  destroy_ubo_ring(shadow_frame_buffer_);
  destroy_ubo_ring(shadow_frame_mid_buffer_);
  destroy_ubo_ring(shadow_frame_far_buffer_);
  destroy_ubo_ring(env_frame_buffer_);
  destroy_ubo_ring(env_sky_ubo_);
  if (env_world_opaque_) vkDestroyPipeline(d, env_world_opaque_, nullptr);
  if (env_world_coverage_) vkDestroyPipeline(d, env_world_coverage_, nullptr);
  if (env_car_opaque_) vkDestroyPipeline(d, env_car_opaque_, nullptr);
  if (env_sky_pipe_base_) vkDestroyPipeline(d, env_sky_pipe_base_, nullptr);
  if (env_sky_pipe_layers_) vkDestroyPipeline(d, env_sky_pipe_layers_, nullptr);
  if (env_prefilter_pipe_) vkDestroyPipeline(d, env_prefilter_pipe_, nullptr);
  if (env_prefilter_pipe_layout_) vkDestroyPipelineLayout(d, env_prefilter_pipe_layout_, nullptr);
  if (env_prefilter_set_layout_) vkDestroyDescriptorSetLayout(d, env_prefilter_set_layout_, nullptr);
  if (env_pool_) vkDestroyDescriptorPool(d, env_pool_, nullptr);
  if (dyn_env_mip0_cube_) vkDestroyImageView(d, dyn_env_mip0_cube_, nullptr);
  for (uint32_t m = 0; m < kDynEnvMips; ++m)
    for (int i = 0; i < 6; ++i)
      if (dyn_env_mip_faces_[m][i]) vkDestroyImageView(d, dyn_env_mip_faces_[m][i], nullptr);
  for (int i = 0; i < 6; ++i)
    if (dyn_env_faces_[i]) vkDestroyImageView(d, dyn_env_faces_[i], nullptr);
  r_.destroy_image(dyn_env_);
  r_.destroy_image(dyn_env_depth_);
  r_.destroy_image(mirror_tex_);
  r_.destroy_image(mirror_depth_);
  r_.destroy_buffer(world_vertices_);
  r_.destroy_buffer(world_indices_);
  destroy_dyn_buffer(overlay_buf_);
  for (auto& [record, t] : world_textures_) r_.destroy_image(t);
  for (VkDescriptorPool p : world_pools_) vkDestroyDescriptorPool(d, p, nullptr);
  if (sampler_) vkDestroySampler(d, sampler_, nullptr);
  if (material_pool_) vkDestroyDescriptorPool(d, material_pool_, nullptr);
  if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
  for (VkPipeline p : {opaque_, opaque_after_z_, opaque_cutout_, opaque_cutout_after_z_, opaque_fade_,
                       car_z_prepass_, glass_, glass_mul_, glass_add_, mask_,
                       contact_shadow_pipe_, world_opaque_, world_opaque_after_z_, world_opaque_dither_,
                       world_z_prepass_, world_coverage_, world_alpha_, world_water_, shadow_world_, shadow_cutout_, shadow_car_,
                       sky_pipe_base_, sky_pipe_layers_, stars_pipe_})
    if (p) vkDestroyPipeline(d, p, nullptr);
  if (contact_shadow_pipe_layout_) vkDestroyPipelineLayout(d, contact_shadow_pipe_layout_, nullptr);
  if (sky_pipe_layout_) vkDestroyPipelineLayout(d, sky_pipe_layout_, nullptr);
  if (stars_pipe_layout_) vkDestroyPipelineLayout(d, stars_pipe_layout_, nullptr);
  if (sky_set_layout_) vkDestroyDescriptorSetLayout(d, sky_set_layout_, nullptr);
  if (sky_pool_) vkDestroyDescriptorPool(d, sky_pool_, nullptr);
  destroy_ubo_ring(sky_ubo_);
  r_.destroy_buffer(star_vertices_);
  r_.destroy_image(sky_sun_);
  for (auto& m : sky_moons_) r_.destroy_image(m);
  r_.destroy_image(sky_dither_);
  r_.destroy_image(sky_far_f_);
  r_.destroy_image(sky_far_b_);
  r_.destroy_image(sky_far_mask_);
  r_.destroy_image(sky_close_ad_f_);
  r_.destroy_image(sky_close_ad_b_);
  r_.destroy_image(sky_close_am_f_);
  r_.destroy_image(sky_close_am_b_);
  r_.destroy_image(sky_close_tb_f_);
  r_.destroy_image(sky_close_tb_b_);
  if (glow_pipe_) vkDestroyPipeline(d, glow_pipe_, nullptr);
  if (glow_pipe_layout_) vkDestroyPipelineLayout(d, glow_pipe_layout_, nullptr);
  if (glow_set_layout_) vkDestroyDescriptorSetLayout(d, glow_set_layout_, nullptr);
  if (glow_pool_) vkDestroyDescriptorPool(d, glow_pool_, nullptr);
  destroy_dyn_buffer(glow_buf_);
  for (auto& g : glow_tex_) r_.destroy_image(g);
  r_.destroy_image(headlight_cookie_);
  destroy_ubo_ring(headlights_ubo_);
  destroy_dyn_buffer(car_inst_buf_);
  r_.destroy_buffer(empty_inst_ssbo_);
  destroy_dyn_buffer(particle_buf_);
  destroy_dyn_buffer(skid_asphalt_buf_);
  destroy_dyn_buffer(skid_rut_buf_);
  destroy_dyn_buffer(debris_buf_);
  destroy_dyn_buffer(spectator_buf_);
  r_.destroy_image(spectator_atlas_);
  spectator_set_ = VK_NULL_HANDLE;
  destroy_dyn_buffer(showcase_buf_);
  destroy_dyn_buffer(driver_buf_);
  r_.destroy_image(showcase_tex_);
  r_.destroy_image(particle_atlas_);
  r_.destroy_image(skid_asphalt_tex_);
  r_.destroy_image(skid_rut_tex_);
  if (particle_alpha_pipe_) vkDestroyPipeline(d, particle_alpha_pipe_, nullptr);
  if (particle_add_pipe_) vkDestroyPipeline(d, particle_add_pipe_, nullptr);
  if (particle_pipe_layout_) vkDestroyPipelineLayout(d, particle_pipe_layout_, nullptr);
  if (particle_set_layout_) vkDestroyDescriptorSetLayout(d, particle_set_layout_, nullptr);
  if (particle_pool_) vkDestroyDescriptorPool(d, particle_pool_, nullptr);
  if (particle_sampler_) vkDestroySampler(d, particle_sampler_, nullptr);
  if (skid_pipe_) vkDestroyPipeline(d, skid_pipe_, nullptr);
  if (skid_pipe_layout_) vkDestroyPipelineLayout(d, skid_pipe_layout_, nullptr);
  if (skid_set_layout_) vkDestroyDescriptorSetLayout(d, skid_set_layout_, nullptr);
  if (skid_pool_) vkDestroyDescriptorPool(d, skid_pool_, nullptr);
  if (skid_sampler_) vkDestroySampler(d, skid_sampler_, nullptr);
  r_.destroy_image(flat_normal_tex_);
  if (world_pipeline_layout_) vkDestroyPipelineLayout(d, world_pipeline_layout_, nullptr);
  if (pipeline_layout_) vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
  if (frame_layout_) vkDestroyDescriptorSetLayout(d, frame_layout_, nullptr);
  if (world_material_layout_) vkDestroyDescriptorSetLayout(d, world_material_layout_, nullptr);
  if (material_layout_) vkDestroyDescriptorSetLayout(d, material_layout_, nullptr);
}

bool Showroom::load(const std::filesystem::path& bake_dir, const std::string& car,
                    const ShowroomOptions& options, std::string& error) {
  car_ = {};
  wheel_ = {};
  tyre_ = {};
  tyre_textures_ = {};
  options_ = options;
  const std::filesystem::path disc_root =
      !options_.game_root.empty() ? options_.game_root : DiscCarCache::instance().configured_game_root();
  if (options_.game_root.empty() && !disc_root.empty()) options_.game_root = disc_root;
  const std::filesystem::path shared_key = !bake_dir.empty() ? bake_dir : disc_root;
  // Keep shared_ loaded across hot car reloads when bake_dir / disc_root hasn't changed.
  if (shared_bake_dir_ != shared_key) {
    shared_ = {};
    shader_settings_ = {};
    shared_bake_dir_.clear();
  }
  if (!shader_settings_.loaded_from_xml && !disc_root.empty()) {
    const auto shared_zip = disc_root / "media" / "cars" / "Shared.zip";
    std::error_code sec;
    if (std::filesystem::is_regular_file(shared_zip, sec)) {
      cars::load_shader_settings(shared_zip, shader_settings_, nullptr);
    }
  }
  vertices_.clear();
  indices_.clear();
  draws_.clear();
  far_draws_.clear();
  textures_.clear();
  texture_by_key_.clear();
  descriptor_by_textures_.clear();
  drawn_parts_.clear();
  skipped_parts_.clear();
  notes_.clear();
  have_bounds_ = false;
  bounds_min_ = {};
  bounds_max_ = {};
  for (auto& rig : cockpit_rigs_) rig = {};
  gauge_calibration_ = {};
  car_shader_overrides_ = {};
  active_paint_params_ = {};
  car_has_crush_weights_ = false;
  has_blur_rim_mesh_ = false;
  has_car_dirt_mask_ = false;
  driver_hands_offset_ = {0.0f, 0.0f, 0.0f};
  driver_hands_scale_ = 1.0f;
  car_attribs_wheel_pos_ = {0.0f, 0.0f, 0.0f};
  car_attribs_wheel_rot_rad_ = 0.0f;
  cockpit_wheel_pos_offset_ = {0.0f, 0.0f, 0.0f};
  cockpit_wheel_rot_offset_rad_ = 0.0f;
  mirror_origins_ = {std::array<float, 3>{-0.95f, 0.95f, 0.25f},
                     std::array<float, 3>{0.0f, 1.18f, 0.25f},
                     std::array<float, 3>{0.95f, 0.95f, 0.25f}};
  mirror_origin_valid_ = {false, false, false};
  dmg_front_bumper_detached_ = false;
  dmg_rear_bumper_detached_ = false;
  dmg_left_mirror_detached_ = false;
  dmg_right_mirror_detached_ = false;
  dmg_wing_detached_ = false;
  for (int k = 0; k < 5; ++k) {
    detached_part_flying_[k] = false;
    identity(detached_part_xform_[k]);
  }
  dmg_hood_ajar_ = 0.0f;
  stock_ = nullptr;
  pending_wheel_ = -1;
  pending_spins_ = false;
  for (int i = 0; i < 4; ++i) {
    stripe_[i] = 1.0f;
    paint_two_tone_[i] = 0.0f;
    damage_dirt_[i] = 0.0f;
    damage_zones_[i] = 0.0f;
    damage_corners_[i] = 0.0f;
  }
  for (int r = 0; r < 3; ++r) {
    region_two_tone_[r][0] = region_two_tone_[r][1] = region_two_tone_[r][2] = 0.0f;
    region_two_tone_[r][3] = 4.0f;
    region_finish_[r][0] = 0.0f;
    region_finish_[r][1] = 0.25f;
    region_finish_[r][2] = 0.0f;
    region_finish_[r][3] = 0.0f;
  }
  paint_finish_[0] = 0.0f;
  paint_finish_[1] = 0.25f;
  paint_finish_[2] = 0.0f;
  paint_finish_[3] = 0.0f;
  std::string err;
  const std::string car_name = lower(car);
  auto try_read_bake = [&](const std::filesystem::path& rel, bake::Bake& dst, std::string& out_err) -> bool {
    const auto resolved = bake_dir.empty() ? cars::mods::resolve_bake(rel) : cars::mods::resolve(bake_dir / rel);
    if (resolved.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(resolved, ec)) return false;
    return bake::read_file(resolved, dst, &out_err);
  };
  if (!try_read_bake(std::filesystem::path("cars") / (car_name + ".shiftbake"), car_, err)) {
    std::string disc_err;
    if (disc_root.empty() || !DiscCarCache::instance().load_car(disc_root, car_name, car_, &disc_err)) {
      error = car_name + ": " + (!err.empty() ? err : (!disc_err.empty() ? disc_err : "not found"));
      return false;
    }
  }
  bake::CarInfo& info = car_.car;
  if (options_.match_rgb >= 0) {
    bool found = false;
    for (size_t i = 0; i < info.colours.size() && !found; ++i)
      if (int64_t(info.colours[i].rgb) == options_.match_rgb) {
        options_.colour = i;
        found = true;
      }
    if (!found) notes_.push_back("no colour row with the requested sRGB: drawing colour " +
                                 std::to_string(options_.colour));
  }
  if (options_.match_sequence >= 0) {
    bool found = false;
    for (size_t i = 0; i < info.colours.size() && !found; ++i)
      if (info.colours[i].sequence == options_.match_sequence) {
        options_.colour = i;
        found = true;
      }
    if (!found) {
      options_.colour = 0;
      notes_.push_back("no colour with Sequence " + std::to_string(options_.match_sequence) + ": the lowest");
    }
  }

  // Apply fitted visual overrides (tyre widths, rim diameters, ride height,
  // CarTrackOffsets, caliper angles, aftermarket rim, per-car tyre).
  const cars::VisualSetup& vis = options_.visual;
  // Capture stock rim/tyre dimensions before applying overrides so upgraded
  // rims & widths preserve the stock rolling outer radius (thinning sidewall).
  const std::array<int64_t, 2> stock_rim_in = {
      (vis.active && vis.stock_rim_inches[0] > 0) ? vis.stock_rim_inches[0] : info.rim_inches[0],
      (vis.active && vis.stock_rim_inches[1] > 0) ? vis.stock_rim_inches[1] : info.rim_inches[1]};
  const std::array<int64_t, 2> stock_width_mm = {
      (vis.active && vis.stock_tyre_width_mm[0] > 0) ? vis.stock_tyre_width_mm[0] : info.tyre_width_mm[0],
      (vis.active && vis.stock_tyre_width_mm[1] > 0) ? vis.stock_tyre_width_mm[1] : info.tyre_width_mm[1]};
  const std::array<int64_t, 2> stock_aspect = {
      (vis.active && vis.stock_tyre_aspect[0] > 0) ? vis.stock_tyre_aspect[0] : info.tyre_aspect[0],
      (vis.active && vis.stock_tyre_aspect[1] > 0) ? vis.stock_tyre_aspect[1] : info.tyre_aspect[1]};
  if (vis.active) {
    for (size_t a = 0; a < 2; ++a) {
      if (vis.tyre_width_mm[a] > 0) info.tyre_width_mm[a] = vis.tyre_width_mm[a];
      if (vis.rim_inches[a] > 0) info.rim_inches[a] = vis.rim_inches[a];
      if (vis.ride_height_m[a] >= 0.0f) info.ride_height[a] = vis.ride_height_m[a];
      if (vis.has_caliper_angle_override) info.caliper_angle[a] = vis.caliper_angle_deg[a];
    }
    if (!vis.wheel_media.empty()) info.wheel_media = lower(vis.wheel_media);
    // Recompute lift, pitch, and wheel centres for upgraded stance / track offsets.
    if (info.has_body && info.wheelbase > 0.0f) {
      info.lift = (info.ride_height[0] + info.ride_height[1]) * 0.5f - info.body_bottom_y;
      info.pitch = std::atan((info.ride_height[1] - info.ride_height[0]) / info.wheelbase);
      const float cs = std::cos(info.pitch), sn = std::sin(info.pitch);
      const size_t wheel_rows[4] = {5, 6, 8, 7};
      for (size_t w = 0; w < 4; ++w) {
        const size_t axle = w < 2 ? 0 : 1;
        const float width = float(info.tyre_width_mm[axle]) / 1000.0f;
        const float outer = cars::compute_tire_outer_radius_m(stock_rim_in[axle], stock_width_mm[axle],
                                                              stock_aspect[axle], info.rim_inches[axle],
                                                              info.tyre_width_mm[axle]);
        const float side = (w % 2 == 0) ? -1.0f : 1.0f;
        const float track_x = (info.track_outer[axle] - width) * 0.5f + vis.track_offset_m[axle];
        const float bx = side * track_x;
        const float by = outer;
        const float bz = info.part_positions[wheel_rows[w]][2];
        info.wheel_centres_body[w] = {bx, by, bz};
        const float y = by - info.lift;
        info.wheel_centres[w] = {bx, cs * y + sn * bz, sn * y - cs * bz};
      }
    }
  }

  // Per-car tyre check (FJ Cruiser TOY_FJCruiser_12, Raptor, Wrangler, H1):
  // prefer the car's own TireA<MediaName> geometry if available in bake or disc,
  // and allow compound texture swapping onto TireB<MediaName> / TireC<MediaName>
  // when present (e.g. TireBFOR_SVTRaptor_12.zip, TireCFOR_SVTRaptor_12.zip).
  std::string want_tyre = vis.active && !vis.tyre_media_override.empty()
                              ? lower(vis.tyre_media_override)
                              : cars::per_car_tyre_media(info.media_name);
  auto try_load_tyre_tex_pack = [&](const std::string& media) -> bool {
    if (media.empty()) return false;
    std::error_code tec;
    if (!bake_dir.empty() &&
        std::filesystem::exists(cars::mods::resolve(bake_dir / "wheels" / (media + ".shiftbake")), tec)) {
      info.tyre_texture_media = media;
      return true;
    }
    if (!disc_root.empty() && DiscCarCache::instance().load_wheel(disc_root, media, true, tyre_textures_, nullptr)) {
      info.tyre_texture_media = media;
      return true;
    }
    return false;
  };
  if (!want_tyre.empty()) {
    if (info.tyre_media == cars::kTyreModel) {
      std::error_code tec;
      if (!bake_dir.empty() &&
          std::filesystem::exists(cars::mods::resolve(bake_dir / "wheels" / (want_tyre + ".shiftbake")), tec)) {
        info.tyre_media = want_tyre;
        info.tyre_texture_media = want_tyre;
      } else if (!disc_root.empty() &&
                 DiscCarCache::instance().load_wheel(disc_root, want_tyre, true, tyre_, nullptr)) {
        info.tyre_media = want_tyre;
        info.tyre_texture_media = want_tyre;
      }
    }
    if (vis.active && want_tyre.size() > 5 && want_tyre.rfind("tirea", 0) == 0) {
      const char letter = cars::tire_compound_texture_letter(vis.tire_compound_level);
      if (letter != 'a') {
        const std::string comp_media = std::string("tire") + letter + want_tyre.substr(5);
        if (!try_load_tyre_tex_pack(comp_media)) {
          info.tyre_texture_media = want_tyre;
        }
      } else {
        info.tyre_texture_media = want_tyre;
      }
    }
  } else if (vis.active) {
    // Select TireA (stock/street, <2), TireB (sport, ==2), or TireC (race/rally, >2)
    // matching List_UpgradeTireCompound.Level; at TU4 Rally Level 5 / TireCompoundID 9 prefer
    // a dedicated rally tyre texture pack if present on disc, else TireC.
    if ((vis.has_rally_tires || vis.tire_compound_level >= 5) &&
        (try_load_tyre_tex_pack("tirerally") || try_load_tyre_tex_pack("tired"))) {
      // Dedicated rally tyre texture archive loaded.
    } else {
      const char letter = cars::tire_compound_texture_letter(vis.tire_compound_level);
      info.tyre_texture_media = std::string("tire") + letter;
    }
  }

  // Load rim: pre-baked .shiftbake first, then on-demand from disc/packages
  // (covers zero-bake mode and all 285+ aftermarket rims in List_Wheels).
  if (!try_read_bake(std::filesystem::path("wheels") / (info.wheel_media + ".shiftbake"), wheel_, err)) {
    std::string disc_err;
    if (disc_root.empty() ||
        !DiscCarCache::instance().load_wheel(disc_root, info.wheel_media, false, wheel_, &disc_err)) {
      error = "wheel " + info.wheel_media + ": " + err + (disc_err.empty() ? "" : " (" + disc_err + ")");
      return false;
    }
  }
  if (tyre_.models.empty()) {
    if (!try_read_bake(std::filesystem::path("wheels") / (info.tyre_media + ".shiftbake"), tyre_, err)) {
      std::string disc_err;
      if (disc_root.empty() ||
          !DiscCarCache::instance().load_wheel(disc_root, info.tyre_media, true, tyre_, &disc_err)) {
        error = !err.empty() ? err : disc_err;
        return false;
      }
    }
  }
  if (tyre_textures_.textures.empty()) {
    if (info.tyre_texture_media == info.tyre_media && !tyre_.textures.empty()) {
      tyre_textures_ = tyre_;
    } else if (!try_read_bake(std::filesystem::path("wheels") / (info.tyre_texture_media + ".shiftbake"),
                              tyre_textures_, err) &&
               !try_read_bake(std::filesystem::path("wheels") / (std::string(cars::kTyreTextures) + ".shiftbake"),
                              tyre_textures_, err)) {
      std::string disc_err;
      const bool ok_own =
          !disc_root.empty() &&
          DiscCarCache::instance().load_wheel(disc_root, info.tyre_texture_media, true, tyre_textures_, &disc_err);
      const bool ok_shared =
          ok_own || (!disc_root.empty() &&
                     DiscCarCache::instance().load_wheel(disc_root, cars::kTyreTextures, true, tyre_textures_, &disc_err));
      if (!ok_shared) {
        error = !err.empty() ? err : disc_err;
        return false;
      }
    }
  }
  if (shared_.textures.empty()) {
    if (!try_read_bake("shared.shiftbake", shared_, err)) {
      std::string disc_err;
      if (disc_root.empty() || !DiscCarCache::instance().load_shared(disc_root, shared_, &disc_err)) {
        error = !err.empty() ? err : disc_err;
        return false;
      }
    }
    shared_bake_dir_ = shared_key;
  }

  // On-demand load of car interior_emissive_LOD0.xds, per-car dirt_mask.xds,
  // CarAttribs.xml (DriverHandsOffset/Scale, CockpitWheel offsets, <Gauges> TickMarks),
  // and per-car shader overrides (ColorShaderSettings1..17.xml, RimShaderSettings.xml,
  // BrakeShaderSettings.xml, ShaderSettings.xml).
  {
    bool has_emissive = false;
    bool has_dirt_mask = false;
    for (const auto& tx : car_.textures) {
      const std::string tn = lower(tx.name);
      if (tn == "interior_emissive_lod0.xds") has_emissive = true;
      if (tn == "dirt_mask.xds" || tn == "dirt_mask_lod0.xds") has_dirt_mask = true;
    }
    if (!disc_root.empty()) {
      cars::FolderRef ref;
      const cars::ShaderFinishParams& base_fp = shader_settings_.normal;
      if (DiscCarCache::instance().find_car_ref(disc_root, info.media_name, ref) &&
          (ref.kind == cars::FolderRef::Kind::kArchive || ref.kind == cars::FolderRef::Kind::kDirectory)) {
        cars::load_car_shader_overrides(ref.path, car_shader_overrides_, &base_fp, nullptr);
      }
      cars::Folder folder;
      if (open_disc_car_folder(disc_root, info.media_name, folder)) {
        std::vector<uint8_t> bytes;
        if (!has_emissive && folder.read("interior_emissive_lod0.xds", bytes, nullptr)) {
          auto xds = fh1::decode_xds(bytes);
          if (!xds.levels.empty() && !xds.levels[0].rgba.empty()) {
            bake::Texture bt;
            bt.name = "interior_emissive_lod0.xds";
            bt.format = bake::TextureFormat::kRgba8;
            bt.levels.push_back({xds.levels[0].width, xds.levels[0].height, std::move(xds.levels[0].rgba)});
            car_.textures.push_back(std::move(bt));
          }
        }
        if (!has_dirt_mask) {
          for (const char* dm_name : {"dirt_mask.xds", "dirt_mask_lod0.xds"}) {
            bytes.clear();
            if (folder.read(dm_name, bytes, nullptr) && !bytes.empty()) {
              fh1::XdsBlocks blocks;
              if (fh1::extract_xds_blocks(bytes, blocks, nullptr) && !blocks.levels.empty()) {
                bake::Texture bt;
                bt.name = dm_name;
                bt.format = bake::TextureFormat(uint32_t(blocks.format));
                bt.swizzle = blocks.swizzle;
                for (auto& l : blocks.levels) bt.levels.push_back({l.width, l.height, std::move(l.blocks)});
                car_.textures.push_back(std::move(bt));
                has_dirt_mask = true;
                break;
              }
            }
          }
        }
        bytes.clear();
        if (folder.read("carattribs.xml", bytes, nullptr) && !bytes.empty()) {
          const ParsedCarAttribs ca =
              parse_car_attribs_xml(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
          driver_hands_offset_ = ca.driver_hands_offset;
          driver_hands_scale_ = ca.driver_hands_scale;
          if (ca.has_wheel_pos) {
            car_attribs_wheel_pos_ = ca.wheel_pos_offset;
            cockpit_wheel_pos_offset_ = ca.wheel_pos_offset;
          }
          if (ca.has_wheel_rot) {
            car_attribs_wheel_rot_rad_ = ca.wheel_rot_offset_deg * (3.14159265358979323846f / 180.0f);
            cockpit_wheel_rot_offset_rad_ = car_attribs_wheel_rot_rad_;
          }
          gauge_calibration_ = ca.gauges;
        }
        // DLC / package fallback when load_car_shader_overrides couldn't open a standalone zip.
        if (!car_shader_overrides_.loaded_any) {
          for (const auto& mname : folder.names()) {
            const std::string ml = lower(mname);
            if (!ends_with(ml, ".xml")) continue;
            bytes.clear();
            if (!folder.read(mname, bytes, nullptr) || bytes.empty()) continue;
            const std::string_view xml_sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (ml == "rimshadersettings.xml") {
              if (cars::parse_rim_shader_settings_xml(xml_sv, car_shader_overrides_.rim))
                car_shader_overrides_.loaded_any = true;
            } else if (ml == "brakeshadersettings.xml") {
              cars::ShaderFinishParams bp = base_fp;
              if (cars::parse_shader_settings_xml_technique(xml_sv, "caliper", bp) ||
                  cars::parse_shader_settings_xml_technique(xml_sv, "brake", bp) ||
                  cars::parse_shader_settings_xml(xml_sv, bp)) {
                car_shader_overrides_.brake.params = bp;
                car_shader_overrides_.brake.has_override = true;
                car_shader_overrides_.loaded_any = true;
              }
            } else if (ml.rfind("colorshadersettings", 0) == 0 && ml.size() > 23) {
              const std::string num_s = ml.substr(19, ml.size() - 23);
              char* stop = nullptr;
              const long idx = std::strtol(num_s.c_str(), &stop, 10);
              if (stop == num_s.c_str() + num_s.size() && idx >= 1 && idx <= 64) {
                cars::ShaderFinishParams fp = base_fp;
                if (cars::parse_shader_settings_xml(xml_sv, fp)) {
                  car_shader_overrides_.color_overrides[int(idx)] = fp;
                  car_shader_overrides_.loaded_any = true;
                }
              }
            } else if (ml == "shadersettings.xml") {
              cars::ShaderFinishParams fp = base_fp;
              if (cars::parse_shader_settings_xml(xml_sv, fp) &&
                  fp.gauge_emissive_scale != base_fp.gauge_emissive_scale) {
                car_shader_overrides_.gauge_emissive_scale = fp.gauge_emissive_scale;
                car_shader_overrides_.has_gauge_emissive_override = true;
                car_shader_overrides_.loaded_any = true;
              }
            }
          }
        }
      }
    }
  }

  textures_.push_back({nullptr, "@white"});
  textures_.push_back({nullptr, "@black"});

  // The row shown: the requested car id, else the baked row.
  stock_ = nullptr;
  for (const auto& s : info.stock_rows)
    if (s.known && (options_.car_id == 0 ? s.car_id == info.car_id : s.car_id == options_.car_id)) stock_ = &s;
  if (options_.car_id != 0 && !stock_)
    notes_.push_back("no stock row for car " + std::to_string(options_.car_id) + " in the bake");
  if (!stock_ && !vis.active) notes_.push_back("no stock parts baked: hiding variants by letter (first real letter)");
  // FORMAT-CARS, Stock & fitted parts: family members by fitted VisualSetup
  // (when active) else the stock rows, seats a weight row removes, and
  // other lettered names by the first real letter.
  auto hidden = [&](const std::string& name, const std::set<std::string>& real) {
    if (options_.all_variants) return false;
    const std::string n = lower(name);
    if (vis.active) {
      const std::string member = family_member(n);
      if (!member.empty()) {
        static const std::regex kExhaust("exhaust[lr]?([a-z])");
        std::smatch m;
        if (std::regex_match(member, m, kExhaust)) {
          if (vis.has_exhaust_override) {
            return !std::binary_search(vis.shown_parts.begin(), vis.shown_parts.end(), member);
          }
          return m[1].str() != "a";
        }
        return !std::binary_search(vis.shown_parts.begin(), vis.shown_parts.end(), member);
      }
      if (std::binary_search(vis.removed_parts.begin(), vis.removed_parts.end(), n)) return true;
      return hidden_variant(n, real);
    }
    if (stock_) {
      const std::string member = family_member(n);
      if (!member.empty()) return stock_hidden(member, *stock_);
      if (std::binary_search(stock_->removed.begin(), stock_->removed.end(), n)) return true;
    }
    return hidden_variant(n, real);
  };

  // Which body: LOD0 when it has geometry (unless prefer_lod1 requests LOD1-5 for traffic/AI), else LOD1-5.
  const bake::Model* body = nullptr;
  const bake::Model* exterior = nullptr;
  const bake::Model* cockpit = nullptr;
  bool exterior_real = false;
  for (const auto& m : car_.models) {
    bool real = false;
    for (const auto& p : m.parts) real |= !placeholder(p);
    if (m.role == Role::kLod0 && real) body = &m;
    if (m.role == Role::kExterior) {
      exterior = &m;
      exterior_real = real;
    }
    if (m.role == Role::kCockpit) cockpit = &m;
  }
  if (options.prefer_lod1 && exterior && exterior_real) {
    body = exterior;
    cockpit = nullptr;
  } else if (!body) {
    body = exterior;
    notes_.push_back("no LOD0 geometry (or only stand-in boxes): drawing the LOD1-5 model");
  }
  if (!body) {
    error = car_name + ": no exterior model";
    return false;
  }

  float identity_m[16];
  identity(identity_m);
  std::set<std::string> body_names, body_real;
  for (const auto& p : body->parts) {
    body_names.insert(lower(p.name));
    if (!placeholder(p)) body_real.insert(lower(p.name));
  }
  std::vector<std::string> body_parts;
  for (const auto& p : body->parts) {
    const std::string n = lower(p.name);
    // The LOD0 model carries a rim ("wheel") at the origin; the wheels are
    // drawn from the wheel model at each centre instead.
    if (n == "wheel" || placeholder(p) || hidden(n, body_real)) {
      skipped_parts_.push_back(body->name + "/" + p.name);
      continue;
    }
    body_parts.push_back(p.name);
  }
  add_model(*body, body_parts, identity_m, car_, 0);
  // The cockpit model: the parts the body does not have (hood, windscreen,
  // headlights, cabin); the gauge needles at their minimum.
  std::set<std::string> cockpit_real;
  if (cockpit)
    for (const auto& p : cockpit->parts)
      if (!placeholder(p)) cockpit_real.insert(lower(p.name));
  if (!cockpit_real.empty()) {
    // Extract living-cockpit rotation rigs for steering_wheel and analog gauge needles (_min/_mid/_max).
    auto pool_of = [&](const bake::Part& p) -> const std::vector<bake::Vertex>* {
      const int32_t b = best_lod(p);
      if (b < 0) return nullptr;
      return b == 0 ? &p.lod0_vertices : &p.lod_vertices;
    };
    auto find_part = [&](const std::string& lname) -> const bake::Part* {
      for (const auto& p : cockpit->parts)
        if (lower(p.name) == lname && !placeholder(p)) return &p;
      return nullptr;
    };
    auto vsub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
      return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    auto vdot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    auto vcross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
      return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto vnorm = [&](const std::array<float, 3>& a) -> std::array<float, 3> {
      const float l = std::sqrt(vdot(a, a));
      return l > 1e-6f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : std::array<float, 3>{0, 0, -1};
    };
    if (const bake::Part* sw = find_part("steering_wheel")) {
      if (const auto* pool = pool_of(*sw); pool && pool->size() >= 3) {
        std::array<double, 3> mean{0, 0, 0};
        for (const auto& v : *pool)
          for (int k = 0; k < 3; ++k) mean[k] += v.position[k];
        for (int k = 0; k < 3; ++k) mean[k] /= double(pool->size());
        // Steering column axis: normal to wheel rim plane (smallest-variance axis in YZ plane).
        double syy = 0, syz = 0, szz = 0;
        for (const auto& v : *pool) {
          const double dy = v.position[1] - mean[1], dz = v.position[2] - mean[2];
          syy += dy * dy, syz += dy * dz, szz += dz * dz;
        }
        const double theta = 0.5 * std::atan2(2.0 * syz, syy - szz);
        // Eigenvector with smaller variance in YZ plane.
        const double ay1 = std::cos(theta), az1 = std::sin(theta);
        const double ay2 = -az1, az2 = ay1;
        const double v1 = ay1 * ay1 * syy + 2.0 * ay1 * az1 * syz + az1 * az1 * szz;
        const double v2 = ay2 * ay2 * syy + 2.0 * ay2 * az2 * syz + az2 * az2 * szz;
        float ay = float(v1 < v2 ? ay1 : ay2), az = float(v1 < v2 ? az1 : az2);
        // Orient forward (-Z in model space) so +wheel_rad rotates top of wheel toward +X (right).
        if (az > 0.0f) ay = -ay, az = -az;
        cockpit_rigs_[0].valid = true;
        cockpit_rigs_[0].pivot = {float(mean[0]), float(mean[1]), float(mean[2])};
        cockpit_rigs_[0].axis = vnorm({0.0f, ay, az});
        // Measure actual steering wheel rim grip radius perpendicular to the steering column axis.
        float max_r = 0.0f;
        for (const auto& v : *pool) {
          const auto d = vsub(v.position, cockpit_rigs_[0].pivot);
          const float proj = vdot(d, cockpit_rigs_[0].axis);
          const std::array<float, 3> rad{d[0] - cockpit_rigs_[0].axis[0] * proj,
                                         d[1] - cockpit_rigs_[0].axis[1] * proj,
                                         d[2] - cockpit_rigs_[0].axis[2] * proj};
          max_r = std::max(max_r, std::sqrt(vdot(rad, rad)));
        }
        cockpit_rigs_[0].radius = (max_r > 0.08f) ? std::clamp(max_r * 0.91f, 0.12f, 0.22f) : 0.165f;
      }
    }
    static const char* const kGauges[5] = {"speed", "tach", "fuel", "boost", "power"};
    for (int g = 0; g < 5; ++g) {
      const bake::Part* pmin = find_part(std::string(kGauges[g]) + "_min");
      const bake::Part* pmid = find_part(std::string(kGauges[g]) + "_mid");
      const bake::Part* pmax = find_part(std::string(kGauges[g]) + "_max");
      if (!pmin || !pmax) continue;
      const auto* v0 = pool_of(*pmin);
      const auto* v1 = pmid ? pool_of(*pmid) : nullptr;
      const auto* v2 = pool_of(*pmax);
      if (!v0 || !v2 || v0->empty() || v2->empty()) continue;
      auto& rig = cockpit_rigs_[g + 1];
      if (v0->size() == v2->size()) {
        size_t i_piv = 0, i_tip = 0;
        float d_min = 1e30f, d_max = -1.0f;
        for (size_t i = 0; i < v0->size(); ++i) {
          const auto diff = vsub((*v2)[i].position, (*v0)[i].position);
          const float d2 = vdot(diff, diff);
          if (d2 < d_min) d_min = d2, i_piv = i;
          if (d2 > d_max) d_max = d2, i_tip = i;
        }
        if (d_max > 1e-6f) {
          const auto piv = (*v0)[i_piv].position;
          const auto u0 = vnorm(vsub((*v0)[i_tip].position, piv));
          const auto u2 = vnorm(vsub((*v2)[i_tip].position, piv));
          if (v1 && v1->size() == v0->size()) {
            const auto u1 = vnorm(vsub((*v1)[i_tip].position, piv));
            auto ax = vcross(u0, u1);
            if (vdot(ax, ax) < 1e-6f) ax = vcross(u0, u2);
            if (vdot(ax, ax) > 1e-6f) {
              ax = vnorm(ax);
              float a1 = std::atan2(vdot(vcross(u0, u1), ax), vdot(u0, u1));
              if (a1 < 0.0f) a1 += 6.2831853f;
              float a2 = std::atan2(vdot(vcross(u1, u2), ax), vdot(u1, u2));
              if (a2 < 0.0f) a2 += 6.2831853f;
              rig.valid = true;
              rig.pivot = piv;
              rig.axis = ax;
              rig.ang_mid = a1;
              rig.ang_max = a1 + a2;
            }
          } else {
            auto ax = vcross(u0, u2);
            if (vdot(ax, ax) > 1e-6f) {
              ax = vnorm(ax);
              float am = std::atan2(vdot(vcross(u0, u2), ax), vdot(u0, u2));
              if (am < 0.0f) am += 6.2831853f;
              rig.valid = true;
              rig.pivot = piv;
              rig.axis = ax;
              rig.ang_mid = am * 0.5f;
              rig.ang_max = am;
            }
          }
        }
      }
    }

    std::vector<std::string> cockpit_parts;
    for (const auto& p : cockpit->parts) {
      const std::string n = lower(p.name);
      const bool needle = ends_with(n, "_mid") || ends_with(n, "_max");
      if (body_names.count(n) || needle || placeholder(p) || hidden(n, cockpit_real)) {
        skipped_parts_.push_back(cockpit->name + "/" + p.name);
        continue;
      }
      cockpit_parts.push_back(p.name);
    }
    add_model(*cockpit, cockpit_parts, identity_m, car_, 0);
  } else {
    notes_.push_back("no cockpit geometry: interior from the body model only");
  }

  // Stand-in parts of LOD0 and the cockpit come from the LOD1-5 model,
  // unless their variant set has a real part already.
  if (exterior && body != exterior) {
    std::set<std::string> stand_ins;
    for (const auto& p : body->parts)
      if (placeholder(p)) stand_ins.insert(lower(p.name));
    if (cockpit)
      for (const auto& p : cockpit->parts)
        if (placeholder(p) && !body_names.count(lower(p.name))) stand_ins.insert(lower(p.name));
    std::set<std::string> real = body_real;
    real.insert(cockpit_real.begin(), cockpit_real.end());
    auto stem_of = [](const std::string& n) {
      return n.size() > 1 && n.back() >= 'a' && n.back() <= 'h' ? n.substr(0, n.size() - 1) : std::string();
    };
    std::set<std::string> stems_drawn;
    for (const auto& n : real)
      if (!stem_of(n).empty()) stems_drawn.insert(stem_of(n));
    std::vector<std::string> fill;
    for (const auto& n : stand_ins) {  // sorted: "bumperfa" before "bumperfb"
      if (n == "wheel" || ends_with(n, "_mid") || ends_with(n, "_max")) continue;
      // A stock/fitted family member is filled from its LOD1-5 twin; another
      // member never stands in for it (no promotion).
      const bool by_stock = (stock_ || vis.active) && !family_member(n).empty();
      if (by_stock && hidden(n, real)) continue;
      if (!by_stock && ends_with(n, "race")) continue;
      if (!by_stock && (stock_ || vis.active) && hidden(n, real)) continue;  // a removed seat
      const std::string stem = by_stock ? std::string() : stem_of(n);
      if (!options_.all_variants && !stem.empty() && stems_drawn.count(stem)) continue;
      for (const auto& e : exterior->parts)
        if (lower(e.name) == n && !placeholder(e)) {
          fill.push_back(e.name);
          if (!stem.empty()) stems_drawn.insert(stem);
        }
    }
    if (!fill.empty()) {
      notes_.push_back(std::to_string(fill.size()) + " stand-in parts drawn from the LOD1-5 model");
      add_model(*exterior, fill, identity_m, car_, 0);
    }
  }

  // Parse CarAnimationEvents.xml (when available on disc) and tag animated body/cockpit
  // parts accurately:
  // - Active rear wings ONLY on cars declaring RearWing / active aero in CarAnimationEvents.xml
  //   or carrying active-aero linkages (winga_arm*/winga_piston/winga_lower/winga_suspension),
  //   so fixed stock winga spoilers on the other ~115 cars never detach.
  // - Pop-up headlights on cars declaring headlight in CarAnimationEvents.xml OR carrying
  //   explicit pop-up parts (headlightLup/Rup/Ldown/Rdown/headlightcover).
  // - Active EngineVent louvers (e.g. Murcielago SV, Aventador, Reventon).
  {
    CarAnimEventsFlags anim_flags;
    if (!disc_root.empty()) {
      cars::Folder folder;
      if (open_disc_car_folder(disc_root, info.media_name, folder)) {
        std::vector<uint8_t> xml_bytes;
        if (folder.read("caranimationevents.xml", xml_bytes, nullptr) && !xml_bytes.empty()) {
          anim_flags = parse_car_anim_events_xml(std::string(xml_bytes.begin(), xml_bytes.end()));
        }
      }
    }
    auto is_wing_linkage = [](const std::string& p) {
      return p.rfind("winga_arm", 0) == 0 || p.rfind("winga_piston", 0) == 0 ||
             p.rfind("winga_lower", 0) == 0 || p.rfind("winga_suspension", 0) == 0;
    };
    bool has_wing_linkages = false;
    bool has_winga_main = false;
    bool has_hl_up = false;
    bool has_hl_down = false;
    for (const auto& d : draws_) {
      if (d.wheel >= 0 || d.cockpit_role >= 0) continue;
      if (d.part_name == "winga") has_winga_main = true;
      if (is_wing_linkage(d.part_name)) has_wing_linkages = true;
      if (d.part_name == "headlightlup" || d.part_name == "headlightrup") has_hl_up = true;
      if (d.part_name == "headlightldown" || d.part_name == "headlightrdown") has_hl_down = true;
    }
    const bool active_wing_car = anim_flags.has_rear_wing || has_wing_linkages;
    const bool dual_popup_mesh = has_hl_up && has_hl_down;
    for (auto& d : draws_) {
      if (d.wheel >= 0 || d.cockpit_role >= 0) continue;
      if (active_wing_car) {
        if (d.part_name == "winga") {
          d.anim_part = 1;
          continue;
        }
        if (is_wing_linkage(d.part_name)) {
          // Cars like Pagani Huayra have no main winga blade; winga_arm1..4 are the 4 aero flaps.
          d.anim_part = has_winga_main ? 8 : 1;
          continue;
        }
      }
      if (dual_popup_mesh) {
        if (d.part_name == "headlightlup" || d.part_name == "headlightrup") {
          d.anim_part = 9;
          std::memset(d.push.model, 0, sizeof d.push.model);
          d.push.model[15] = 1.0f;
          continue;
        }
        if (d.part_name == "headlightldown" || d.part_name == "headlightrdown") {
          d.anim_part = 10;
          continue;
        }
      } else {
        const bool explicit_popup = d.part_name == "headlightlup" || d.part_name == "headlightrup" ||
                                    d.part_name == "headlightldown" || d.part_name == "headlightrdown" ||
                                    d.part_name.rfind("headlightcover", 0) == 0 ||
                                    d.part_name.find("popup") != std::string::npos ||
                                    d.part_name.find("retract") != std::string::npos;
        const bool xml_popup = anim_flags.has_headlight &&
                               (d.part_name == "headlightl" || d.part_name == "headlightr");
        if (explicit_popup || xml_popup) {
          d.anim_part = 2;
          continue;
        }
      }
      if (anim_flags.has_engine_vent && d.part_name.find("vent") != std::string::npos) {
        d.anim_part = 7;
      }
    }
  }

  // The inverse pose: model = R (body - (0, lift, 0)); R is its own inverse.
  const float cp = std::cos(info.pitch), sp = std::sin(info.pitch);
  const float pose[16] = {1, 0, 0, 0, 0, cp, sp, 0, 0, sp, -cp, 0, 0, -cp * info.lift, -sp * info.lift, 1};
  std::memcpy(body_to_model_, pose, sizeof pose);
  const float* body_to_model = body_to_model_;

  // Brakes: each corner's rotor and calliper, centred on the hub, rotated by
  // CaliperAngleFront/Rear (radians, upgraded generic calipers only), tilted by
  // static camber, and laterally offset by List_Wheels.BrakeDrawOffset.
  // When upgraded brakes (Level >= 1) are fitted and game_root is set, load
  // the upgraded rotor & caliper models from media/brakes/.
  static constexpr bake::Corner kCorners[4] = {bake::Corner::kLF, bake::Corner::kRF, bake::Corner::kLR,
                                               bake::Corner::kRR};
  std::array<bake::Model, 2> upg_rotor, upg_caliper;
  std::array<bool, 2> has_upg_rotor{false, false}, has_upg_caliper{false, false};
  if (vis.active && !options_.game_root.empty()) {
    for (int a = 0; a < 2; ++a) {
      if (vis.brakes[size_t(a)].level >= 1 && vis.brakes[size_t(a)].size_in > 0) {
        has_upg_rotor[size_t(a)] =
            cars::load_brake_model(options_.game_root, vis.brakes[size_t(a)].size_in, vis.brakes[size_t(a)].type_id,
                                   vis.brakes[size_t(a)].pistons, true, upg_rotor[size_t(a)], nullptr);
        has_upg_caliper[size_t(a)] =
            cars::load_brake_model(options_.game_root, vis.brakes[size_t(a)].size_in, vis.brakes[size_t(a)].type_id,
                                   vis.brakes[size_t(a)].pistons, false, upg_caliper[size_t(a)], nullptr);
      }
    }
  }
  auto add_brake_at = [&](const bake::Model& m, int corner, bool is_rotor, bool mirror_right,
                          const bake::Bake& tex_pack) {
    const int axle = corner < 2 ? 0 : 1;
    const bool right = (corner % 2) == 1;
    const float side = right ? 1.0f : -1.0f;
    const float brake_dx = vis.active ? (side * vis.brake_draw_offset) : 0.0f;
    // Data_Car.CaliperAngleFront/Rear are already in radians; only rotate generic
    // upgraded media/brakes/* calipers (stock per-car calipers are authored in place).
    const float cal_rad = (!is_rotor && mirror_right) ? info.caliper_angle[size_t(axle)] : 0.0f;
    const float ca = std::cos(cal_rad), sa = std::sin(cal_rad);
    // Local transform about hub in body frame: optional half-turn for loose
    // right-side models, caliper angle rotation around axle X, lateral offset,
    // and static alignment camber around Z matching the wheel/tyre transform.
    float rot_x[16];
    identity(rot_x);
    rot_x[5] = ca;
    rot_x[6] = sa;
    rot_x[9] = -sa;
    rot_x[10] = ca;
    float local_body[16];
    if (mirror_right) {
      float flip[16];
      identity(flip);
      flip[0] = right ? -1.0f : 1.0f;
      flip[10] = right ? -1.0f : 1.0f;
      multiply(rot_x, flip, local_body);
    } else {
      // Stock brake models are authored in model frame: convert linear part via pose.
      float lin_pose[16];
      std::memcpy(lin_pose, pose, sizeof lin_pose);
      lin_pose[13] = lin_pose[14] = 0;
      multiply(rot_x, lin_pose, local_body);
    }
    local_body[12] += brake_dx;
    const float cam = vis.active ? (right ? -vis.camber_rad[size_t(axle)] : vis.camber_rad[size_t(axle)]) : 0.0f;
    if (cam != 0.0f) {
      float rz[16], tilted[16];
      identity(rz);
      const float cc = std::cos(cam), sc = std::sin(cam);
      rz[0] = cc;
      rz[1] = sc;
      rz[4] = -sc;
      rz[5] = cc;
      multiply(rz, local_body, tilted);
      std::memcpy(local_body, tilted, sizeof local_body);
    }
    std::memcpy(pending_local_, local_body, sizeof local_body);
    pending_wheel_ = corner;
    pending_spins_ = is_rotor;
    const auto& cb = info.wheel_centres_body[size_t(corner)];
    local_body[12] += cb[0];
    local_body[13] = cb[1];
    local_body[14] = cb[2];
    float m4[16];
    multiply(body_to_model, local_body, m4);
    std::vector<std::string> parts;
    for (const auto& p : m.parts) parts.push_back(p.name);
    add_model(m, parts, m4, tex_pack, axle);
  };
  for (int c = 0; c < 4; ++c) {
    const size_t axle = c < 2 ? 0 : 1;
    if (has_upg_rotor[axle]) add_brake_at(upg_rotor[axle], c, true, true, car_);
    if (has_upg_caliper[axle] && !upg_caliper[axle].parts.empty())
      add_brake_at(upg_caliper[axle], c, false, true, car_);
  }
  for (const auto& m : car_.models) {
    if (m.role != Role::kCaliper && m.role != Role::kRotor) continue;
    int corner = -1;
    for (int c = 0; c < 4; ++c)
      if (m.corner == kCorners[c]) corner = c;
    if (corner < 0) {
      notes_.push_back(m.name + ": no corner");
      continue;
    }
    const size_t axle = corner < 2 ? 0 : 1;
    if (m.role == Role::kRotor && has_upg_rotor[axle]) continue;
    if (m.role == Role::kCaliper && has_upg_caliper[axle]) continue;
    add_brake_at(m, corner, m.role == Role::kRotor, false, car_);
  }
  pending_wheel_ = -1;

  // Wheels (FORMAT-CARS, Wheels): built in the body frame at each centre and
  // mapped to the model frame by the inverse of the model-to-body pose.
  // Rims are sized by their morph target, never scaled radially; tyres by
  // their two targets; both are scaled across from the rim header's design
  // width. Right wheels are the same models turned half a turn about y.
  const bake::Model* rim = nullptr;
  for (const auto& m : wheel_.models)
    if (m.role == Role::kWheel) rim = &m;
  const bake::Model* tyre = nullptr;
  for (const auto& m : tyre_.models)
    if (m.role == Role::kTyre) tyre = &m;
  // A model's own tyre set (FORMAT-CARS, Wheels) is one unit wide like
  // the shared tyre and carries the same two targets (on the FJ Cruiser and
  // the Raptor-sized sets target A is the modelled radius, on the Wrangler
  // it grows the tyre): ours, until its sizing is settled, it is sized as
  // the shared tyre is (spec question Q39).
  if (info.tyre_media.size() > 5) notes_.push_back("own tyre set " + info.tyre_media + ": sized as the shared tyre");
  bool header_ok = false, extent_ok = false;
  if (rim) {
    const auto& hb = rim->header_bounds;
    auto finite = [](float f) { return std::isfinite(f) && std::fabs(f) < 1e3f; };
    header_ok = rim->header_kind == 3 && finite(hb[1]) && finite(hb[3]) && hb[1] - hb[3] > 0.01f;
    extent_ok = header_ok && finite(hb[0]) && finite(hb[2]) && hb[0] - hb[2] > 0.01f;
    if (!header_ok) notes_.push_back("rim header design width not finite: rim drawn unscaled across");
    else if (!extent_ok)
      notes_.push_back("rim header x extent not finite: tyre fitted to the design width (spec question Q13)");
    bool morph = false, lod0 = false;
    for (const auto& p : rim->parts) {
      const int32_t lod = best_lod(p);
      if (lod == 0) lod0 = true;
      if (!(lod == 0 ? p.lod0_morphs : p.lod_morphs).empty()) morph = true;
    }
    if (!morph) notes_.push_back("rim has no morph target: drawn as authored");
    if (!lod0) notes_.push_back("rim drawn from LOD1 (its LOD0 pool is missing or a stand-in)");
  }
  // Morph a copy of a wheel model: each drawn pool's targets at the weights.
  auto morphed = [](const bake::Model& model, const std::array<float, 2>& weights) {
    bake::Model out = model;
    for (auto& p : out.parts) {
      for (int lod = 0; lod < 2; ++lod) {
        auto& pool = lod == 0 ? p.lod0_vertices : p.lod_vertices;
        const auto& targets = lod == 0 ? p.lod0_morphs : p.lod_morphs;
        std::vector<std::array<float, 3>> base(pool.size());
        for (size_t i = 0; i < pool.size(); ++i) base[i] = pool[i].position;
        for (size_t t = 0; t < targets.size() && t < 2; ++t) {
          if (weights[t] == 0 || targets[t].size() != pool.size()) continue;
          for (size_t i = 0; i < pool.size(); ++i)
            for (size_t k = 0; k < 3; ++k)
              pool[i].position[k] += weights[t] * (targets[t][i][k] - base[i][k]);
        }
      }
      p.lod0_morphs.clear();
      p.lod_morphs.clear();
    }
    return out;
  };
  for (int c = 0; c < 4; ++c) {
    const int axle = c < 2 ? 0 : 1;
    const bool right = c % 2 == 1;
    const float inches = info.rim_inches[size_t(axle)] > 0 ? float(info.rim_inches[size_t(axle)]) : 14.0f;
    const float width_m = info.tyre_width_mm[size_t(axle)] > 0
                              ? float(info.tyre_width_mm[size_t(axle)]) / 1000.0f
                              : 0.2f;
    const float outer = cars::compute_tire_outer_radius_m(stock_rim_in[size_t(axle)], stock_width_mm[size_t(axle)],
                                                          stock_aspect[size_t(axle)], info.rim_inches[size_t(axle)],
                                                          info.tyre_width_mm[size_t(axle)]);
    const float w_rim = (inches - 14.0f) / 10.0f;
    const float rim_scale = rim && header_ok ? width_m / (rim->header_bounds[1] - rim->header_bounds[3]) : 1.0f;
    // Body-frame wheel matrix: centre, static alignment camber about Z, the
    // half turn for right wheels, then the lateral scale (and the tyre's
    // lateral offset in the rim's x).
    const float cam = vis.active ? (right ? -vis.camber_rad[size_t(axle)] : vis.camber_rad[size_t(axle)]) : 0.0f;
    const float cc = std::cos(cam), sc = std::sin(cam);
    auto wheel_matrix = [&](float sx, float dx, float* m4) {
      float base_m[16];
      identity(base_m);
      const float turn = right ? -1.0f : 1.0f;
      base_m[0] = turn * sx;
      base_m[10] = turn;
      base_m[12] = turn * dx;
      float local[16];
      if (cam != 0.0f) {
        float rz[16];
        identity(rz);
        rz[0] = cc;
        rz[1] = sc;
        rz[4] = -sc;
        rz[5] = cc;
        multiply(rz, base_m, local);
      } else {
        std::memcpy(local, base_m, sizeof local);
      }
      const auto& cb = info.wheel_centres_body[size_t(c)];
      std::memcpy(pending_local_, local, sizeof local);  // about the centre (set_wheel_poses)
      pending_wheel_ = c;
      pending_spins_ = true;
      local[12] += cb[0];
      local[13] += cb[1];
      local[14] += cb[2];
      multiply(body_to_model, local, m4);
    };
    if (rim) {
      float m4[16];
      const float rim_norm_dx =
          vis.active ? std::clamp(axle == 0 ? vis.rim_offset_front : vis.rim_offset_rear, -0.08f, 0.08f) : 0.0f;
      wheel_matrix(rim_scale, rim_norm_dx, m4);
      const bake::Model sized = morphed(*rim, {w_rim, 0});
      std::vector<std::string> parts;
      for (const auto& p : sized.parts) parts.push_back(p.name);
      add_model(sized, parts, m4, wheel_, axle);
    }
    if (tyre) {
      // Target A raises the outer radius, target B the inner one.
      const float w_outer = (outer - 0.2850f) / 0.1301f;
      // The rim's x extent (bytes 32 and 40), or its design faces (36 and 44)
      // where the extent is empty.
      const auto& hb = rim ? rim->header_bounds : std::array<float, 4>{};
      const float extent = extent_ok ? hb[0] - hb[2] : header_ok ? hb[1] - hb[3] : width_m;
      const float centre = extent_ok ? (hb[0] + hb[2]) / 2 : header_ok ? (hb[1] + hb[3]) / 2 : 0.0f;
      float m4[16];
      wheel_matrix(rim_scale * extent, rim_scale * centre, m4);
      const bake::Model sized = morphed(*tyre, {w_outer, w_rim});
      std::vector<std::string> parts;
      for (const auto& p : sized.parts) parts.push_back(p.name);
      add_model(sized, parts, m4, tyre_, axle);
    }
  }
  pending_wheel_ = -1;
  if (!rim) notes_.push_back("no rim model in " + info.wheel_media);
  if (!tyre) notes_.push_back("no tyre model in " + info.tyre_media);
  if (draws_.empty()) {
    error = car_name + ": nothing to draw";
    return false;
  }

  // Pre-register any alternate factory color/livery atlas textures in car_.car.colours so
  // preview_colour() can swap Combo_Colors.BaseTexture instantaneously without GPU stalls.
  if (car_.car.colours.size() > 1) {
    std::set<std::string> alt_atlases;
    for (const auto& col : car_.car.colours) {
      if (!col.atlas_lod0.empty()) alt_atlases.insert(col.atlas_lod0);
      if (!col.atlas_lod.empty()) alt_atlases.insert(col.atlas_lod);
    }
    for (const auto& member : alt_atlases) {
      (void)texture_index("car:" + member, car_, true);
    }
  }

  // Detect whether the wheel archive provided retail blur_rim / blur_lip geometry; if so,
  // tag static 3D rim spokes with kFlagHasBlurMesh so they cross-fade cleanly to the blur disc.
  has_blur_rim_mesh_ = false;
  for (const auto& d : draws_) {
    if (d.is_blur_rim) {
      has_blur_rim_mesh_ = true;
      break;
    }
  }
  if (has_blur_rim_mesh_) {
    for (auto& d : draws_) {
      if (d.wheel < 0 || !d.spins || d.is_blur_rim) continue;
      const auto kl = MaterialClass(uint32_t(d.push.params[0] + 0.5f));
      const int sm = int(d.push.params[2] + 0.5f);
      // Cross-fade outer spokes (kRim submat 0/1 or kChrome) while keeping inner barrel (2), rotor (3), hub (4) solid.
      if ((kl == MaterialClass::kRim && sm != 2 && sm != 3 && sm != 4) || kl == MaterialClass::kChrome) {
        d.is_static_spoke = true;
        uint32_t fl = uint32_t(std::lround(d.push.params[1]));
        fl |= kFlagHasBlurMesh;
        d.push.params[1] = float(fl);
      }
    }
  }

  // Derive per-car left, center, and right cockpit mirror camera origins from actual mirror
  // geometry bounds (mirrorLeft/mirrorMiddle/mirrorRight glass preferred, then mirrorL/R parts),
  // falling back to scaled car bounding-box offsets only when a mirror part is absent.
  {
    std::array<std::array<double, 3>, 3> glass_sum{};
    std::array<int, 3> glass_cnt{0, 0, 0};
    std::array<std::array<double, 3>, 3> part_sum{};
    std::array<int, 3> part_cnt{0, 0, 0};
    for (const auto& d : draws_) {
      if (d.wheel >= 0) continue;
      const uint32_t fl = uint32_t(std::lround(d.push.params[1]));
      const int sm = int(d.push.params[2] + 0.5f);
      if ((fl & kFlagMirror) != 0u) {
        const int band = (sm == 10) ? 0 : (sm == 12 ? 2 : 1);
        for (int k = 0; k < 3; ++k) glass_sum[size_t(band)][size_t(k)] += double(d.centre[size_t(k)]);
        ++glass_cnt[size_t(band)];
      } else if (d.part_name.rfind("mirrorl", 0) == 0) {
        for (int k = 0; k < 3; ++k) part_sum[0][size_t(k)] += double(d.centre[size_t(k)]);
        ++part_cnt[0];
      } else if (d.part_name.rfind("mirrorr", 0) == 0) {
        for (int k = 0; k < 3; ++k) part_sum[2][size_t(k)] += double(d.centre[size_t(k)]);
        ++part_cnt[2];
      } else if (d.part_name.find("mirrormid") != std::string::npos ||
                 d.part_name.find("mirrorcenter") != std::string::npos) {
        for (int k = 0; k < 3; ++k) part_sum[1][size_t(k)] += double(d.centre[size_t(k)]);
        ++part_cnt[1];
      }
    }
    const float bw_x = have_bounds_ ? (bounds_max_[0] - bounds_min_[0]) : 1.85f;
    const float bh_y = have_bounds_ ? (bounds_max_[1] - bounds_min_[1]) : 1.35f;
    const float mid_x = have_bounds_ ? 0.5f * (bounds_min_[0] + bounds_max_[0]) : 0.0f;
    const float mid_z = have_bounds_ ? 0.5f * (bounds_min_[2] + bounds_max_[2]) : 0.0f;
    const std::array<std::array<float, 3>, 3> fallback = {
        std::array<float, 3>{(have_bounds_ ? bounds_min_[0] : -0.92f) - 0.04f,
                             (have_bounds_ ? bounds_min_[1] : 0.0f) + bh_y * 0.66f, mid_z - 0.12f},
        std::array<float, 3>{mid_x, (have_bounds_ ? bounds_min_[1] : 0.0f) + bh_y * 0.82f, mid_z + 0.05f},
        std::array<float, 3>{(have_bounds_ ? bounds_max_[0] : 0.92f) + 0.04f,
                             (have_bounds_ ? bounds_min_[1] : 0.0f) + bh_y * 0.66f, mid_z - 0.12f},
    };
    (void)bw_x;
    for (size_t b = 0; b < 3; ++b) {
      if (glass_cnt[b] > 0) {
        for (size_t k = 0; k < 3; ++k) mirror_origins_[b][k] = float(glass_sum[b][k] / double(glass_cnt[b]));
        if (b == 0) mirror_origins_[b][0] -= 0.04f, mirror_origins_[b][2] += 0.08f;
        else if (b == 2) mirror_origins_[b][0] += 0.04f, mirror_origins_[b][2] += 0.08f;
        else mirror_origins_[b][2] += 0.10f;
        mirror_origin_valid_[b] = true;
      } else if (part_cnt[b] > 0) {
        for (size_t k = 0; k < 3; ++k) mirror_origins_[b][k] = float(part_sum[b][k] / double(part_cnt[b]));
        if (b == 0) mirror_origins_[b][0] -= 0.05f, mirror_origins_[b][2] += 0.10f;
        else if (b == 2) mirror_origins_[b][0] += 0.05f, mirror_origins_[b][2] += 0.10f;
        else mirror_origins_[b][2] += 0.10f;
        mirror_origin_valid_[b] = true;
      } else {
        mirror_origins_[b] = fallback[b];
        mirror_origin_valid_[b] = false;
      }
    }
  }

  // Initialize active shader finish parameters from Shared XML + per-car ColorShaderSettings/RimShaderSettings.
  {
    const auto& colours = car_.car.colours;
    const bake::Colour* col = colours.empty() ? nullptr : &colours[std::min(options_.colour, colours.size() - 1)];
    active_paint_params_ = (col && col->metallic) ? shader_settings_.metallic : shader_settings_.normal;
    if (col) {
      if (auto it = car_shader_overrides_.color_overrides.find(int(col->sequence));
          it != car_shader_overrides_.color_overrides.end()) {
        active_paint_params_ = it->second;
      } else if (auto it2 = car_shader_overrides_.color_overrides.find(int(options_.colour + 1));
                 it2 != car_shader_overrides_.color_overrides.end()) {
        active_paint_params_ = it2->second;
      }
    }
    sync_paint_shader_uniforms();
  }
  return true;
}

uint32_t Showroom::texture_index(const std::string& slot, const bake::Bake& pack, bool lod0, bool optional) {
  if (slot.empty()) return UINT32_MAX;
  const bake::Bake* source = nullptr;
  std::string member, key;
  bool allow_shared_fallback = false;
  if (slot == "@atlas") {
    const auto& colours = car_.car.colours;
    if (colours.empty()) return UINT32_MAX;
    const auto& colour = colours[std::min(options_.colour, colours.size() - 1)];
    member = lod0 && !colour.atlas_lod0.empty() ? colour.atlas_lod0 : colour.atlas_lod;
    if (member.empty()) member = colour.atlas_lod0;
    source = &car_;
    key = "car:" + member;
  } else if (slot.rfind("car:", 0) == 0) {
    member = slot.substr(4);
    source = &car_;
    key = slot;
    allow_shared_fallback = !optional;
  } else if (slot.rfind("wheel:", 0) == 0) {
    member = slot.substr(6);
    source = &pack;
    key = (&pack == &wheel_ ? "wheel:" : "tyre-model:") + member;
  } else if (slot.rfind("tyre:", 0) == 0) {
    member = tyre_texture_member(slot.substr(5), car_.car.tyre_texture_media);
    source = &tyre_textures_;
    key = "tyre:" + member;
  } else if (slot.rfind("shared:", 0) == 0) {
    member = slot.substr(7);
    source = &shared_;
    key = slot;
  } else if (slot == "livery_mask") {
    member = slot;
    source = &car_;
    key = "car:livery_mask";
  }
  if (!source) return UINT32_MAX;
  if (auto it = texture_by_key_.find(key); it != texture_by_key_.end()) {
    if (it->second != UINT32_MAX || optional) return it->second;
  }
  auto find_in = [](const bake::Bake& b, const std::string& name) -> const bake::Texture* {
    const std::string low = lower(name);
    for (const auto& t : b.textures) {
      if (t.name == name || lower(t.name) == low) return &t;
    }
    return nullptr;
  };
  const bake::Texture* found = find_in(*source, member);
  if (!found && allow_shared_fallback && source != &shared_) {
    const std::string shared_key = "shared:" + lower(member);
    if (auto it = texture_by_key_.find(shared_key); it != texture_by_key_.end() && it->second != UINT32_MAX) {
      texture_by_key_[key] = it->second;
      return it->second;
    }
    found = find_in(shared_, member);
    if (found) {
      const uint32_t index = uint32_t(textures_.size());
      textures_.push_back({found, shared_key});
      texture_by_key_[shared_key] = index;
      texture_by_key_[key] = index;
      return index;
    }
  }
  if (!found) {
    if (!optional) {
      if (texture_by_key_.find(key) == texture_by_key_.end()) {
        notes_.push_back("missing texture " + key);
      }
      texture_by_key_[key] = UINT32_MAX;
    }
    return UINT32_MAX;
  }
  const uint32_t index = uint32_t(textures_.size());
  textures_.push_back({found, key});
  texture_by_key_[key] = index;
  return index;
}


}  // namespace carview
