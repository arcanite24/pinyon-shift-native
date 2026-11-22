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
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.42f);
  ui4::title_banner(l, s, 56.0f * s, 26.0f * s, "MODS", "PINYON SHIFT // F6 OVERLAY", true, 430.0f);

  // The save indicator (top right): green for the player's own save, amber
  // for an isolated copy.
  {
    const bool iso = page.save_isolated();
    const float x1 = screen_w - 56.0f * s, x0 = x1 - 360.0f * s, y0 = 30.0f * s, y1 = 92.0f * s;
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, iso ? ui4::kAmber : ui4::kGreen, 0, true);
    l.circle(x0 + 22.0f * s, y0 + 22.0f * s, 6.0f * s, iso ? ui4::kAmber : ui4::kGreen);
    l.text(x0 + 36.0f * s, y0 + 14.0f * s, "NEXT START PLAYS", ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
    ui::TextStyle st = ui4::style(ui::Font::kE, 16.0f * s, iso ? ui4::kAmber : ui4::kGreen);
    ui4::fit_text(page.save_label(), st, 300.0f * s, 10.0f * s);
    l.text(x0 + 36.0f * s, y0 + 32.0f * s, page.save_label(), st);
    ui::TextStyle sm = ui4::style(ui::Font::kB, 9.5f * s, ui4::kWhite, ui::Align::kRight);
    const std::string tag = iso ? "SAVE ISOLATED" : "SHARED SAVE";
    l.text(x1 - 14.0f * s, y0 + 14.0f * s, tag, sm);
  }

  ui4::tab_strip(l, s, 56.0f * s, 108.0f * s, screen_w - 56.0f * s, {"LOAD ORDER", "TEXTURES", "DLC"}, 0, false,
                 ui4::kPink);

  const float top_y = 152.0f * s, bot_y = screen_h - 82.0f * s;
  const float lx0 = 56.0f * s, lw = 700.0f * s;
  const float rx0 = lx0 + lw + 18.0f * s, rx1 = screen_w - 56.0f * s;
  ui4::panel(l, lx0, top_y, lx0 + lw, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, lx0 + 12.0f * s, top_y + 10.0f * s, lx0 + lw - 12.0f * s, top_y + 34.0f * s, s, false);
  const auto rows = page.rows();
  int active = 0;
  for (const auto& r : rows) active += r.active;
  l.text(lx0 + 22.0f * s, top_y + 15.0f * s, "LOAD ORDER  •  TOP WINS", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
  l.text(lx0 + lw - 22.0f * s, top_y + 15.0f * s,
         std::to_string(active) + " OF " + std::to_string(rows.size()) + " ACTIVE" + (page.dirty() ? "  •  UNSAVED" : ""),
         ui4::style(ui::Font::kB, 11.5f * s, page.dirty() ? ui4::kPink : ui4::kInk, ui::Align::kRight));

  const float row_h = 48.0f * s;
  const float notice_reserve = page.notice().empty() ? 0.0f : 38.0f * s;
  const int max_vis = std::max(1, int((bot_y - top_y - 48.0f * s - notice_reserve) / row_h));
  const int n = int(rows.size());
  const int first = std::clamp(page.selected() - max_vis / 2, 0, std::max(0, n - max_vis));
  float ry = top_y + 42.0f * s;
  if (rows.empty()) {
    ui::TextStyle es = ui4::style(ui::Font::kB, 13.0f * s, ui4::kDim, ui::Align::kLeft, true);
    l.text(lx0 + 24.0f * s, ry + 12.0f * s, "No mods installed. Put mod folders in <user data>/mods.", es);
  }
  for (int i = first; i < std::min(n, first + max_vis); ++i) {
    const auto& r = rows[size_t(i)];
    const bool sel = i == page.selected();
    if (sel) ui4::brush(l, lx0 + 6.0f * s, ry, lx0 + lw - 6.0f * s, ry + row_h - 4.0f * s, ui4::kPink);
    else
      l.rect(lx0 + 12.0f * s, ry, lx0 + lw - 12.0f * s, ry + row_h - 6.0f * s,
             (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
    // Position, switch box, title, then status.
    l.text(lx0 + 24.0f * s, ry + 15.0f * s, std::to_string(i + 1), ui4::style(ui::Font::kE, 13.0f * s, ui4::kDim));
    const float bx = lx0 + 52.0f * s, by = ry + 11.0f * s, bs = 20.0f * s;
    l.rect(bx, by, bx + bs, by + bs, r.enabled ? (r.active ? ui4::kGreen : ui4::kAmber) : ui::rgba(255, 255, 255, 30));
    if (r.enabled) {
      l.line(bx + 4.0f * s, by + 10.0f * s, bx + 8.5f * s, by + 15.0f * s, 2.4f * s, ui4::kInk);
      l.line(bx + 8.5f * s, by + 15.0f * s, bx + 16.0f * s, by + 5.0f * s, 2.4f * s, ui4::kInk);
    }
    ui::TextStyle ts = ui4::style(sel ? ui::Font::kE : ui::Font::kB, (sel ? 15.0f : 14.0f) * s, ui4::kWhite);
    ui4::fit_text(r.title, ts, 330.0f * s, 10.0f * s);
    l.text(lx0 + 84.0f * s, ry + 8.0f * s, r.title, ts);
    std::string sub = (r.version.empty() ? "" : "V" + r.version + "  •  ") + r.summary;
    ui::TextStyle subs = ui4::style(ui::Font::kB, 10.0f * s, sel ? ui4::kWhite : ui4::kDim);
    ui4::fit_text(sub, subs, 380.0f * s, 8.0f * s);
    l.text(lx0 + 84.0f * s, ry + 28.0f * s, sub, subs);
    // Tags: save, native.
    float tx = lx0 + lw - 190.0f * s;
    if (r.touches_save) {
      l.rect(tx, ry + 13.0f * s, tx + 44.0f * s, ry + 31.0f * s, ui::fade(ui4::kAmber, 0.85));
      l.text(tx + 22.0f * s, ry + 17.0f * s, "SAVE", ui4::style(ui::Font::kB, 9.5f * s, ui4::kInk, ui::Align::kCenter));
    }
    if (r.native) {
      l.rect(tx + 50.0f * s, ry + 13.0f * s, tx + 104.0f * s, ry + 31.0f * s, ui::fade(ui4::kBlue, 0.9));
      l.text(tx + 77.0f * s, ry + 17.0f * s, "NATIVE", ui4::style(ui::Font::kB, 9.5f * s, ui4::kWhite, ui::Align::kCenter));
    }
    const uint32_t sc = r.active ? ui4::kGreen : (r.enabled ? ui4::kRed : ui4::kDim);
    l.text(lx0 + lw - 22.0f * s, ry + 15.0f * s, r.status,
           ui4::style(ui::Font::kE, 13.0f * s, sel && r.active ? ui4::kYellow : sc, ui::Align::kRight));
    ry += row_h;
  }

  // Details.
  ui4::panel(l, rx0, top_y, rx1, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, rx0 + 12.0f * s, top_y + 10.0f * s, rx1 - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "MOD DETAILS", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
  const float dw = (rx1 - rx0) - 40.0f * s;
  if (const auto* m = page.selected_mod()) {
    const auto& man = m->manifest;
    const auto& r = rows[size_t(page.selected())];
    ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s, ui4::kPink);
    ui::TextStyle tt = ui4::style(ui::Font::kE, 17.0f * s, ui4::kWhite);
    ui4::fit_text(man.title, tt, dw, 11.0f * s);
    l.text(rx0 + 20.0f * s, top_y + 54.0f * s, man.title, tt);
    float y = top_y + 96.0f * s;
    std::string by = (man.author.empty() ? std::string("UNKNOWN AUTHOR") : "BY " + man.author) +
                     (man.version.empty() ? "" : "  •  V" + man.version);
    ui::TextStyle bs = ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim);
    ui4::fit_text(by, bs, dw, 8.0f * s);
    l.text(rx0 + 20.0f * s, y, by, bs);
    y += 22.0f * s;
    ui::TextStyle ds = ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite, ui::Align::kLeft, true);
    const auto lines = wrap_text(man.description.empty() ? std::string("No description.") : man.description, ds, dw);
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 4); ++i, y += 18.0f * s) l.text(rx0 + 20.0f * s, y, lines[i], ds);
    y += 8.0f * s;
    auto kv = [&](const std::string& k, const std::string& v, uint32_t vc) {
      l.text(rx0 + 20.0f * s, y, k, ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
      ui::TextStyle vs = ui4::style(ui::Font::kE, 12.0f * s, vc);
      ui4::fit_text(v, vs, dw - 110.0f * s, 8.0f * s);
      l.text(rx0 + 128.0f * s, y - 1.0f * s, v, vs);
      y += 21.0f * s;
    };
    kv("CHANGES", m->content.summary(), ui4::kWhite);
    auto join = [](const std::vector<std::string>& v) {
      std::string o;
      for (const auto& x : v) o += (o.empty() ? "" : ", ") + x;
      return o.empty() ? std::string("-") : o;
    };
    kv("NEEDS", join(man.dependencies), ui4::kWhite);
    kv("CONFLICTS", join(man.conflicts), ui4::kWhite);
    kv("SAVE", !man.profile.empty() ? "OWN SAVE (user-" + man.profile + ")" : man.touches_save ? "MODDED COPY" : "KEEPS YOURS",
       man.touches_save || !man.profile.empty() ? ui4::kAmber : ui4::kGreen);
    kv("BUILD", man.game_version.empty() ? "ANY" : man.game_version, ui4::kWhite);
    if (!man.options.empty()) {
      const auto& o = man.options.front();
      std::string cur = o.choices[size_t(o.def)];
      if (auto it = page.config().options.find(m->name); it != page.config().options.end())
        if (auto jt = it->second.find(o.key); jt != it->second.end()) cur = jt->second;
      kv(o.label, "<  " + cur + "  >", ui4::kYellow);
    }
    if (!r.reason.empty()) kv("NOT LOADED", r.reason, ui4::kRed);
    if (man.kind == cars::modmgr::ModKind::kXe) kv("XE", "V" + man.xe_version + (man.xe_version == "1.0" ? " (NEEDS 1.01 HOTFIX)" : ""), ui4::kAmber);
  }
  // Status block: the save indicator detail, textures and UDLC.
  const float sy = bot_y - 112.0f * s;
  l.rect(rx0 + 16.0f * s, sy, rx1 - 16.0f * s, sy + 96.0f * s, ui::rgba(255, 255, 255, 14));
  ui::TextStyle hs = ui4::style(ui::Font::kB, 11.0f * s, ui4::kWhite, ui::Align::kLeft, true);
  const auto detail = wrap_text(page.save_detail(), hs, dw - 12.0f * s);
  float y = sy + 10.0f * s;
  for (size_t i = 0; i < std::min<size_t>(detail.size(), 2); ++i, y += 16.0f * s) l.text(rx0 + 26.0f * s, y, detail[i], hs);
  l.text(rx0 + 26.0f * s, sy + 50.0f * s, page.textures_line, ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
  if (!page.udlc_line.empty())
    l.text(rx0 + 26.0f * s, sy + 70.0f * s, page.udlc_line, ui4::style(ui::Font::kB, 10.5f * s, ui4::kPink));

  if (!page.notice().empty()) {
    l.rect(lx0 + 12.0f * s, bot_y - 34.0f * s, lx0 + lw - 12.0f * s, bot_y - 8.0f * s, ui::rgba(14, 15, 20, 220));
    ui::TextStyle ns = ui4::style(ui::Font::kE, 12.5f * s, ui4::kYellow, ui::Align::kCenter);
    ui4::fit_text(page.notice(), ns, lw - 44.0f * s, 9.0f * s);
    l.text(lx0 + lw * 0.5f, bot_y - 28.0f * s, page.notice(), ns);
  }
  ui4::button_bar(l,
                  {{"A", "SWITCH"}, {"LB", "MOVE UP"}, {"RB", "MOVE DOWN"}, {"DPAD_LR", "OPTION"}, {"X", "RELOAD TEXTURES"},
                   {"Y", "SAVE"}, {"B", "BACK"}},
                  screen_w, screen_h, 56.0f);
}

}  // namespace carview
