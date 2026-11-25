#include "club_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "fh1data/stringtables.h"
#include "game_hud.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kGold = ui::rgba(246, 198, 56, 255);
constexpr uint32_t kCyan = ui::rgba(56, 194, 240, 255);

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  const std::string ref = "_&" + std::to_string(packed);
  const std::string resolved = game_text(ref);
  if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  return fallback;
}

bool parse_class_pi(std::string_view raw, std::string& out_cls, int& out_pi) {
  out_cls.clear();
  out_pi = 0;
  size_t i = 0;
  while (i < raw.size() && (raw[i] == ' ' || raw[i] == '[')) ++i;
  if (i >= raw.size()) return false;
  // Class token: "R1", "R2", "R3" or single letter "F".."X".
  if ((raw[i] == 'R' || raw[i] == 'r') && i + 1 < raw.size() && (raw[i + 1] >= '1' && raw[i + 1] <= '3')) {
    out_cls.push_back('R');
    out_cls.push_back(raw[i + 1]);
    i += 2;
  } else if (std::isalpha(static_cast<unsigned char>(raw[i]))) {
    out_cls.push_back(char(std::toupper(static_cast<unsigned char>(raw[i]))));
    ++i;
  } else {
    return false;
  }
  while (i < raw.size() && !std::isdigit(static_cast<unsigned char>(raw[i]))) ++i;
  if (i >= raw.size()) return false;
  int val = 0;
  while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i]))) {
    val = val * 10 + (raw[i] - '0');
    ++i;
  }
  out_pi = std::clamp(val, 100, 999);
  return !out_cls.empty();
}

std::string sanitize_club_text(std::string s) {
  // Replace raw internal outpost/transform IDs (e.g. "OUTPOST_REDROCK_NODE") with clean display names.
  auto replace_all = [&](const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all("OUTPOST_REDROCK_NODE", "Red Rock Outpost");
  replace_all("OUTPOST_REDROCK", "Red Rock Outpost");
  replace_all("OUTPOST_BEAUMONT_NODE", "Beaumont Outpost");
  replace_all("OUTPOST_BEAUMONT", "Beaumont Outpost");
  replace_all("OUTPOST_CARSON_NODE", "Carson Outpost");
  replace_all("OUTPOST_CARSON", "Carson Outpost");
  replace_all("OUTPOST_CLEAR_SPRINGS_NODE", "Clear Springs Outpost");
  replace_all("OUTPOST_CLEAR_SPRINGS", "Clear Springs Outpost");
  replace_all("OUTPOST_FINLEY_NODE", "Finley Dam Outpost");
  replace_all("OUTPOST_DAM", "Finley Dam Outpost");
  replace_all("OUTPOST_GOLF", "Golf Course Outpost");
  replace_all("OUTPOST_MONTANO", "Montano Plains Outpost");
  replace_all("OUTPOST_BUNKERS", "Bunker Outpost");
  // Generic fallback for any remaining OUTPOST_XXX token
  for (size_t pos = 0; (pos = s.find("OUTPOST_", pos)) != std::string::npos;) {
    size_t end = pos + 8;
    while (end < s.size() && (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_')) ++end;
    std::string body = s.substr(pos + 8, end - (pos + 8));
    std::string nice;
    bool cap = true;
    for (char c : body) {
      if (c == '_') {
        nice.push_back(' ');
        cap = true;
      } else {
        nice.push_back(char(cap ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c))));
        cap = false;
      }
    }
    if (nice.empty()) nice = "Horizon";
    nice += " Outpost";
    s.replace(pos, end - pos, nice);
    pos += nice.size();
  }
  return s;
}

std::string resolve_club_car_name(const std::string& raw) {
  if (raw.empty()) return "COLORADO GARAGE";
  const std::string looked = car_name(raw);
  if (!looked.empty() && looked != raw && looked.find('_') == std::string::npos) return looked;
  return ui4::pretty_car_media_name(raw);
}

// Draws authentic FH1 Gold/Silver/Bronze medal artwork (T:UI4/Common/Icon_Gold, TicketStickerGold, Padlock).
void draw_medal_badge(ui::List& out, float cx, float cy, float r, bool earned, bool active_ring) {
  const float sz = r * 2.15f;
  const ui4::BadgeIconKind kind = earned ? ui4::BadgeIconKind::kGoldMedal
                                         : (active_ring ? ui4::BadgeIconKind::kSilverMedal : ui4::BadgeIconKind::kLocked);
  const uint32_t tint = earned ? ui4::kWhite : (active_ring ? ui::rgba(210, 214, 224, 155) : ui::rgba(160, 166, 178, 175));
  if (ui4::badge_icon(out, cx - 0.5f * sz, cy - 0.5f * sz, cx + 0.5f * sz, cy + 0.5f * sz, kind, tint)) {
    return;
  }
  // Sharp-edged fallback diamond badge (ZERO raw SDF circles).
  out.rect(cx - r * 0.85f, cy - r * 0.85f, cx + r * 0.85f, cy + r * 0.85f,
           earned ? kGold : ui::rgba(28, 30, 36, 232));
  out.frame(cx - r * 0.85f, cy - r * 0.85f, cx + r * 0.85f, cy + r * 0.85f, earned ? ui4::kWhite : kGold, 1.5f);
}

void draw_text(ui::List& out, float x, float y, float size, std::string_view text, uint32_t col,
               ui::Font font = ui::Font::kB, bool keep_case = false, ui::Align align = ui::Align::kLeft) {
  ui::TextStyle ts = ui4::style(font, size, col, align, keep_case);
  if (font == ui::Font::kE) ts.slant = 0.12f;
  out.text(x, y, text, ts);
}

}  // namespace

