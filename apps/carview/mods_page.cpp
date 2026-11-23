#include "mods_page.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <sqlite3.h>

#include "cars/db_patch.h"
#include "cars/texture_mods.h"
#include "ui4_chrome.h"

namespace carview {
namespace fs = std::filesystem;
namespace tm = cars::texture_mods;

// ---------------------------------------------------------------- session

struct ModsSession::Tracked {
  struct Rec {
    uint64_t content = 0;
    std::string name;
    bool srgb = false;
    uint32_t base_w = 0, base_h = 0;
    std::shared_ptr<cars::bake::Texture> original;  // kept while a replacement is shown
  };
  std::mutex mutex;
  std::map<uint32_t, Rec> records;
};

ModsSession::ModsSession() : plugins_(std::make_unique<cars::plugins::Host>()), tracked_(std::make_unique<Tracked>()) {
  plugins_->set_log([this](const std::string& mod, std::string_view m) {
    std::printf("mod %s: %.*s\n", mod.c_str(), int(m.size()), m.data());
    log.push_back(mod + ": " + std::string(m));
  });
  plugins_->set_notify([this](const std::string& mod, std::string_view m, float seconds) {
    notices.emplace_back(mod + ": " + std::string(m), seconds);
  });
}

ModsSession::~ModsSession() { shutdown(); }

void ModsSession::set_state(fs::path root, std::vector<cars::modmgr::ModInfo> mods, cars::modmgr::Config cfg,
                            cars::modmgr::GameBuild build) {
  root_ = std::move(root);
  mods_ = std::move(mods);
  cfg_ = std::move(cfg);
  build_ = build;
  res_ = cars::modmgr::resolve(mods_, cfg_, build_);
}

fs::path ModsSession::boot(const fs::path& mods_root, const fs::path& main_save_dir, cars::modmgr::GameBuild build,
                           const fs::path& state_root) {
  root_ = mods_root;
  main_save_ = main_save_dir;
  build_ = build;
  mods_ = cars::modmgr::scan(mods_root);
  cars::modmgr::read_config(cars::modmgr::config_path(mods_root), cfg_);
  if (!state_root.empty()) udlc_ = cars::modmgr::detect_udlc(state_root);
  res_ = cars::modmgr::resolve(mods_, cfg_, build_);
  for (const auto& e : res_.entries) {
    const auto& m = mods_[e.mod];
    log.push_back(m.name + " (" + m.manifest.title + "): " + m.content.summary() + ", " +
                  (e.active ? "on" : (e.enabled ? "not loaded: " + e.reason : "off")));
    for (const auto& w : m.warnings) log.push_back(m.name + ": " + w);
  }
  for (const auto& n : res_.notes) log.push_back(n);
  iso_ = cars::modmgr::Isolation{};
  iso_.save_dir = main_save_dir;
  if (!main_save_dir.empty()) {
    std::string err;
    iso_ = cars::modmgr::isolate_save(main_save_dir, mods_, res_, &err);
    if (!err.empty()) log.push_back("save isolation: " + err);
    if (iso_.isolated)
      log.push_back("save: " + iso_.save_dir.string() + (iso_.created ? " (made now)" : "") +
                    " -- the player's own save is not opened while these mods are on");
  }
  return iso_.save_dir;
}

void ModsSession::activate(const fs::path& bake_dir, const fs::path& sound_dir, const fs::path& game_root) {
  cars::modmgr::ActivateOptions opts;
  opts.bake_dir = bake_dir;
  opts.sound_dir = sound_dir;
  opts.game_root = game_root;
  auto flag = [&](const char* key) {
    auto it = cfg_.enabled.find(key);
    return it == cfg_.enabled.end() || it->second;
  };
  opts.udlc = udlc_.mountable() && flag("dlc:udlc");
  opts.udlc_traffic = opts.udlc && udlc_.traffic_mountable() && flag("dlc:udlc-traffic");
  report_ = cars::modmgr::activate(mods_, res_, opts);
  for (const auto& n : report_.notes) log.push_back(n);
  if (opts.udlc) log.push_back(std::string("UDLC v1.2 mounted") + (opts.udlc_traffic ? " with the Traffic Car Unlocker" : ""));
  else if (udlc_.installed) log.push_back("UDLC not mounted: " + (udlc_.problem.empty() ? "switched off" : udlc_.problem));
}

void ModsSession::load_plugins(bool opt_in) {
  if (!res_.native_requested) return;
  if (!opt_in && !cfg_.native_plugins) {
    log.push_back("native mods are off (switch NATIVE MODS on in F6 > MODS, or start with --mods-native)");
    return;
  }
  for (size_t i : res_.active) {
    const auto& m = mods_[i];
    if (m.manifest.library.empty()) continue;
    std::map<std::string, std::string> options;
    for (const auto& o : m.manifest.options) options[o.key] = o.choices[size_t(o.def)];
    if (auto it = cfg_.options.find(m.name); it != cfg_.options.end())
      for (const auto& [k, v] : it->second) options[k] = v;
    std::string err;
    plugins_->load(m.name, m.dir, m.dir / m.manifest.library, std::move(options), &err);
  }
}

void ModsSession::tick(const ShiftModFrame& frame) {
  if (plugins_->alive()) plugins_->tick(frame);
}

void ModsSession::shutdown() {
  if (plugins_) plugins_->shutdown();
}

void ModsSession::on_world_texture(uint32_t record, bool srgb, cars::bake::Texture& texture) {
  if (res_.active.empty() || texture.levels.empty()) return;
  Tracked::Rec rec;
  rec.content = tm::content_hash(texture.levels[0].data);
  rec.name = texture.name;
  rec.srgb = srgb;
  rec.base_w = texture.levels[0].width;
  rec.base_h = texture.levels[0].height;
  if (tm::any()) {
    cars::bake::Texture out;
    if (tm::substitute(texture, out)) {
      rec.original = std::make_shared<cars::bake::Texture>(std::move(texture));
      texture = std::move(out);
    }
  }
  std::lock_guard<std::mutex> lock(tracked_->mutex);
  tracked_->records[record] = std::move(rec);
}

std::vector<std::string> ModsSession::reload_textures(const Reupload& reupload, size_t* reuploaded) {
  const auto changed = tm::registry().reload();
  if (reuploaded) *reuploaded = 0;
  if (changed.empty()) return changed;
  const std::set<std::string> keys(changed.begin(), changed.end());
  std::vector<std::pair<uint32_t, Tracked::Rec*>> hits;
  std::lock_guard<std::mutex> lock(tracked_->mutex);
  for (auto& [record, rec] : tracked_->records)
    if (keys.count(tm::content_key(rec.content)) || keys.count(tm::name_key(rec.name))) hits.emplace_back(record, &rec);
  for (auto& [record, rec] : hits) {
    cars::bake::Texture tex;
    bool have = false;
    if (auto file = tm::registry().find(rec->content, rec->name)) {
      tm::Image img;
      std::string why;
      if (tm::decode_image(*file, img, &why) && tm::scale_factor(rec->base_w, rec->base_h, img.width, img.height)) {
        tex = tm::to_texture(img, rec->name);
        have = true;
      } else {
        log.push_back("texture " + file->filename().string() + ": " + (why.empty() ? "wrong size" : why));
      }
    } else if (rec->original) {
      tex = *rec->original;  // the replacement was removed: back to the game's
      rec->original.reset();
      have = true;
    }
    if (have && reupload && reupload(record, tex, rec->srgb) && reuploaded) ++*reuploaded;
  }
  return changed;
}

// ---------------------------------------------------------------- page

void ModsPage::open(const ModsSession& session) {
  root_ = session.root();
  mods_ = session.mods();
  cfg_ = session.config();
  build_ = session.build();
  running_profile_ = session.resolution().save_profile;
  const auto& u = session.udlc();
  udlc_line = u.mountable() ? "UDLC V" + (u.version.empty() ? std::string("1.2") : u.version) +
                                  (cars::modmgr::udlc_mounted() ? " MOUNTED" : " OFF") +
                                  (u.traffic_mountable() ? "  +  TRAFFIC UNLOCKER" : "")
              : u.installed ? "UDLC: " + u.problem
                            : std::string();
  textures_line = std::to_string(tm::registry().size()) + " REPLACEMENT TEXTURES  •  F5 RELOADS";
  selected_ = 0;
  dirty_ = false;
  notice_.clear();
  open_ = true;
  refresh();
}

void ModsPage::refresh() {
  preview_ = cars::modmgr::resolve(mods_, cfg_, build_);
  selected_ = std::clamp(selected_, 0, std::max(0, int(preview_.entries.size()) - 1));
}

const cars::modmgr::ModInfo* ModsPage::selected_mod() const {
  if (preview_.entries.empty()) return nullptr;
  return &mods_[preview_.entries[size_t(selected_)].mod];
}

std::vector<ModsPage::Row> ModsPage::rows() const {
  std::vector<Row> out;
  for (const auto& e : preview_.entries) {
    const auto& m = mods_[e.mod];
    Row r;
    r.name = m.name;
    r.title = m.manifest.title;
    r.version = m.manifest.version;
    r.author = m.manifest.author;
    r.summary = m.content.summary();
    r.enabled = e.enabled;
    r.active = e.active;
    r.reason = e.reason;
    r.native = !m.manifest.library.empty();
    r.touches_save = m.manifest.touches_save || !m.manifest.profile.empty();
    r.status = e.active ? "ON" : (e.enabled ? "BLOCKED" : "OFF");
    out.push_back(std::move(r));
  }
  return out;
}

bool ModsPage::nav(Nav n) {
  const int count = int(preview_.entries.size());
  auto order_names = [&]() {
    std::vector<std::string> names;
    for (const auto& e : preview_.entries) names.push_back(mods_[e.mod].name);
    return names;
  };
  switch (n) {
    case Nav::kUp:
      if (count) selected_ = (selected_ + count - 1) % count;
      break;
    case Nav::kDown:
      if (count) selected_ = (selected_ + 1) % count;
      break;
    case Nav::kAccept:
      if (const auto* m = selected_mod()) {
        const bool now = preview_.entries[size_t(selected_)].enabled;
        cfg_.enabled[m->name] = !now;
        dirty_ = true;
        refresh();
        notice_ = std::string(now ? "SWITCHED OFF " : "SWITCHED ON ") + m->manifest.title + " (NEXT START)";
      }
      break;
    case Nav::kTabPrev:
    case Nav::kTabNext: {
      if (count < 2) break;
      auto names = order_names();
      const int to = selected_ + (n == Nav::kTabPrev ? -1 : 1);
      if (to < 0 || to >= count) break;
      std::swap(names[size_t(selected_)], names[size_t(to)]);
      const std::string moved = names[size_t(to)];
      cfg_.order = names;
      dirty_ = true;
      refresh();
      for (int i = 0; i < int(preview_.entries.size()); ++i)
        if (mods_[preview_.entries[size_t(i)].mod].name == moved) selected_ = i;
      break;
    }
    case Nav::kLeft:
    case Nav::kRight:
      if (const auto* m = selected_mod(); m && !m->manifest.options.empty()) {
        const auto& o = m->manifest.options.front();
        std::string cur = o.choices[size_t(o.def)];
        if (auto it = cfg_.options[m->name].find(o.key); it != cfg_.options[m->name].end()) cur = it->second;
        const auto at = std::find(o.choices.begin(), o.choices.end(), cur);
        int idx = at == o.choices.end() ? o.def : int(at - o.choices.begin());
        idx = (idx + (n == Nav::kLeft ? -1 : 1) + int(o.choices.size())) % int(o.choices.size());
        cfg_.options[m->name][o.key] = o.choices[size_t(idx)];
        dirty_ = true;
      }
      break;
    case Nav::kX:
      return true;  // reload textures
    case Nav::kY: {
      std::string err;
      if (!save(&err)) notice_ = "NOT SAVED: " + err;
      break;
    }
    case Nav::kBack:
      close();
      break;
    default:
      break;
  }
  return false;
}

bool ModsPage::save(std::string* error) {
  if (cfg_.order.empty())
    for (const auto& e : preview_.entries) cfg_.order.push_back(mods_[e.mod].name);
  if (!cars::modmgr::write_config(cars::modmgr::config_path(root_), cfg_, error)) return false;
  dirty_ = false;
  notice_ = "SAVED TO MODS.TOML: CHANGES APPLY AT THE NEXT START";
  return true;
}

std::string ModsPage::save_label() const {
  if (preview_.save_profile.empty()) return "YOUR SAVE";
  if (preview_.save_profile == "modded") return "MODDED COPY";
  std::string p = preview_.save_profile;
  for (char& c : p) c = char(std::toupper(static_cast<unsigned char>(c)));
  return "OWN SAVE: " + p;
}

std::string ModsPage::save_detail() const {
  std::string d = preview_.save_profile.empty()
                      ? "No active mod changes saves: you play your own profile."
                  : preview_.save_profile == "modded"
                      ? "user-modded: a copy of your save. Your own save is untouched while these mods are on."
                      : "user-" + preview_.save_profile + ": a new save for this mod. Your own save is untouched.";
  if (preview_.save_profile != running_profile_) d += " The next start switches save.";
  return d;
}

// ---------------------------------------------------------------- drawing

namespace {
std::atomic<ModsSession*> g_texture_session{nullptr};
}

void set_world_texture_session(ModsSession* session) { g_texture_session = session; }

void world_texture_hook(uint32_t record, bool srgb, cars::bake::Texture& texture) {
  if (ModsSession* s = g_texture_session.load()) s->on_world_texture(record, srgb, texture);
}

size_t extend_baked_catalog(std::map<int64_t, std::string>& baked_ids, const cars::PartsDb* db,
                            const ModsSession& session) {
  size_t added = 0;
  std::error_code ec;
  // 1. Any new car physics files shipped directly in active mods' bake/cars/.
  for (size_t i : session.resolution().active) {
    const fs::path dir = session.mods()[i].dir / "bake" / "cars";
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
      if (!f.path().string().ends_with(".physics.txt")) continue;
      std::ifstream in(f.path());
      for (std::string line; std::getline(in, line);) {
        if (line.rfind("car ", 0) != 0) continue;
        std::istringstream ls(line.substr(4));
        int64_t id = 0;
        std::string media;
        if ((ls >> id >> media) && !baked_ids.count(id)) {
          baked_ids[id] = media;
          ++added;
        }
        break;
      }
    }
  }
  // 2. Cars added by SQL / *_merge.slt database patches that reuse a baked model.
  if (db && db->is_open()) {
    std::vector<std::pair<int64_t, std::string>> rows;
    std::string err;
    db->each(
        "SELECT Id, MediaName FROM Data_Car", {},
        [&](sqlite3_stmt* st) {
          const int64_t id = sqlite3_column_int64(st, 0);
          const auto* txt = sqlite3_column_text(st, 1);
          if (txt) rows.emplace_back(id, reinterpret_cast<const char*>(txt));
        },
        &err);
    added += cars::db_patch::extend_catalog(baked_ids, rows).size();
  }
  return added;
}

