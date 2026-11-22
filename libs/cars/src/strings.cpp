#include "cars/strings.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "fh1data/archive.h"

namespace cars {
namespace fs = std::filesystem;
namespace {

uint32_t priority_of(const std::string& stem) {
  const auto at = lower(stem).rfind("_pri_");
  if (at == std::string::npos) return 0;
  try {
    return uint32_t(std::stoul(stem.substr(at + 5)));
  } catch (...) {
    return 0;
  }
}

// Puts every member under `prefix` (lower case, ending in '/') that is a
// `.str` file directly in it; returns the count put.
size_t put_members(fh1::Archive& archive, const std::string& prefix, fh1::StringTables& out, size_t& rejected) {
  size_t n = 0;
  for (const auto& m : archive.members()) {
    std::string name = lower(m.name);
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.size() <= prefix.size() + 4 || name.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string rest = name.substr(prefix.size());
    if (rest.find('/') != std::string::npos || rest.compare(rest.size() - 4, 4, ".str") != 0) continue;
    auto r = archive.read(m);
    fh1::StringTable t;
    if (!r.ok() || !fh1::parse_string_table(r.data, t, nullptr)) {
      ++rejected;
      continue;
    }
    out.put(rest, std::move(t));
    ++n;
  }
  return n;
}

}  // namespace

bool load_string_tables(const Install& install, fh1::StringTables& out, std::string* error,
                        const std::string& language, StringTableLoad* counts) {
  out = fh1::StringTables{};
  StringTableLoad c;
  const std::string lang = lower(language);
  auto load_lang_layer = [&](const std::string& l, bool count_stats) -> bool {
    const fs::path st_dir = install.media / "stringtables";
    const std::string want = l + ".zip";
    fs::path archive_path = st_dir / (l + ".zip");
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(st_dir, ec)) {
      if (e.is_regular_file() && lower(e.path().filename().string()) == want) {
        archive_path = e.path();
        break;
      }
    }
    size_t rej = 0;
    auto disc = fh1::Archive::open(archive_path, count_stats ? error : nullptr);
    if (!disc) return false;
    const size_t n_disc = put_members(*disc, "", out, rej);
    size_t n_v4 = 0, n_pkg = 0;
    if (!install.v4_media_zip.empty()) {
      std::string why;
      if (auto v4 = fh1::Archive::open(install.v4_media_zip, &why))
        n_v4 = put_members(*v4, "stringtables/" + l + "/", out, rej);
    }
    std::vector<std::pair<uint32_t, fs::path>> zips;
    std::set<fs::path> folders;
    for (const auto& p : install.packages)
      for (const auto& a : p.archives) folders.insert(a.parent_path());
    for (const auto& folder : folders)
      for (const auto& e : fs::directory_iterator(folder, ec)) {
        const std::string fname = lower(e.path().filename().string());
        if (e.is_regular_file() && fname.rfind("stringtables_pri_", 0) == 0 && lower(e.path().extension().string()) == ".zip")
          zips.emplace_back(priority_of(e.path().stem().string()), e.path());
      }
    std::sort(zips.begin(), zips.end());
    for (const auto& [q, zip] : zips) {
      std::string why;
      if (auto a = fh1::Archive::open(zip, &why)) n_pkg += put_members(*a, l + "/", out, rej);
    }
    if (count_stats) {
      c.disc = n_disc;
      c.v4 = n_v4;
      c.packages = n_pkg;
      c.rejected = rej;
    }
    return true;
  };
  // Load EN first as a per-table fallback when requesting a non-English language.
  if (lang != "en") load_lang_layer("en", false);
  if (!load_lang_layer(lang, true)) return false;
  if (counts) *counts = c;
  return true;
}

}  // namespace cars