void draw_club_screen(ui::List& out, int w, int h, const ClubScreenState& st) {
  if (!st.active) return;
  const float W = float(w), H = float(h);

  const std::string car_hdr = resolve_club_car_name(st.car_name);
  char status_val[96];
  std::snprintf(status_val, sizeof status_val, "%d / %d MEDALS IN THIS CAR", st.car_medals,
                std::max(1, st.car_total));
  char status_tag[64];
  std::snprintf(status_tag, sizeof status_tag, "CAREER: %d (%d CARS)", st.stats.total_medals,
                st.stats.cars_with_medal);

  ui4::TiltedBladeSpec spec;
  spec.title = loc_str("1000Club", "IDS_Title", "1000 CLUB");
  spec.eyebrow = car_hdr;
  spec.scene_hint = "H_FIXED_SETTING_LIST";
  spec.status_kicker = loc_str("1000Club", "IDS_Horizon1000Club", "HORIZON 1000 CLUB");
  spec.status_value = status_val;
  spec.status_tag = status_tag;
  spec.status_color = kGold;
  spec.list_header = loc_str("1000Club", "IDS_CarChallenges", "CAR CHALLENGES");
  spec.list_header_right = st.challenges_begun ? loc_str("1000Club", "IDS_ChallengesActive", "CHALLENGES ACTIVE")
                                               : loc_str("1000Club", "IDS_ReadyToBegin", "READY TO BEGIN");
  spec.selected_index = st.selected;
  spec.max_visible = 7;

  if (!st.gate_reason.empty()) {
    spec.empty_message = st.gate_reason;
    spec.detail_header = loc_str("1000Club", "IDS_Locked", "1000 CLUB LOCKED");
    spec.detail_title = "GREEN WRISTBAND REQUIRED";
    spec.detail_title_color = ui4::kAmber;
    spec.detail_icon_kind = int(ui4::BadgeIconKind::kLocked);
    spec.detail_paragraphs = {st.gate_reason,
                              "Earn the Horizon Green Wristband and drive one of your own Colorado garage cars to enter."};
    spec.prompts = {{"B", loc_str("HelpButtons", "IDS_Back", "BACK")}};
    ui4::tilted_blade_screen(out, W, H, spec);
    return;
  }

  for (size_t i = 0; i < st.rows.size(); ++i) {
    const auto& r = st.rows[i];
    ui4::BladeRow br;
    br.label = sanitize_club_text(r.title);
    br.subtitle = sanitize_club_text(r.description);
    br.selected = (int(i) == st.selected);
    br.badge_icon_kind = r.completed ? int(ui4::BadgeIconKind::kGoldMedal)
                                     : (st.challenges_begun ? int(ui4::BadgeIconKind::kSilverMedal)
                                                            : int(ui4::BadgeIconKind::kLocked));
    if (r.completed) {
      br.value = "MEDAL EARNED";
      br.value_color = kGold;
    } else {
      br.value = r.status.empty() ? (st.challenges_begun ? "ACTIVE" : "READY") : sanitize_club_text(r.status);
      br.value_color = st.challenges_begun ? kCyan : ui4::kDim;
    }
    spec.rows.push_back(std::move(br));
  }

  // Right compact PANEL01 card: focused challenge details + 1000 Club Medal Milestones (NO full-screen charcoal wall!)
  spec.detail_header = "1000 CLUB MILESTONES  •  " + std::to_string(st.stats.achievements_unlocked) + "/10";
  if (st.selected >= 0 && st.selected < int(st.rows.size())) {
    const auto& sr = st.rows[size_t(st.selected)];
    spec.detail_title = sanitize_club_text(sr.title);
    spec.detail_title_color = sr.completed ? kGold : ui4::kPink;
    spec.detail_icon_kind = sr.completed ? int(ui4::BadgeIconKind::kGoldMedal) : int(ui4::BadgeIconKind::kSilverMedal);
    spec.detail_paragraphs.push_back(sanitize_club_text(sr.description));
    if (!sr.hint.empty()) spec.detail_paragraphs.push_back("Hint: " + sanitize_club_text(sr.hint));
    if (!sr.completed) {
      spec.detail_progress = float(std::clamp(sr.progress, 0.0, 1.0));
      spec.detail_progress_label = sr.status.empty() ? "CHALLENGE PROGRESS" : sanitize_club_text(sr.status);
      spec.detail_progress_color = ui4::kYellow;
    }
  } else {
    spec.detail_title = "GOLD MEDAL MILESTONES";
    spec.detail_title_color = kGold;
    spec.detail_icon_kind = int(ui4::BadgeIconKind::kGoldMedal);
  }

  auto mk_kv = [&](const char* lbl, int cur, int target, bool done) {
    char vb[24];
    std::snprintf(vb, sizeof vb, "%d / %d", std::min(cur, target), target);
    spec.detail_kvs.push_back({lbl, vb, done ? ui4::kGreen : ui4::kWhite});
  };
  mk_kv("FIRST MEDAL", std::min(1, st.stats.total_medals), 1, st.stats.ach_first_medal);
  mk_kv("DIFFERENT CARS", st.stats.cars_with_medal, 5, st.stats.ach_multi_cars);
  mk_kv("FORD / FERRARI", st.stats.ford_cars + st.stats.ferrari_cars, 8, st.stats.ach_ford && st.stats.ach_ferrari);
  mk_kv("NISSAN / BMW", st.stats.nissan_cars + st.stats.bmw_cars, 6, st.stats.ach_nissan && st.stats.ach_bmw);
  mk_kv("BRITISH CARS", st.stats.british_cars, 5, st.stats.ach_british);
  mk_kv("1,000,000+ CR CAR", st.stats.million_cr_cars, 1, st.stats.ach_million_cr);

  if (st.show_intro) {
    spec.detail_footer = "Complete challenges in each garage car to earn Gold Medals and unlock 1000 Club Achievements.";
  }

  if (!st.challenges_begun) spec.prompts.push_back({"A", loc_str("1000Club", "IDS_BeginChallenges", "BEGIN CHALLENGES IN THIS CAR")});
  else spec.prompts.push_back({"A", loc_str("1000Club", "IDS_ChallengesActive", "CHALLENGES ACTIVE")});
  spec.prompts.push_back({"DPAD", loc_str("HelpButtons", "IDS_Inspect", "INSPECT")});
  spec.prompts.push_back({"B", loc_str("HelpButtons", "IDS_Back", "RETURN TO DRIVE")});
  ui4::tilted_blade_screen(out, W, H, spec);
}