std::vector<std::string> wrap_text(const std::string& text, const ui::TextStyle& style, float width) {
  std::vector<std::string> out;
  std::string para;
  auto flush_para = [&](const std::string& p) {
    std::string line, word;
    auto push_word = [&]() {
      if (word.empty()) return;
      const std::string trial = line.empty() ? word : line + " " + word;
      if (!line.empty() && ui::List::measure(trial, style) > width) {
        out.push_back(line);
        line = word;
      } else {
        line = trial;
      }
      word.clear();
    };
    for (char c : p) {
      if (c == ' ') push_word();
      else word += c;
    }
    push_word();
    out.push_back(line);
  };
  for (char c : text + "\n") {
    if (c == '\n') {
      flush_para(para);
      para.clear();
    } else {
      para += c;
    }
  }
  while (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

void draw_mods_page(ui::List& l, float screen_w, float screen_h, const ModsPage& page) {
  if (!page.is_open()) return;

  const auto rows = page.rows();
  int active = 0;
  for (const auto& r : rows) active += r.active;

  const bool iso = page.save_isolated();
  ui4::TiltedBladeSpec spec;
  spec.title = "MODS";
  spec.eyebrow = "PINYON SHIFT // F6 OVERLAY";
  spec.scene_hint = "H_FIXED_SETTING_LIST";
  spec.status_kicker = "NEXT START PLAYS";
  spec.status_value = page.save_label();
  spec.status_tag = iso ? "SAVE ISOLATED" : "SHARED SAVE";
  spec.status_color = iso ? ui4::kAmber : ui4::kGreen;
  spec.tabs = {"LOAD ORDER", "TEXTURES", "DLC"};
  spec.active_tab = 0;
  spec.show_tab_bumpers = false;
  spec.list_header = "LOAD ORDER  •  TOP WINS";
  spec.list_header_right =
      std::to_string(active) + " OF " + std::to_string(rows.size()) + " ACTIVE" + (page.dirty() ? "  •  UNSAVED" : "");
  spec.empty_message = "No mods installed. Put mod folders in <user data>/mods.";
  spec.selected_index = page.selected();
  spec.max_visible = 9;
  spec.notice = page.notice();

  spec.rows.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    ui4::BladeRow br;
    br.index_label = std::to_string(i + 1);
    br.has_checkbox = true;
    br.checked = r.enabled;
    br.checkbox_color = r.active ? ui4::kGreen : ui4::kAmber;
    br.label = r.title;
    br.subtitle = (r.version.empty() ? "" : "V" + r.version + "  •  ") + r.summary;
    br.selected = (int(i) == page.selected());
    if (r.touches_save) {
      br.badge_tag = "SAVE";
      br.badge_color = ui4::kAmber;
    } else if (r.native) {
      br.badge_tag = "NATIVE";
      br.badge_color = ui4::kBlue;
    }
    br.value = r.status;
    br.value_color = r.active ? ui4::kGreen : (r.enabled ? ui4::kRed : ui4::kDim);
    spec.rows.push_back(std::move(br));
  }

  spec.detail_header = "MOD DETAILS";
  if (const auto* m = page.selected_mod()) {
    const auto& man = m->manifest;
    const auto& r = rows[size_t(page.selected())];
    spec.detail_title = man.title;
    spec.detail_title_color = ui4::kPink;
    const std::string by = (man.author.empty() ? std::string("UNKNOWN AUTHOR") : "BY " + man.author) +
                           (man.version.empty() ? "" : "  •  V" + man.version);
    spec.detail_paragraphs.push_back(by);
    spec.detail_paragraphs.push_back(man.description.empty() ? std::string("No description.") : man.description);

    auto join = [](const std::vector<std::string>& v) {
      std::string o;
      for (const auto& x : v) o += (o.empty() ? "" : ", ") + x;
      return o.empty() ? std::string("NONE") : o;
    };
    spec.detail_kvs.push_back({"CHANGES", m->content.summary(), ui4::kWhite});
    spec.detail_kvs.push_back({"NEEDS", join(man.dependencies), man.dependencies.empty() ? ui4::kDim : ui4::kWhite});
    spec.detail_kvs.push_back({"CONFLICTS", join(man.conflicts), man.conflicts.empty() ? ui4::kDim : ui4::kWhite});
    spec.detail_kvs.push_back(
        {"SAVE",
         !man.profile.empty() ? "OWN SAVE (user-" + man.profile + ")" : man.touches_save ? "MODDED COPY" : "KEEPS YOURS",
         (man.touches_save || !man.profile.empty()) ? ui4::kAmber : ui4::kGreen});
    spec.detail_kvs.push_back({"BUILD", man.game_version.empty() ? "ANY" : man.game_version, ui4::kWhite});
    if (!man.options.empty()) {
      const auto& o = man.options.front();
      std::string cur = o.choices[size_t(o.def)];
      if (auto it = page.config().options.find(m->name); it != page.config().options.end())
        if (auto jt = it->second.find(o.key); jt != it->second.end()) cur = jt->second;
      spec.detail_kvs.push_back({o.label, cur, ui4::kYellow});
    }
    if (!r.reason.empty()) spec.detail_kvs.push_back({"NOT LOADED", r.reason, ui4::kRed});
    if (man.kind == cars::modmgr::ModKind::kXe) {
      spec.detail_kvs.push_back(
          {"XE", "V" + man.xe_version + (man.xe_version == "1.0" ? " (NEEDS 1.01 HOTFIX)" : ""), ui4::kAmber});
    }
  }
  spec.detail_footer = page.save_detail();
  if (!page.textures_line.empty()) spec.detail_footer += "  " + page.textures_line;
  if (!page.udlc_line.empty()) spec.detail_footer += "  " + page.udlc_line;

  spec.prompts = {{"A", "SWITCH"},      {"LB", "MOVE UP"},        {"RB", "MOVE DOWN"}, {"DPAD_LR", "OPTION"},
                  {"X", "RELOAD TEXTURES"}, {"Y", "SAVE"},        {"B", "BACK"}};
  ui4::tilted_blade_screen(l, screen_w, screen_h, spec);
}

}  // namespace carview
