#include "car_list_screen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "fh1data/ui_scene.h"
#include "game_hud.h"
#include "pause_screen.h"
#include "race_screens.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);
constexpr uint32_t kInk = ui::rgba(20, 20, 22);
// Unselected tiles read dimmer than the cursor's (19_autoshow_orig.png).
constexpr uint32_t kTileDim = ui::rgba(206, 206, 206);
// Measured on 19_autoshow_orig.png (canvas px): the bars' length, the info
// bar's, where the drivetrain icon sits, the player bar's class after the
// car's name and its pill past the class.
constexpr float kBarLength = 144.0f;
constexpr float kPhoto[4] = {832.0f, 100.0f, 1197.0f, 277.0f};  // the panel's photo, unturned
constexpr float kDrivetrainAt[2] = {1097.0f, 75.0f};
constexpr float kDrivetrainHalf[2] = {12.5f, 25.0f};  // the icon's half size before it is laid down
constexpr float kClassGap = 12.0f;
constexpr float kPillTail = 10.0f;

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

// A tile's car (its thumbnail's 3/4 render), turned with the strip (the
// scene's 0.052 rad); the tile's studio sweep is the scene's InfinityCurve.
void draw_tile(ui::List& l, const SceneFrame& f, float cx, float cy, float hw, float hh, float angle, int64_t car_id,
               bool selected) {
  const uint32_t tint = selected ? kWhite : kTileDim;
  l.turn(f.x(cx), f.y(cy), angle);
  const float x0 = f.x(cx - hw), y0 = f.y(cy - hh), x1 = f.x(cx + hw), y1 = f.y(cy + hh);
  if (car_id > 0) {
    // The thumbnail's 3/4 render fills the tile's lower middle.
    const float tw = (x1 - x0) * 0.86f, th = tw * (0.70f / 0.62f) * (288.0f / 768.0f) * 1.0f;
    const float mx = 0.5f * (x0 + x1), my = y0 + (y1 - y0) * 0.56f;
    ui4::car_thumbnail(l, mx - 0.5f * tw, my - 0.5f * th, mx + 0.5f * tw, my + 0.5f * th, car_id, tint);
  }
  l.unturn();
}

}  // namespace

const std::vector<const char*>& car_list_nodes() {
  static const std::vector<const char*> nodes = {
      "TitleTape/Group/TitleTapeText",
      "CAR_LIST/Group/CAR_LIST_ITEM_0/IMAGE/CAR_IMAGE_NEW",
      "CAR_LIST/Group/CAR_LIST_ITEM_6/Icons/Ready_Icon",
      "CAR_LIST/Group/CAR_LIST_ITEM_3/Icons/Not_Ready_Icon",
      "CAR_LIST/Group/CAR_LIST_ITEM_3/Icons/Undiscovered_Icon",
      "CAR_LIST/Group/CAR_LIST_ITEM_3/Icons/Car_Lock_LG",
      "CAR_LIST/HIGHLIGHT/TEXT_CURSOR",
      "CAR_LIST/HIGHLIGHT/CursorBig",
      "CAR_STATS/TitleBar/TEXT_CAR_NAME",
      "CAR_STATS/INFO/TEXT_MANUFACTURER",
      "CAR_STATS/INFO/FLAG",
      "CAR_STATS/INFO/CLASS_SM/Group/TEXT_PI",
      "CAR_STATS/INFO/CLASS_SM/Group/CLASS_A",
      "CAR_STATS/INFO/DRIVETRAIN",
      "CAR_STATS/CAR/Car_Thumb_",
      "CAR_STATS/STATS/CAR_STATS_PERFORMANCE_BARS/Group/TEXT_NAME",
      "CAR_STATS/STATS/CAR_STATS_PERFORMANCE_BARS5/Group/TEXT_VALUE",
      "PlayerBar/MiddleRow/TEXT_CAR",
      "PlayerBar/BottomRow/TEXT_CR",
      "Back_Pane",
      "ButtonBar",
  };
  return nodes;
}

std::string car_list_credits(int64_t cr) { return format1(text_of(car_list_text::kCredits), thousands(cr)); }

std::string drivetrain_texture(int drive_type, int engine_placement) {
  const char* drive = drive_type == 1 ? "FWD" : drive_type == 2 ? "RWD" : drive_type == 3 ? "AWD" : nullptr;
  if (!drive) return {};
  // The icons UI4/Common ships: FWD front only; RWD and AWD front, mid, rear.
  const char* where = engine_placement == 2 ? "Mid" : engine_placement == 3 ? "Rear" : "Front";
  if (drive_type == 1) where = "Front";
  return std::string("T:UI4/Common/Drivetrain_") + drive + "_" + where;
}

