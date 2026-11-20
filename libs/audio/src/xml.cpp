#include "audio/xml.h"

#include <cctype>
#include <cstdlib>

namespace audio {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

const XmlNode* XmlNode::child(std::string_view n) const {
  for (const auto& c : children)
    if (iequals(c.name, n)) return &c;
  return nullptr;
}

const XmlNode* XmlNode::find(std::string_view path) const {
  const XmlNode* at = this;
  while (at && !path.empty()) {
    const size_t slash = path.find('/');
    at = at->child(path.substr(0, slash));
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
  }
  return at;
}

bool XmlNode::has(std::string_view a) const {
  for (const auto& [k, v] : attributes)
    if (iequals(k, a)) return true;
  return false;
}

std::string XmlNode::attr(std::string_view a, std::string fallback) const {
  for (const auto& [k, v] : attributes)
    if (iequals(k, a)) return v;
  return fallback;
}

double XmlNode::number(std::string_view a, double fallback) const {
  for (const auto& [k, v] : attributes)
    if (iequals(k, a)) {
      char* end = nullptr;
      const double x = std::strtod(v.c_str(), &end);
      return end != v.c_str() ? x : fallback;
    }
  return fallback;
}

namespace {

struct Parser {
  std::string_view s;
  size_t at = 0;
  std::string error;

  bool fail(const char* what) {
    error = std::string(what) + " at byte " + std::to_string(at);
    return false;
  }
  void space() {
    while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
  }
  bool starts(std::string_view p) const { return s.substr(at, p.size()) == p; }
  // Skips text, comments, declarations and processing instructions up to
  // the next element tag ('<' followed by a name or '/').
  bool skip_misc() {
    for (;;) {
      while (at < s.size() && s[at] != '<') ++at;
      if (at >= s.size()) return true;
      if (starts("<!--")) {
        const size_t end = s.find("-->", at + 4);
        if (end == std::string_view::npos) return fail("unterminated comment");
        at = end + 3;
      } else if (starts("<?") || starts("<!")) {
        const size_t end = s.find('>', at);
        if (end == std::string_view::npos) return fail("unterminated declaration");
        at = end + 1;
      } else {
        return true;
      }
    }
  }
  static bool name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':';
  }
  std::string name() {
    const size_t start = at;
    while (at < s.size() && name_char(s[at])) ++at;
    return std::string(s.substr(start, at - start));
  }
  static std::string unescape(std::string_view v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] == '&') {
        const size_t semi = v.find(';', i);
        if (semi != std::string_view::npos) {
          const std::string_view e = v.substr(i + 1, semi - i - 1);
          char c = 0;
          if (e == "lt") c = '<';
          else if (e == "gt") c = '>';
          else if (e == "amp") c = '&';
          else if (e == "quot") c = '"';
          else if (e == "apos") c = '\'';
          if (c) {
            out += c;
            i = semi;
            continue;
          }
        }
      }
      out += v[i];
    }
    return out;
  }
  bool element(XmlNode& node, int depth) {
    if (depth > 64) return fail("nesting too deep");
    if (!starts("<")) return fail("expected '<'");
    ++at;
    node.name = name();
    if (node.name.empty()) return fail("expected an element name");
    for (;;) {
      space();
      if (at >= s.size()) return fail("unterminated tag");
      if (starts("/>")) {
        at += 2;
        return true;
      }
      if (s[at] == '>') {
        ++at;
        break;
      }
      std::string key = name();
      if (key.empty()) return fail("expected an attribute name");
      space();
      if (at >= s.size() || s[at] != '=') return fail("expected '='");
      ++at;
      space();
      if (at >= s.size() || (s[at] != '"' && s[at] != '\'')) return fail("expected a quoted value");
      const char q = s[at++];
      const size_t end = s.find(q, at);
      if (end == std::string_view::npos) return fail("unterminated value");
      node.attributes.emplace_back(std::move(key), unescape(s.substr(at, end - at)));
      at = end + 1;
    }
    for (;;) {
      if (!skip_misc()) return false;
      if (at >= s.size()) return fail("unterminated element");
      if (starts("</")) {
        at += 2;
        const std::string closing = name();
        if (!iequals(closing, node.name)) return fail("mismatched closing tag");
        space();
        if (at >= s.size() || s[at] != '>') return fail("expected '>'");
        ++at;
        return true;
      }
      node.children.emplace_back();
      if (!element(node.children.back(), depth + 1)) return false;
    }
  }
};

}  // namespace

bool parse_xml(std::string_view text, XmlNode& root, std::string* error) {
  Parser p{text};
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) p.at = 3;  // UTF-8 BOM
  root = {};
  if (!p.skip_misc() || p.at >= text.size() || !p.element(root, 0)) {
    if (error) *error = p.error.empty() ? "no root element" : p.error;
    return false;
  }
  return true;
}

}  // namespace audio
