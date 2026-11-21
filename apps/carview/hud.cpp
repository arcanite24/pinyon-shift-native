#include "hud.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace carview {
namespace {

// A 5 x 7 font: each glyph is seven rows of five columns, '#' set.
struct Glyph {
  char c;
  const char* rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {'k', {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."}},
    {'m', {".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"}},
    {'h', {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"}},
    {'r', {".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."}},
    {'p', {".....", ".....", "####.", "#...#", "####.", "#....", "#...."}},
    {'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'[', {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."}},
    {']', {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
    {'&', {".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#"}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
    {'\'', {"..#..", "..#..", ".#...", ".....", ".....", ".....", "....."}},
    {',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
};

const Glyph* glyph(char c) {
  for (const auto& g : kFont)
    if (g.c == c) return &g;
  return nullptr;
}

struct Canvas {
  HudImage& img;
  int scale;
  void rect(int x, int y, int w, int h, const uint8_t rgb[3]) {  // in font pixels
    const int x0 = std::max(0, x * scale), y0 = std::max(0, y * scale);
    const int x1 = std::min(int(img.width), (x + w) * scale), y1 = std::min(int(img.height), (y + h) * scale);
    for (int py = y0; py < y1; ++py)
      for (int px = x0; px < x1; ++px) {
        uint8_t* p = &img.rgba[(size_t(py) * img.width + size_t(px)) * 4];
        p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2], p[3] = 255;
      }
  }
  // Text at (x, y) with each font pixel `size` x `size`; 6 per character.
  void text(int x, int y, const char* s, int size, const uint8_t rgb[3]) {
    for (; *s; ++s, x += 6 * size) {
      const Glyph* g = glyph(*s);
      if (!g) continue;  // a space, or a character the font lacks
      for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
          if (g->rows[row][col] == '#') rect(x + col * size, y + row * size, size, size, rgb);
    }
  }
};

}  // namespace

int hud_scale(int height) { return std::max(1, height / 300); }

HudImage draw_radio_popup(const RadioPopupState& st, int height) {
  const int scale = hud_scale(height);
  auto upper = [](std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
  };
  const std::string title = upper(st.off ? std::string("radio off") : st.title), artist = upper(st.artist);
  // The logo in screen pixels: 44 at 720, its aspect kept.
  const int logo_h = st.logo && st.logo_height ? std::max(8, height * 44 / 720) : 0;
  const int logo_w = logo_h ? int(int64_t(logo_h) * st.logo_width / st.logo_height) : 0;
  // Font pixels: 4 of padding, lines of 11 (title) and 11 (artist label).
  const int text_w = 6 * int(std::max(title.size(), st.off ? size_t(0) : artist.size())) + 8;
  const int w_fp = std::max(text_w, (logo_w + scale - 1) / scale + 8);
  const int logo_fp = logo_h ? (logo_h + scale - 1) / scale + 4 : 0;
  const int h_fp = 4 + logo_fp + 11 + (st.off || artist.empty() ? 0 : 12) + 2;
  HudImage img;
  img.width = uint32_t(w_fp * scale);
  img.height = uint32_t(h_fp * scale);
  img.rgba.assign(size_t(img.width) * img.height * 4, 0);
  Canvas c{img, scale};
  static constexpr uint8_t kBack[3] = {18, 20, 24};
  const double a = std::clamp(st.alpha, 0.0, 1.0);
  auto fade = [&](const uint8_t rgb[3], uint8_t out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = uint8_t(std::lround(kBack[i] + (rgb[i] - kBack[i]) * a));
  };
  c.rect(0, 0, w_fp, h_fp, kBack);
  if (logo_h) {
    // Box-filtered into place, right-aligned, blended over the panel.
    const int x0 = int(img.width) - 4 * scale - logo_w, y0 = 4 * scale;
    for (int y = 0; y < logo_h; ++y)
      for (int x = 0; x < logo_w; ++x) {
        const uint32_t sx0 = uint32_t(int64_t(x) * st.logo_width / logo_w),
                       sx1 = std::max(sx0 + 1, uint32_t(int64_t(x + 1) * st.logo_width / logo_w));
        const uint32_t sy0 = uint32_t(int64_t(y) * st.logo_height / logo_h),
                       sy1 = std::max(sy0 + 1, uint32_t(int64_t(y + 1) * st.logo_height / logo_h));
        double acc[4] = {0, 0, 0, 0};
        int n = 0;
        for (uint32_t sy = sy0; sy < sy1 && sy < st.logo_height; ++sy)
          for (uint32_t sx = sx0; sx < sx1 && sx < st.logo_width; ++sx, ++n) {
            const uint8_t* p = st.logo + (size_t(sy) * st.logo_width + sx) * 4;
            const double al = p[3] / 255.0;
            for (int k = 0; k < 3; ++k) acc[k] += p[k] * al;
            acc[3] += al;
          }
        if (!n || acc[3] <= 0) continue;
        const double cover = acc[3] / n * a;
        const int px = x0 + x, py = y0 + y;
        if (px < 0 || py < 0 || px >= int(img.width) || py >= int(img.height)) continue;
        uint8_t* q = &img.rgba[(size_t(py) * img.width + size_t(px)) * 4];
        for (int k = 0; k < 3; ++k) q[k] = uint8_t(std::lround(q[k] + (acc[k] / acc[3] - q[k]) * cover));
      }
  }
  static constexpr uint8_t kWhite[3] = {240, 242, 245}, kPink[3] = {226, 38, 128};
  uint8_t col[3];
  const int y_title = 4 + logo_fp;
  fade(kWhite, col);
  c.text(w_fp - 4 - 6 * int(title.size()) + 1, y_title + 2, title.c_str(), 1, col);
  if (!st.off && !artist.empty()) {
    const int lw = 6 * int(artist.size()) + 3, y = y_title + 11;
    fade(kPink, col);
    c.rect(w_fp - 4 - lw + 1, y, lw, 11, col);
    fade(kWhite, col);
    c.text(w_fp - 4 - 6 * int(artist.size()) + 1, y + 2, artist.c_str(), 1, col);
  }
  return img;
}

const char* PauseMenu::label(int item) {
  switch (item) {
    case kResume: return "resume";
    case kMap: return "map";
    case kEvent: return "race";
    case kChangeCar: return "change car";
    case kUpgrades: return "upgrades";
    case kAutoshow: return "autoshow";
    case kCareer: return "career";
    case kReset: return "reset to road";
    case kQuit: return "quit";
  }
  return "";
}

void PauseMenu::fill(HudState& st) const {
  if (!open) return;
  st.menu_title = "paused";
  st.menu.clear();
  for (int k = 0; k < kCount; ++k)
    st.menu.push_back(k == kEvent && !event_label.empty() ? event_label : std::string(label(k)));
  st.menu_selected = selected;
}

HudImage draw_hud(const HudState& state, int scale) {
  scale = std::max(1, scale);
  // Font pixels: the panel, widened for the banner, which takes a line
  // above it.
  constexpr int kPanel = 140, kPanelHeight = 35, kBannerHeight = 11;
  std::string banner = state.banner;
  for (char& ch : banner) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  // The menu: its title and items at size 2 (18 font pixels a line), with
  // room for the marker.
  constexpr int kMenuLine = 18;
  std::vector<std::string> menu;
  if (!state.menu.empty()) {
    menu.push_back(state.menu_title);
    for (const auto& m : state.menu) menu.push_back(m);
    for (auto& m : menu)
      for (char& ch : m) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  }
  int menu_width = 0;
  for (const auto& m : menu) menu_width = std::max(menu_width, 12 * int(m.size()) + 26);
  const int menu_height = menu.empty() ? 0 : kMenuLine * int(menu.size()) + 6;
  const int top = menu_height + (banner.empty() ? 0 : kBannerHeight);
  const int kWidth = std::max({kPanel, 6 * int(banner.size()) + 7, menu_width}), kHeight = kPanelHeight + top;
  HudImage img;
  img.width = uint32_t(kWidth * scale);
  img.height = uint32_t(kHeight * scale);
  img.rgba.assign(size_t(img.width) * img.height * 4, 0);
  Canvas c{img, scale};
  static constexpr uint8_t kBack[3] = {18, 20, 24}, kText[3] = {235, 238, 242}, kDim[3] = {140, 146, 156},
                           kAmber[3] = {255, 176, 32}, kTrack[3] = {48, 52, 60}, kBar[3] = {90, 200, 120},
                           kRed[3] = {235, 64, 52};
  c.rect(0, 0, kWidth, kHeight, kBack);
  for (size_t k = 0; k < menu.size(); ++k) {
    const int y = 4 + kMenuLine * int(k);
    const bool title = k == 0, chosen = !title && int(k) - 1 == state.menu_selected;
    if (chosen) c.text(4, y, ">", 2, kAmber);
    c.text(title ? 4 : 18, y, menu[k].c_str(), 2, title ? kDim : chosen ? kAmber : kText);
  }
  if (!menu.empty()) c.rect(0, menu_height - 1, kWidth, 1, kTrack);
  if (!banner.empty()) {
    c.text(4, menu_height + 3, banner.c_str(), 1, kAmber);
    c.rect(0, top - 1, kWidth, 1, kTrack);
  }
  // The panel's rows, right-aligned under a wider banner.
  const int ox = kWidth - kPanel, oy = top;

  // The speed, three digits at three times the size.
  char buf[32];
  std::snprintf(buf, sizeof buf, "%3d", std::min(999, int(std::lround(std::fabs(state.kmh)))));
  c.text(ox + 4, oy + 4, buf, 3, kText);
  c.text(ox + 62, oy + 18, "km/h", 1, kDim);
  // The rpm, and the ABS flag while a wheel is released.
  std::snprintf(buf, sizeof buf, "%4d rpm", std::max(0, int(std::lround(state.rpm / 10) * 10)));
  c.text(ox + 62, oy + 4, buf, 1, kText);
  if (state.abs) c.text(ox + 92, oy + 18, "ABS", 1, kAmber);
  // The gear.
  const char gear[2] = {state.gear < 0 ? 'R' : state.gear == 0 ? 'N' : char('0' + std::min(state.gear, 9)), 0};
  c.text(ox + 118, oy + 4, gear, 3, kText);
  // The tachometer bar, red from 90 % of the redline.
  const int bar_x = ox + 4, bar_w = kPanel - 8, bar_y = oy + 29;
  c.rect(bar_x, bar_y, bar_w, 3, kTrack);
  const double frac = state.redline_rpm > 0 ? std::clamp(state.rpm / state.redline_rpm, 0.0, 1.0) : 0;
  const int fill = int(std::lround(frac * bar_w));
  const int red_from = int(std::lround(0.9 * bar_w));
  c.rect(bar_x, bar_y, std::min(fill, red_from), 3, kBar);
  if (fill > red_from) c.rect(bar_x + red_from, bar_y, fill - red_from, 3, kRed);
  return img;
}

}  // namespace carview
