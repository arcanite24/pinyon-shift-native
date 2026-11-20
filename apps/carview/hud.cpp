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

HudImage draw_hud(const HudState& state, int scale) {
  scale = std::max(1, scale);
  // Font pixels: the panel, widened for the banner, which takes a line
  // above it.
  constexpr int kPanel = 140, kPanelHeight = 35, kBannerHeight = 11;
  std::string banner = state.banner;
  for (char& ch : banner) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  const int top = banner.empty() ? 0 : kBannerHeight;
  const int kWidth = std::max(kPanel, 6 * int(banner.size()) + 7), kHeight = kPanelHeight + top;
  HudImage img;
  img.width = uint32_t(kWidth * scale);
  img.height = uint32_t(kHeight * scale);
  img.rgba.assign(size_t(img.width) * img.height * 4, 0);
  Canvas c{img, scale};
  static constexpr uint8_t kBack[3] = {18, 20, 24}, kText[3] = {235, 238, 242}, kDim[3] = {140, 146, 156},
                           kAmber[3] = {255, 176, 32}, kTrack[3] = {48, 52, 60}, kBar[3] = {90, 200, 120},
                           kRed[3] = {235, 64, 52};
  c.rect(0, 0, kWidth, kHeight, kBack);
  if (!banner.empty()) {
    c.text(4, 3, banner.c_str(), 1, kAmber);
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
