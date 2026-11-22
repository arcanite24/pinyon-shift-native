#include "cars/mod_manager.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#include "cars/db_patch.h"
#include "cars/mod_merge.h"
#include "cars/mods.h"
#include "cars/shader_mods.h"
#include "cars/sources.h"
#include "cars/texture_mods.h"
#include "toml_lite.h"

namespace cars::modmgr {
namespace fs = std::filesystem;
namespace {

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return f ? std::string(std::istreambuf_iterator<char>(f), {}) : std::string();
}

std::string lower_ascii(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> split_list(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s + ",") {
    if (c == ',' || c == ' ') {
      if (!cur.empty()) out.push_back(cur), cur.clear();
    } else {
      cur += c;
    }
  }
  return out;
}

int count_files(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return 0;
  int n = 0;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec))
    if (it->is_regular_file(ec)) ++n;
  return n;
}

// A JSON string field ("key": "value"), enough for the XE install record.
std::string json_string(const std::string& text, const std::string& key) {
  const auto at = text.find("\"" + key + "\"");
  if (at == std::string::npos) return {};
  const auto colon = text.find(':', at);
  const auto q0 = colon == std::string::npos ? colon : text.find('"', colon);
  const auto q1 = q0 == std::string::npos ? q0 : text.find('"', q0 + 1);
  return q1 == std::string::npos ? std::string() : text.substr(q0 + 1, q1 - q0 - 1);
}

GameBuild build_of(const std::string& game_version) {
  const std::string v = lower_ascii(game_version);
  if (v.empty() || v == "any") return GameBuild::kAny;
  // Pinyon Shift manifests name the executable's SHA-256 prefix.
  if (v == "base" || v.rfind("db40df605ade49a6", 0) == 0) return GameBuild::kBase;
  if (v == "v4" || v.rfind("74b033805ab1bcaa", 0) == 0) return GameBuild::kV4;
  return GameBuild::kAny;
}

// The registry discover_install consults.
std::mutex g_hidden_mutex;
std::set<std::string> g_hidden;  // lower case
std::atomic<bool> g_udlc{false}, g_udlc_traffic{false};

}  // namespace

const char* build_name(GameBuild b) {
  switch (b) {
    case GameBuild::kBase: return "base";
    case GameBuild::kV4: return "v4";
    default: return "any";
  }
}

std::string Content::summary() const {
  std::string out;
  auto add = [&](const char* label, int n) {
    if (n > 0) out += (out.empty() ? "" : "  ") + std::string(label) + " " + std::to_string(n);
  };
  add("CARS", bake);
  add("SOUND", sound);
  add("FILES", media + radio);
  add("DB", db);
  add("MEMBERS", members);
  add("XML", xml);
  add("TEXT", strings);
  add("TEX", textures);
  add("SHADERS", shaders);
  if (native) out += (out.empty() ? "" : "  ") + std::string("NATIVE");
  return out.empty() ? "EMPTY" : out;
}

bool parse_manifest_toml(std::string_view text, Manifest& out, std::vector<std::string>* warnings) {
  const toml::Doc doc = toml::parse(text);
  if (warnings)
    for (const auto& e : doc.errors) warnings->push_back(e);
  std::map<std::string, ModOption> options;
  std::vector<std::string> option_order;
  for (const toml::Item& it : doc.items) {
    const toml::Value& v = it.value;
    if (it.table.rfind("options.", 0) == 0) {
      const std::string key = it.table.substr(8);
      if (!options.count(key)) option_order.push_back(key);
      ModOption& o = options[key];
      o.key = key;
      if (it.key == "label") o.label = v.s;
      else if (it.key == "choices") o.choices = v.arr;
      else if (it.key == "default") o.def = v.kind == toml::Value::kInt ? int(v.i) : 0;
      continue;
    }
    if (!it.table.empty()) continue;
    const std::string& k = it.key;
    if (k == "name") out.name = v.s;
    else if (k == "title") out.title = v.s;
    else if (k == "author") out.author = v.s;
    else if (k == "version") out.version = v.kind == toml::Value::kString ? v.s : std::to_string(v.i);
    else if (k == "description") out.description = v.s;
    else if (k == "preview") out.preview = v.s;
    else if (k == "requires") out.dependencies = v.arr;
    else if (k == "conflicts") out.conflicts = v.arr;
    else if (k == "load_after") out.load_after = v.arr;
    else if (k == "hide_dlc") out.hide_dlc = v.arr;
    else if (k == "priority") out.priority = int(v.i);
    else if (k == "enabled") out.enabled = v.b;
    else if (k == "touches_save") out.touches_save = v.b;
    else if (k == "shares_save") out.touches_save = !v.b;  // Pinyon Shift's spelling
    else if (k == "profile") out.profile = v.s;
    else if (k == "library") out.library = v.s;
    else if (k == "game_version") out.game_version = v.s;
    else if (k == "abi" || k == "kind") continue;
    else if (warnings) warnings->push_back("line " + std::to_string(it.line) + ": unknown key " + k);
  }
  for (const auto& key : option_order) {
    ModOption o = options[key];
    if (o.label.empty()) o.label = key;
    if (o.choices.empty()) continue;
    o.def = std::clamp(o.def, 0, int(o.choices.size()) - 1);
    out.options.push_back(std::move(o));
  }
  return doc.errors.empty();
}

