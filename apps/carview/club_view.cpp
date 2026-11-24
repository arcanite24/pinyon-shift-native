#include "club_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "game_hud.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kGold = ui::rgba(246, 198, 56, 255);
constexpr uint32_t kCyan = ui::rgba(56, 194, 240, 255);

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
  spec.title = "1000 CLUB";
  spec.eyebrow = car_hdr;
  spec.scene_hint = "H_FIXED_SETTING_LIST";
  spec.status_kicker = "HORIZON 1000 CLUB";
  spec.status_value = status_val;
  spec.status_tag = status_tag;
  spec.status_color = kGold;
  spec.list_header = "CAR CHALLENGES";
  spec.list_header_right = st.challenges_begun ? "CHALLENGES ACTIVE" : "PRESS [A] TO BEGIN";
  spec.selected_index = st.selected;
  spec.max_visible = 7;

  if (!st.gate_reason.empty()) {
    spec.empty_message = st.gate_reason;
    spec.detail_header = "1000 CLUB LOCKED";
    spec.detail_title = "GREEN WRISTBAND REQUIRED";
    spec.detail_title_color = ui4::kAmber;
    spec.detail_icon_kind = int(ui4::BadgeIconKind::kLocked);
    spec.detail_paragraphs = {st.gate_reason,
                              "Earn the Horizon Green Wristband and drive one of your own Colorado garage cars to enter."};
    spec.prompts = {{"B", "BACK"}};
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

  if (!st.challenges_begun) spec.prompts.push_back({"A", "BEGIN CHALLENGES IN THIS CAR"});
  else spec.prompts.push_back({"A", "CHALLENGES ACTIVE"});
  spec.prompts.push_back({"DPAD", "INSPECT"});
  spec.prompts.push_back({"B", "RETURN TO DRIVE"});
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

  ui4::subtle_vignette(out, W, H, 0.36f);
  const std::string tag_disp = st.club_tag.empty() ? "NO TAG" : ("[" + st.club_tag + "]");
  const std::string name_disp = st.club_name.empty() ? "HORIZON CAR CLUB" : st.club_name;
  ui4::title_banner(out, s, 72.0f * sx, 34.0f * sy, "CAR CLUB", tag_disp + "  •  " + name_disp, true, 540.0f);

  const float lx0 = 72.0f * sx, lx1 = 760.0f * sx;
  const float ly0 = 132.0f * sy, ly1 = H - 84.0f * sy;
  ui4::panel(out, lx0, ly0, lx1, ly1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(out, lx0 + 12.0f * sx, ly0 + 10.0f * sy, lx1 - 12.0f * sx, ly0 + 34.0f * sy, s, false);
  out.text(lx0 + 22.0f * sx, ly0 + 15.0f * sy, "CLUB SETTINGS & SHARED GARAGE",
           ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

  int shared_count = 0;
  for (const auto& c : st.cars)
    if (c.shared) ++shared_count;

  const int total_rows = 2 + int(st.cars.size());
  const float row_h = 40.0f * sy;
  const float rows_top = ly0 + 44.0f * sy;
  const int max_vis = std::max(1, int((ly1 - rows_top - 10.0f * sy) / row_h));
  const int sel = std::clamp(st.selected, 0, std::max(0, total_rows - 1));
  const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, total_rows - max_vis));

  for (int i = first; i < std::min(total_rows, first + max_vis); ++i) {
    const float ry0 = rows_top + float(i - first) * row_h;
    const float ry1 = ry0 + row_h - 5.0f * sy;
    const bool is_sel = (i == sel);
    out.add_hit(lx0 + 10.0f * sx, ry0, lx1 - 10.0f * sx, ry1, ui::kHitRow, i);
    ui4::pill(out, lx0 + 10.0f * sx, ry0, lx1 - 10.0f * sx, ry1,
              is_sel ? ui4::kPink : ((i % 2 == 0) ? ui4::kPillGrey : ui::rgba(24, 26, 32, 232)),
              ui4::PillKind::kPill02, false, is_sel);

    if (i == 0) {
      out.text(lx0 + 24.0f * sx, ry0 + 9.0f * sy, "CLUB NAME", ui4::style(ui::Font::kD, 13.0f * s, ui4::kWhite));
      out.text(lx1 - 24.0f * sx, ry0 + 9.0f * sy, "< " + name_disp + " >",
               ui4::style(ui::Font::kE, 13.5f * s, is_sel ? ui4::kYellow : ui4::kWhite, ui::Align::kRight));
    } else if (i == 1) {
      out.text(lx0 + 24.0f * sx, ry0 + 9.0f * sy, "CLUB TAG (4-CHAR)",
               ui4::style(ui::Font::kD, 13.0f * s, ui4::kWhite));
      out.text(lx1 - 24.0f * sx, ry0 + 9.0f * sy, "< " + tag_disp + " >",
               ui4::style(ui::Font::kE, 13.5f * s, is_sel ? ui4::kYellow : ui4::kWhite, ui::Align::kRight));
    } else {
      const auto& c = st.cars[size_t(i - 2)];
      std::string lbl = c.class_pi.empty() ? c.display_name : ("[" + c.class_pi + "]  " + c.display_name);
      ui::TextStyle cs = ui4::style(ui::Font::kD, 13.0f * s, ui4::kWhite);
      ui4::fit_text(lbl, cs, (lx1 - lx0) - 210.0f * sx, 9.0f * s);
      out.text(lx0 + 24.0f * sx, ry0 + 9.0f * sy, lbl, cs);
      const std::string badge = c.shared ? (c.active ? "SHARED • DRIVING" : "SHARED IN CLUB") : "PRIVATE";
      out.text(lx1 - 24.0f * sx, ry0 + 9.5f * sy, badge,
               ui4::style(ui::Font::kE, 12.0f * s, c.shared ? (is_sel ? ui4::kYellow : ui4::kGreen) : ui4::kDim,
                          ui::Align::kRight));
    }
  }

  // Right-hand Club Summary card
  const float rx0 = 784.0f * sx, rx1 = W - 72.0f * sx;
  ui4::panel(out, rx0, ly0, rx1, ly0 + 290.0f * sy, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::brush(out, rx0 - 6.0f * sx, ly0 + 12.0f * sy, rx1 + 6.0f * sx, ly0 + 48.0f * sy, ui4::kPink);
  out.text(rx0 + 18.0f * sx, ly0 + 19.0f * sy, tag_disp + "  " + name_disp,
           ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite));
  out.text(rx0 + 18.0f * sx, ly0 + 68.0f * sy,
           "SHARED GARAGE: " + std::to_string(shared_count) + " / " + std::to_string(st.cars.size()) + " CARS",
           ui4::style(ui::Font::kB, 13.5f * s, ui4::kYellow));
  out.text(rx0 + 18.0f * sx, ly0 + 102.0f * sy, "YOUR 4-CHAR [TAG] APPEARS ON YOUR",
           ui4::style(ui::Font::kB, 12.5f * s, ui4::kOffWhite));
  out.text(rx0 + 18.0f * sx, ly0 + 122.0f * sy, "DRIVER STRIP AND RACE LEADERBOARDS.",
           ui4::style(ui::Font::kB, 12.5f * s, ui4::kOffWhite));
  out.text(rx0 + 18.0f * sx, ly0 + 162.0f * sy, "PRESS [A / ENTER] ON A CAR TO SHARE",
           ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim));
  out.text(rx0 + 18.0f * sx, ly0 + 182.0f * sy, "OR UNSHARE IT WITH THE CLUB GARAGE.",
           ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim));
  out.text(rx0 + 18.0f * sx, ly0 + 212.0f * sy, "PRESS [X / D] ON A SHARED CAR TO",
           ui4::style(ui::Font::kB, 12.0f * s, ui4::kGreen));
  out.text(rx0 + 18.0f * sx, ly0 + 232.0f * sy, "BORROW & DRIVE IT IMMEDIATELY.",
           ui4::style(ui::Font::kB, 12.0f * s, ui4::kGreen));

  if (!st.notice.empty()) {
    ui4::pill(out, rx0, ly0 + 306.0f * sy, rx1, ly0 + 340.0f * sy, ui4::kPink, ui4::PillKind::kHud01);
    out.text(0.5f * (rx0 + rx1), ly0 + 315.0f * sy, st.notice,
             ui4::style(ui::Font::kE, 12.5f * s, ui4::kWhite, ui::Align::kCenter));
  }

  ui4::button_bar(out, {{"A", "TOGGLE / CYCLE"}, {"X", "DRIVE SHARED CAR"}, {"B", "CLOSE"}}, W, H, 72.0f);
}

}  // namespace carview