void draw_club_medal_ceremony(ui::List& out, int w, int h, const ClubMedalCeremony& c) {
  if (!c.active || c.age < 0.0 || c.age > c.duration) return;
  const float W = float(w), H = float(h);
  const float sx = W / 1280.0f, sy = H / 720.0f;
  const float s = std::min(sx, sy);

  float alpha = 1.0f;
  if (c.age < 0.25) alpha = float(c.age / 0.25);
  else if (c.age > c.duration - 0.5) alpha = float((c.duration - c.age) / 0.5);
  alpha = std::clamp(alpha, 0.0f, 1.0f);

  const float bw = 520.0f * sx;
  const float bh = 102.0f * sy;
  const float bx = c.dock_bottom_right ? (W - bw - 68.0f * sx) : (W - bw) * 0.5f;
  const float by = c.dock_bottom_right ? (H - bh - 56.0f * sy) : 18.0f * sy;

  ui4::TiltScope tilt(out, bx + 0.5f * bw, by + 0.5f * bh, ui4::kTiltRad * 0.6f);
  ui4::panel01(out, bx, by, bx + bw, by + bh, s, ui::fade(ui4::kCharcoalDark, alpha), ui::fade(kGold, alpha));
  draw_medal_badge(out, bx + 48.0f * sx, by + bh * 0.5f, 27.0f * s, true, true);

  draw_text(out, bx + 92.0f * sx, by + 11.0f * sy, 13.0f * s, "1000 CLUB MEDAL EARNED!", ui::fade(kGold, alpha),
            ui::Font::kE);
  const std::string ctitle = sanitize_club_text(c.challenge_title);
  ui::TextStyle tts = ui4::style(ui::Font::kE, 18.5f * s, ui::fade(ui4::kWhite, alpha));
  ui4::fit_text(ctitle, tts, bw - 108.0f * sx, 11.0f * s);
  out.text(bx + 92.0f * sx, by + 30.0f * sy, ctitle, tts);
  if (!c.challenge_desc.empty()) {
    const std::string cdesc = sanitize_club_text(c.challenge_desc);
    ui::TextStyle dts = ui4::style(ui::Font::kB, 12.0f * s, ui::fade(ui4::kDim, alpha), ui::Align::kLeft, true);
    ui4::fit_text(cdesc, dts, bw - 108.0f * sx, 9.0f * s);
    out.text(bx + 92.0f * sx, by + 56.0f * sy, cdesc, dts);
  }

  char foot[128];
  std::snprintf(foot, sizeof foot, "%d / %d IN THIS CAR   |   TOTAL: %d MEDALS (%d CARS)", c.car_medals,
                std::max(1, c.car_total), c.total_medals, c.cars_with_medal);
  draw_text(out, bx + 92.0f * sx, by + 78.0f * sy, 11.5f * s, foot, ui::fade(kCyan, alpha));
}