void parse_manifest_txt(std::string_view text, Manifest& out) {
  std::istringstream in{std::string(text)};
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const auto sp = line.find(' ');
    if (line.empty() || line[0] == '#' || sp == std::string::npos) continue;
    const std::string k = line.substr(0, sp), v = line.substr(sp + 1);
    if (k == "title") out.title = v;
    else if (k == "name") out.name = v;
    else if (k == "author") out.author = v;
    else if (k == "version") out.version = v;
    else if (k == "description") out.description = v;
    else if (k == "preview") out.preview = v;
    else if (k == "enabled") out.enabled = v != "0";
    else if (k == "priority") out.priority = std::atoi(v.c_str());
    else if (k == "requires") out.dependencies = split_list(v);
    else if (k == "conflicts") out.conflicts = split_list(v);
    else if (k == "load_after") out.load_after = split_list(v);
    else if (k == "hide_dlc") out.hide_dlc = split_list(v);
    else if (k == "touches_save") out.touches_save = v != "0";
    else if (k == "profile") out.profile = v;
    else if (k == "library") out.library = v;
    else if (k == "game_version") out.game_version = v;
  }
}

std::vector<ModInfo> scan(const fs::path& root) {
  std::vector<ModInfo> out;
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root, ec)) return out;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (!e.is_directory(ec)) continue;
    ModInfo m;
    m.name = e.path().filename().string();
    if (m.name.empty() || m.name[0] == '_' || m.name[0] == '.') continue;
    m.dir = e.path();
    const bool has_toml = fs::is_regular_file(m.dir / "mod.toml", ec);
    const bool has_txt = fs::is_regular_file(m.dir / "mod.txt", ec);
    const bool has_xe = fs::is_regular_file(m.dir / "xe-install.json", ec);
    if (!has_toml && !has_txt && !has_xe) continue;
    m.manifest.title = m.name;
    if (has_toml) parse_manifest_toml(read_text(m.dir / "mod.toml"), m.manifest, &m.warnings);
    else if (has_txt) parse_manifest_txt(read_text(m.dir / "mod.txt"), m.manifest);
    if (has_xe) {
      // Forza Horizon XE as tools/pinyon.py xe install lays it out: XE's
      // changed game files under game/, made for the base disc, hiding
      // Horizon Rally (whose database XE replaces) and playing its own save.
      Manifest& x = m.manifest;
      x.kind = ModKind::kXe;
      x.xe_version = json_string(read_text(m.dir / "xe-install.json"), "version");
      if (x.title == m.name) x.title = "Forza Horizon XE " + x.xe_version;
      if (x.author.empty()) x.author = "XE team";
      if (x.game_version.empty()) x.game_version = "base";
      if (x.profile.empty()) x.profile = "xe";
      if (std::find(x.hide_dlc.begin(), x.hide_dlc.end(), kRallyPackage) == x.hide_dlc.end())
        x.hide_dlc.push_back(kRallyPackage);
      if (x.xe_version == "1.0" && std::find(x.hide_dlc.begin(), x.hide_dlc.end(), kUdlcPackage) == x.hide_dlc.end())
        x.hide_dlc.push_back(kUdlcPackage);
      if (x.description.empty())
        x.description = "About 170 cars and engine swaps. Base disc only; hides Horizon Rally; plays its own save.";
    }
    if (m.manifest.name.empty()) m.manifest.name = m.name;
    if (m.manifest.name != m.name)
      m.warnings.push_back("manifest name " + m.manifest.name + " differs from the folder; using " + m.name);
    Content& c = m.content;
    c.bake = count_files(m.dir / "bake");
    c.sound = count_files(m.dir / "sound");
    c.media = count_files(m.dir / "media") + count_files(m.dir / "game" / "media");
    c.radio = count_files(m.dir / "radio");
    c.db = count_files(m.dir / "db");
    c.members = count_files(m.dir / "members") + count_files(m.dir / "add");
    c.xml = count_files(m.dir / "xml_patches") + count_files(m.dir / "merge");
    c.strings = count_files(m.dir / "stringtables") + count_files(m.dir / "strings");
    c.textures = count_files(m.dir / "textures");
    c.shaders = count_files(m.dir / "shaders");
    c.native = !m.manifest.library.empty() && fs::is_regular_file(m.dir / m.manifest.library, ec);
    if (!m.manifest.library.empty() && !c.native)
      m.warnings.push_back("library " + m.manifest.library + " is missing");
    const std::string preview = m.manifest.preview.empty() ? "preview.png" : m.manifest.preview;
    m.has_preview = fs::is_regular_file(m.dir / preview, ec);
    out.push_back(std::move(m));
  }
  std::sort(out.begin(), out.end(), [](const ModInfo& a, const ModInfo& b) { return a.name < b.name; });
  return out;
}

