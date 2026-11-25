// Directory walks that never throw, and paths past MAX_PATH on Windows.
//
// A range-for over std::filesystem::(recursive_)directory_iterator throws
// filesystem_error from operator++ even when the iterator was built with an
// error_code, and on Windows a directory deeper than 260 characters cannot
// be opened without the \\?\ prefix. The content packages' folders
// (<state>/user/.../<package id>/Media/DLCZips/<zip>/Media/...) and the DLC
// catalogue under the recomp's .local/dlc pass that length, so a walk there
// must use these.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace fh1 {

// The path as the OS can open it past MAX_PATH: on Windows, absolute,
// normalised, backslashed and with the \\?\ (or \\?\UNC\) prefix; elsewhere
// unchanged. A path that cannot be made absolute comes back as it was.
inline std::filesystem::path long_path(const std::filesystem::path& p) {
#ifdef _WIN32
  if (p.empty()) return p;
  const std::wstring& raw = p.native();
  if (raw.rfind(L"\\\\?\\", 0) == 0) return p;
  std::error_code ec;
  std::filesystem::path a = p.is_absolute() ? p : std::filesystem::absolute(p, ec);
  if (ec) return p;
  a = a.lexically_normal();
  a.make_preferred();
  std::wstring n = a.native();
  // The prefix takes no trailing separator on a directory (lexically_normal
  // leaves one after "dir/").
  while (n.size() > 3 && n.back() == L'\\') n.pop_back();
  if (n.rfind(L"\\\\", 0) == 0) return std::filesystem::path(L"\\\\?\\UNC\\" + n.substr(2));
  return std::filesystem::path(L"\\\\?\\" + n);
#else
  return p;
#endif
}

// The path without long_path's prefix (for logs and for paths kept in
// settings or compared with others).
inline std::filesystem::path short_path(const std::filesystem::path& p) {
#ifdef _WIN32
  const std::wstring& n = p.native();
  if (n.rfind(L"\\\\?\\UNC\\", 0) == 0) return std::filesystem::path(L"\\\\" + n.substr(8));
  if (n.rfind(L"\\\\?\\", 0) == 0) return std::filesystem::path(n.substr(4));
#endif
  return p;
}

// The short form when it fits in MAX_PATH, else the long one: what a walk's
// entry is best kept as (shown, compared, opened).
inline std::filesystem::path usable_path(const std::filesystem::path& p) {
  std::filesystem::path s = short_path(p);
#ifdef _WIN32
  if (s.native().size() >= 260) return long_path(p);
#endif
  return s;
}

// Calls fn(const directory_entry&) for every entry under `root`, depth
// first, entries of a directory before its subdirectories' entries are
// done; fn returns false to stop the walk. A directory that cannot be read
// is skipped with its subtree; symlinked directories are not followed; at
// most `max_depth` levels. Never throws. The entries' paths carry
// long_path's prefix on Windows (short_path strips it). False when fn
// stopped the walk.
template <class Fn>
bool walk_tree(const std::filesystem::path& root, Fn&& fn, int max_depth = 64) {
  struct Walker {
    Fn& fn;
    int max_depth;
    bool dir(const std::filesystem::path& d, int depth) {
      std::error_code ec;
      std::filesystem::directory_iterator it(d, std::filesystem::directory_options::skip_permission_denied, ec);
      const std::filesystem::directory_iterator end;
      for (; !ec && it != end; it.increment(ec)) {
        const std::filesystem::directory_entry& e = *it;
        if (!fn(e)) return false;
        std::error_code tec;
        const bool sub = e.is_directory(tec) && !tec && !e.is_symlink(tec) && !tec;
        if (sub && depth + 1 < max_depth && !dir(e.path(), depth + 1)) return false;
      }
      return true;
    }
  };
  Walker w{fn, max_depth};
  return w.dir(long_path(root), 0);
}

}  // namespace fh1
