#include "scene_screen.h"

#include <algorithm>

#include "fh1data/stringtables.h"
#include "game_hud.h"

namespace carview {

uint32_t string_key(const StringRef& ref) {
  const uint16_t id = ref.id.empty() ? ref.raw : fh1::string_hash(ref.id);
  return uint32_t(fh1::string_hash(ref.table)) << 16 | id;
}

std::optional<std::string> lookup(const StringRef& ref) {
  const std::string key = "_&" + std::to_string(string_key(ref));
  std::string t = game_text_raw(key);
  if (t.empty() || t == key) return std::nullopt;
  // Strip the markup as game_text() does, but keep the "{0}".."{9}" format
  // slots for format1().
  auto swap_all = [&t](const std::string& from, const std::string& to) {
    for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p + to.size())) t.replace(p, from.size(), to);
  };
  for (char d = '0'; d <= '9'; ++d) swap_all({'{', d, '}'}, {'\x01', d, '\x02'});
  t = strip_ui_inline_tags(std::move(t));
  for (char d = '0'; d <= '9'; ++d) swap_all({'\x01', d, '\x02'}, {'{', d, '}'});
  return t;
}

std::string text_of(const StringRef& ref) {
  if (ref.engine_label) return std::string(ref.fallback);
  if (auto t = lookup(ref)) return *t;
  return std::string(ref.fallback);
}

std::string format1(std::string_view fmt, std::string_view arg0) {
  std::string out(fmt);
  if (auto p = out.find("{0}"); p != std::string::npos) out.replace(p, 3, arg0);
  return out;
}

std::string_view ui_play_name(UiEvent e) {
  switch (e) {
    case UiEvent::kOpen: return "Pause";
    case UiEvent::kClose: return "UnPause";
    case UiEvent::kMove: return "Vscroll";
    case UiEvent::kAccept: return "Accept";
    case UiEvent::kBack: return "Cancel";
    case UiEvent::kDeny: return "DenyAccess";
  }
  return {};
}

int wrap_row(int selected, int by, int count) {
  if (count <= 0) return 0;
  return ((selected + by) % count + count) % count;
}

int window_first(int selected, int count, int window) {
  if (window <= 0 || count <= window) return 0;
  return std::clamp(selected - window / 2, 0, count - window);
}

SceneScreen::SceneScreen(std::string_view scene_name, bool turn)
    : name_(scene_name), draw_(resolve_scene_draw(name_, turn)) {}

int SceneScreen::node(std::string_view path, int under) const {
  if (!draw_.ok()) return -1;
  if (path.find('/') != std::string_view::npos) return draw_.find_path(path, under);
  return draw_.find(path, under);
}

bool SceneScreen::text(int n, std::string s) {
  if (n < 0) return false;
  at(n).text = std::move(s);
  return true;
}

bool SceneScreen::hide(int n) {
  if (n < 0) return false;
  at(n).visible = false;
  return true;
}

bool SceneScreen::show(int n, float alpha) {
  if (n < 0) return false;
  at(n).visible = true;
  at(n).alpha = alpha;
  return true;
}

std::string SceneScreen::wrap(int n, std::string_view s, float width) const {
  if (n < 0) return std::string(s);
  const float box = width > 0.0f ? width : draw_.text_width(n);
  if (box <= 0.0f) return std::string(s);
  std::string out, line;
  size_t i = 0;
  while (i <= s.size()) {
    const size_t end = std::min(s.find_first_of(" \n", i), s.size());
    const std::string_view word = s.substr(i, end - i);
    const std::string next = line.empty() ? std::string(word) : line + " " + std::string(word);
    if (!line.empty() && draw_.measure(n, next) > box) {
      out += line + "\n";
      line = std::string(word);
    } else {
      line = next;
    }
    if (end < s.size() && s[end] == '\n') out += line + "\n", line.clear();
    i = end + 1;
  }
  return out + line;
}

SceneFrame SceneScreen::frame(float w, float h) {
  const float s = std::min(w / 1280.0f, h / 720.0f);
  return {0.5f * w - 640.0f * s, 0.5f * h - 360.0f * s, s};
}

int SceneScreen::draw(ui::List& l, const SceneFrame& f, int root) const {
  return draw_.ok() ? draw_.draw_subtree(l, f, root, b_) : 0;
}

}  // namespace carview