fs::path config_path(const fs::path& root) { return root.empty() ? root : root / "mods.toml"; }

bool read_config(const fs::path& file, Config& out) {
  out = Config{};
  std::error_code ec;
  if (file.empty() || !fs::is_regular_file(file, ec)) return false;
  const toml::Doc doc = toml::parse(read_text(file));
  for (const toml::Item& it : doc.items) {
    if (it.table.rfind("options.", 0) == 0) {
      out.options[it.table.substr(8)][it.key] =
          it.value.kind == toml::Value::kString ? it.value.s : std::to_string(it.value.i);
      continue;
    }
    if (!it.table.empty()) continue;
    if (it.key == "order") out.order = it.value.arr;
    else if (it.key == "enabled")
      for (const auto& n : it.value.arr) out.enabled[n] = true;
    else if (it.key == "disabled")
      for (const auto& n : it.value.arr) out.enabled[n] = false;
    else if (it.key == "native_plugins") out.native_plugins = it.value.b;
  }
  return true;
}

bool write_config(const fs::path& file, const Config& cfg, std::string* error) {
  std::vector<std::string> on, off;
  for (const auto& [n, e] : cfg.enabled) (e ? on : off).push_back(n);
  std::ostringstream s;
  s << "# shift-engine mods: the load order (the winner first) and switches.\n"
    << "# Written by the Mods page; edit with the game closed.\n";
  s << "native_plugins = " << (cfg.native_plugins ? "true" : "false") << "\n";
  s << "order = " << toml::quote_list(cfg.order) << "\n";
  s << "enabled = " << toml::quote_list(on) << "\n";
  s << "disabled = " << toml::quote_list(off) << "\n";
  for (const auto& [mod, kv] : cfg.options) {
    if (kv.empty()) continue;
    s << "\n[options." << mod << "]\n";
    for (const auto& [k, v] : kv) s << k << " = " << toml::quote(v) << "\n";
  }
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !(f << s.str())) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    if (error) *error = "cannot replace " + file.string() + ": " + ec.message();
    return false;
  }
  return true;
}

