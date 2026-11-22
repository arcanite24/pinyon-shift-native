#include "cars/mod_merge.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>

#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "toml_lite.h"

namespace cars::mod_merge {
namespace fs = std::filesystem;
namespace {

std::string lower_ascii(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string upper_ascii(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return f ? std::string(std::istreambuf_iterator<char>(f), {}) : std::string();
}

bool is_space(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

// ---- A small XML tree that keeps what it does not understand verbatim ----

struct Node {
  enum Type { kElement, kText, kRaw } type = kElement;
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;  // values as written (escaped)
  std::vector<Node> children;
  std::string text;  // kText / kRaw: verbatim
};

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  bool document(std::vector<Node>& out, std::string* error) {
    while (at_ < s_.size()) {
      if (s_[at_] == '<') {
        Node n;
        if (!markup(n, error)) return false;
        out.push_back(std::move(n));
      } else {
        Node t;
        t.type = Node::kText;
        const size_t lt = s_.find('<', at_);
        t.text = std::string(s_.substr(at_, lt == std::string_view::npos ? std::string_view::npos : lt - at_));
        at_ = lt == std::string_view::npos ? s_.size() : lt;
        out.push_back(std::move(t));
      }
    }
    return true;
  }

 private:
  // A comment, processing instruction, doctype or CDATA kept verbatim, or an element.
  bool markup(Node& n, std::string* error) {
    auto raw_until = [&](std::string_view end) {
      const size_t e = s_.find(end, at_);
      const size_t stop = e == std::string_view::npos ? s_.size() : e + end.size();
      n.type = Node::kRaw;
      n.text = std::string(s_.substr(at_, stop - at_));
      at_ = stop;
      return true;
    };
    if (s_.compare(at_, 4, "<!--") == 0) return raw_until("-->");
    if (s_.compare(at_, 9, "<![CDATA[") == 0) return raw_until("]]>");
    if (s_.compare(at_, 2, "<?") == 0) return raw_until("?>");
    if (s_.compare(at_, 2, "<!") == 0) return raw_until(">");
    return element(n, error);
  }

  bool element(Node& n, std::string* error) {
    ++at_;  // '<'
    n.type = Node::kElement;
    n.name = name();
    if (n.name.empty()) return fail(error, "element without a name");
    for (;;) {
      skip_ws();
      if (at_ >= s_.size()) return fail(error, "unterminated <" + n.name);
      if (s_.compare(at_, 2, "/>") == 0) {
        at_ += 2;
        return true;
      }
      if (s_[at_] == '>') {
        ++at_;
        break;
      }
      std::string key = name();
      if (key.empty()) return fail(error, "bad attribute in <" + n.name);
      skip_ws();
      if (at_ >= s_.size() || s_[at_] != '=') return fail(error, "attribute " + key + " without a value");
      ++at_;
      skip_ws();
      if (at_ >= s_.size() || (s_[at_] != '"' && s_[at_] != '\'')) return fail(error, "unquoted " + key);
      const char q = s_[at_++];
      const size_t end = s_.find(q, at_);
      if (end == std::string_view::npos) return fail(error, "unterminated " + key);
      n.attrs.emplace_back(key, std::string(s_.substr(at_, end - at_)));
      at_ = end + 1;
    }
    // Children until </name>.
    while (at_ < s_.size()) {
      if (s_.compare(at_, 2, "</") == 0) {
        at_ += 2;
        const std::string close = name();
        skip_ws();
        if (close != n.name || at_ >= s_.size() || s_[at_] != '>')
          return fail(error, "</" + close + "> closes <" + n.name + ">");
        ++at_;
        return true;
      }
      if (s_[at_] == '<') {
        Node c;
        if (!markup(c, error)) return false;
        n.children.push_back(std::move(c));
      } else {
        Node t;
        t.type = Node::kText;
        const size_t lt = s_.find('<', at_);
        if (lt == std::string_view::npos) return fail(error, "unterminated <" + n.name);
        t.text = std::string(s_.substr(at_, lt - at_));
        at_ = lt;
        n.children.push_back(std::move(t));
      }
    }
    return fail(error, "unterminated <" + n.name);
  }

  std::string name() {
    const size_t start = at_;
    while (at_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[at_])) || s_[at_] == '_' ||
                               s_[at_] == '-' || s_[at_] == ':' || s_[at_] == '.'))
      ++at_;
    return std::string(s_.substr(start, at_ - start));
  }
  void skip_ws() {
    while (at_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[at_]))) ++at_;
  }
  static bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
  }

  std::string_view s_;
  size_t at_ = 0;
};

