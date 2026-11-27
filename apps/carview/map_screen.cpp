#include "map_screen.h"

#include <algorithm>
#include <cmath>

#include "game_hud.h"
#include "pause_screen.h"
#include "race_screens.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);
constexpr uint32_t kGrey = ui::rgba(190, 190, 190);
// The popup's box: one translucent black panel behind its rows
// (map_popup_outpost_orig.png).
constexpr uint32_t kBox = ui::rgba(0, 0, 0, 190);
constexpr float kBoxPad = 12.0f;
// The rows sit a little higher than the scene's (map_popup_outpost_orig.png).
constexpr float kRowsDy = -7.0f;
// The lock-on ring: a faint white ring about 54 px across round the icon
// (map_popup_event_orig.png); the scene's is pink and 90 px.
constexpr uint32_t kLockRing = ui::rgba(255, 255, 255, 150);
constexpr float kLockScale = 0.6f;
// The event chips (map_popup_event_orig.png): points pink, prize grey.
constexpr uint32_t kChipPink = ui4::kPink;
constexpr uint32_t kChipGrey = ui::rgba(112, 112, 112);
// The filter rows: black, the focused row pink (map_filter_orig.png).
constexpr uint32_t kRowBlack = ui::rgba(22, 22, 22, 235);
constexpr uint32_t kRowPink = ui4::kPink;
constexpr float kRowPitch = 28.0f;  // FixedSettingsBar .. FixedSettingsBar5
constexpr int kFilterWindow = 8;    // rows the original shows at once
// The list sits higher than the scene lays it out (the "All" row's tape from
// y 257 in map_filter_orig.png, the scene's first Pill from 285).
constexpr float kFilterDy = -30.0f;
constexpr float kFilterLean = 0.052f;
constexpr float kFilterNameX = 371.0f;  // the first row name's left edge
constexpr float kFilterPillW = 494.0f;  // rows end about x 860 (map_filter_orig.png)
constexpr float kTrackX0 = 870.0f, kTrackY0 = 242.0f, kTrackY1 = 463.0f, kThumbLen = 56.0f;
constexpr uint32_t kTrack = ui::rgba(20, 20, 20, 235);
constexpr uint32_t kThumb = ui::rgba(214, 214, 214, 250);
constexpr float kFilterTitleDy = -28.0f;
constexpr float kFilterBarDy = 14.0f;
constexpr uint32_t kFilterBar = ui::rgba(226, 226, 226);

int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

std::string hub_name(int i) { return i == 0 ? "StreetRaceHUB" : "StreetRaceHUB" + std::to_string(i + 1); }

const char* pip_node(MapPip p) {
  switch (p) {
    case MapPip::kGold: return "Gold";
    case MapPip::kSilver: return "Silver";
    case MapPip::kBronze: return "Bronze";
    case MapPip::kAvailable: return "Available";
    default: return "Unavailable";
  }
}

std::vector<ui4::Prompt> prompts_of(const std::vector<std::pair<std::string, std::string>>& in,
                                    std::vector<std::string>& keep) {
  keep.clear();
  keep.reserve(in.size());
  std::vector<ui4::Prompt> out;
  for (const auto& [b, label] : in) {
    keep.push_back(b);
    out.push_back({nullptr, label});
  }
  for (size_t i = 0; i < out.size(); ++i) out[i].button = keep[i].c_str();
  return out;
}

// --- H_MAP's DETAIL popup ----------------------------------------------------

struct PopupRows {
  std::vector<int> texts;  // the rows' text nodes, top to bottom
  std::vector<int> pills;  // the box's tapes
};