Resolution resolve(const std::vector<ModInfo>& mods, const Config& cfg, GameBuild build) {
  Resolution r;
  const size_t n = mods.size();
  std::map<std::string, size_t> by_name;
  for (size_t i = 0; i < n; ++i) by_name[mods[i].name] = i;

  // The load order: the configured names, then the rest by priority.
  std::vector<size_t> order;
  std::vector<bool> placed(n, false);
  for (const auto& name : cfg.order)
    if (auto it = by_name.find(name); it != by_name.end() && !placed[it->second])
      order.push_back(it->second), placed[it->second] = true;
  std::vector<size_t> rest;
  for (size_t i = 0; i < n; ++i)
    if (!placed[i]) rest.push_back(i);
  std::stable_sort(rest.begin(), rest.end(), [&](size_t a, size_t b) {
    return mods[a].manifest.priority != mods[b].manifest.priority
               ? mods[a].manifest.priority > mods[b].manifest.priority
               : mods[a].name < mods[b].name;
  });
  order.insert(order.end(), rest.begin(), rest.end());

  // A mod that builds on another goes above it (its changes win).
  auto pos_of = [&](size_t idx) { return size_t(std::find(order.begin(), order.end(), idx) - order.begin()); };
  bool changed = true;
  size_t passes = 0;
  while (changed && passes++ < n * n + 1) {
    changed = false;
    for (size_t i = 0; i < n && !changed; ++i) {
      std::vector<std::string> deps = mods[i].manifest.dependencies;
      deps.insert(deps.end(), mods[i].manifest.load_after.begin(), mods[i].manifest.load_after.end());
      for (const auto& d : deps) {
        auto it = by_name.find(d);
        if (it == by_name.end() || it->second == i) continue;
        const size_t pm = pos_of(i), pd = pos_of(it->second);
        if (pm > pd) {
          order.erase(order.begin() + long(pm));
          order.insert(order.begin() + long(pd), i);
          changed = true;
          break;
        }
      }
    }
  }
  if (changed) r.notes.push_back("the mods' requires / load_after form a cycle; the order is as listed");

  std::vector<bool> enabled(n), active(n);
  std::vector<std::string> reason(n);
  for (size_t i = 0; i < n; ++i) {
    auto it = cfg.enabled.find(mods[i].name);
    enabled[i] = it != cfg.enabled.end() ? it->second : mods[i].manifest.enabled;
    active[i] = enabled[i];
    if (!enabled[i]) continue;
    const GameBuild want = build_of(mods[i].manifest.game_version);
    if (build != GameBuild::kAny && want != GameBuild::kAny && want != build) {
      active[i] = false;
      reason[i] = std::string("made for the ") + build_name(want) + " build";
    }
  }
  for (bool again = true; again;) {
    again = false;
    for (size_t oi = 0; oi < n; ++oi) {
      const size_t i = order[oi];
      if (!active[i]) continue;
      auto drop = [&](const std::string& why) {
        active[i] = false;
        reason[i] = why;
        again = true;
      };
      for (const auto& req : mods[i].manifest.dependencies) {
        auto it = by_name.find(req);
        if (it == by_name.end() || !active[it->second]) {
          drop("needs " + req);
          break;
        }
      }
      if (!active[i]) continue;
      // A conflict either way with an active mod above it.
      for (size_t oj = 0; oj < oi && active[i]; ++oj) {
        const size_t j = order[oj];
        if (!active[j]) continue;
        const auto& cj = mods[j].manifest.conflicts;
        const auto& ci = mods[i].manifest.conflicts;
        if (std::find(cj.begin(), cj.end(), mods[i].name) != cj.end() ||
            std::find(ci.begin(), ci.end(), mods[j].name) != ci.end())
          drop("conflicts with " + mods[j].name);
      }
    }
  }
  // One save per run: the first active mod with its own profile decides.
  std::string profile;
  for (size_t oi = 0; oi < n; ++oi) {
    const size_t i = order[oi];
    if (!active[i] || mods[i].manifest.profile.empty()) continue;
    if (profile.empty()) {
      profile = mods[i].manifest.profile;
    } else if (mods[i].manifest.profile != profile) {
      active[i] = false;
      reason[i] = "plays its own save (" + mods[i].manifest.profile + ")";
    }
  }
  // Anything that required a mod dropped for its profile.
  for (bool again = true; again;) {
    again = false;
    for (size_t i = 0; i < n; ++i) {
      if (!active[i]) continue;
      for (const auto& req : mods[i].manifest.dependencies)
        if (auto it = by_name.find(req); it == by_name.end() || !active[it->second]) {
          active[i] = false, reason[i] = "needs " + req, again = true;
          break;
        }
    }
  }

  std::set<std::string> hidden;
  for (size_t oi = 0; oi < n; ++oi) {
    const size_t i = order[oi];
    Entry e;
    e.mod = i;
    e.enabled = enabled[i];
    e.active = active[i];
    e.reason = reason[i];
    r.entries.push_back(e);
    if (!active[i]) continue;
    r.active.push_back(i);
    const Manifest& m = mods[i].manifest;
    for (const auto& h : m.hide_dlc)
      if (hidden.insert(h).second) r.hidden_dlc.push_back(h);
    if (m.touches_save || !m.profile.empty()) r.touches_save = true;
    if (!m.library.empty()) r.native_requested = true;
    if (m.kind == ModKind::kXe && m.xe_version == "1.0")
      r.notes.push_back("XE 1.0 without its 1.01 hotfix: UDLC is hidden while XE is on");
  }
  r.save_profile = !profile.empty() ? profile : (r.touches_save ? "modded" : "");
  if (r.native_requested && !cfg.native_plugins)
    r.notes.push_back("native mods are off: switch them on to run mod code");
  return r;
}