void write(const Node& n, std::string& out) {
  if (n.type != Node::kElement) {
    out += n.text;
    return;
  }
  out += "<" + n.name;
  for (const auto& [k, v] : n.attrs) out += " " + k + "=\"" + v + "\"";
  if (n.children.empty()) {
    out += "/>";
    return;
  }
  out += ">";
  for (const auto& c : n.children) write(c, out);
  out += "</" + n.name + ">";
}

const std::string* attr(const Node& n, std::string_view key) {
  for (const auto& [k, v] : n.attrs)
    if (k == key) return &v;
  return nullptr;
}

bool add_marker(const Node& n) {
  for (const char* k : {"shift-add", "pinyon-add"})
    if (const auto* v = attr(n, k); v && (*v == "true" || *v == "1")) return true;
  return false;
}

Node without_markers(Node n) {
  n.attrs.erase(std::remove_if(n.attrs.begin(), n.attrs.end(),
                               [](const auto& a) { return a.first == "shift-add" || a.first == "pinyon-add"; }),
                n.attrs.end());
  for (auto& c : n.children)
    if (c.type == Node::kElement) c = without_markers(std::move(c));
  return n;
}

void merge(Node& base, const Node& patch) {
  for (const auto& [k, v] : patch.attrs) {
    if (k == "shift-add" || k == "pinyon-add") continue;
    bool set = false;
    for (auto& [bk, bv] : base.attrs)
      if (bk == k) bv = v, set = true;
    if (!set) base.attrs.emplace_back(k, v);
  }
  bool patch_has_elements = false, patch_has_text = false;
  for (const auto& c : patch.children) {
    if (c.type == Node::kElement) patch_has_elements = true;
    if (c.type == Node::kText && !is_space(c.text)) patch_has_text = true;
  }
  if (patch_has_text && !patch_has_elements) {
    base.children.erase(std::remove_if(base.children.begin(), base.children.end(),
                                       [](const Node& c) { return c.type == Node::kText; }),
                        base.children.end());
    for (const auto& c : patch.children)
      if (c.type == Node::kText) base.children.push_back(c);
    return;
  }
  std::map<std::string, int> seen;  // tag -> same-tag siblings of the patch so far
  for (const auto& pc : patch.children) {
    if (pc.type != Node::kElement) continue;
    const int nth = seen[pc.name]++;
    if (add_marker(pc)) {
      base.children.push_back(without_markers(pc));
      continue;
    }
    const char* key_name = nullptr;
    for (const char* k : {"name", "id", "key", "type"})
      if (attr(pc, k)) {
        key_name = k;
        break;
      }
    Node* match = nullptr;
    int k = 0;
    for (auto& bc : base.children) {
      if (bc.type != Node::kElement || bc.name != pc.name) continue;
      if (key_name) {
        const auto* bv = attr(bc, key_name);
        if (bv && *bv == *attr(pc, key_name)) {
          match = &bc;
          break;
        }
      } else if (k++ == nth) {
        match = &bc;
        break;
      }
    }
    if (match) merge(*match, pc);
    else base.children.push_back(without_markers(pc));
  }
}

// ---- INI ----

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

std::vector<std::string> lines_of(std::string_view s) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= s.size()) {
    size_t e = s.find('\n', at);
    if (e == std::string_view::npos) e = s.size();
    std::string l(s.substr(at, e - at));
    if (!l.empty() && l.back() == '\r') l.pop_back();
    out.push_back(l);
    at = e + 1;
  }
  if (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

// ---- The registry ----

struct Registry {
  // archive key (lower, '/') -> member (lower, '/') -> the winner's file
  std::map<std::string, std::map<std::string, fs::path>> members;
  // archive key -> member -> patch files, the winner last
  std::map<std::string, std::map<std::string, std::vector<fs::path>>> xml;
  // "ALL" or a language (upper) -> table (lower, no extension) -> files, the winner last
  std::map<std::string, std::map<std::string, std::vector<fs::path>>> strings;
  // archive file name -> the keys ending in it
  std::map<std::string, std::vector<std::string>> by_file;
};

std::mutex g_mutex;
std::shared_ptr<const Registry> g_registry;

std::shared_ptr<const Registry> registry() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry;
}

