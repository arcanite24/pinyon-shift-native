#include "toml_lite.h"

#include <cctype>
#include <cstdlib>

namespace cars::toml {
namespace {

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

void skip_ws(std::string_view s, size_t& at) {
  while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) ++at;
}

// A basic ("...") or literal ('...') string at `at`.
bool read_string(std::string_view s, size_t& at, std::string& out) {
  if (at >= s.size()) return false;
  const char q = s[at];
  if (q != '"' && q != '\'') return false;
  ++at;
  out.clear();
  while (at < s.size()) {
    const char c = s[at++];
    if (c == q) return true;
    if (q == '"' && c == '\\' && at < s.size()) {
      const char e = s[at++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'u':
        case 'U': {
          const size_t n = e == 'u' ? 4 : 8;
          if (at + n > s.size()) return false;
          const uint32_t cp = uint32_t(std::strtoul(std::string(s.substr(at, n)).c_str(), nullptr, 16));
          at += n;
          append_utf8(out, cp);
          break;
        }
        default: out += e; break;
      }
    } else {
      out += c;
    }
  }
  return false;
}

bool read_key(std::string_view s, size_t& at, std::string& key) {
  skip_ws(s, at);
  if (at < s.size() && (s[at] == '"' || s[at] == '\'')) return read_string(s, at, key);
  const size_t start = at;
  while (at < s.size() && (std::isalnum(static_cast<unsigned char>(s[at])) || s[at] == '_' || s[at] == '-' ||
                           s[at] == '.'))
    ++at;
  key = std::string(s.substr(start, at - start));
  return !key.empty();
}

// The rest of a line after a value: only a comment or space.
bool line_rest_ok(std::string_view s, size_t at) {
  skip_ws(s, at);
  return at >= s.size() || s[at] == '#' || s[at] == '\r';
}

}  // namespace

Doc parse(std::string_view text) {
  Doc doc;
  std::string table;
  std::vector<std::string_view> lines;
  for (size_t at = 0; at <= text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    at = end + 1;
  }
  for (size_t li = 0; li < lines.size(); ++li) {
    std::string_view line = lines[li];
    size_t at = 0;
    skip_ws(line, at);
    if (at >= line.size() || line[at] == '#') continue;
    const int lineno = int(li + 1);
    if (line[at] == '[') {
      const size_t close = line.find(']', at);
      if (close == std::string_view::npos || !line_rest_ok(line, close + 1)) {
        doc.errors.push_back("line " + std::to_string(lineno) + ": bad table header");
        continue;
      }
      std::string t(line.substr(at + 1, close - at - 1));
      while (!t.empty() && t.back() == ' ') t.pop_back();
      while (!t.empty() && t.front() == ' ') t.erase(t.begin());
      table = t;
      continue;
    }
    Item item;
    item.table = table;
    item.line = lineno;
    if (!read_key(line, at, item.key)) {
      doc.errors.push_back("line " + std::to_string(lineno) + ": no key");
      continue;
    }
    skip_ws(line, at);
    if (at >= line.size() || line[at] != '=') {
      doc.errors.push_back("line " + std::to_string(lineno) + ": no '=' after " + item.key);
      continue;
    }
    ++at;
    skip_ws(line, at);
    if (at >= line.size()) {
      doc.errors.push_back("line " + std::to_string(lineno) + ": no value for " + item.key);
      continue;
    }
    Value& v = item.value;
    bool ok = true;
    if (line[at] == '"' || line[at] == '\'') {
      v.kind = Value::kString;
      ok = read_string(line, at, v.s) && line_rest_ok(line, at);
    } else if (line[at] == '[') {
      v.kind = Value::kArray;
      ++at;
      // Arrays may continue on later lines.
      for (;;) {
        skip_ws(line, at);
        if (at >= line.size() || line[at] == '#') {
          if (li + 1 >= lines.size()) {
            ok = false;
            break;
          }
          line = lines[++li];
          at = 0;
          continue;
        }
        if (line[at] == ']') {
          ++at;
          ok = line_rest_ok(line, at);
          break;
        }
        if (line[at] == ',') {
          ++at;
          continue;
        }
        std::string elem;
        if (line[at] == '"' || line[at] == '\'') {
          if (!read_string(line, at, elem)) {
            ok = false;
            break;
          }
        } else {
          const size_t start = at;
          while (at < line.size() && line[at] != ',' && line[at] != ']' && line[at] != ' ') ++at;
          elem = std::string(line.substr(start, at - start));
        }
        v.arr.push_back(elem);
      }
    } else if (line.compare(at, 4, "true") == 0) {
      v.kind = Value::kBool;
      v.b = true;
      ok = line_rest_ok(line, at + 4);
    } else if (line.compare(at, 5, "false") == 0) {
      v.kind = Value::kBool;
      v.b = false;
      ok = line_rest_ok(line, at + 5);
    } else {
      v.kind = Value::kInt;
      char* end = nullptr;
      const std::string rest(line.substr(at));
      v.i = std::strtoll(rest.c_str(), &end, 10);
      ok = end != rest.c_str() && line_rest_ok(rest, size_t(end - rest.c_str()));
      v.s = rest.substr(0, size_t(end - rest.c_str()));
    }
    if (!ok) {
      doc.errors.push_back("line " + std::to_string(lineno) + ": bad value for " + item.key);
      continue;
    }
    doc.items.push_back(std::move(item));
  }
  return doc;
}

std::string quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out + "\"";
}

std::string quote_list(const std::vector<std::string>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + quote(v[i]);
  return out + "]";
}

}  // namespace cars::toml