// Crops the box's tapes to the widest row; returns the box's middle (canvas
// x, before the group's offset).
float place_box(SceneScreen& sc, const PopupRows& rows, const std::vector<float>& rights) {
  const SceneDraw& sd = sc.scene();
  float right = 0.0f;
  for (float r : rights) right = std::max(right, r);
  float left = 1e9f;
  for (int p : rows.pills) {
    if (p < 0) continue;
    left = std::min(left, sd.rect(p)[0]);
    sc.at(p).visible_width = std::max(40.0f, right - sd.rect(p)[0] + kBoxPad);
    sc.at(p).tint = kBox;
  }
  return left < 1e8f ? 0.5f * (left + right + kBoxPad) : 640.0f;
}

float text_right(SceneScreen& sc, int node, const std::string& s) {
  if (node < 0) return 0.0f;
  return sc.scene().at(node)[0] + sc.scene().measure(node, s);
}

void bind_popup(SceneScreen& sc, const MapPopupHud& p) {
  const SceneDraw& sd = sc.scene();
  const int detail = sc.node("DETAIL");
  static constexpr std::array<std::pair<MapPopupKind, const char*>, 7> kGroups{{
      {MapPopupKind::kCareerRace, "CareerRace"},
      {MapPopupKind::kExhibition, "Exhibition"},
      {MapPopupKind::kStreetRace, "StreetRace"},
      {MapPopupKind::kGeneric, "Generic"},
      {MapPopupKind::kSimple, "Simple"},
      {MapPopupKind::kNemesis, "Nemesis"},
      {MapPopupKind::kOutpost, "Outpost"},
  }};
  int group = -1;
  for (const auto& [k, name] : kGroups) {
    const int g = child(sd, detail, name);
    if (k == p.kind) group = g;
    else sc.hide(g);
  }
  if (group < 0) return;
  sc.show(group);
  // The scene's popups sit over a cursor at 640,360.
  sc.at(detail).dx = p.x - 640.0f;
  sc.at(detail).dy = p.y - 360.0f;
  const int top = child(sd, group, "Top"), middle = child(sd, group, "Middle"), bottom = child(sd, group, "Bottom");
  PopupRows rows;
  std::vector<float> rights;
  auto name_row = [&](int under) {
    const int t = child(sd, under, "TEXT_NAME");
    sc.text(t, p.title);
    if (t >= 0) sc.at(t).tint = kWhite;
    rights.push_back(text_right(sc, t, p.title));
    return t;
  };
  switch (p.kind) {
    case MapPopupKind::kCareerRace: {
      // The stackers are not run: the rows are laid out lower than the box
      // the game stacks them into (two rows ending above the pin).
      sc.at(top).dy = 281.0f - 435.0f;
      sc.at(middle).dy = 306.0f - 456.0f;
      if (const int pin = child(sd, group, "Pin"); pin >= 0) {
        sc.at(pin).dy = -42.0f;
        sc.at(pin).extra_angle = 3.14159265f;
      }
      sc.hide(bottom);
      name_row(top);
      const int warn = child(sd, top, "Pill_warning");
      if (p.line.empty()) {
        sc.hide(warn);
      } else if (warn >= 0) {
        sc.show(warn);
        for (int c : sd.children_of(warn)) sc.show(c);
      }
      rows.pills = {child(sd, top, "Pill_top"), child(sd, top, "Pill_bottom")};
      const int cls = child(sd, middle, "Class"), pts = child(sd, middle, "TEXT_PTS");
      const int pill_pts = child(sd, middle, "Pillpoints"), money = child(sd, middle, "TEXT_MONEY");
      const int pill_money = child(sd, middle, "Pillmoney");
      float x = sd.rect(cls)[0];
      if (!p.class_name.empty() && cls >= 0) {
        sc.at(cls).texture = "PICLASS/CLASS_" + p.class_name + "_SM";
        x = sd.rect(cls)[2] + 4.0f;
      } else {
        sc.hide(cls);
      }
      auto chip = [&](int text, int pill, const std::string& s, uint32_t colour) {
        if (text < 0) return;
        if (s.empty()) {
          sc.hide(text), sc.hide(pill);
          return;
        }
        const float w = sd.measure(text, s);
        const float shift = x + 6.0f - sd.at(text)[0];
        sc.text(text, s);
        sc.at(text).dx = shift;
        sc.at(text).tint = kWhite;
        if (pill >= 0) {
          sc.at(pill).dx = x - sd.rect(pill)[0];
          sc.at(pill).visible_width = w + 12.0f;
          sc.at(pill).tint = colour;
        }
        x += w + 16.0f;
      };
      chip(pts, pill_pts, p.points, kChipPink);
      chip(money, pill_money, p.money, kChipGrey);
      rights.push_back(x);
      break;
    }
    case MapPopupKind::kExhibition: {
      name_row(top);
      rows.pills = {child(sd, top, "Pill_top"), child(sd, top, "Pill_bottom")};
      const int money = child(sd, middle, "Money");
      const int text = child(sd, money, "TEXT_MONEY"), pill = child(sd, money, "Pillmoney");
      const int cls = child(sd, middle, "Class");
      if (!p.class_name.empty()) sc.at(cls).texture = "PICLASS/CLASS_" + p.class_name + "_SM";
      else sc.hide(cls);
      sc.text(text, p.money);
      if (pill >= 0) {
        sc.at(pill).visible_width = sd.measure(text, p.money) + 12.0f;
        sc.at(pill).tint = kChipGrey;
      }
      rights.push_back(text_right(sc, text, p.money) + 6.0f);
      break;
    }
    case MapPopupKind::kStreetRace:
    case MapPopupKind::kOutpost: {
      name_row(top);
      rows.pills = {child(sd, top, "Pilltop"), child(sd, top, "Pillmiddle"), child(sd, top, "Pillbottom")};
      const int line = child(sd, middle, "TEXT_COMPLETE");
      sc.text(line, p.line);
      if (line >= 0) sc.at(line).tint = kGrey;
      rights.push_back(text_right(sc, line, p.line));
      const int list = child(sd, bottom, "HubList");
      float pips_right = 0.0f;
      for (int i = 0; i < 10; ++i) {
        const int hub = child(sd, list, hub_name(i));
        if (hub < 0) continue;
        if (size_t(i) >= p.pips.size()) {
          sc.hide(hub);
          continue;
        }
        sc.show(hub);
        for (const char* state : {"Gold", "Silver", "Bronze", "Available", "Unavailable"}) {
          const int s = child(sd, hub, state);
          if (std::string_view(state) == pip_node(p.pips[size_t(i)])) sc.show(s);
          else sc.hide(s);
        }
        pips_right = std::max(pips_right, sd.rect(hub)[2] + 8.0f);
      }
      if (p.kind == MapPopupKind::kOutpost) {
        const int label = child(sd, bottom, "TEXT_DISCOUNT"), value = child(sd, bottom, "TEXT_VALUE");
        sc.text(label, p.discount_label);
        sc.text(value, p.discount_value);
        // Packed after the pips (the scene's stacker).
        const float lx = pips_right > 0.0f ? pips_right + 4.0f : sd.at(label)[0];
        if (label >= 0) {
          sc.show(label);
          sc.at(label).dx = lx - sd.at(label)[0];
          sc.at(label).tint = kGrey;
        }
        const float vx = lx + sd.measure(label, p.discount_label) + 8.0f;
        if (value >= 0) {
          sc.show(value);
          sc.at(value).dx = vx - sd.at(value)[0];
          sc.at(value).tint = kWhite;
        }
        rights.push_back(vx + sd.measure(value, p.discount_value));
      } else {
        rights.push_back(pips_right);
      }
      break;
    }
    case MapPopupKind::kGeneric: {
      name_row(top);
      rows.pills = {child(sd, top, "Pill_top"), child(sd, top, "Pill_bottom")};
      const int line = child(sd, middle, "TEXT_COMPLETE");
      sc.text(line, p.line);
      if (line >= 0) sc.at(line).tint = kGrey;
      rights.push_back(text_right(sc, line, p.line));
      break;
    }
    case MapPopupKind::kSimple: {
      // One row above the pin (the player's name in map_open_orig.png).
      sc.at(top).dy = 306.0f - 434.0f;
      name_row(top);
      rows.pills = {child(sd, top, "Pill_top")};
      break;
    }
    case MapPopupKind::kNemesis: {
      name_row(top);
      rows.pills = {child(sd, top, "Pill_top"), child(sd, top, "Pill_bottom")};
      const int cls = child(sd, middle, "Class"), line = child(sd, middle, "TEXT_COMPLETE");
      if (!p.class_name.empty()) sc.at(cls).texture = "PICLASS/CLASS_" + p.class_name + "_SM";
      else sc.hide(cls);
      sc.text(line, p.line);
      if (line >= 0) sc.at(line).tint = kGrey;
      rights.push_back(text_right(sc, line, p.line));
      break;
    }
    default: break;
  }
  // The box is centred over the icon and its last row ends about 60 px
  // above it (map_popup_event_orig.png, map_popup_outpost_orig.png); the pin
  // stays on the icon.
  const float mid = place_box(sc, rows, rights);
  sc.at(group).dx = 640.0f - mid;
  sc.at(group).dy = kRowsDy;
  if (const int pin = child(sd, group, "Pin"); pin >= 0) {
    sc.at(pin).dx -= 640.0f - mid;
    sc.at(pin).dy -= kRowsDy;
    sc.at(pin).tint = kBox;
  }
}