std::string ui_texture_key(const std::string& game_path) {
  if (game_path.empty()) return {};
  bool horizon = false;
  std::string m = fh1::ui_texture_member(game_path, horizon);
  if (m.size() > 4 && m[m.size() - 4] == '.') m.resize(m.size() - 4);
  for (char& ch : m)
    if (ch == '\\') ch = '/';
  return horizon ? m : "T:" + m;
}

bool draw_car_list(ui::List& l, const DriveHud& st, const CarListHud& c, float w, float h) {
  SceneScreen sc(kCarListScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.bindings().fit_text = false;
  sc.hide(sc.node("ButtonBar"));
  sc.hide(sc.node("Timer"));
  // The panes behind the strip draw misplaced (their crop is not decoded);
  // 19_autoshow_orig.png's dark band behind the tiles is drawn below.
  sc.hide(sc.node("Back_Pane"));

  // Title.
  const int title = sc.node("TitleTape/Group/TitleTapeText");
  sc.text(title, c.title);

  // The strip: slot k holds tile selected + k - 3.
  const int n = int(c.tiles.size());
  const int strip = sc.node("CAR_LIST");
  // The band behind the strip from the cursor on: HIGHLIGHT's BigPane01
  // draws misplaced (its crop is not decoded), so it is hidden and its band
  // drawn as measured on 19_autoshow_orig.png.
  sc.hide(sc.node("CAR_LIST/HIGHLIGHT/BigPane01"));
  l.turn(f.x(900), f.y(510), -0.052f);
  l.rect(f.x(494), f.y(424), f.x(1320), f.y(596), ui::rgba(10, 12, 16, 120));
  l.unturn();
  struct Shown {
    float cx, cy, hw, hh;
    int64_t car;
    bool selected;
    std::string icon;  // the state's icon (a scene texture key), over the car
    float icon_half = 0.0f;
  };
  std::vector<Shown> shown_tiles;
  for (int k = 0; k < kCarListSlots; ++k) {
    const int item = sc.node("Group/CAR_LIST_ITEM_" + std::to_string(k), strip);
    if (item < 0) continue;
    const int ti = c.selected + k - kCarListCursorSlot;
    if (ti < 0 || ti >= n) {
      sc.hide(item);
      continue;
    }
    const CarTile& t = c.tiles[size_t(ti)];
    const int image = child(sd, item, "IMAGE");
    const int car = child(sd, image, "CAR_IMAGE_NEW");
    sc.hide(child(sd, image, "CLUB_IMAGE"));
    sc.hide(car);
    const int icons = child(sd, item, "Icons");
    if (icons >= 0) sc.at(icons).alpha = 1.0f;
    for (const char* icon : {"Undiscovered_Icon", "Not_Ready_Icon", "Ready_Icon", "Icon_1", "Icon_2", "Icon_3",
                             "Car_Lock_LG", "COMMON_LOADING_ICON_B"})
      sc.hide(child(sd, icons, icon));
    // The state's icon: its material is clear until the list shows it, and
    // it sits under the car, so it is drawn after the thumbnails.
    const char* icon = nullptr;
    switch (t.state) {
      case CarTileState::kLocked: icon = "Car_Lock_LG"; break;
      case CarTileState::kUndiscovered: icon = "Undiscovered_Icon"; break;
      case CarTileState::kNotReady: icon = "Not_Ready_Icon"; break;
      case CarTileState::kReady: icon = "Ready_Icon"; break;
      case CarTileState::kNormal: break;
    }
    std::string icon_key;
    float icon_half = 0.0f;
    if (const int in = icon ? child(sd, icons, icon) : -1; in >= 0) {
      icon_key = sd.texture_key_of(in);
      const auto r = sd.rect(in);
      icon_half = 0.5f * std::fabs(r[2] - r[0]);
    }
    // The studio sweep behind the car, dimmer off the cursor.
    const int bg = child(sd, item, "BG");
    if (const int sweep = child(sd, bg, "InfinityCurve"); sweep >= 0) {
      sc.at(sweep).alpha = 1.0f;
      if (ti != c.selected) sc.at(sweep).tint = kTileDim;
    }
    sc.hide(child(sd, bg, "BigPane01"));
    if (car >= 0) {
      const auto r = sd.rect(car);  // x0, y0, x1, y1 (canvas)
      const float cx = 0.5f * (r[0] + r[2]), cy = 0.5f * (r[1] + r[3]);
      const float hw = 0.5f * std::fabs(r[2] - r[0]) - 4.0f, hh = 0.5f * std::fabs(r[3] - r[1]) - 4.0f;
      // An undiscovered barn shows no car.
      const int64_t shown = t.state == CarTileState::kUndiscovered ? 0 : t.car_id;
      shown_tiles.push_back({cx, cy, hw, hh, shown, ti == c.selected, icon_key, icon_half});
      l.add_hit(f.x(cx - hw), f.y(cy - hh), f.x(cx + hw), f.y(cy + hh), ui::kHitRow, ti);
    }
  }
  const int cursor = sc.node("CAR_LIST/HIGHLIGHT/TEXT_CURSOR");
  if (n > 0 && c.selected >= 0 && c.selected < n) {
    const std::string& line = c.tiles[size_t(c.selected)].cursor;
    sc.text(cursor, line);
    // No line (the garage's own cars): the frame alone.
    if (line.empty()) {
      sc.hide(cursor);
      sc.hide(sc.node("CAR_LIST/HIGHLIGHT/Pill01"));
    }
  } else {
    sc.hide(sc.node("CAR_LIST/HIGHLIGHT"));
  }

  // The selected car's panel.
  const int stats = sc.node("CarStatsGroup");
  if (!c.details.valid) {
    sc.hide(stats);
  } else {
    const CarListDetails& d = c.details;
    // The dirt panel behind the card and its stats.
    sc.at(sc.node("CAR_STATS/BG/HUDPanel01", stats)).wrap = true;
    sc.text(sc.node("CAR_STATS/TitleBar/TEXT_CAR_NAME", stats), d.model);
    sc.text(sc.node("CAR_STATS/INFO/TEXT_MANUFACTURER", stats), d.year_make);
    // The make's flag; without one the year and make move into its place.
    if (const int flag = sc.node("CAR_STATS/INFO/FLAG", stats); flag >= 0) {
      if (!d.flag.empty()) {
        sc.at(flag).texture = d.flag;
      } else {
        sc.hide(flag);
        const int mk = sc.node("CAR_STATS/INFO/TEXT_MANUFACTURER", stats);
        if (mk >= 0) sc.at(mk).dx = sd.rect(flag)[0] - sd.at(mk)[0];
      }
    }
    const int pi = sc.node("CAR_STATS/INFO/CLASS_SM/Group/TEXT_PI", stats);
    const int cls = sc.node("CAR_STATS/INFO/CLASS_SM/Group/CLASS_A", stats);
    if (!d.car_class.empty()) {
      sc.text(pi, std::to_string(d.pi));
      if (pi >= 0) sc.at(pi).tint = kInk;
      if (cls >= 0) sc.at(cls).texture = "PICLASS/CLASS_" + upper(d.car_class);
    } else {
      sc.hide(sc.node("CAR_STATS/INFO/CLASS_SM", stats));
    }
    // The year and make in ink on the photo's light frame.
    if (const int mk = sc.node("CAR_STATS/INFO/TEXT_MANUFACTURER", stats); mk >= 0) sc.at(mk).tint = kInk;
    // The drivetrain icon rests in the bars, turned and flipped; the capture
    // has it before the class, lying down (drawn below).
    sc.hide(sc.node("CAR_STATS/INFO/DRIVETRAIN", stats));
    static constexpr const StringRef* kNames[5] = {&car_list_text::kSpeed, &car_list_text::kHandling,
                                                   &car_list_text::kAcceleration, &car_list_text::kLaunch,
                                                   &car_list_text::kBraking};
    for (int i = 0; i < 5; ++i) {
      const std::string bars = i == 0 ? "CAR_STATS_PERFORMANCE_BARS" : "CAR_STATS_PERFORMANCE_BARS" + std::to_string(i + 1);
      const int group = sc.node("CAR_STATS/STATS/" + bars + "/Group", stats);
      if (group < 0) continue;
      const double v = std::clamp(d.stats[size_t(i)], 0.0, 10.0);
      sc.text(child(sd, group, "TEXT_NAME"), text_of(*kNames[i]));
      char buf[16];
      std::snprintf(buf, sizeof buf, "%.1f", v);
      sc.text(child(sd, group, "TEXT_VALUE"), buf);
      // The fill's crop width (canvas px) is its share of the empty bar's.
      const int bar = child(sd, group, "Bar");
      const int fill = child(sd, child(sd, child(sd, child(sd, bar, "C_BUY_PROGRESS_BAR"), "FILL"),
                                       "PROGRESS_BAR_STATIC_BKG"),
                             "StatBarFill");
      const int compare = child(sd, bar, "C_BUY_PROGRESS_BAR_BKG");
      const int empty = child(sd, child(sd, bar, "PROGRESS_BAR_STATIC_BKG"), "StatBarEmpty");
      sc.hide(compare);
      // The bars' crop is not decoded: their length as measured.
      if (empty >= 0) sc.at(empty).visible_width = kBarLength;
      if (fill >= 0) sc.at(fill).visible_width = float(kBarLength * v / 10.0);
    }
  }

  bind_player_bar(sc, st);
  // The player bar's class follows the car's name (19_autoshow_orig.png),
  // its pill cut after it.
  {
    const int bar = sc.node("PlayerBar");
    const int car = sc.node("MiddleRow/TEXT_CAR", bar), cls = sc.node("MiddleRow/CLASS_SM", bar);
    const int pill = sc.node("MiddleRow/pill_middle", bar);
    if (car >= 0 && cls >= 0 && !st.car_class.empty()) {
      const float name_end = sd.at(car)[0] + 0.9f * sd.measure(car, st.car_name);
      const float cls_left = sd.rect(sc.node("Group/CLASS_A", cls))[0];
      sc.at(cls).dx = name_end + kClassGap - cls_left;
      if (pill >= 0) {
        const auto pr = sd.rect(pill);
        const float cls_right = sd.rect(sc.node("Group/CLASS_A", cls))[2] + sc.at(cls).dx;
        sc.at(pill).visible_width = cls_right + kPillTail - pr[0];
      }
    }
  }
  sc.draw(l, f);
  for (const Shown& s : shown_tiles) {
    draw_tile(l, f, s.cx, s.cy, s.hw, s.hh, -0.052f, s.car, s.selected);
    std::array<float, 4> uv{};
    if (!s.icon.empty() && ui4::texture(s.icon, uv))
      l.image_rotated(f.x(s.cx), f.y(s.cy), s.icon_half * f.s, s.icon_half * f.s, -0.052f, uv,
                      s.selected ? ui::rgba(255, 255, 255) : kTileDim);
  }

  // The panel's photo: interim (SPEC_QUESTIONS Q128), the car's thumbnail
  // where the title shows its photo.
  if (c.details.valid) {
    const int thumb = sc.node("CAR_STATS/CAR/Car_Thumb_", stats);
    if (thumb >= 0) {
      const float cx = 0.5f * (kPhoto[0] + kPhoto[2]), cy = 0.5f * (kPhoto[1] + kPhoto[3]);
      const float hw = 0.5f * (kPhoto[2] - kPhoto[0]), hh = 0.5f * (kPhoto[3] - kPhoto[1]);
      l.turn(f.x(cx), f.y(cy), -0.052f);
      std::array<float, 4> uv{};
      if (ui4::texture("KITPARTS/INFINITYCURVE", uv)) l.image(f.x(cx - hw), f.y(cy - hh), f.x(cx + hw), f.y(cy + hh), uv);
      const float tw = 2.0f * hw * 0.92f, th = tw * (0.70f / 0.62f) * (288.0f / 768.0f);
      ui4::car_thumbnail(l, f.x(cx - 0.5f * tw), f.y(cy + 12.0f - 0.5f * th), f.x(cx + 0.5f * tw),
                         f.y(cy + 12.0f + 0.5f * th), c.details.car_id);
      l.unturn();
    }
    std::array<float, 4> uv{};
    const std::string dt_tex = drivetrain_texture(c.details.drive_type, c.details.engine_placement);
    if (!dt_tex.empty() && ui4::texture(dt_tex, uv)) {
      // The icon's car points up; the capture's lies nose left.
      l.image_rotated(f.x(kDrivetrainAt[0]), f.y(kDrivetrainAt[1]), kDrivetrainHalf[0] * f.s, kDrivetrainHalf[1] * f.s,
                      -1.5708f - 0.052f, uv);
    }
  }

  if (!c.notice.empty()) {
    const float s = std::max(0.25f, h / 720.0f);
    ui4::brush(l, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    std::string text = c.notice;
    ui4::fit_text(text, ns, 340.0f * s, 9.0f * s);
    l.text(w - 68.0f * s, h - 60.0f * s, text, ns);
  }
  std::vector<ui4::Prompt> prompts;
  for (const auto& [b, label] : c.prompts) prompts.push_back({b.c_str(), label});
  if (c.popup.open) {
    // The popup takes the help bar.
    std::vector<ui4::Prompt> pp;
    for (const auto& [b, label] : c.popup.prompts) pp.push_back({b.c_str(), label});
    draw_message_popup(l, c.popup.title, c.popup.message, pp, w, h);
    return true;
  }
  ui4::button_bar(l, prompts, w, h);
  return true;
}

}  // namespace carview
