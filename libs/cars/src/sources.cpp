#include "cars/sources.h"

#include "cars/mod_manager.h"
#include "cars/mods.h"
#include "fh1data/fs_walk.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>

namespace cars {
namespace fs = std::filesystem;

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

namespace {

std::string normal(const std::string& name) {
  std::string out = lower(name);
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}

// _pri_<n> in an archive or folder stem; 0 when absent.
uint32_t priority_of(const std::string& stem) {
  const auto at = lower(stem).rfind("_pri_");
  if (at == std::string::npos) return 0;
  try {
    return uint32_t(std::stoul(stem.substr(at + 5)));
  } catch (...) {
    return 0;
  }
}

bool is_content_archive(const std::string& file_name) {
  const std::string l = lower(file_name);
  return l.rfind("stringtables", 0) != 0 && l.rfind("cameras", 0) != 0;
}

// Registers media/cars/<X>/ and media/wheels/<X>/ folders and merge
// databases from member names (already normalised).
void scan_names(const std::vector<std::string>& names, const std::vector<std::string>& spelled,
                const FolderRef& base, Package& package) {
  for (size_t i = 0; i < names.size(); ++i) {
    const std::string& n = names[i];
    if (n.rfind("media/", 0) != 0) continue;
    for (const char* kind : {"cars/", "wheels/"}) {
      const std::string head = std::string("media/") + kind;
      if (n.rfind(head, 0) != 0) continue;
      const auto slash = n.find('/', head.size());
      if (slash == std::string::npos || slash + 1 >= n.size()) continue;  // a file, not a folder
      const std::string media = n.substr(head.size(), slash - head.size());
      auto& map = kind[0] == 'c' ? package.cars : package.wheels;
      if (map.count(media)) continue;
      FolderRef ref = base;
      ref.prefix = n.substr(0, slash + 1);
      ref.name = spelled[i].substr(head.size(), slash - head.size());
      if (ref.kind == FolderRef::Kind::kDirectory) {
        ref.path = base.path / spelled[i].substr(0, slash);
        ref.prefix.clear();
      }
      map.emplace(media, ref);
    }
    if (n.rfind("media/db/patch/", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".slt") == 0)
      package.merges.emplace_back(base.path, spelled[i]);
  }
}

}  // namespace

bool Folder::open(const FolderRef& ref, std::string* error) {
  ref_ = ref;
  archive_.reset();
  members_.clear();
  files_.clear();
  names_.clear();
  if (ref.kind == FolderRef::Kind::kDirectory) {
    std::error_code ec;
    if (!fs::is_directory(ref.path, ec)) {
      if (error) *error = ref.path.string() + ": " + (ec ? ec.message() : std::string("not a folder"));
      return false;
    }
    // Content packages' paths pass MAX_PATH: a walk that never throws.
    const fs::path base = fh1::long_path(ref.path);
    fh1::walk_tree(ref.path, [&](const fs::directory_entry& e) {
      std::error_code fec;
      if (e.is_regular_file(fec))
        files_.emplace(normal(e.path().lexically_relative(base).generic_string()), fh1::usable_path(e.path()));
      return true;
    });
    for (const auto& [n, _] : files_) names_.push_back(n);
    return true;
  }
  archive_ = fh1::Archive::open(mods::resolve(ref.path), error);
  if (!archive_) return false;
  for (const auto& m : archive_->members()) {
    std::string n = normal(m.name);
    if (!n.empty() && n.back() == '/') continue;  // directory entries
    if (ref.kind == FolderRef::Kind::kArchiveFolder) {
      if (n.rfind(ref.prefix, 0) != 0) continue;
      n = n.substr(ref.prefix.size());
    }
    members_.emplace(n, &m);  // the first member of a repeated name wins
  }
  for (const auto& [n, _] : members_) names_.push_back(n);
  return true;
}

bool Folder::contains(const std::string& name) const {
  const std::string key = normal(name);
  return archive_ ? members_.count(key) != 0 : files_.count(key) != 0;
}

bool Folder::read(const std::string& name, std::vector<uint8_t>& out, std::string* error) {
  const std::string key = normal(name);
  if (archive_) {
    const auto it = members_.find(key);
    if (it == members_.end()) {
      if (error) *error = name + ": not in " + ref_.path.string();
      return false;
    }
    auto r = archive_->read(*it->second);
    if (!r.ok()) {
      if (error) *error = name + ": " + r.error;
      return false;
    }
    out = std::move(r.data);
    return true;
  }
  const auto it = files_.find(key);
  if (it == files_.end()) {
    if (error) *error = name + ": not in " + ref_.path.string();
    return false;
  }
  std::ifstream f(it->second, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot open " + it->second.string();
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

namespace {

std::optional<FolderRef> find_in(const std::map<std::string, FolderRef>& base,
                                 const std::vector<Package>& packages, bool wheels,
                                 const std::string& media, std::vector<FolderRef>* others) {
  const std::string key = lower(media);
  std::optional<FolderRef> first;
  if (auto it = base.find(key); it != base.end()) first = it->second;
  for (const Package& p : packages) {
    const auto& map = wheels ? p.wheels : p.cars;
    auto it = map.find(key);
    if (it == map.end()) continue;
    if (!first) {
      first = it->second;
    } else if (others) {
      others->push_back(it->second);
    }
  }
  return first;
}

}  // namespace

std::optional<FolderRef> Install::find_car(const std::string& name, std::vector<FolderRef>* others) const {
  return find_in(base_cars, packages, false, name, others);
}

std::optional<FolderRef> Install::find_wheel(const std::string& name, std::vector<FolderRef>* others) const {
  return find_in(base_wheels, packages, true, name, others);
}

bool discover_install(const fs::path& game_root, const fs::path& state_root, const fs::path& dlc_extracted,
                      Install& out, std::string* error) {
  out = Install{};
  std::error_code ec;
  out.media = fs::is_directory(game_root / "media", ec) ? game_root / "media" : game_root;
  const bool has_cars_dir = fs::is_directory(out.media / "cars", ec);
  const bool has_db_file = fs::is_regular_file(out.media / "db" / "gamedb.slt", ec) ||
                           fs::is_regular_file(out.media / "DB" / "gamedb.slt", ec);
  if (!has_cars_dir && !has_db_file) {
    if (error) *error = "no media/cars folder under " + game_root.string();
    return false;
  }
  for (const char* kind : {"cars", "wheels"}) {
    if (!fs::is_directory(out.media / kind, ec)) continue;
    auto& map = kind[0] == 'c' ? out.base_cars : out.base_wheels;
    std::vector<fs::path> zips;
    for (const auto& e : fs::directory_iterator(out.media / kind, ec))
      if (e.is_regular_file() && lower(e.path().extension().string()) == ".zip") zips.push_back(e.path());
    std::sort(zips.begin(), zips.end());
    for (const auto& zip : zips) {
      FolderRef ref;
      ref.kind = FolderRef::Kind::kArchive;
      ref.path = zip;
      ref.origin = "base";
      ref.name = zip.stem().string();
      map.emplace(lower(ref.name), ref);
    }
  }
  if (!state_root.empty()) {
    const fs::path v4 = state_root / "title-update-v4" / "media.zip";
    if (fs::is_regular_file(v4, ec)) out.v4_media_zip = v4;
  }

  std::vector<Package> packages;
  if (!dlc_extracted.empty()) {
    // <package>/<offer zip stem>/media/...
    std::vector<fs::path> folders;
    for (const auto& e : fs::directory_iterator(dlc_extracted, ec))
      if (e.is_directory()) folders.push_back(e.path());
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
      Package p;
      p.id = folder.filename().string();
      if (modmgr::package_hidden(p.id)) continue;  // community UDLC unless mounted, mods' hide_dlc
      std::vector<fs::path> offers;
      for (const auto& e : fs::directory_iterator(folder, ec))
        if (e.is_directory() && is_content_archive(e.path().filename().string())) offers.push_back(e.path());
      std::sort(offers.begin(), offers.end(), [](const fs::path& a, const fs::path& b) {
        const uint32_t pa = priority_of(a.filename().string()), pb = priority_of(b.filename().string());
        return pa != pb ? pa < pb : a < b;
      });
      p.priority = UINT32_MAX;
      for (const auto& offer : offers) {
        p.priority = std::min(p.priority, priority_of(offer.filename().string()));
        p.archives.push_back(offer);
        std::vector<std::string> names, spelled;
        const fs::path offer_long = fh1::long_path(offer);
        fh1::walk_tree(offer, [&](const fs::directory_entry& e) {
          std::error_code dec;
          const std::string rel = e.path().lexically_relative(offer_long).generic_string();
          spelled.push_back(rel + (e.is_directory(dec) ? "/" : ""));
          names.push_back(normal(spelled.back()));
          return true;
        });
        // Stable order: sort the pairs by normalised name.
        std::vector<size_t> order(names.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return names[a] < names[b]; });
        std::vector<std::string> n2, s2;
        for (size_t i : order) {
          n2.push_back(names[i]);
          s2.push_back(spelled[i]);
        }
        FolderRef base;
        base.kind = FolderRef::Kind::kDirectory;
        base.path = offer;
        base.origin = p.id;
        scan_names(n2, s2, base, p);
      }
      if (p.priority == UINT32_MAX) p.priority = 0;
      // Merges were registered with the offer folder as "archive": make
      // them plain files.
      for (auto& [where, member] : p.merges) {
        where = where / member;
        member.clear();
      }
      packages.push_back(std::move(p));
    }
  } else if (!state_root.empty()) {
    const fs::path market = state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002";
    std::vector<fs::path> folders;
    if (fs::is_directory(market, ec))
      for (const auto& e : fs::directory_iterator(market, ec))
        if (e.is_directory()) folders.push_back(e.path());
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
      Package p;
      p.id = folder.filename().string();
      if (modmgr::package_hidden(p.id)) continue;  // community UDLC unless mounted, mods' hide_dlc
      std::vector<fs::path> archives;
      std::vector<fs::path> loose_dirs;
      const fs::path zips = folder / "Media" / "DLCZips";
      for (const auto& e : fs::directory_iterator(zips, ec)) {
        const std::string fname = e.path().filename().string();
        if (lower(fname).find("_pri_") == std::string::npos || !is_content_archive(fname)) continue;
        if (e.is_regular_file() && lower(e.path().extension().string()) == ".zip") {
          archives.push_back(e.path());
        } else if (e.is_directory()) {
          loose_dirs.push_back(e.path());
        }
      }
      // Also support loose <offer>_pri_<p>/ folders placed directly under Media/ or package root (e.g. UDLC 729_pri_99/).
      for (const fs::path& probe_dir : {folder / "Media", folder}) {
        std::error_code pec;
        for (const auto& e : fs::directory_iterator(probe_dir, pec)) {
          if (!e.is_directory()) continue;
          const std::string fname = e.path().filename().string();
          if (lower(fname).find("_pri_") != std::string::npos && is_content_archive(fname))
            loose_dirs.push_back(e.path());
        }
      }
      std::sort(archives.begin(), archives.end(), [](const fs::path& a, const fs::path& b) {
        const uint32_t pa = priority_of(a.stem().string()), pb = priority_of(b.stem().string());
        return pa != pb ? pa < pb : a < b;
      });
      std::sort(loose_dirs.begin(), loose_dirs.end(), [](const fs::path& a, const fs::path& b) {
        const uint32_t pa = priority_of(a.filename().string()), pb = priority_of(b.filename().string());
        return pa != pb ? pa < pb : a < b;
      });
      p.priority = (archives.empty() && loose_dirs.empty()) ? 0 : UINT32_MAX;
      for (const auto& zip : archives) {
        std::string why;
        auto archive = fh1::Archive::open(zip, &why);
        if (!archive) continue;  // not a ZIP: no content
        p.priority = std::min(p.priority, priority_of(zip.stem().string()));
        p.archives.push_back(zip);
        std::vector<std::pair<std::string, std::string>> named;
        for (const auto& m : archive->members()) named.emplace_back(normal(m.name), m.name);
        std::stable_sort(named.begin(), named.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> names, spelled;
        for (auto& [n, s] : named) {
          names.push_back(n);
          std::string sp = s;
          std::replace(sp.begin(), sp.end(), '\\', '/');
          spelled.push_back(sp);
        }
        FolderRef base;
        base.kind = FolderRef::Kind::kArchiveFolder;
        base.path = zip;
        base.origin = p.id;
        scan_names(names, spelled, base, p);
      }
      for (const auto& offer : loose_dirs) {
        p.priority = std::min(p.priority, priority_of(offer.filename().string()));
        p.archives.push_back(offer);
        const size_t merges_before = p.merges.size();
        std::vector<std::string> names, spelled;
        const fs::path offer_long = fh1::long_path(offer);
        fh1::walk_tree(offer, [&](const fs::directory_entry& e) {
          std::error_code dec;
          const std::string rel = e.path().lexically_relative(offer_long).generic_string();
          spelled.push_back(rel + (e.is_directory(dec) ? "/" : ""));
          names.push_back(normal(spelled.back()));
          return true;
        });
        std::vector<size_t> order(names.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return names[a] < names[b]; });
        std::vector<std::string> n2, s2;
        for (size_t i : order) {
          n2.push_back(names[i]);
          s2.push_back(spelled[i]);
        }
        FolderRef base;
        base.kind = FolderRef::Kind::kDirectory;
        base.path = offer;
        base.origin = p.id;
        scan_names(n2, s2, base, p);
        for (size_t mi = merges_before; mi < p.merges.size(); ++mi) {
          p.merges[mi].first = p.merges[mi].first / p.merges[mi].second;
          p.merges[mi].second.clear();
        }
      }
      if (p.priority == UINT32_MAX) p.priority = 0;
      packages.push_back(std::move(p));
    }
  }
  // The offer number and licence mask of each package (SPEC-V4-CONTENT,
  // Ownership: offers and licence masks).
  if (!state_root.empty()) {
    const fs::path market = state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002";
    for (auto& p : packages) {
      for (const auto& e : fs::directory_iterator(market / p.id / "Media", ec))
        if (e.is_regular_file() && lower(e.path().extension().string()) == ".puboffer") {
          const std::string stem = e.path().stem().string();
          char* end = nullptr;
          const long long n = std::strtoll(stem.c_str(), &end, 10);
          if (end && *end == 0 && !stem.empty()) p.puboffer = n;
        }
      std::ifstream f(state_root / "dlc" / (p.id + ".json"));
      const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      const auto key = text.find("\"license_mask\"");
      if (key != std::string::npos) {
        const auto q0 = text.find('"', text.find(':', key) + 1);
        const auto q1 = q0 == std::string::npos ? q0 : text.find('"', q0 + 1);
        if (q1 != std::string::npos) p.licence_mask = uint32_t(std::strtoul(text.substr(q0 + 1, q1 - q0 - 1).c_str(), nullptr, 16));
      }
    }
  }
  std::sort(packages.begin(), packages.end(), [](const Package& a, const Package& b) {
    return a.priority != b.priority ? a.priority < b.priority : a.id < b.id;
  });
  out.packages = std::move(packages);
  return true;
}