fs::path save_dir_for(const fs::path& main_save_dir, const std::string& profile) {
  return profile.empty() ? main_save_dir : main_save_dir / ("user-" + profile);
}

uint64_t mod_hash(const ModInfo& m) {
  // FNV-1a over the manifest text and the sorted file list with sizes.
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](std::string_view s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  };
  std::error_code ec;
  for (const char* man : {"mod.toml", "mod.txt", "xe-install.json"}) mix(read_text(m.dir / man));
  std::vector<std::string> files;
  for (auto it = fs::recursive_directory_iterator(m.dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec))
    if (it->is_regular_file(ec))
      files.push_back(fs::relative(it->path(), m.dir, ec).generic_string() + ":" +
                      std::to_string(it->file_size(ec)));
  std::sort(files.begin(), files.end());
  for (const auto& f : files) mix(f);
  return h;
}

Isolation isolate_save(const fs::path& main_save_dir, const std::vector<ModInfo>& mods, const Resolution& r,
                       std::string* error) {
  Isolation out;
  out.save_dir = main_save_dir;
  out.profile = r.save_profile;
  if (r.save_profile.empty() || main_save_dir.empty()) return out;
  out.isolated = true;
  out.save_dir = save_dir_for(main_save_dir, r.save_profile);
  std::error_code ec;
  const bool exists = fs::is_directory(out.save_dir, ec);
  if (!exists) {
    fs::create_directories(out.save_dir, ec);
    if (ec) {
      if (error) *error = "cannot make " + out.save_dir.string() + ": " + ec.message();
      return out;
    }
    out.created = true;
    // The shared modded save starts as a copy of the player's own; a mod's
    // own profile starts new (its readme asks for one).
    if (r.save_profile == "modded")
      for (const auto& e : fs::directory_iterator(main_save_dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string fname = e.path().filename().string();
        if (fname == "mods.lock" || fname.ends_with(".tmp")) continue;
        fs::copy_file(e.path(), out.save_dir / fname, fs::copy_options::skip_existing, ec);
      }
  }
  std::ostringstream lock;
  lock << "shift-mods-lock 1\nprofile " << r.save_profile << "\n";
  for (size_t i : r.active) {
    const auto& m = mods[i];
    lock << "mod " << m.name << " " << (m.manifest.version.empty() ? "-" : m.manifest.version) << " "
         << texture_mods::hex16(mod_hash(m)) << "\n";
  }
  std::ofstream f(out.save_dir / "mods.lock", std::ios::binary | std::ios::trunc);
  f << lock.str();
  return out;
}

UdlcStatus detect_udlc(const fs::path& state_root) {
  UdlcStatus s;
  if (state_root.empty()) {
    s.problem = "no state folder";
    return s;
  }
  std::error_code ec;
  const fs::path title = state_root / "user" / "0000000000000000" / "4D5309C9";
  auto probe = [&](const std::string& id, bool& installed, bool& header, bool& enabled) {
    enabled = fs::is_directory(title / "00000002" / id, ec);
    installed = enabled || fs::is_directory(title / "Disabled" / "00000002" / id, ec);
    header = fs::is_regular_file(title / "Headers" / "00000002" / (id + ".header"), ec) ||
             fs::exists(title / "Headers" / "00000002" / id, ec);
  };
  probe(kUdlcPackage, s.installed, s.header, s.enabled);
  probe(kUdlcTrafficPackage, s.traffic_installed, s.traffic_header, s.traffic_enabled);
  const fs::path record = state_root / "dlc" / (std::string(kUdlcPackage) + ".json");
  if (fs::is_regular_file(record, ec)) {
    s.record = true;
    const std::string text = read_text(record);
    s.version = json_string(text, "version");
    const std::string name = json_string(text, "display_name");
    if (s.version.empty() && name.find("1.2") != std::string::npos) s.version = "1.2";
  }
  if (!s.installed) s.problem = "UDLC is not installed";
  else if (!s.header) s.problem = "UDLC has no content header; import the package again";
  else if (!s.enabled) s.problem = "UDLC is turned off";
  else if (s.record && !s.version.empty() && s.version != "1.2")
    s.problem = "UDLC " + s.version + " is not supported; install v1.2";
  return s;
}

bool package_hidden(const std::string& package_id) {
  const std::string id = lower_ascii(package_id);
  if (id == lower_ascii(kUdlcPackage) && !g_udlc.load()) return true;
  if (id == lower_ascii(kUdlcTrafficPackage) && !(g_udlc.load() && g_udlc_traffic.load())) return true;
  std::lock_guard<std::mutex> lock(g_hidden_mutex);
  return g_hidden.count(id) != 0;
}

void set_hidden_packages(std::vector<std::string> ids) {
  std::lock_guard<std::mutex> lock(g_hidden_mutex);
  g_hidden.clear();
  for (auto& id : ids) g_hidden.insert(lower_ascii(id));
}

void set_udlc_mount(bool udlc, bool traffic) {
  g_udlc = udlc;
  g_udlc_traffic = udlc && traffic;
}

bool udlc_mounted() { return g_udlc.load(); }

ActivationReport activate(const std::vector<ModInfo>& mods, const Resolution& r, const ActivateOptions& opts) {
  ActivationReport rep;
  std::vector<cars::mods::Mod> legacy;
  std::vector<fs::path> dirs;
  std::vector<std::pair<std::string, fs::path>> named;
  for (size_t i : r.active) {
    const ModInfo& m = mods[i];
    cars::mods::Mod l;
    l.name = m.name;
    l.title = m.manifest.title;
    l.dir = m.dir;
    l.enabled = true;
    l.priority = m.manifest.priority;
    l.files = m.content.total();
    legacy.push_back(std::move(l));
    dirs.push_back(m.dir);
    named.emplace_back(m.name, m.dir);
  }
  cars::mods::activate(legacy, opts.bake_dir, opts.sound_dir, opts.game_root);
  auto patches = db_patch::collect(named);
  rep.db_patches = patches.size();
  db_patch::set_active(std::move(patches));
  const auto counts = mod_merge::set_active(dirs);
  rep.members = counts.members;
  rep.xml_patches = counts.xml;
  rep.string_patches = counts.strings;
  texture_mods::registry().scan(dirs);
  rep.textures = texture_mods::registry().size();
  const fs::path cache = dirs.empty() ? fs::path{} : dirs.front().parent_path() / ".shader-cache";
  shader_mods::registry().scan(dirs, cache);
  rep.shaders = shader_mods::registry().size();
  for (const auto& [name, why] : shader_mods::registry().refused()) rep.notes.push_back("shader " + name + ": " + why);
  set_hidden_packages(r.hidden_dlc);
  bool udlc = opts.udlc, traffic = opts.udlc_traffic;
  if (std::find(r.hidden_dlc.begin(), r.hidden_dlc.end(), kUdlcPackage) != r.hidden_dlc.end()) {
    if (udlc) rep.notes.push_back("UDLC hidden by an active mod");
    udlc = traffic = false;
  }
  set_udlc_mount(udlc, traffic);
  return rep;
}

}  // namespace cars::modmgr