void draw_map(ui::List& l, const DriveHud& st, const MapScreenHud& ms, const SceneFrame& f, bool dim) {
  SceneScreen sc(kMapScene);
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  // The scene's placeholders the original never shows over the map.
  for (const char* n : {"STATISTICS", "CONTEXT_POPUP", "Tip", "ButtonBar", "Layer2"}) sc.hide(sc.node(n));
  // The cursor: the cross-hair ring when free, the lock-on ring on an icon.
  const int cursor = sc.node("CURSOR"), snap = sc.node("CURSOR_SNAP");
  const bool overlay = ms.filter_open || ms.dialog != MapDialog::kNone || ms.details;
  // The cursor fades in after the map (map_open_f22_orig.png .. f30).
  const float in = float(std::clamp((ms.age - 0.55) / 0.3, 0.0, 1.0));
  if (!overlay && cursor >= 0) {
    if (ms.locked) {
      sc.hide(cursor);
      sc.show(snap, in);
      const int ring = child(sd, snap, "Rectangle3");
      sc.show(ring, in);
      if (ring >= 0) sc.at(ring).tint = kLockRing;
      sc.at(snap).dx = ms.popup.x - 640.0f;
      sc.at(snap).dy = ms.popup.y - 360.0f;
      sc.at(snap).scale = kLockScale;
    } else {
      sc.hide(snap);
      sc.at(cursor).dx = ms.cursor[0] - 640.0f;
      sc.at(cursor).dy = ms.cursor[1] - 360.0f;
      const int group = child(sd, cursor, "Group");
      sc.show(group, in);
      sc.show(child(sd, group, "Rectangle"), in);
    }
  } else {
    sc.hide(cursor);
    sc.hide(snap);
  }
  const int detail = sc.node("DETAIL");
  if (!overlay && ms.popup.kind != MapPopupKind::kNone && in > 0.0f) {
    bind_popup(sc, ms.popup);
    if (detail >= 0) sc.at(detail).alpha = in;
  } else {
    sc.hide(detail);
  }
  bind_pause_player_bar(sc, st, ms.band_texture);
  if (dim) sc.bindings().opacity = 0.55f;
  sc.draw(l, f);
}