namespace {

std::string json_text(const std::string& text, const char* key) {
  const auto k = text.find(std::string("\"") + key + "\"");
  if (k == std::string::npos) return {};
  const auto q0 = text.find('"', text.find(':', k) + 1);
  const auto q1 = q0 == std::string::npos ? q0 : text.find('"', q0 + 1);
  return q1 == std::string::npos ? std::string() : text.substr(q0 + 1, q1 - q0 - 1);
}

}  // namespace

std::vector<PackInfo> list_packs(const fs::path& state_root) {
  std::vector<PackInfo> out;
  if (state_root.empty()) return out;
  std::error_code ec;
  const fs::path title = state_root / "user" / "0000000000000000" / "4D5309C9";
  std::map<std::string, PackInfo> by_id;
  for (const auto& e : fs::directory_iterator(state_root / "dlc", ec))
    if (e.is_regular_file() && lower(e.path().extension().string()) == ".json") {
      PackInfo p;
      p.id = e.path().stem().string();
      std::ifstream f(e.path());
      const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      p.name = json_text(text, "display_name");
      by_id[lower(p.id)] = p;
    }
  auto scan = [&](const fs::path& dir, int what) {
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      std::string id = what == 1 ? e.path().stem().string() : e.path().filename().string();
      if (what == 1 && lower(e.path().extension().string()) != ".header") continue;
      if (what != 1 && !e.is_directory()) continue;
      auto& p = by_id[lower(id)];
      if (p.id.empty()) p.id = id;
      if (what == 0) p.installed = p.enabled = true;
      else if (what == 1) p.header = true;
      else p.installed = true;  // disabled: present but not enabled
    }
  };
  scan(title / "00000002", 0);
  scan(title / "Headers" / "00000002", 1);
  scan(title / "Disabled" / "00000002", 2);
  for (auto& [id, p] : by_id) {
    if (id == lower(kCommunityPackage) && !modmgr::udlc_mounted()) continue;
    for (const char* where : {"00000002", "Disabled/00000002"})
      for (const auto& e : fs::directory_iterator(title / where / p.id / "Media", ec))
        if (e.is_regular_file() && lower(e.path().extension().string()) == ".puboffer") {
          const std::string stem = e.path().stem().string();
          char* end = nullptr;
          const long long n = std::strtoll(stem.c_str(), &end, 10);
          if (end && *end == 0 && !stem.empty()) p.puboffer = n;
        }
    if (p.name.empty()) p.name = p.id;
    out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const PackInfo& a, const PackInfo& b) { return a.name < b.name; });
  return out;
}

}  // namespace cars