// Splits <root>/<a>/<b.zip>/<member...> into ("a/b.zip", "member...").
bool split_archive_path(const fs::path& rel, std::string& key, std::string& member) {
  std::vector<std::string> parts;
  for (const auto& p : rel) parts.push_back(lower_ascii(p.string()));
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    if (parts[i].size() > 4 && parts[i].ends_with(".zip")) {
      key.clear();
      member.clear();
      for (size_t j = 0; j <= i; ++j) key += (j ? "/" : "") + parts[j];
      for (size_t j = i + 1; j < parts.size(); ++j) member += (j > i + 1 ? "/" : "") + parts[j];
      return true;
    }
  }
  return false;
}

template <typename Fn>
void walk(const fs::path& dir, Fn&& fn) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec))
    if (it->is_regular_file(ec)) files.push_back(it->path());
  std::sort(files.begin(), files.end());
  for (const auto& f : files) fn(fs::relative(f, dir, ec), f);
}

// The keys of `r` that name `archive` (by its path's end).
std::vector<std::string> keys_for(const Registry& r, const fs::path& archive) {
  std::vector<std::string> out;
  const std::string file = lower_ascii(archive.filename().string());
  auto it = r.by_file.find(file);
  if (it == r.by_file.end()) return out;
  const std::string full = lower_ascii(archive.generic_string());
  for (const auto& key : it->second)
    if (full == key || (full.size() > key.size() && full.ends_with(key) && full[full.size() - key.size() - 1] == '/'))
      out.push_back(key);
  // The longest (most specific) key first.
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  return out;
}

bool apply_xml_patches(const Registry& r, const std::vector<std::string>& keys, const std::string& member,
                       std::vector<uint8_t>& data) {
  bool any = false;
  for (const auto& key : keys) {
    auto a = r.xml.find(key);
    if (a == r.xml.end()) continue;
    auto m = a->second.find(member);
    if (m == a->second.end()) continue;
    std::string text(data.begin(), data.end());
    for (const auto& patch : m->second) {
      const std::string p = read_text(patch);
      if (member.ends_with(".ini")) {
        merge_ini(text, p);
        any = true;
      } else {
        std::string merged = text, why;
        if (merge_xml(merged, p, &why)) text = std::move(merged), any = true;
      }
    }
    data.assign(text.begin(), text.end());
  }
  return any;
}

}  // namespace

bool merge_xml(std::string& base, std::string_view patch, std::string* error) {
  std::vector<Node> bdoc, pdoc;
  if (!Parser(base).document(bdoc, error)) return false;
  if (!Parser(patch).document(pdoc, error)) return false;
  Node* broot = nullptr;
  const Node* proot = nullptr;
  for (auto& n : bdoc)
    if (n.type == Node::kElement) {
      broot = &n;
      break;
    }
  for (const auto& n : pdoc)
    if (n.type == Node::kElement) {
      proot = &n;
      break;
    }
  if (!broot || !proot) {
    if (error) *error = "no root element";
    return false;
  }
  if (broot->name != proot->name) {
    if (error) *error = "the patch's root <" + proot->name + "> is not <" + broot->name + ">";
    return false;
  }
  merge(*broot, *proot);
  std::string out;
  for (const auto& n : bdoc) write(n, out);
  base = std::move(out);
  return true;
}