// --- The FILTER popup (H_FIXED_SETTING_LIST) -----------------------------------

void draw_filter(ui::List& l, const MapScreenHud& ms, const SceneFrame& f) {
  SceneScreen sc(kMapFilterScene);
  if (!sc.ok()) return;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  sc.hide(sc.node("HELP_BUTTON_BAR"));
  // The title block sits higher than the list's offset puts it, and the
  // tape under it is a light bar the list's width (map_filter_orig.png:
  // FILTER at y 202, the bar from x 362 to 860 at y 228..262).
  const int title = sc.node("TitleTapeText");
  sc.text(title, text_of(map_text::kFilterTitle));
  if (title >= 0) sc.at(title).dy = kFilterTitleDy;
  if (const int clip = sc.node("Clipon_2"); clip >= 0) sc.at(clip).dy = kFilterTitleDy;
  sc.hide(sc.node("TitleTape/Group/Clipon"));
  sc.hide(sc.node("TitleTape/Group/Dirt_Tile"));
  sc.hide(sc.node("TitleTape/Group/Smudge_Tape"));
  if (const int t = sc.node("TitleTape/Group/Crisp_Tape"); t >= 0) {
    sc.at(t).tint = kFilterBar;
    sc.at(t).dx = 362.0f - sc.scene().rect(t)[0];
    sc.at(t).visible_width = 860.0f - 362.0f;
    sc.at(t).dy = kFilterBarDy;
  }
  const int list = sc.node("SCROLLING_LIST");
  const int bars_group = child(sd, list, "Group");
  // Everything but the rows and the scroll indicator, then the rows one by
  // one and the indicator by hand.
  sc.hide(bars_group);
  sc.hide(sc.node("NAV_POSITION_INDICATOR"));
  sc.bindings().dx = 0.0f;
  sc.bindings().dy = kFilterDy;
  sc.draw(l, f);
  sc.at(bars_group) = NodeOverride{};

  std::array<int, 5> bars{};
  for (int i = 0; i < 5; ++i) bars[size_t(i)] = sc.node(i == 0 ? "FixedSettingsBar" : "FixedSettingsBar" + std::to_string(i + 1));
  // The list scrolls only when the cursor passes its first or last row
  // (map_filter_2_orig.png: Outposts in the last of eight with All on top).
  const int first = std::clamp(ms.filter_first >= 0 ? ms.filter_first : ms.filter_row - (kFilterWindow - 1), 0,
                               kMapFilterRows - kFilterWindow);
  for (int slot = 0; slot < kFilterWindow && first + slot < kMapFilterRows; ++slot) {
    const int bar = bars[size_t(slot % 5)];
    if (bar < 0) continue;
    const int row = first + slot;
    const int g = child(sd, bar, "Group");
    const int pill = child(sd, child(sd, g, "_6pxPill"), "Pill");
    const int name = child(sd, g, "TEXT_SETTING_NAME"), value = child(sd, g, "TEXT_SETTING");
    SceneBindings& b = sc.bindings();
    b.by_node.clear();
    // The rows lie on one leaning line (the bars' 0.052 rad): the original
    // starts each row's name 1.5 px right of the one above, from x 371 for
    // the first (map_filter_orig.png), where the scene's five bars carry
    // their own few pixels of jitter. Rows past the fifth reuse the bars
    // 140 px lower.
    const float drop = float(slot / 5) * 5.0f * kRowPitch;
    const float name_x = name >= 0 ? sd.rect(name)[0] : 0.0f;
    b.dx = name >= 0 ? kFilterNameX + float(slot) * kRowPitch * kFilterLean - name_x : 0.0f;
    b.dy = kFilterDy + drop;
    const bool focus = row == ms.filter_row;
    sc.text(name, text_of(map_filter_text(MapFilterRow(row))));
    sc.hide(value);
    if (pill >= 0) {
      sc.at(pill).tint = focus ? kRowPink : kRowBlack;
      // Narrower than the scene's pill, left edge kept (single-texture
      // models take no crop, so the pill is squeezed).
      const auto pr = sd.rect(pill);
      if (const float pw = pr[2] - pr[0]; pw > kFilterPillW) {
        sc.at(pill).stretch_x = kFilterPillW / pw;
        sc.at(pill).dx = -0.5f * (pw - kFilterPillW);
      }
    }
    sc.draw(l, f, bar);
    // The tick, right of the row (a check glyph the font lacks).
    if (ms.filter.ticked[size_t(row)] && pill >= 0) {
      const auto r = sd.rect(pill);
      const float cx = r[2] - 41.0f + b.dx, cy = 0.5f * (r[1] + r[3]) + b.dy - 13.0f;
      const float k = f.s;
      l.line(f.x(cx - 7.0f), f.y(cy), f.x(cx - 2.0f), f.y(cy + 5.0f), 3.0f * k, kWhite);
      l.line(f.x(cx - 2.0f), f.y(cy + 5.0f), f.x(cx + 8.0f), f.y(cy - 6.0f), 3.0f * k, kWhite);
    }
  }
  // The scroll indicator: a dark track leaning with the list right of the
  // rows and a light thumb at the selected row's share of it
  // (map_filter_orig.png at All, map_filter_2_orig.png at Outposts,
  // map_filter_3_orig.png at Barn Finds).
  const float t = float(ms.filter_row) / float(kMapFilterRows - 1);
  auto track_x = [](float y) { return kTrackX0 + (y - kTrackY0) * kFilterLean; };
  const float k = f.s;
  l.line(f.x(track_x(kTrackY0)), f.y(kTrackY0), f.x(track_x(kTrackY1)), f.y(kTrackY1), 4.0f * k, kTrack);
  const float y0 = kTrackY0 + t * (kTrackY1 - kTrackY0 - kThumbLen), y1 = y0 + kThumbLen;
  l.line(f.x(track_x(y0)), f.y(y0 + 3.0f), f.x(track_x(y1)), f.y(y1 - 3.0f), 6.0f * k, kThumb);
}

}  // namespace