void draw_club_active_strip(ui::List& out, int w, int h, const ClubActiveStrip& strip) {
  if (!strip.active) return;
  const float W = float(w), H = float(h);
  const float sx = W / 1280.0f, sy = H / 720.0f;
  const float s = std::min(sx, sy);

  const float bw = 400.0f * sx;
  const float bh = 46.0f * sy;
  const float bx = (W - bw) * 0.5f;
  const float by = 18.0f * sy;

  ui4::pill(out, bx, by, bx + bw, by + bh, ui4::kPillGrey, ui4::PillKind::kHud01);
  out.rect(bx, by, bx + 4.0f * sx, by + bh, kGold);
  const std::string ttl = "1000 CLUB  " + sanitize_club_text(strip.title);
  ui::TextStyle ts = ui4::style(ui::Font::kE, 12.5f * s, kGold);
  ui4::fit_text(ttl, ts, bw - 28.0f * sx, 9.0f * s);
  out.text(bx + 14.0f * sx, by + 7.0f * sy, ttl, ts);
  const std::string st_txt = sanitize_club_text(strip.status);
  ui::TextStyle ss = ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite, ui::Align::kLeft, true);
  ui4::fit_text(st_txt, ss, bw - 28.0f * sx, 8.5f * s);
  out.text(bx + 14.0f * sx, by + 24.0f * sy, st_txt, ss);
  const float p = float(std::clamp(strip.progress, 0.0, 1.0));
  if (p > 0.001f) {
    out.rect(bx + 4.0f * sx, by + bh - 4.0f * sy, bx + 4.0f * sx + (bw - 4.0f * sx) * p, by + bh, ui4::kYellow);
  }
}

