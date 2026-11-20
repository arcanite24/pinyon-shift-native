// A small XML reader for the sound tuning files: elements and attributes
// (text content is ignored), with the declaration and comments skipped.
// Names compare without case, as the game matches them.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace audio {

struct XmlNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<XmlNode> children;

  // The first child with the name (any case), or null.
  const XmlNode* child(std::string_view name) const;
  // A path of child names separated by '/', e.g. "DSP/Volume/Gain".
  const XmlNode* find(std::string_view path) const;
  bool has(std::string_view attribute) const;
  std::string attr(std::string_view attribute, std::string fallback = {}) const;
  double number(std::string_view attribute, double fallback = 0) const;
};

// Parses a document; `root` receives its single top-level element.
bool parse_xml(std::string_view text, XmlNode& root, std::string* error);

bool iequals(std::string_view a, std::string_view b);

}  // namespace audio