SceneFrame map_screen_frame(float w, float h) { return SceneScreen::frame(w, h); }

bool draw_map_screen(ui::List& l, const DriveHud& st, const MapScreenHud& ms, float w, float h) {
  if (!ms.active) return false;
  {
    SceneScreen probe(kMapScene);
    if (!probe.ok()) return false;
  }
  const SceneFrame f = SceneScreen::frame(w, h);
  const bool overlay = ms.filter_open || ms.dialog != MapDialog::kNone;
  draw_map(l, st, ms, f, false);
  std::vector<std::string> keep;
  if (ms.details && draw_event_ticket(l, st, *ms.details, w, h, false)) {
    // The ticket over the undimmed map, the map's player bar kept
    // (map_popup_event_details_orig.png).
    ui4::button_bar(l,
                    {{"A", text_of(map_text::kSetRoute)},
                     {"B", text_of(map_text::kBack)},
                     {"Y", text_of(map_text::kRestrictions)}},
                    w, h);
  } else if (ms.filter_open) {
    draw_filter(l, ms, f);
    ui4::button_bar(l, {{"A", text_of(map_text::kToggle)}, {"B", text_of(map_text::kBack)}}, w, h);
  } else if (ms.dialog != MapDialog::kNone) {
    std::vector<ui4::Prompt> prompts;
    if (ms.dialog == MapDialog::kConfirm)
      prompts = {{"A", text_of(map_text::kOk)}, {"B", text_of(map_text::kCancel)}};
    else
      prompts = {{"A", text_of(map_text::kOk)}};
    draw_message_popup(l, text_of(map_text::kFastTravelTitle), ms.dialog_body, prompts, w, h);
  } else if (!overlay && ms.please_wait) {
    draw_please_wait_corner(l, w, h, ms.age);
    ui4::draw_info_tip(l, ms.tip, w, h);
  } else if (!overlay) {
    ui4::button_bar(l, prompts_of(ms.prompts, keep), w, h);
    ui4::draw_info_tip(l, ms.tip, w, h);
  }
  return true;
}

}  // namespace carview