CarClubEditorState& car_club_editor_state() {
  static CarClubEditorState s_state;
  return s_state;
}

std::string sanitize_club_name(const std::string& raw, size_t max_len) {
  std::string out;
  out.reserve(std::min(raw.size(), max_len));
  bool prev_space = true;
  for (unsigned char ch : raw) {
    if (ch < 32 || ch > 126) continue;
    if (std::isspace(ch)) {
      if (!prev_space && out.size() < max_len) {
        out.push_back(' ');
        prev_space = true;
      }
    } else if (out.size() < max_len) {
      out.push_back(char(ch));
      prev_space = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string sanitize_club_tag(const std::string& raw) {
  std::string out;
  out.reserve(4);
  for (unsigned char ch : raw) {
    if (std::isalnum(ch)) {
      out.push_back(char(std::toupper(ch)));
      if (out.size() == 4) break;
    }
  }
  return out;
}

bool toggle_club_shared_garage_car(std::vector<int64_t>& shared_garage_ids, int64_t garage_id) {
  if (garage_id <= 0) return false;
  auto it = std::find(shared_garage_ids.begin(), shared_garage_ids.end(), garage_id);
  if (it != shared_garage_ids.end()) {
    shared_garage_ids.erase(it);
    return false;
  }
  shared_garage_ids.push_back(garage_id);
  return true;
}

bool toggle_club_shared_car(std::vector<int>& shared_ids, int car_id) {
  if (car_id <= 0) return false;
  auto it = std::find(shared_ids.begin(), shared_ids.end(), car_id);
  if (it != shared_ids.end()) {
    shared_ids.erase(it);
    return false;
  }
  shared_ids.push_back(car_id);
  return true;
}

void draw_car_club_screen(ui::List& out, int w, int h, const CarClubScreenState& st) {
  if (!st.active) return;
  const float W = float(w), H = float(h);
  const float sx = W / 1280.0f, sy = H / 720.0f;
  const float s = std::min(sx, sy);

  const auto& ed = car_club_editor_state();
  auto row_is_shared = [&](size_t ci, const CarClubCarEntry& c) -> bool {
    if (ed.has_garage_sync && ci < ed.row_garage_ids.size() && ed.row_garage_ids[ci] > 0) {
      const int64_t gid = ed.row_garage_ids[ci];
      return std::find(ed.shared_garage_ids.begin(), ed.shared_garage_ids.end(), gid) != ed.shared_garage_ids.end();
    }
    return c.shared;
  };
  auto row_is_borrowed = [&](size_t ci, const CarClubCarEntry& c) -> bool {
    if (c.borrowed) return true;
    const int64_t bg = st.borrowed_garage_id > 0 ? st.borrowed_garage_id : ed.borrowed_garage_id;
    if (bg <= 0) return false;
    if (c.garage_id > 0) return c.garage_id == bg;
    if (ed.has_garage_sync && ci < ed.row_garage_ids.size()) return ed.row_garage_ids[ci] == bg;
    return false;
  };

  ui4::subtle_vignette(out, W, H, 0.28f);
  const std::string tag_disp = st.club_tag.empty() ? "NO TAG" : ("[" + st.club_tag + "]");
  const std::string name_disp = st.club_name.empty() ? "HORIZON CAR CLUB" : st.club_name;

  int shared_count = 0;
  for (size_t ci = 0; ci < st.cars.size(); ++ci)
    if (row_is_shared(ci, st.cars[ci])) ++shared_count;

  const int64_t borrowed_id = st.borrowed_garage_id > 0 ? st.borrowed_garage_id : ed.borrowed_garage_id;

  // Compact top-right Club Status pill (matches FH1 tilted blade chrome).
  {
    const float sx1 = W - 58.0f * sx;
    const float sx0 = sx1 - 320.0f * sx;
    const float sy0 = 32.0f * sy;
    out.rect(sx0, sy0, sx1, sy0 + 48.0f * sy, ui::rgba(14, 16, 22, 238));
    ui4::pill(out, sx0, sy0, sx1, sy0 + 48.0f * sy, ui4::kCharcoalDark, ui4::PillKind::kPillBig, false, true);
    out.rect(sx0, sy0, sx0 + 5.0f * sx, sy0 + 48.0f * sy, ui4::kPink);
    out.text(sx0 + 16.0f * sx, sy0 + 7.0f * sy, loc_str("Clubs", "IDS_ClubGarage", "CLUB GARAGE"),
             ui4::style(ui::Font::kB, 10.0f * s, ui4::kOffWhite));
    out.text(sx1 - 14.0f * sx, sy0 + 7.0f * sy, tag_disp,
             ui4::style(ui::Font::kE, 10.5f * s, ui4::kYellow, ui::Align::kRight));
    out.text(sx0 + 16.0f * sx, sy0 + 24.0f * sy,
             std::to_string(shared_count) + " / " + std::to_string(st.cars.size()) + " SHARED",
             ui4::style(ui::Font::kE, 15.5f * s, ui4::kWhite));
  }

  // ---- Entire Car Club blade tilted +3.0 deg (451_CLUB_SETTINGS authentic UI4 chrome) ----
  {
    ui4::TiltScope rot(out, 0.5f * W, 0.5f * H, ui4::kTiltRad);

    // Eyebrow + TitleTape (no raw inline key hints).
    ui::TextStyle es = ui4::style(ui::Font::kE, 14.0f * s, ui4::kWhite);
    const std::string eye = tag_disp + "  •  " + name_disp;
    ui4::fit_text(eye, es, 540.0f * s, 9.5f * s);
    const float ew = ui::List::measure(eye, es);
    ui4::brush(out, 74.0f * sx, 32.0f * sy, 74.0f * sx + ew + 34.0f * sx, 56.0f * sy, ui4::kBrushBlack);
    out.text(90.0f * sx, 36.5f * sy, eye, es);
    ui4::title_tape(out, s, 80.0f * sx, 54.0f * sy, loc_str("Clubs", "IDS_CarClub", "CAR CLUB"), ui4::kPink, 34.0f, 540.0f);

    const float lx0 = 78.0f * sx;
    const float lw = 696.0f * sx;
    const float lx1 = lx0 + lw;
    const float content_y = 116.0f * sy;
    const float list_bot = H - 96.0f * sy;

    // PILL01 #BFBFBF section header strip.
    ui4::pill(out, lx0, content_y, lx1, content_y + 28.0f * sy, ui::rgba(0xBF, 0xBF, 0xBF, 242), ui4::PillKind::kMenu,
              false, true);
    out.text(lx0 + 16.0f * sx, content_y + 7.0f * sy,
             loc_str("Clubs", "IDS_ClubSettings", "CLUB SETTINGS & SHARED GARAGE"),
             ui4::style(ui::Font::kD, 13.0f * s, ui4::kInk));
    out.text(lx1 - 16.0f * sx, content_y + 7.5f * sy,
             std::to_string(st.cars.size()) + " CARS",
             ui4::style(ui::Font::kB, 11.5f * s, ui4::kInk, ui::Align::kRight));

    const int total_rows = 2 + int(st.cars.size());
    const float row_h = 36.0f * sy;
    const float row_pitch = row_h + 5.0f * sy;
    const float rows_top = content_y + 34.0f * sy;
    const int max_vis = std::max(1, int((list_bot - rows_top) / row_pitch));
    const int sel = std::clamp(st.selected, 0, std::max(0, total_rows - 1));
    const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, total_rows - max_vis));

    float ry = rows_top;
    for (int i = first; i < std::min(total_rows, first + max_vis); ++i) {
      const float ry0 = ry;
      const float ry1 = ry0 + row_h;
      const bool is_sel = (i == sel);
      out.add_hit(lx0, ry0, lx1, ry1, ui::kHitRow, i);
      ui4::pill(out, lx0, ry0, lx1, ry1,
                is_sel ? ui4::kPink : ((i % 2 == 0) ? ui4::kPillGrey : ui::rgba(0x18, 0x18, 0x1C, 236)),
                (i % 2 == 0) ? ui4::PillKind::kPill02 : ui4::PillKind::kPill03, false, is_sel);

      if (i == 0 || i == 1) {
        const std::string row_lbl =
            (i == 0) ? loc_str("Clubs", "IDS_ClubName", "CLUB NAME") : loc_str("Clubs", "IDS_ClubTag", "CLUB TAG");
        const std::string& row_val = (i == 0) ? name_disp : tag_disp;
        out.text(lx0 + 18.0f * sx, ry0 + 0.5f * (row_h - 14.0f * s), row_lbl,
                 ui4::style(ui::Font::kD, 14.0f * s, ui4::kWhite));
        const float vx0 = lx1 - 248.0f * sx;
        const float vx1 = lx1 - 16.0f * sx;
        const float vcy = ry0 + 0.5f * row_h;
        const uint32_t arr_col = is_sel ? ui4::kWhite : ui::rgba(190, 196, 208, 175);
        ui4::opt_arrow(out, vx0 + 10.0f * sx, vcy, 14.0f * s, -1, arr_col);
        ui4::opt_arrow(out, vx1 - 10.0f * sx, vcy, 14.0f * s, +1, arr_col);
        ui::TextStyle vs = ui4::style(ui::Font::kE, 13.5f * s, is_sel ? ui4::kYellow : ui4::kWhite, ui::Align::kCenter);
        ui4::fit_text(row_val, vs, (vx1 - vx0) - 42.0f * sx, 9.0f * s);
        out.text(0.5f * (vx0 + vx1), ry0 + 0.5f * (row_h - vs.size), row_val, vs);
      } else {
        const size_t ci = size_t(i - 2);
        const auto& c = st.cars[ci];
        const bool shared = row_is_shared(ci, c);
        const bool borrowed = row_is_borrowed(ci, c);

        float tx = lx0 + 16.0f * sx;
        std::string cls_name;
        int pi_val = 0;
        if (parse_class_pi(c.class_pi, cls_name, pi_val)) {
          const float bh = 22.0f * sy;
          ui4::class_badge(out, tx, ry0 + 0.5f * (row_h - bh), bh, cls_name, pi_val);
          tx += bh * 3.45f + 12.0f * sx;
        }
        const std::string car_lbl =
            !c.display_name.empty() ? c.display_name : resolve_club_car_name(c.media_name);
        ui::TextStyle cs = ui4::style(ui::Font::kD, 13.5f * s, ui4::kWhite);
        ui4::fit_text(car_lbl, cs, (lx1 - 178.0f * sx) - tx, 9.0f * s);
        out.text(tx, ry0 + 0.5f * (row_h - cs.size), car_lbl, cs);

        const std::string badge = borrowed ? "BORROWED"
                                  : shared ? (c.active ? "SHARED / ACTIVE" : "SHARED IN CLUB")
                                           : "PRIVATE";
        const uint32_t bcol = borrowed ? kCyan
                              : shared ? (is_sel ? ui4::kYellow : ui4::kGreen)
                                       : ui4::kDim;
        out.text(lx1 - 16.0f * sx, ry0 + 0.5f * (row_h - 12.0f * s), badge,
                 ui4::style(ui::Font::kE, 12.0f * s, bcol, ui::Align::kRight));
      }
      ry += row_pitch;
    }

    ui4::scroll_indicator(out, lx1 + 8.0f * sx, rows_top, lx1 + 16.0f * sx, ry - 4.0f * sy, first, max_vis, total_rows,
                          s);

    // Compact Right-hand DIRTMASKS/PANEL01 Club Summary card (no inline raw keyboard hints!).
    const float rx0 = lx1 + 28.0f * sx;
    const float rx1 = W - 68.0f * sx;
    const float card_h = borrowed_id > 0 ? 290.0f * sy : 244.0f * sy;
    ui4::panel01(out, rx0, content_y, rx1, content_y + card_h, s, ui4::kCharcoalDark, ui4::kPink);
    ui4::pill(out, rx0 + 8.0f * sx, content_y + 8.0f * sy, rx1 - 8.0f * sx, content_y + 32.0f * sy,
              ui::rgba(0xBF, 0xBF, 0xBF, 240), ui4::PillKind::kMenu);
    out.text(rx0 + 18.0f * sx, content_y + 13.5f * sy, loc_str("Clubs", "IDS_ClubOverview", "CLUB OVERVIEW"),
             ui4::style(ui::Font::kD, 12.0f * s, ui4::kInk));
    ui4::brush(out, rx0 - 4.0f * sx, content_y + 38.0f * sy, rx1 + 4.0f * sx, content_y + 72.0f * sy, ui4::kPink);
    ui::TextStyle hts = ui4::style(ui::Font::kE, 15.5f * s, ui4::kWhite);
    ui4::fit_text(tag_disp + "  " + name_disp, hts, (rx1 - rx0) - 28.0f * sx, 10.0f * s);
    out.text(rx0 + 16.0f * sx, content_y + 46.0f * sy, tag_disp + "  " + name_disp, hts);

    float dy = content_y + 86.0f * sy;
    auto kv_row = [&](std::string_view k, std::string_view v, uint32_t vc) {
      out.text(rx0 + 16.0f * sx, dy, k, ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
      out.text(rx1 - 16.0f * sx, dy - 0.5f * sy, v,
               ui4::style(ui::Font::kE, 12.0f * s, vc, ui::Align::kRight, true));
      dy += 23.0f * sy;
    };
    kv_row("CLUB TAG", tag_disp, ui4::kYellow);
    kv_row("SHARED GARAGE", std::to_string(shared_count) + " / " + std::to_string(st.cars.size()) + " CARS",
           ui4::kGreen);
    kv_row("STATUS", borrowed_id > 0 ? "DRIVING SHARED BUILD" : "CLUB READY", borrowed_id > 0 ? kCyan : ui4::kWhite);

    dy += 8.0f * sy;
    out.text(rx0 + 16.0f * sx, dy, "Share your custom builds with the club",
             ui4::style(ui::Font::kB, 11.5f * s, ui4::kOffWhite, ui::Align::kLeft, true));
    out.text(rx0 + 16.0f * sx, dy + 18.0f * sy, "or borrow any shared car to drive.",
             ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim, ui::Align::kLeft, true));

    if (borrowed_id > 0) {
      ui4::pill(out, rx0 + 14.0f * sx, content_y + card_h - 44.0f * sy, rx1 - 14.0f * sx,
                content_y + card_h - 14.0f * sy, kCyan, ui4::PillKind::kHud01);
      out.text(0.5f * (rx0 + rx1), content_y + card_h - 36.0f * sy,
               "BORROWING CLUB CAR #" + std::to_string(borrowed_id),
               ui4::style(ui::Font::kE, 12.0f * s, ui4::kInk, ui::Align::kCenter));
    }

    if (!st.notice.empty()) {
      const float ny = content_y + card_h + 12.0f * sy;
      ui4::brush(out, rx0, ny, rx1, ny + 30.0f * sy, ui4::kPink);
      ui::TextStyle ns = ui4::style(ui::Font::kE, 12.0f * s, ui4::kWhite, ui::Align::kCenter);
      ui4::fit_text(st.notice, ns, (rx1 - rx0) - 24.0f * sx, 8.5f * s);
      out.text(0.5f * (rx0 + rx1), ny + 8.0f * sy, st.notice, ns);
    }
  }

  ui4::button_bar(out,
                  {{"A", loc_str("Clubs", "IDS_ToggleShare", "TOGGLE / CYCLE")},
                   {"Y", loc_str("Clubs", "IDS_EditNameTag", "EDIT NAME / TAG")},
                   {"X", borrowed_id > 0 ? loc_str("Clubs", "IDS_ReturnCar", "BORROW / RETURN")
                                         : loc_str("Clubs", "IDS_DriveShared", "DRIVE SHARED CAR")},
                   {"B", loc_str("HelpButtons", "IDS_Back", "CLOSE")}},
                  W, H, 72.0f);

  const OnScreenKeyboard* active_osk = (st.osk && st.osk->is_open()) ? st.osk
                                       : ed.osk.is_open()            ? &ed.osk
                                                                     : nullptr;
  if (active_osk) {
    active_osk->draw(out, W, H);
  }
}

}  // namespace carview
