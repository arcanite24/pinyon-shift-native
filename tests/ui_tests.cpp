// The UI layer's CPU rasterizer: text measures and draws inside its box,
// and SHIFT_UI_SPECIMEN=<file.png> writes a specimen of the font and the
// shapes to look at.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "ui.h"

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

using namespace carview::ui;

int main() {
  // Every letter and digit has strokes.
  CHECK(!font_segments().empty());
  TextStyle s;
  s.size = 20;
  const float w = List::measure("ABC", s);
  CHECK(w > 30 && w < 60);
  CHECK(List::measure("", s) == 0);

  // Text lands where it is drawn: some coverage inside its box, none far
  // outside.
  List list;
  list.text(10, 10, "HELLO 123", s);
  std::vector<uint8_t> px;
  rasterize(list.items(), px, 200, 40);
  int inside = 0, outside = 0;
  for (uint32_t y = 0; y < 40; ++y)
    for (uint32_t x = 0; x < 200; ++x) {
      const bool box = x >= 7 && x <= 13 + uint32_t(List::measure("HELLO 123", s)) && y >= 6 && y <= 33;
      if (px[(y * 200 + x) * 4 + 3] > 0) ++(box ? inside : outside);
    }
  CHECK(inside > 200);
  CHECK(outside == 0);

  if (const char* out = std::getenv("SHIFT_UI_SPECIMEN")) {
    const uint32_t W = 1280, H = 720;
    std::vector<uint8_t> img(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        uint8_t* p = &img[(size_t(y) * W + x) * 4];
        p[0] = uint8_t(150 + 60 * x / W), p[1] = uint8_t(120 + 40 * y / H), p[2] = 90, p[3] = 255;
      }
    List l;
    l.shadow(40, 40, 700, 330, rgba(0, 0, 0, 140), 14, 18);
    l.rect(40, 40, 700, 330, rgba(18, 20, 26, 215), 14, rgba(10, 11, 15, 235));
    TextStyle t{.size = 34, .weight = 0.1f, .color = rgba(255, 255, 255)};
    l.text(70, 70, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", t);
    t.size = 22;
    l.text(70, 125, "0123456789 .,:-+/%()[]!?<>'_=*#&", t);
    t.size = 16, t.weight = 0.09f, t.color = rgba(200, 205, 215);
    l.text(70, 170, "2009 FERRARI CALIFORNIA   1500 CR   LEVEL 12", t);
    t.size = 64, t.weight = 0.12f, t.slant = 0.18f, t.color = rgba(255, 255, 255);
    l.text(70, 220, "247", t);
    t.size = 40, t.color = rgba(255, 196, 0);
    l.text(300, 240, "3RD", t);
    t = TextStyle{.size = 12, .weight = 0.1f, .tracking = 0.25f, .color = rgba(255, 196, 0)};
    l.text(470, 230, "POSITION", t);
    // A gauge.
    const float cx = 1000, cy = 460, R = 150, a0 = -2.35f, a1 = 2.35f;
    l.circle(cx, cy, R + 24, rgba(10, 12, 16, 200));
    l.arc(cx, cy, R, 10, a0, a1, rgba(255, 255, 255, 50));
    l.arc(cx, cy, R, 10, a0, a0 + 0.7f * (a1 - a0), rgba(255, 255, 255));
    l.arc(cx, cy, R, 10, a0 + 0.85f * (a1 - a0), a1, rgba(230, 40, 40));
    for (int i = 0; i <= 8; ++i) {
      const float a = a0 + (a1 - a0) * float(i) / 8;
      l.line(cx + (R - 22) * std::sin(a), cy - (R - 22) * std::cos(a), cx + (R - 12) * std::sin(a),
             cy - (R - 12) * std::cos(a), 3, rgba(255, 255, 255, 180));
    }
    TextStyle big{.size = 70, .weight = 0.12f, .slant = 0.12f, .color = rgba(255, 255, 255), .align = Align::kCenter};
    l.text(cx, cy - 40, "184", big);
    TextStyle small{.size = 14, .weight = 0.1f, .tracking = 0.3f, .color = rgba(180, 185, 195), .align = Align::kCenter};
    l.text(cx, cy + 45, "KM/H", small);
    l.circle(cx, cy + 105, 26, rgba(255, 196, 0));
    TextStyle gear{.size = 30, .weight = 0.13f, .color = rgba(15, 15, 15), .align = Align::kCenter};
    l.text(cx, cy + 90, "4", gear);
    // A menu.
    l.rect(40, 380, 520, 690, rgba(12, 14, 18, 220), 12);
    TextStyle item{.size = 22, .weight = 0.1f, .color = rgba(230, 232, 238)};
    const char* items[] = {"RESUME", "MAP", "RACE FR22", "CHANGE CAR", "UPGRADES", "SETTINGS"};
    for (int i = 0; i < 6; ++i) {
      const float y = 400 + 46.0f * i;
      if (i == 2) l.rect(52, y - 8, 508, y + 30, rgba(255, 196, 0), 6);
      item.color = i == 2 ? rgba(15, 15, 15) : rgba(230, 232, 238);
      l.text(72, y, items[i], item);
    }
    rasterize(l.items(), img, W, H);
    stbi_write_png(out, int(W), int(H), 4, img.data(), int(W * 4));
    std::printf("ui_tests: specimen %s\n", out);
  }
  std::printf("ui_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