void merge_ini(std::string& base, std::string_view patch) {
  std::vector<std::string> lines = lines_of(base);
  const bool crlf = base.find("\r\n") != std::string::npos;
  auto header_of = [](const std::string& l, std::string& name) {
    const std::string t = trim(l);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    name = trim(t.substr(1, t.size() - 2));
    return true;
  };
  // The lines of section `sec` ("" before the first header): [b, e).
  auto find_section = [&](const std::string& sec, size_t& b, size_t& e) {
    bool in = sec.empty();
    b = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      std::string name;
      if (!header_of(lines[i], name)) continue;
      if (in) {
        e = i;
        return true;
      }
      if (lower_ascii(name) == lower_ascii(sec)) in = true, b = i + 1;
    }
    e = lines.size();
    return in;
  };
  std::string section;
  for (const std::string& pl : lines_of(patch)) {
    const std::string t = trim(pl);
    if (t.empty() || t[0] == ';' || t[0] == '#') continue;
    std::string name;
    if (header_of(t, name)) {
      section = name;
      continue;
    }
    const auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(t.substr(0, eq)), value = trim(t.substr(eq + 1));
    size_t begin = 0, end = 0;
    if (!find_section(section, begin, end)) {
      lines.push_back("[" + section + "]");
      lines.push_back(key + "=" + value);
      continue;
    }
    bool replaced = false;
    for (size_t i = begin; i < end; ++i) {
      const std::string bt = trim(lines[i]);
      const auto beq = bt.find('=');
      if (bt.empty() || bt[0] == ';' || bt[0] == '#' || beq == std::string::npos) continue;
      if (lower_ascii(trim(bt.substr(0, beq))) != lower_ascii(key)) continue;
      const auto leq = lines[i].find('=');
      const bool spaced = lines[i].size() > leq + 1 && lines[i][leq + 1] == ' ';
      lines[i] = lines[i].substr(0, leq + 1) + (spaced ? " " : "") + value;
      replaced = true;
      break;
    }
    if (!replaced) {
      size_t at = end;
      while (at > begin && trim(lines[at - 1]).empty()) --at;
      lines.insert(lines.begin() + long(at), key + "=" + value);
    }
  }
  std::string out;
  for (const auto& l : lines) out += l + (crlf ? "\r\n" : "\n");
  base = std::move(out);
}

bool parse_string_patch(std::string_view text, std::vector<std::pair<std::string, std::string>>& out,
                        std::string* error) {
  const toml::Doc doc = toml::parse(text);
  for (const auto& it : doc.items)
    if (it.value.kind == toml::Value::kString) out.emplace_back(it.key, it.value.s);
  if (!doc.errors.empty()) {
    if (error) *error = doc.errors.front();
    return false;
  }
  return true;
}

size_t apply_string_patch(fh1::StringTable& table, const std::vector<std::pair<std::string, std::string>>& entries) {
  size_t n = 0;
  for (const auto& [ids, utf8] : entries) {
    std::u16string text;
    for (size_t i = 0; i < utf8.size();) {
      const unsigned char c = static_cast<unsigned char>(utf8[i]);
      uint32_t cp = c;
      size_t len = 1;
      if (c >= 0xF0 && i + 3 < utf8.size() + 0) {
        cp = (c & 0x07u) << 18 | (utf8[i + 1] & 0x3Fu) << 12 | (utf8[i + 2] & 0x3Fu) << 6 | (utf8[i + 3] & 0x3Fu);
        len = 4;
      } else if (c >= 0xE0 && i + 2 < utf8.size()) {
        cp = (c & 0x0Fu) << 12 | (utf8[i + 1] & 0x3Fu) << 6 | (utf8[i + 2] & 0x3Fu);
        len = 3;
      } else if (c >= 0xC0 && i + 1 < utf8.size()) {
        cp = (c & 0x1Fu) << 6 | (utf8[i + 1] & 0x3Fu);
        len = 2;
      }
      i += len;
      if (cp >= 0x10000) {
        cp -= 0x10000;
        text += char16_t(0xD800 + (cp >> 10));
        text += char16_t(0xDC00 + (cp & 0x3FF));
      } else {
        text += char16_t(cp);
      }
    }
    const uint16_t key = fh1::string_hash(ids);
    auto it = std::lower_bound(table.keys.begin(), table.keys.end(), key);
    const size_t at = size_t(it - table.keys.begin());
    if (it != table.keys.end() && *it == key) {
      table.text[at] = std::move(text);
    } else {
      table.keys.insert(it, key);
      table.text.insert(table.text.begin() + long(at), std::move(text));
    }
    ++n;
  }
  return n;
}

Counts set_active(const std::vector<fs::path>& mod_dirs_winner_first) {
  auto r = std::make_shared<Registry>();
  Counts c;
  // Replacements: the winner's file; patches: every mod, the winner last.
  for (const auto& dir : mod_dirs_winner_first) {
    for (const char* sub : {"members", "add"})
      walk(dir / sub, [&](const fs::path& rel, const fs::path& file) {
        std::string key, member;
        if (!split_archive_path(rel, key, member)) return;
        if (r->members[key].emplace(member, file).second) ++c.members;
      });
  }
  for (auto it = mod_dirs_winner_first.rbegin(); it != mod_dirs_winner_first.rend(); ++it) {
    for (const char* sub : {"xml_patches", "merge"})
      walk(*it / sub, [&](const fs::path& rel, const fs::path& file) {
        std::string key, member;
        if (!split_archive_path(rel, key, member)) return;
        r->xml[key][member].push_back(file);
        ++c.xml;
      });
    for (const char* sub : {"stringtables", "strings"})
      walk(*it / sub, [&](const fs::path& rel, const fs::path& file) {
        auto parts = std::vector<fs::path>(rel.begin(), rel.end());
        if (parts.size() != 2 || lower_ascii(file.extension().string()) != ".toml") return;
        r->strings[upper_ascii(parts[0].string())][lower_ascii(file.stem().string())].push_back(file);
        ++c.strings;
      });
  }
  for (const auto& [key, m] : r->members) r->by_file[key.substr(key.rfind('/') + 1)].push_back(key);
  for (const auto& [key, m] : r->xml) {
    auto& v = r->by_file[key.substr(key.rfind('/') + 1)];
    if (std::find(v.begin(), v.end(), key) == v.end()) v.push_back(key);
  }
  const bool any_archive = !r->members.empty() || !r->xml.empty();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_registry = r;
  }
  if (!any_archive) {
    fh1::set_member_override(nullptr);
    return c;
  }
  auto hooks = std::make_shared<fh1::MemberOverride>();
  hooks->replace = [](const fs::path& archive, std::string_view member, std::vector<uint8_t>& out) {
    const auto reg = registry();
    if (!reg) return false;
    const auto keys = keys_for(*reg, archive);
    if (keys.empty()) return false;
    const std::string m = fh1::normalize_member_name(member);
    for (const auto& key : keys) {
      auto a = reg->members.find(key);
      if (a == reg->members.end()) continue;
      auto f = a->second.find(m);
      if (f == a->second.end()) continue;
      std::ifstream in(f->second, std::ios::binary);
      if (!in) return false;
      out.assign(std::istreambuf_iterator<char>(in), {});
      apply_xml_patches(*reg, keys, m, out);
      return true;
    }
    return false;
  };
  hooks->patch = [](const fs::path& archive, std::string_view member, std::vector<uint8_t>& data) {
    const auto reg = registry();
    if (!reg || reg->xml.empty()) return;
    const auto keys = keys_for(*reg, archive);
    if (!keys.empty()) apply_xml_patches(*reg, keys, fh1::normalize_member_name(member), data);
  };
  fh1::set_member_override(hooks);
  return c;
}

bool resolve_member(const fs::path& archive, std::string_view member, std::vector<uint8_t>& data, bool have_base) {
  const auto hooks = fh1::member_override();
  if (!hooks) return false;
  if (hooks->replace && hooks->replace(archive, member, data)) return true;
  if (!have_base) return false;
  const auto before = data;
  if (hooks->patch) hooks->patch(archive, member, data);
  return data != before;
}

size_t apply_string_patches(fh1::StringTables& tables, const std::string& language) {
  const auto reg = registry();
  if (!reg) return 0;
  size_t n = 0;
  for (const std::string lang : {std::string("ALL"), upper_ascii(language)}) {
    auto it = reg->strings.find(lang);
    if (it == reg->strings.end()) continue;
    for (const auto& [table, files] : it->second)
      for (const auto& f : files) {
        std::vector<std::pair<std::string, std::string>> entries;
        std::string why;
        parse_string_patch(read_text(f), entries, &why);
        n += apply_string_patch(tables.table(table), entries);
      }
  }
  return n;
}

}  // namespace cars::mod_merge
