#include "ui_widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "events_page.h"
#include "fh1data/stringtables.h"
#include "game_hud.h"
#include "settings_page.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kPink = ui4::kPink;
constexpr uint32_t kBlack = ui4::kBrushBlack;
constexpr uint32_t kWhite = ui4::kWhite;
constexpr uint32_t kDim = ui4::kDim;

void draw_brush_strip(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour) {
  ui4::brush(l, x0, y0, x1, y1, colour);
}

std::string radio_logo_texture_path(const std::string& station_name, bool off) {
  if (off || station_name == "Radio4_Silent") return "HUD/RADIO/RS_OFF";
  if (station_name == "Radio1") return "HUD/RADIO/RS_BASSARENA";
  if (station_name == "Radio2") return "HUD/RADIO/RS_PULSE";
  if (station_name == "Radio3") return "HUD/RADIO/RS_2ELEVEN";
  return "HUD/RADIO/RS_BASSARENA";
}

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  const std::string ref = "_&" + std::to_string(packed);
  const std::string resolved = game_text(ref);
  if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  return fallback;
}

}  // namespace

bool looks_like_raw_key(std::string_view s) {
  if (s.empty()) return true;
  if (s.size() >= 2 && s[0] == '_' && s[1] == '&') return true;
  bool has_non_ascii = false;
  bool has_space = false;
  bool all_ident_ascii = true;
  for (char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (u >= 0x80u) {
      has_non_ascii = true;
      break;
    }
    if (u == ' ' || u == '\t' || u == '\n') has_space = true;
    if (!(std::isalnum(u) || u == '_' || u == ':' || u == '/' || u == '.' || u == '-')) {
      all_ident_ascii = false;
    }
  }
  // Strings containing non-ASCII UTF-8 bytes (JP, CHT, CHS, KO, Cyrillic, etc.)
  // are localized human-readable text, never raw ASCII string-table keys.
  if (has_non_ascii) return false;
  if (s.rfind("IDS_", 0) == 0 || s.rfind("UI_", 0) == 0 || s.rfind("STR_", 0) == 0 ||
      s.find(":IDS_") != std::string_view::npos || s.find("/IDS_") != std::string_view::npos) {
    return true;
  }
  if (!has_space && all_ident_ascii) return true;
  return false;
}

// ------------------------------------------------ GPU Vector-Font Radio Popup (947_HUD/RadioStation)

void draw_radio_popup_gpu(ui::List& l, const RadioPopupGpuState& st, float screen_w, float screen_h) {
  const float a = std::clamp(st.alpha, 0.0f, 1.0f);
  if (a <= 0.005f) return;
  const float s = screen_h / 720.0f;
  // 947_HUD/RadioStation: RS_* logo [1053,353..1221,433], #212121 HUDPILL01 title (B@15px R),
  // #F90064 HUDPILL01 artist (B@15px R), right-aligned above the tachometer.
  const float right_x = screen_w - (1280.0f - 1221.0f) * s;
  const float top_y = screen_h - (720.0f - 353.0f) * s;

  std::array<float, 4> logo_uv{};
  const std::string tex = radio_logo_texture_path(st.station_name, st.off);
  const bool have_logo = ui_texture(tex, logo_uv);
  const float logo_w = 168.0f * s;
  const float logo_h = 80.0f * s;

  const std::string title = st.off ? (st.station_label.empty() ? std::string("RADIO OFF") : st.station_label)
                                   : st.title;
  const std::string artist = st.off ? std::string{} : st.artist;

  ui::TextStyle title_ts = ui4::style(ui::Font::kB, 15.0f * s, ui::fade(kWhite, a), ui::Align::kRight, true);
  ui::TextStyle artist_ts = ui4::style(ui::Font::kB, 15.0f * s, ui::fade(kWhite, a), ui::Align::kRight, true);

  if (have_logo) {
    l.image(right_x - logo_w, top_y, right_x, top_y + logo_h, logo_uv, ui::fade(kWhite, a));
  } else {
    ui::TextStyle st_ts = ui4::style(ui::Font::kE, 16.0f * s, ui::fade(kPink, a), ui::Align::kRight);
    const std::string sl = !st.station_label.empty() ? st.station_label : st.station_name;
    ui4::shadowed_text(l, right_x, top_y + 28.0f * s, sl, st_ts, s);
  }

  // Title line on #212121 HUDPILL01 (right-aligned under logo).
  const float title_y = top_y + logo_h + 2.0f * s;
  if (!title.empty()) {
    const float tw = std::min(340.0f * s, ui::List::measure(title, title_ts) + 28.0f * s);
    ui4::pill(l, right_x - tw, title_y, right_x, title_y + 24.0f * s, ui::fade(ui4::kPillGrey, a),
              ui4::PillKind::kHud01);
    l.text(right_x - 10.0f * s, title_y + 4.5f * s, title, title_ts);
  }

  // Artist line on #F90064 HUDPILL01 right below title.
  if (!artist.empty()) {
    const float ay = title_y + 26.0f * s;
    const float aw = std::min(320.0f * s, ui::List::measure(artist, artist_ts) + 28.0f * s);
    ui4::pill(l, right_x - aw, ay, right_x, ay + 24.0f * s, ui::fade(ui4::kPink, a), ui4::PillKind::kHud01);
    l.text(right_x - 10.0f * s, ay + 4.5f * s, artist, artist_ts);
  }
}

// ------------------------------------------------ UI4 Scene Parity Renderers

void draw_button_bar_ui4(ui::List& l, const std::vector<ButtonBarItem>& items, float screen_w, float screen_h) {
  std::vector<ui4::Prompt> prompts;
  prompts.reserve(items.size());
  for (const auto& it : items) prompts.push_back({it.button, it.label});
  ui4::button_bar(l, prompts, screen_w, screen_h);
}

void draw_events_ui4(ui::List& l, float screen_w, float screen_h, const EventsPage& page,
                     const std::vector<garage::ClassBand>& bands) {
  if (!page.is_open()) return;
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.24f);

  float tilt_rad = ui4::kTiltRad;
  if (const fh1::UiScene* sc_ptr = ui_scene("H_STREET_RACE_HUB")) {
    SceneDraw sc(sc_ptr, true);
    if (const int rn = sc.find("Rotation"); rn >= 0) {
      const float a = -sc.angle(rn);
      if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) tilt_rad = a;
    }
  }

  const bool is_hub = page.hub_filter() > 0;
  const std::string header =
      is_hub ? ("STREET RACE HUB " + std::to_string(page.hub_filter())) : "RACE CENTRAL";

  // Top +3.0 deg H_STREET_RACE_HUB TitleTapeText + optional category tab strip
  {
    ui4::TiltScope top_rot(l, 0.5f * screen_w, 120.0f * s, tilt_rad);
    ui4::title_tape(l, s, 82.0f * s, 42.0f * s, header, ui4::kPink, 38.0f, 520.0f);
    if (!is_hub) {
      static const std::vector<std::string> kEventTabs = {"ALL", "FESTIVAL", "STREET", "SHOWCASES", "SHOWDOWNS"};
      ui4::tab_strip(l, s, 82.0f * s, 108.0f * s, screen_w - 82.0f * s, kEventTabs, int(page.active_tab()), true,
                     ui4::kPink);
    }
  }

  // ---- H_STREET_RACE_HUB: +3.0 deg Horizontal Tilted Event Ticket Carousel (pitch ~288px, inner text +6 deg) ----
  const auto& entries = page.entries();
  const uint32_t kTicketPink = ui::rgba(0xEF, 0x24, 0x93, 255);
  std::array<float, 4> drop_uv{}, top_sr_uv{}, top_uv{}, tape_uv{}, new_uv{}, gold_uv{}, sil_uv{}, brz_uv{};
  const bool has_drop = ui_texture("KITPARTS/DROPSHADOW", drop_uv);
  const bool has_top_sr = ui_texture("KITPARTS/TICKETDIRTTOPSR", top_sr_uv);
  const bool has_top = ui_texture("KITPARTS/TICKETDIRTTOP", top_uv);
  const bool has_tape = ui_texture("KITPARTS/LARGETAPE", tape_uv);
  const bool has_new = ui_texture("ICONS/TICKETSTICKERNEW", new_uv);
  const bool has_gold = ui_texture("ICONS/TICKETSTICKERGOLD", gold_uv);
  const bool has_sil = ui_texture("ICONS/TICKETSTICKERSILVER", sil_uv);
  const bool has_brz = ui_texture("ICONS/TICKETSTICKERBRONZE", brz_uv);

  {
    ui4::TiltScope rot(l, 0.5f * screen_w, 0.58f * screen_h, tilt_rad);
    if (entries.empty()) {
      ui4::panel01(l, 320.0f * s, 260.0f * s, 960.0f * s, 360.0f * s, s, ui4::kCharcoalDark, ui4::kPink);
      l.text(0.5f * screen_w, 300.0f * s, "NO EVENTS IN THIS CATEGORY YET",
             ui4::style(ui::Font::kE, 18.0f * s, ui4::kDim, ui::Align::kCenter));
    } else {
      const int n = int(entries.size());
      const int sel = int(std::min(page.selected_index(), entries.size() - 1));
      const int count = std::min(n, 4);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float pitch = 288.0f * s;
      const float cw = 268.0f * s;
      const float ch = 354.0f * s;
      const float total_w = float(count - 1) * pitch + cw;
      const float start_x = std::max(56.0f * s, 0.5f * (screen_w - total_w));
      const float card_y0 = is_hub ? 148.0f * s : 168.0f * s;

      for (int i = first; i < first + count; ++i) {
        const auto& e = entries[size_t(i)];
        const bool act = (i == sel);
        const float lift = 0.0f;
        const float cx0 = start_x + float(i - first) * pitch;
        const float cy0 = card_y0 - lift;
        const float cx1 = cx0 + cw;
        const float cy1 = cy0 + ch;
        l.add_hit(cx0, cy0, cx1, cy1, ui::kHitRow, i);

        // 1. KITPARTS/DROPSHADOW behind ticket
        if (has_drop) {
          l.image(cx0 - 8.0f * s, cy0 + 8.0f * s, cx1 + 14.0f * s, cy1 + 16.0f * s, drop_uv,
                  ui::rgba(0, 0, 0, act ? 195 : 135));
        } else {
          l.rect(cx0 + 6.0f * s, cy0 + 8.0f * s, cx1 + 6.0f * s, cy1 + 8.0f * s, ui::rgba(0, 0, 0, 130));
        }

        // 2. Ticket body + REAL Horizon.zip/EventTickets/{FestivalRaces,StreetRaces,Showcases,NemesisTickets}/*.xds!
        l.rect(cx0, cy0, cx1, cy1, ui::rgba(22, 24, 28, 244));
        const float art_y0 = cy0 + 34.0f * s;
        const float art_y1 = cy0 + 216.0f * s;
        const int ev_id = e.event ? int(e.event->event_id) : (i + 1);
        const int ev_style = e.event ? int(e.event->style) : (is_hub ? 2 : 1);
        const uint32_t art_tint = !e.unlocked ? ui::rgba(145, 150, 160, 210)
                                              : (act ? ui4::kWhite : ui::rgba(225, 228, 234, 232));
        if (e.is_collect_wristband) {
          l.rect(cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y1, ui::rgba(26, 22, 18, 250));
          l.rect(cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y0 + 38.0f * s, ui::rgba(255, 165, 20, 38));
          l.rect(cx0 + 4.0f * s, art_y1 - 36.0f * s, cx1 - 4.0f * s, art_y1, ui::rgba(239, 36, 147, 46));
          std::string up = e.display_name;
          for (char& ch : up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
          const char* col_key = "ORANGE";
          static constexpr const char* kCols[] = {"YELLOW", "GREEN", "BLUE", "PINK", "ORANGE", "PURPLE", "GOLD"};
          for (const char* ck : kCols) {
            if (up.find(ck) != std::string::npos) {
              col_key = ck;
              break;
            }
          }
          std::array<float, 4> wb_uv{};
          const bool has_wb = ui_texture(std::string("WRISTBANDS/") + col_key + "XL", wb_uv) ||
                              ui_texture(std::string("WRISTBANDS/") + col_key + "LARGE", wb_uv) ||
                              ui_texture(std::string("WRISTBANDS/") + col_key, wb_uv) ||
                              ui_texture("WRISTBANDS/WRISTBAND", wb_uv);
          if (has_wb) {
            const float wsz = 148.0f * s;
            const float wx0 = 0.5f * (cx0 + cx1) - 0.5f * wsz;
            const float wy0 = 0.5f * (art_y0 + art_y1) - 0.5f * wsz;
            l.image(wx0, wy0, wx0 + wsz, wy0 + wsz, wb_uv, art_tint);
          }
          l.text(0.5f * (cx0 + cx1), art_y1 - 24.0f * s, "HORIZON FESTIVAL STAGE",
                 ui4::style(ui::Font::kB, 10.5f * s, ui4::kYellow, ui::Align::kCenter));
        } else if (!ui4::event_ticket_art(l, cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y1, ev_id, ev_style, art_tint)) {
          l.rect(cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y1, ui::rgba(44, 48, 56, 230));
        }

        // 3. Top distressed ticket header (KITPARTS/TICKETDIRTTOPSR or TICKETDIRTTOP)
        const uint32_t hdr_col = e.is_collect_wristband ? ui4::kYellow
                                 : act                  ? kTicketPink
                                                        : ui4::kPillGrey;
        if (!e.is_collect_wristband && (ev_style == 2 || is_hub) && has_top_sr) {
          l.image(cx0, cy0, cx1, cy0 + 38.0f * s, top_sr_uv, hdr_col);
        } else if (has_top) {
          l.image(cx0, cy0, cx1, cy0 + 38.0f * s, top_uv, hdr_col);
        } else {
          l.rect(cx0, cy0, cx1, cy0 + 34.0f * s, hdr_col);
        }
        const std::string kind_lbl = e.is_collect_wristband ? "WRISTBAND CEREMONY"
                                     : e.event              ? std::string(race::CareerCatalog::event_style_label(*e.event))
                                                            : "HORIZON EVENT";
        l.text(cx0 + 12.0f * s, cy0 + 10.0f * s, kind_lbl,
               ui4::style(ui::Font::kE, 12.5f * s, e.is_collect_wristband ? ui4::kInk : ui4::kWhite));

        // 4. PICLASS/CLASS_* badge overlaid bottom-left of ticket art
        if (e.event && e.event->target_class > 0 && size_t(e.event->target_class) < bands.size()) {
          const auto& cb = bands[size_t(e.event->target_class)];
          ui4::class_badge(l, cx0 + 10.0f * s, art_y1 - 30.0f * s, 22.0f * s, cb.name,
                           int(std::lround(cb.shown_max)), e.event->target_class);
        }

        // 5. Authentic TICKETSTICKERNEW / GOLD / SILVER / BRONZE corner sticker!
        const float st_sz = 58.0f * s;
        const float sx0 = cx1 - st_sz - 4.0f * s, sy0 = cy0 + 26.0f * s;
        if (e.best_place == 1 && has_gold) {
          l.image(sx0, sy0, sx0 + st_sz, sy0 + st_sz, gold_uv);
        } else if (e.best_place == 2 && has_sil) {
          l.image(sx0, sy0, sx0 + st_sz, sy0 + st_sz, sil_uv);
        } else if (e.best_place == 3 && has_brz) {
          l.image(sx0, sy0, sx0 + st_sz, sy0 + st_sz, brz_uv);
        } else if (e.unlocked && e.best_place == 0 && has_new) {
          l.image(sx0, sy0, sx0 + st_sz, sy0 + st_sz, new_uv, kTicketPink);
          l.text(sx0 + 0.5f * st_sz, sy0 + 0.5f * st_sz - 7.0f * s, "NEW",
                 ui4::style(ui::Font::kE, 13.5f * s, ui4::kWhite, ui::Align::kCenter));
        }

        // 6. LARGETAPE title banner (#EF2493 active / #212121 inactive) + event name
        const float tape_y0 = art_y1 + 4.0f * s, tape_y1 = art_y1 + 52.0f * s;
        if (act) {
          ui4::brush(l, cx0 - 6.0f * s, tape_y0 + 2.0f * s, cx1 + 6.0f * s, tape_y1 - 2.0f * s, kTicketPink);
        } else if (has_tape) {
          l.image(cx0 - 4.0f * s, tape_y0, cx1 + 4.0f * s, tape_y1, tape_uv, ui4::kPillGrey);
        } else {
          l.rect(cx0, tape_y0, cx1, tape_y1, ui4::kPillGrey);
        }
        ui::TextStyle ts = ui4::style(ui::Font::kE, 17.0f * s, !e.unlocked ? ui4::kDim : ui4::kWhite);
        ui4::fit_text(e.display_name, ts, cw - 24.0f * s, 10.5f * s);
        l.text(cx0 + 12.0f * s, tape_y0 + 0.5f * ((tape_y1 - tape_y0) - ts.size), e.display_name, ts);

        // 7. Bottom ticket stub: Cash Prize (E@22px) + status pill
        const std::string prize_str =
            e.is_collect_wristband ? "COLLECT WRISTBAND"
            : e.event              ? ui4::format_credits(int64_t(e.event->cash_prize))
                                   : "";
        l.text(cx0 + 12.0f * s, tape_y1 + 10.0f * s, e.is_collect_wristband ? "WRISTBAND READY" : "1ST PRIZE",
               ui4::style(ui::Font::kB, 10.0f * s, ui4::kDim));
        ui::TextStyle pzs = ui4::style(ui::Font::kE, 21.0f * s, act ? ui4::kYellow : ui4::kWhite);
        ui4::fit_text(prize_str, pzs, cw - 24.0f * s, 12.0f * s);
        l.text(cx0 + 12.0f * s, tape_y1 + 24.0f * s, prize_str, pzs);

        std::string status = "READY TO RACE";
        uint32_t st_col = ui4::kGreen;
        if (e.is_collect_wristband) status = "CEREMONY READY", st_col = ui4::kYellow;
        else if (!e.unlocked) status = "LOCKED", st_col = ui4::kRed;
        else if (e.best_place == 1) status = "1ST PLACE WON", st_col = ui4::kYellow;
        else if (e.best_place > 0)
          status = "BEST: " + std::to_string(e.best_place) + place_suffix(e.best_place), st_col = ui4::kAmber;
        else if (!e.eligible) status = "WRONG CAR CLASS", st_col = ui4::kRed;
        ui4::pill(l, cx0 + 10.0f * s, cy1 - 30.0f * s, cx1 - 10.0f * s, cy1 - 8.0f * s,
                  act ? ui4::kBrushBlack : ui::rgba(14, 16, 20, 200), ui4::PillKind::kPill02);
        l.text(0.5f * (cx0 + cx1), cy1 - 24.5f * s, status,
               ui4::style(ui::Font::kE, 11.5f * s, st_col, ui::Align::kCenter));

        if (act) {
          l.frame(cx0 - 2.0f * s, cy0 - 2.0f * s, cx1 + 2.0f * s, cy1 + 2.0f * s, kTicketPink, 2.8f * s);
        }
      }

      // Compact bottom tilted cursor counter + selected restriction strip (ZERO giant side wall!)
      std::string d_title, dummy_t;
      std::vector<std::string> dummy_lines, d_lines;
      int dummy_sel = 0;
      page.fill(dummy_t, dummy_lines, dummy_sel, d_title, d_lines);
      const float info_y = card_y0 + ch + 14.0f * s;
      char cbuf[32];
      std::snprintf(cbuf, sizeof cbuf, "%d / %d", sel + 1, n);
      ui4::pill(l, 68.0f * s, info_y, 180.0f * s, info_y + 28.0f * s, ui4::kBrushBlack, ui4::PillKind::kMenu);
      l.text(124.0f * s, info_y + 7.0f * s, cbuf, ui4::style(ui::Font::kD, 14.0f * s, ui4::kWhite, ui::Align::kCenter));
      if (!d_lines.empty()) {
        std::string summary = d_lines[0];
        if (d_lines.size() > 1) summary += "   •   " + d_lines[1];
        ui4::pill(l, 192.0f * s, info_y, screen_w - 68.0f * s, info_y + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kPillBig);
        ui::TextStyle ss = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kLeft, true);
        ui4::fit_text(summary, ss, screen_w - 290.0f * s, 9.5f * s);
        l.text(206.0f * s, info_y + 7.0f * s, summary, ss);
      }
    }
  }

  std::vector<ui4::Prompt> bar = {{"A", loc_str("Activities", "IDS_EnterNow", "START EVENT")},
                                  {"DPAD_LR", "BROWSE TICKETS"}};
  if (!is_hub) bar.push_back({"LB", "FILTER TAB"});
  bar.push_back({"B", loc_str("HelpButtons", "IDS_Back", "BACK")});
  ui4::button_bar(l, bar, screen_w, screen_h, 56.0f);
}

void draw_outpost_hub_ui4(ui::List& l, float screen_w, float screen_h, std::string_view outpost_name,
                          const std::vector<OutpostMissionCard>& cards, int selected_index) {
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.22f);

  float tilt_rad = ui4::kTiltRad;
  if (const fh1::UiScene* sc_ptr = ui_scene("H_MISSIONS_HUB")) {
    SceneDraw sc(sc_ptr, true);
    if (const int rn = sc.find("Rotation"); rn >= 0) {
      const float a = -sc.angle(rn);
      if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) tilt_rad = a;
    }
  }

  const std::string hdr = outpost_name.empty() ? std::string("HORIZON OUTPOST") : std::string(outpost_name);
  {
    ui4::TiltScope top_rot(l, 0.5f * screen_w, 96.0f * s, tilt_rad);
    ui4::title_tape(l, s, 82.0f * s, 44.0f * s, hdr + "  //  PR STUNTS", ui4::kPink, 36.0f, 680.0f);
  }

  const uint32_t kTicketPink = ui::rgba(0xEF, 0x24, 0x93, 255);
  std::array<float, 4> drop_uv{}, top_uv{}, tape_uv{}, done_uv{}, new_uv{};
  const bool has_drop = ui_texture("KITPARTS/DROPSHADOW", drop_uv);
  const bool has_top = ui_texture("KITPARTS/TICKETDIRTTOP", top_uv) || ui_texture("KITPARTS/TICKETDIRTTOPSR", top_uv);
  const bool has_tape = ui_texture("KITPARTS/LARGETAPE", tape_uv);
  const bool has_done = ui_texture("ICONS/MISSIONCOMPLETE", done_uv);
  const bool has_new = ui_texture("ICONS/TICKETSTICKERNEW", new_uv);

  {
    ui4::TiltScope rot(l, 0.5f * screen_w, 0.56f * screen_h, tilt_rad);
    const int n = int(cards.size());
    if (n > 0) {
      const int sel = std::clamp(selected_index, 0, n - 1);
      const int count = std::min(n, 4);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float pitch = 288.0f * s;
      const float cw = 268.0f * s;
      const float ch = 358.0f * s;
      const float total_w = float(count - 1) * pitch + cw;
      const float start_x = std::max(56.0f * s, 0.5f * (screen_w - total_w));
      const float card_y0 = 152.0f * s;

      for (int i = first; i < first + count; ++i) {
        const auto& c = cards[size_t(i)];
        const bool act = (i == sel);
        const float lift = 0.0f;
        const float cx0 = start_x + float(i - first) * pitch;
        const float cy0 = card_y0 - lift;
        const float cx1 = cx0 + cw;
        const float cy1 = cy0 + ch;
        l.add_hit(cx0, cy0, cx1, cy1, ui::kHitRow, i);

        if (has_drop) {
          l.image(cx0 - 8.0f * s, cy0 + 8.0f * s, cx1 + 14.0f * s, cy1 + 16.0f * s, drop_uv,
                  ui::rgba(0, 0, 0, act ? 195 : 135));
        } else {
          l.rect(cx0 + 6.0f * s, cy0 + 8.0f * s, cx1 + 6.0f * s, cy1 + 8.0f * s, ui::rgba(0, 0, 0, 130));
        }

        l.rect(cx0, cy0, cx1, cy1, ui::rgba(22, 24, 28, 244));
        const float art_y0 = cy0 + 34.0f * s, art_y1 = cy0 + 216.0f * s;
        if (!c.is_stunt) {
          // Dedicated Outpost service card backdrop (Change Car / Fast Travel / Outpost Service — never a race flyer).
          l.rect(cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y1, ui::rgba(24, 28, 36, 250));
          l.rect(cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y0 + 34.0f * s, ui::rgba(239, 36, 147, 36));
          l.rect(cx0 + 4.0f * s, art_y1 - 32.0f * s, cx1 - 4.0f * s, art_y1, ui::rgba(218, 255, 0, 26));
          std::string tup = c.title;
          for (char& ch : tup) ch = char(std::toupper(static_cast<unsigned char>(ch)));
          std::array<float, 4> svc_uv{};
          const bool is_garage = tup.find("CAR") != std::string::npos || tup.find("GARAGE") != std::string::npos;
          const bool has_svc =
              is_garage ? (ui_texture("ICONS/CAROUSELICONS/GARAGELARGE", svc_uv) ||
                           ui_texture("ICONS/CAROUSELICONS/GARAGE", svc_uv) ||
                           ui_texture("ICONS/ACTIVATIONZONEICONS/GARAGE", svc_uv))
                        : (ui_texture("ICONS/ACTIVATIONZONEICONS/AUTOSHOW", svc_uv) ||
                           ui_texture("WRISTBANDS/HEADLINEEVENT", svc_uv) ||
                           ui_texture("ICONS/CAROUSELICONS/GARAGELARGE", svc_uv));
          if (has_svc) {
            const float isz = 112.0f * s;
            const float ix0 = 0.5f * (cx0 + cx1) - 0.5f * isz;
            const float iy0 = 0.5f * (art_y0 + art_y1) - 0.5f * isz - 4.0f * s;
            l.image(ix0, iy0, ix0 + isz, iy0 + isz, svc_uv, act ? ui4::kWhite : ui::rgba(220, 224, 232, 232));
          }
          l.text(0.5f * (cx0 + cx1), art_y1 - 22.0f * s, is_garage ? "OUTPOST GARAGE SERVICE" : "OUTPOST SERVICE",
                 ui4::style(ui::Font::kB, 10.5f * s, ui4::kOffWhite, ui::Align::kCenter));
        } else if (!ui4::event_ticket_art(l, cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y1, c.event_id, c.art_style,
                                          act ? ui4::kWhite : ui::rgba(224, 228, 235, 232))) {
          l.rect(cx0 + 4.0f * s, art_y0, cx1 - 4.0f * s, art_y1, ui::rgba(42, 46, 54, 232));
        }

        const uint32_t hdr_col = act ? kTicketPink : (c.completed ? ui4::kGreen : ui4::kPillGrey);
        if (has_top) l.image(cx0, cy0, cx1, cy0 + 38.0f * s, top_uv, hdr_col);
        else l.rect(cx0, cy0, cx1, cy0 + 34.0f * s, hdr_col);
        l.text(cx0 + 12.0f * s, cy0 + 10.0f * s, c.kind_label,
               ui4::style(ui::Font::kE, 12.5f * s, (!act && c.completed) ? ui4::kInk : ui4::kWhite));

        // Authentic ICONS/MISSIONCOMPLETE or TICKETSTICKERNEW stamp!
        const float st_sz = 62.0f * s;
        const float sx0 = cx1 - st_sz - 6.0f * s, sy0 = cy0 + 28.0f * s;
        if (c.completed && has_done) {
          l.image(sx0, sy0, sx0 + st_sz, sy0 + st_sz, done_uv);
        } else if (c.is_stunt && !c.completed && has_new) {
          l.image(sx0, sy0, sx0 + st_sz, sy0 + st_sz, new_uv, kTicketPink);
          l.text(sx0 + 0.5f * st_sz, sy0 + 0.5f * st_sz - 7.0f * s, "NEW",
                 ui4::style(ui::Font::kE, 13.5f * s, ui4::kWhite, ui::Align::kCenter));
        }

        const float tape_y0 = art_y1 + 4.0f * s, tape_y1 = art_y1 + 52.0f * s;
        if (act) {
          ui4::brush(l, cx0 - 6.0f * s, tape_y0 + 2.0f * s, cx1 + 6.0f * s, tape_y1 - 2.0f * s, kTicketPink);
        } else if (has_tape) {
          l.image(cx0 - 4.0f * s, tape_y0, cx1 + 4.0f * s, tape_y1, tape_uv, ui4::kPillGrey);
        } else {
          l.rect(cx0, tape_y0, cx1, tape_y1, ui4::kPillGrey);
        }
        ui::TextStyle ts = ui4::style(ui::Font::kE, 16.5f * s, ui4::kWhite);
        ui4::fit_text(c.title, ts, cw - 24.0f * s, 10.0f * s);
        l.text(cx0 + 12.0f * s, tape_y0 + 0.5f * ((tape_y1 - tape_y0) - ts.size), c.title, ts);

        if (!c.target_line.empty()) {
          ui::TextStyle tgs = ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite, ui::Align::kLeft, true);
          ui4::fit_text(c.target_line, tgs, cw - 24.0f * s, 9.0f * s);
          l.text(cx0 + 12.0f * s, tape_y1 + 10.0f * s, c.target_line, tgs);
        }
        if (!c.reward_line.empty()) {
          ui::TextStyle rws = ui4::style(ui::Font::kE, 14.0f * s, act ? ui4::kYellow : ui4::kOffWhite);
          ui4::fit_text(c.reward_line, rws, cw - 24.0f * s, 9.5f * s);
          l.text(cx0 + 12.0f * s, tape_y1 + 32.0f * s, c.reward_line, rws);
        }

        ui4::pill(l, cx0 + 10.0f * s, cy1 - 30.0f * s, cx1 - 10.0f * s, cy1 - 8.0f * s,
                  act ? ui4::kBrushBlack : ui::rgba(14, 16, 20, 200), ui4::PillKind::kPill02);
        l.text(0.5f * (cx0 + cx1), cy1 - 24.5f * s,
               c.status_line.empty() ? (c.completed ? "COMPLETED" : "START STUNT") : c.status_line,
               ui4::style(ui::Font::kE, 11.5f * s, c.completed ? ui4::kGreen : ui4::kYellow, ui::Align::kCenter));

        if (act) {
          l.frame(cx0 - 2.0f * s, cy0 - 2.0f * s, cx1 + 2.0f * s, cy1 + 2.0f * s, kTicketPink, 2.8f * s);
        }
      }

      // Compact bottom instruction strip for focused card
      const auto& sc = cards[size_t(sel)];
      const float info_y = card_y0 + ch + 14.0f * s;
      char cbuf[32];
      std::snprintf(cbuf, sizeof cbuf, "%d / %d", sel + 1, n);
      ui4::pill(l, 68.0f * s, info_y, 180.0f * s, info_y + 28.0f * s, ui4::kBrushBlack, ui4::PillKind::kMenu);
      l.text(124.0f * s, info_y + 7.0f * s, cbuf, ui4::style(ui::Font::kD, 14.0f * s, ui4::kWhite, ui::Align::kCenter));
      if (!sc.detail_lines.empty()) {
        std::string line = sc.detail_lines[0];
        if (sc.detail_lines.size() > 1) line += "   •   " + sc.detail_lines[1];
        ui4::pill(l, 192.0f * s, info_y, screen_w - 68.0f * s, info_y + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kPillBig);
        ui::TextStyle ss = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kLeft, true);
        ui4::fit_text(line, ss, screen_w - 290.0f * s, 9.5f * s);
        l.text(206.0f * s, info_y + 7.0f * s, line, ss);
      }
    }
  }

  ui4::button_bar(l,
                  {{"A", loc_str("HelpButtons", "IDS_Select", "SELECT")},
                   {"DPAD_LR", "BROWSE"},
                   {"B", loc_str("HelpButtons", "IDS_Back", "BACK")}},
                  screen_w, screen_h, 56.0f);
}

void draw_settings_ui4(ui::List& l, float screen_w, float screen_h, const SettingsPage& page, bool live_overlay) {
  if (!page.is_open()) return;
  const float s = screen_h / 720.0f;
  // Subtle edge vignette only — NO giant enclosing charcoal wall over Colorado!
  ui4::subtle_vignette(l, screen_w, screen_h, 0.24f);

  const int cur_tab = int(page.active_tab());
  const char* scene_name = (cur_tab == 1)   ? "837_I_AUDIO_OPTIONS"
                           : (cur_tab == 2) ? "300_I_OPT_ASSISTS_DIFFICULTY"
                           : (cur_tab == 3) ? "831_C_OPT_CONTROLLER"
                                            : "H_FIXED_SETTING_LIST";
  float tilt_rad = ui4::kTiltRad;
  if (const fh1::UiScene* sc_ptr = ui_scene(scene_name)) {
    SceneDraw sc(sc_ptr, true);
    int rn = sc.find("RotationHelper");
    if (rn < 0) rn = sc.find("Group_BLADE");
    if (rn < 0) rn = sc.find("Group_BODY");
    if (rn >= 0) {
      const float a = -sc.angle(rn);
      if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) tilt_rad = a;
    }
  }

  static const std::vector<std::string> kTabs = {"VIDEO", "AUDIO", "HUD & GAMEPLAY", "CONTROLS", "ENGINE"};
  const auto all_rows = page.display_rows();
  std::vector<SettingsPage::DisplayRow> tab_rows;
  int sel_in_tab = 0;
  for (const auto& r : all_rows) {
    if (int(r.tab) == cur_tab || r.is_save) {
      if (r.selected) sel_in_tab = int(tab_rows.size());
      tab_rows.push_back(r);
    }
  }

  // ---- Entire Settings blade tilted +3.0 deg over open Colorado (H_FIXED_SETTING_LIST / 300_I_OPT_*) ----
  {
    ui4::TiltScope rot(l, 0.5f * screen_w, 0.5f * screen_h, tilt_rad);

    // 1. TitleTape (TAPECLIPON02 + SMUDGETAPE + CRISPTAPE + DIRTTILE01) + LB/RB category pills
    const std::string opt_title = loc_str("PauseMenu", "IDS_Options", "OPTIONS");
    ui4::title_tape(l, s, 82.0f * s, 40.0f * s,
                    live_overlay ? "LIVE SETTINGS" : (opt_title + "  //  " + kTabs[size_t(std::clamp(cur_tab, 0, 4))]),
                    ui4::kPink, 36.0f, 620.0f);
    ui4::tab_strip(l, s, 82.0f * s, 106.0f * s, screen_w - 82.0f * s, kTabs, cur_tab, true, ui4::kPink);

    // 2. Floating PILL02 / PILL03 Option Rows over open Colorado (NO giant enclosing charcoal box!)
    const float lx0 = 82.0f * s, lw = 730.0f * s;
    const float list_y0 = 152.0f * s;
    const float list_y1 = screen_h - 102.0f * s;

    // Header PILL01 (#BFBFBF) + PILLCLIPON01
    ui4::pill(l, lx0, list_y0, lx0 + lw, list_y0 + 28.0f * s, ui::rgba(0xBF, 0xBF, 0xBF, 242), ui4::PillKind::kMenu,
              false, true);
    l.text(lx0 + 16.0f * s, list_y0 + 7.0f * s, kTabs[size_t(std::clamp(cur_tab, 0, 4))] + " OPTIONS",
           ui4::style(ui::Font::kD, 13.5f * s, ui4::kInk));
    std::string header_right = page.capturing_key() ? "PRESS ANY KEY TO BIND (ESC CANCELS)" : "CHANGES APPLY LIVE";
    uint32_t header_right_col = page.capturing_key() ? ui4::kPink : ui4::kInk;
    if (!page.capturing_key() && cur_tab == 2) {
      header_right = "DIFFICULTY BONUS: +" + std::to_string(page.difficulty_credit_bonus_pct()) + "% CR";
      header_right_col = ui4::kPink;
    }
    l.text(lx0 + lw - 16.0f * s, list_y0 + 7.5f * s, header_right,
           ui4::style(ui::Font::kB, 12.0f * s, header_right_col, ui::Align::kRight));

    const float row_pitch = 38.0f * s;
    const float row_h = 33.0f * s;
    const float rows_top = list_y0 + 34.0f * s;
    const int max_vis = std::max(1, int((list_y1 - rows_top) / row_pitch));
    const int n = int(tab_rows.size());
    const int first = std::clamp(sel_in_tab - max_vis / 2, 0, std::max(0, n - max_vis));

    auto parse_slider_norm = [](const std::string& val, float& out_norm) -> bool {
      if (val.empty()) return false;
      // Match "75%", "80 %", "1.0x" or "0..100" percentage strings for STATBARFILL sliders!
      const size_t pct_pos = val.find('%');
      if (pct_pos != std::string::npos) {
        try {
          const float v = std::stof(val.substr(0, pct_pos));
          if (v >= 0.0f && v <= 200.0f) {
            out_norm = std::clamp(v / (v > 100.0f ? 200.0f : 100.0f), 0.0f, 1.0f);
            return true;
          }
        } catch (...) {
        }
      }
      return false;
    };

    float ry = rows_top;
    for (int i = first; i < std::min(n, first + max_vis); ++i) {
      const auto& r = tab_rows[size_t(i)];
      const bool act = r.selected;
      const uint32_t row_col = act ? (r.is_save ? ui4::kGreen : ui4::kPink)
                                   : ((i % 2 == 0) ? ui4::kPillGrey : ui::rgba(0x18, 0x18, 0x1C, 236));
      l.add_hit(lx0, ry, lx0 + lw, ry + row_h, ui::kHitRow, r.flat_index);
      ui4::pill(l, lx0, ry, lx0 + lw, ry + row_h, row_col,
                (i % 2 == 0) ? ui4::PillKind::kPill02 : ui4::PillKind::kPill03, false, act);

      const uint32_t fg = (act && r.is_save) ? ui4::kInk : ui4::kWhite;
      // Fixed font family & size across focus changes so rows never jump or resize.
      ui::TextStyle ls = ui4::style(ui::Font::kD, 14.0f * s, fg);
      ui4::fit_text(r.name, ls, lw - 310.0f * s, 9.5f * s);
      l.text(lx0 + 18.0f * s, ry + 0.5f * (row_h - ls.size), r.name, ls);

      const float vx0 = lx0 + lw - 278.0f * s;
      const float vx1 = lx0 + lw - 18.0f * s;
      const float vcy = ry + 0.5f * row_h;

      float sl_norm = 0.0f;
      if (r.is_save) {
        l.text(vx1 - 8.0f * s, ry + 9.0f * s, r.value,
               ui4::style(ui::Font::kE, 14.0f * s, act ? ui4::kInk : ui4::kGreen, ui::Align::kRight));
      } else if (r.is_rebind) {
        ui4::pill(l, vx0 + 14.0f * s, ry + 4.0f * s, vx1, ry + row_h - 4.0f * s,
                  r.capturing ? ui4::kYellow : ui4::kBrushBlack, ui4::PillKind::kMenu);
        l.text(0.5f * (vx0 + 14.0f * s + vx1), ry + 9.0f * s, r.value,
               ui4::style(ui::Font::kE, 13.5f * s, r.capturing ? ui4::kInk : (act ? ui4::kYellow : ui4::kWhite),
                          ui::Align::kCenter));
      } else if (parse_slider_norm(r.value, sl_norm)) {
        // Real DIRTTEXTURES/STATBARFILL + OPT_SLIDER_FRAME slider + value text (837_I_AUDIO_OPTIONS style)
        ui4::opt_slider(l, vx0 + 4.0f * s, vcy - 5.5f * s, vx1 - 66.0f * s, vcy + 5.5f * s, sl_norm, s,
                        act ? ui4::kYellow : ui4::kPink, act);
        l.text(vx1 - 4.0f * s, ry + 9.0f * s, r.value,
               ui4::style(ui::Font::kE, 13.5f * s, act ? ui4::kWhite : ui4::kOffWhite, ui::Align::kRight));
      } else {
        // Real OPTIONS/OPT_BAR_ARROW.xds arrows flanking centered value — ZERO ASCII "< >"!
        const uint32_t arr_col = act ? ui4::kWhite : ui::rgba(190, 196, 208, 175);
        ui4::opt_arrow(l, vx0 + 10.0f * s, vcy, 15.0f * s, -1, arr_col);
        ui4::opt_arrow(l, vx1 - 10.0f * s, vcy, 15.0f * s, +1, arr_col);
        l.add_hit(vx0 - 4.0f * s, ry, vx0 + 32.0f * s, ry + row_h, ui::kHitStepLeft, r.flat_index);
        l.add_hit(vx1 - 32.0f * s, ry, vx1 + 4.0f * s, ry + row_h, ui::kHitStepRight, r.flat_index);
        ui::TextStyle vs = ui4::style(ui::Font::kB, 14.0f * s,
                                      act ? ui4::kYellow : ui4::kWhite, ui::Align::kCenter);
        ui4::fit_text(r.value, vs, (vx1 - vx0) - 44.0f * s, 9.0f * s);
        l.text(0.5f * (vx0 + vx1), ry + 0.5f * (row_h - vs.size), r.value, vs);
      }
      ry += row_pitch;
    }

    // Authentic vertical COMMON/SCROLLINDICATOR + SPECULAR/SPEC_SHEEN2
    ui4::scroll_indicator(l, lx0 + lw + 8.0f * s, rows_top, lx0 + lw + 16.0f * s, ry - 4.0f * s, first, max_vis, n, s);

    // 3. Compact right-hand DIRTMASKS/PANEL01 Description Card (sized tightly to content — ZERO empty void!)
    const float rx0 = lx0 + lw + 28.0f * s, rx1 = screen_w - 72.0f * s;
    if (sel_in_tab >= 0 && sel_in_tab < int(tab_rows.size())) {
      const auto& sr = tab_rows[size_t(sel_in_tab)];
      const float card_h = 248.0f * s;
      ui4::panel01(l, rx0, list_y0, rx1, list_y0 + card_h, s, ui4::kCharcoalDark, ui4::kPink);

      ui4::brush(l, rx0 - 4.0f * s, list_y0 + 12.0f * s, rx1 + 4.0f * s, list_y0 + 44.0f * s, ui4::kPink);
      ui::TextStyle stt = ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite);
      ui4::fit_text(sr.name, stt, (rx1 - rx0) - 28.0f * s, 10.5f * s);
      l.text(rx0 + 16.0f * s, list_y0 + 19.0f * s, sr.name, stt);

      l.text(rx0 + 16.0f * s, list_y0 + 56.0f * s, "CURRENT VALUE", ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
      l.text(rx0 + 16.0f * s, list_y0 + 72.0f * s, sr.value, ui4::style(ui::Font::kE, 21.0f * s, ui4::kYellow));

      std::string nm_up = sr.name;
      for (char& ch : nm_up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
      const char* hint = nullptr;
      if (sr.is_save) hint = "Writes changes to settings.txt and closes Settings.";
      else if (sr.is_rebind) hint = "Press [A] / Enter then press any key to rebind.";
      else if (nm_up.find("PRESET") != std::string::npos || nm_up.find("QUALITY") != std::string::npos)
        hint = "Selects graphics profile (resolution, shadows, MSAA & post-FX).";
      else if (nm_up.find("RESOLUTION") != std::string::npos || nm_up.find("SCALE") != std::string::npos)
        hint = "Internal 3D render scale before upscaling to display resolution.";
      else if (nm_up.find("VSYNC") != std::string::npos || nm_up.find("FRAME") != std::string::npos)
        hint = "Synchronizes presentation with display refresh rate.";
      else if (nm_up.find("MSAA") != std::string::npos || nm_up.find("ANTI") != std::string::npos)
        hint = "Smooths jagged geometry edges on cars and Colorado terrain.";
      else if (nm_up.find("SHADOW") != std::string::npos)
        hint = "Controls cascaded sun shadow resolution and draw distance.";
      else if (nm_up.find("SSAO") != std::string::npos || nm_up.find("BLOOM") != std::string::npos ||
               nm_up.find("HDR") != std::string::npos)
        hint = "Post-processing lighting, ambient occlusion & dynamic range.";
      else if (nm_up.find("CAMERA") != std::string::npos || nm_up.find("FOV") != std::string::npos ||
               nm_up.find("HEAD") != std::string::npos)
        hint = "Adjusts driving camera rig, FOV & head-tracking response.";
      else if (nm_up.find("DAY") != std::string::npos || nm_up.find("TIME") != std::string::npos)
        hint = "Sets Colorado day/night cycle length or fixed time of day.";
      else if (nm_up.find("VOLUME") != std::string::npos || nm_up.find("AUDIO") != std::string::npos ||
               nm_up.find("RADIO") != std::string::npos || nm_up.find("DJ") != std::string::npos)
        hint = "Adjusts FMOD 8-bus audio mix, Horizon radio & DJ chatter.";
      else if (nm_up.find("ABS") != std::string::npos || nm_up.find("TCS") != std::string::npos ||
               nm_up.find("STAB") != std::string::npos || nm_up.find("STEER") != std::string::npos)
        hint = "Driving assist setting (adjusts difficulty credit bonus).";
      else if (nm_up.find("SHIFT") != std::string::npos || nm_up.find("CLUTCH") != std::string::npos)
        hint = "Automatic, Manual, or Manual with Clutch transmission.";
      else if (nm_up.find("REWIND") != std::string::npos)
        hint = "Enables or disables instant 60-second driving rewind [Y/R].";
      else if (nm_up.find("DIFFICULTY") != std::string::npos || nm_up.find("AI") != std::string::npos)
        hint = "Adjusts AI rival pace and festival race credit bonus.";
      else if (nm_up.find("FFB") != std::string::npos || nm_up.find("WHEEL") != std::string::npos ||
               nm_up.find("GYRO") != std::string::npos || nm_up.find("RUMBLE") != std::string::npos)
        hint = "Tunes force feedback, wheel angle, rumble & DualSense gyro.";
      else if (nm_up.find("DEADZONE") != std::string::npos || nm_up.find("SENSITIV") != std::string::npos)
        hint = "Shapes analog stick, wheel and pedal deadzones & linearity.";
      else if (nm_up.find("UNITS") != std::string::npos)
        hint = "Switches Speedometer, Speed Traps/Zones, PR Stunts & GPS between km/h and MPH.";
      else if (nm_up.find("LANGUAGE") != std::string::npos)
        hint = "Switches UI text & localization pack (17 FH1 languages).";
      else
        hint = cur_tab == 0   ? "Display, camera rig, day length & post-FX."
               : cur_tab == 1 ? "FMOD master volume, radio DJ & 8-bus mixer."
               : cur_tab == 2 ? "Assists, AI difficulty, rubber-band & damage."
               : cur_tab == 3 ? "Controller layout, rumble, deadzones & keys."
                              : "Pinyon Shift QoL: language, intros & photo.";
      ui::TextStyle hs = ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite, ui::Align::kLeft, true);
      ui4::fit_text(hint, hs, (rx1 - rx0) - 32.0f * s, 9.0f * s);
      l.text(rx0 + 16.0f * s, list_y0 + 108.0f * s, hint, hs);

      const LiveSettings ls = page.live_settings();
      ui4::pill(l, rx0 + 12.0f * s, list_y0 + 144.0f * s, rx1 - 12.0f * s, list_y0 + 168.0f * s, ui4::kPillGrey,
                ui4::PillKind::kPill02);
      l.text(rx0 + 22.0f * s, list_y0 + 150.0f * s, "LANGUAGE: " + ls.language,
             ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite));
      ui4::pill(l, rx0 + 12.0f * s, list_y0 + 174.0f * s, rx1 - 12.0f * s, list_y0 + 198.0f * s, ui4::kPillGrey,
                ui4::PillKind::kPill02);
      l.text(rx0 + 22.0f * s, list_y0 + 180.0f * s,
             std::string("AUDIO: ") + (ls.sound ? "ON" : "OFF") + "  •  RADIO: " + (ls.radio ? "ON" : "OFF"),
             ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite));
      ui4::pill(l, rx0 + 12.0f * s, list_y0 + 204.0f * s, rx1 - 12.0f * s, list_y0 + 228.0f * s, ui4::kPillGrey,
                ui4::PillKind::kPill02);
      l.text(rx0 + 22.0f * s, list_y0 + 210.0f * s,
             std::string("REWIND: ") + (ls.controls.assists.rewind ? "ON" : "OFF") + "  •  TCS: " +
                 (ls.controls.assists.traction_control ? "ON" : "OFF"),
             ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    }
  }

  const std::string a_prompt = [&]() -> std::string {
    if (sel_in_tab >= 0 && sel_in_tab < int(tab_rows.size())) {
      const auto& sr = tab_rows[size_t(sel_in_tab)];
      if (sr.is_save) return loc_str("HelpButtons", "IDS_Save", "SAVE & APPLY");
      if (sr.is_rebind) return "REBIND KEY";
    }
    return loc_str("HelpButtons", "IDS_Select", "TOGGLE / CHANGE");
  }();
  ui4::button_bar(l,
                  {{"DPAD_LR", "CHANGE"},
                   {"LB", "CATEGORY"},
                   {"A", a_prompt},
                   {"X", "MODS"},
                   {"Y", "SAVES / SYNC"},
                   {"B", loc_str("HelpButtons", "IDS_Back", "BACK")}},
                  screen_w, screen_h, 56.0f);
}

// ------------------------------------------------ Animated Loading Screen (2_C_INI_LOADING)

namespace {
std::string resolve_loading_key(std::string_view key, bool allow_fallback_prettify = true) {
  if (key.empty()) return {};
  // Format in LoadingDefs.xml: "Tips:IDS_PopularityTitle"
  const size_t colon = key.find(':');
  const std::string_view tbl = colon != std::string_view::npos ? key.substr(0, colon) : std::string_view("Tips");
  const std::string_view id = colon != std::string_view::npos ? key.substr(colon + 1) : key;
  // 1. Authentic StringTables resolution via `_&((hash(table) << 16) | hash(id))`.
  const uint32_t packed = (uint32_t(fh1::string_hash(tbl)) << 16) | uint32_t(fh1::string_hash(id));
  const std::string ref = "_&" + std::to_string(packed);
  const std::string by_hash = game_text(ref);
  if (!by_hash.empty() && by_hash != ref && by_hash.rfind("_&", 0) != 0) return by_hash;
  // 2. Direct probe fallback for synthetic/test resolvers keyed by "Tips/IDS_...".
  const std::string probe = std::string(tbl) + "/" + std::string(id);
  const std::string resolved = game_text(probe);
  if (!resolved.empty() && resolved != probe) return resolved;
  // 3. If the XML already carries localized text rather than a raw key, keep it verbatim.
  if (!looks_like_raw_key(key)) return std::string(key);
  if (!allow_fallback_prettify) return {};
  // Clean human-readable fallback from "IDS_SpeedTrapTitle" -> "SPEED TRAP"
  std::string s(id);
  if (s.rfind("IDS_", 0) == 0) s = s.substr(4);
  for (const char* suf : {"Title", "Tips", "Tip", "Desc"}) {
    const size_t slen = std::strlen(suf);
    if (s.size() > slen && s.compare(s.size() - slen, slen, suf) == 0) {
      s.resize(s.size() - slen);
      break;
    }
  }
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '_') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      continue;
    }
    if (i > 0 && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(s[i - 1]))) {
      out.push_back(' ');
    }
    out.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

size_t utf8_char_len(std::string_view s, size_t pos) {
  if (pos >= s.size()) return 0;
  const unsigned char b = static_cast<unsigned char>(s[pos]);
  size_t len = (b < 0x80u) ? 1u : ((b >> 5) == 0x6u) ? 2u : ((b >> 4) == 0xEu) ? 3u : ((b >> 3) == 0x1Eu) ? 4u : 1u;
  return std::min(len, s.size() - pos);
}

std::vector<std::string> wrap_tip_lines(std::string_view text, const ui::TextStyle& ts, float max_w, int max_lines = 6) {
  std::vector<std::string> lines;
  std::string cur;
  size_t pos = 0;
  while (pos < text.size() && int(lines.size()) < max_lines) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;
    if (text[pos] == '\n') {
      if (!cur.empty()) lines.push_back(cur), cur.clear();
      ++pos;
      continue;
    }
    size_t end = pos;
    while (end < text.size() && text[end] != ' ' && text[end] != '\n') ++end;
    const std::string word(text.substr(pos, end - pos));
    pos = end;
    const std::string cand = cur.empty() ? word : (cur + " " + word);
    if (!cur.empty() && ui::List::measure(cand, ts) > max_w) {
      lines.push_back(cur);
      cur.clear();
    }
    if (cur.empty() && ui::List::measure(word, ts) > max_w) {
      // Long spaceless token (e.g. Japanese / Traditional Chinese loading tip): wrap by UTF-8 codepoint.
      size_t wp = 0;
      while (wp < word.size() && int(lines.size()) < max_lines) {
        const size_t clen = utf8_char_len(word, wp);
        const std::string ch = word.substr(wp, clen);
        const std::string next = cur + ch;
        if (!cur.empty() && ui::List::measure(next, ts) > max_w) {
          lines.push_back(cur);
          cur = ch;
        } else {
          cur = next;
        }
        wp += clen;
      }
    } else {
      cur = cur.empty() ? word : (cur + " " + word);
    }
  }
  if (!cur.empty() && int(lines.size()) < max_lines) lines.push_back(cur);
  return lines;
}
}  // namespace

void LoadingScreen::start(std::string destination_title, std::string subtitle) {
  active_ = true;
  title_ = std::move(destination_title);
  subtitle_ = std::move(subtitle);
  elapsed_ = 0.0;
  tip_timer_ = 0.0;
  progress_ = 0.0f;
  if (tips_.empty()) {
    tip_titles_ = {"POPULARITY & SKILLS", "DISCOUNT SIGNS", "FESTIVAL RADIO", "DRIVING ASSISTS"};
    tips_ = {
        "Chain together drifts, near misses, and drafting to build your Skill Multiplier.",
        "Smash all 100 Horizon Discount Signs across Colorado for free upgrades at the Horizon Garage.",
        "Listen to Horizon Bass Arena, Pulse, and Rocks for Barn Find rumours from the festival DJs.",
        "Adjust ABS, Traction Control, and Manual + Clutch in Settings to boost your race credit bonus.",
    };
  }
}

bool LoadingScreen::load_definitions(const std::filesystem::path& game_root, const std::string& group) {
  std::ifstream f(game_root / "media" / "ui" / "LoadingDefs.xml", std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  std::vector<fh1::UiLoadingTip> parsed;
  if (!fh1::parse_loading_defs_xml(ss.str(), parsed)) return false;

  std::string want = group;
  for (char& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
  group_ = want.empty() ? "freeroam" : want;

  std::vector<std::string> new_titles;
  std::vector<std::string> new_msgs;
  auto accept_tip = [](const std::string& ttl, const std::string& msg) {
    if (looks_like_raw_key(msg) || msg == ttl) return false;
    bool has_non_ascii = false;
    for (char ch : msg) {
      if (static_cast<unsigned char>(ch) >= 0x80u) {
        has_non_ascii = true;
        break;
      }
    }
    return has_non_ascii ? (msg.size() >= 12) : (msg.size() >= 20);
  };
  for (const auto& t : parsed) {
    if (t.group != group_) continue;
    const std::string ttl = resolve_loading_key(t.title_key, true);
    const std::string msg = resolve_loading_key(t.message_key, false);
    if (accept_tip(ttl, msg)) {
      new_titles.push_back(ttl.empty() ? "FESTIVAL TIP" : ttl);
      new_msgs.push_back(msg);
    }
  }
  if (new_msgs.empty()) {
    for (const auto& t : parsed) {
      const std::string ttl = resolve_loading_key(t.title_key, true);
      const std::string msg = resolve_loading_key(t.message_key, false);
      if (accept_tip(ttl, msg)) {
        new_titles.push_back(ttl.empty() ? "FESTIVAL TIP" : ttl);
        new_msgs.push_back(msg);
        if (new_msgs.size() >= 12) break;
      }
    }
  }
  if (new_msgs.empty()) return false;
  tip_titles_ = std::move(new_titles);
  tips_ = std::move(new_msgs);
  tip_idx_ = 0;
  return true;
}

void LoadingScreen::set_tips(std::vector<std::string> tips) {
  if (!tips.empty()) {
    tips_ = std::move(tips);
    tip_titles_.assign(tips_.size(), "FESTIVAL TIP");
    tip_idx_ = 0;
  }
}

void LoadingScreen::set_progress(float p) { progress_ = std::clamp(p, 0.0f, 1.0f); }

void LoadingScreen::update(double dt) {
  if (!active_) return;
  dt = std::max(0.0, dt);
  elapsed_ += dt;
  tip_timer_ += dt;
  if (tip_timer_ >= 5.0 && !tips_.empty()) {
    tip_timer_ -= 5.0;
    tip_idx_ = (tip_idx_ + 1) % int(tips_.size());
  }
}

const std::string& LoadingScreen::current_tip() const {
  static const std::string kEmpty;
  if (tips_.empty()) return kEmpty;
  return tips_[size_t(tip_idx_ % int(tips_.size()))];
}

const std::string& LoadingScreen::current_tip_title() const {
  static const std::string kDef = "FESTIVAL TIP";
  if (tip_titles_.empty()) return kDef;
  return tip_titles_[size_t(tip_idx_ % int(tip_titles_.size()))];
}

void LoadingScreen::draw(ui::List& l, float screen_w, float screen_h) const {
  if (!active_) return;
  const float s = screen_h / 720.0f;
  const SceneFrame f{0.5f * screen_w - 640.0f * s, 0.5f * screen_h - 360.0f * s, s};
  const fh1::UiScene* sc_ptr = ui_scene("2_C_INI_LOADING");
  SceneDraw sc(sc_ptr, true);

  // 1. Real full-screen LOADING/BGLOADINGIMAGES/ImageN backdrop (ZERO flat purple gradient!).
  std::array<float, 4> bg_uv{};
  const int img_num = 1 + ((std::max(0, bg_idx_ - 1) + tip_idx_) % 10);
  const bool have_bg = ui_texture("LOADING/BGLOADINGIMAGES/IMAGE" + std::to_string(img_num), bg_uv) ||
                       ui_texture("LOADING/BGLOADINGIMAGES/IMAGE1", bg_uv) ||
                       ui_texture("LOADING/BGLOADINGIMAGES/CONTROLLERLOADINGBACKGROUND", bg_uv);
  if (have_bg) {
    l.image(0, 0, screen_w, screen_h, bg_uv, ui::rgba(255, 255, 255, 255));
    // Subtle dark scrim along bottom & left for legible typography over bright daylight photos.
    l.rect(0, 0, screen_w, screen_h, ui::rgba(8, 10, 14, 72), 0.0f, ui::rgba(8, 10, 14, 155));
  } else {
    l.rect(0, 0, screen_w, screen_h, ui4::kCharcoalDark);
    std::array<float, 4> noise{};
    if (ui_texture("DIRTMASKS/BGNOISE01", noise)) {
      l.image(0, 0, screen_w, screen_h, noise, ui::rgba(255, 255, 255, 24));
    }
  }

  // 2. Drive 2_C_INI_LOADING +3.0 deg tilted TitleText, large pink Title, TipLayout PILL_BIG lines, and LOADSPINNER.
  const std::string tip_hdr = current_tip_title();
  const std::string dest_title = title_.empty() ? "HORIZON COLORADO" : title_;
  const std::string tip_body = current_tip();

  // Top-left eyebrow / tip heading on #0E0E0E tape (2_C_INI_LOADING TitleText @(137,154), E@36px).
  {
    ui4::TiltScope tilt(l, f.x(360.0f), f.y(165.0f), ui4::kTiltRad);
    const float hx = f.x(110.0f), hy = f.y(132.0f);
    ui::TextStyle hts = ui4::style(ui::Font::kE, 30.0f * s, ui4::kWhite);
    ui4::fit_text(tip_hdr, hts, 560.0f * s, 16.0f * s);
    const float hw = ui::List::measure(tip_hdr, hts);
    ui4::brush(l, hx - 16.0f * s, hy - 8.0f * s, hx + std::max(260.0f * s, hw + 44.0f * s), hy + hts.size + 12.0f * s,
               ui4::kBrushBlack, true);
    l.text(hx + 10.0f * s, hy + 2.0f * s, tip_hdr, hts);
  }

  // Tilted TipLayout@(190,280, rot=+3 deg): crisp white text on dark HUDPILL01/charcoal strips for high contrast!
  {
    ui4::TiltScope tilt(l, f.x(410.0f), f.y(295.0f), ui4::kTiltRad);
    ui::TextStyle line_ts = ui4::style(ui::Font::kB, 16.0f * s, ui4::kWhite, ui::Align::kLeft, true);
    const auto lines = wrap_tip_lines(tip_body, line_ts, 560.0f * s, 6);
    float ly = f.y(210.0f);
    for (const auto& ln : lines) {
      const float lw = ui::List::measure(ln, line_ts);
      const float px0 = f.x(122.0f), px1 = px0 + lw + 34.0f * s;
      const float py0 = ly - 4.0f * s, py1 = ly + 23.0f * s;
      l.rect(px0, py0, px1, py1, ui::rgba(14, 16, 22, 228));
      l.rect(px0, py0, px0 + 4.0f * s, py1, ui4::kPink);
      l.text(px0 + 16.0f * s, ly + 1.0f * s, ln, line_ts);
      ly += 29.0f * s;
    }
  }

  // Lower-left large pink destination title (2_C_INI_LOADING E@54px @(105,519) on CRISPTAPEBIG01/SMUDGETAPEBIG01).
  {
    ui4::TiltScope tilt(l, f.x(380.0f), f.y(540.0f), ui4::kTiltRad);
    const float tx = f.x(92.0f), ty = f.y(500.0f);
    ui::TextStyle tts = ui4::style(ui::Font::kE, 48.0f * s, ui4::kWhite);
    ui4::fit_text(dest_title, tts, 740.0f * s, 22.0f * s);
    const float tw = ui::List::measure(dest_title, tts);
    ui4::brush(l, tx - 28.0f * s, ty - 12.0f * s, tx + std::max(340.0f * s, tw + 56.0f * s), ty + tts.size + 16.0f * s,
               ui4::kPink, true);
    l.text(tx + 12.0f * s, ty + 2.0f * s, dest_title, tts);

    if (!subtitle_.empty()) {
      ui::TextStyle sts = ui4::style(ui::Font::kB, 17.0f * s, ui4::kWhite);
      ui4::fit_text(subtitle_, sts, 580.0f * s, 11.0f * s);
      const float sw = ui::List::measure(subtitle_, sts);
      const float sy = ty + tts.size + 22.0f * s;
      ui4::brush(l, tx - 10.0f * s, sy - 4.0f * s, tx + std::max(220.0f * s, sw + 38.0f * s), sy + sts.size + 10.0f * s,
                 ui4::kBrushBlack, false);
      l.text(tx + 12.0f * s, sy + 2.0f * s, subtitle_, sts);
    }
  }

  // Sharp FH1 progress strip (STATBARFILL / BARBASE8PX — ZERO rounded progress bar!).
  if (progress_ > 0.0f) {
    const float bx0 = f.x(92.0f), bx1 = f.x(1070.0f), by = f.y(648.0f), bh = 8.0f * s;
    std::array<float, 4> bar_empty{}, bar_fill{};
    if (ui_texture("KITPARTS/BARS/BARBASE8PX", bar_empty)) {
      l.image(bx0, by, bx1, by + bh, bar_empty, ui::rgba(255, 255, 255, 64));
    } else {
      l.rect(bx0, by, bx1, by + bh, ui::rgba(255, 255, 255, 40));
    }
    const float fw = std::max(4.0f * s, (bx1 - bx0) * std::clamp(progress_, 0.0f, 1.0f));
    if (ui_texture("KITPARTS/BARS/BARBASE8PX", bar_fill)) {
      l.image(bx0, by, bx0 + fw, by + bh, bar_fill, ui4::kPink);
    } else {
      l.rect(bx0, by, bx0 + fw, by + bh, ui4::kPink);
    }
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(progress_ * 100.0f)));
    ui::TextStyle pct_ts = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    l.text(bx1, by - 20.0f * s, pct, pct_ts);
  }

  // Real rotating COMMON/LOADSPINNER.xds (or LOADING/LOADINGSPINNER50PX) sprite!
  const float sp_cx = f.x(1152.0f);
  const float sp_cy = f.y(646.0f);
  const float ang = float(std::fmod(elapsed_ * 4.2, 6.2831853));
  std::array<float, 4> sp_uv{};
  if (ui_texture("COMMON/LOADSPINNER", sp_uv) || ui_texture("LOADING/LOADINGSPINNER50PX", sp_uv) ||
      ui_texture("COMMON/SPINNER", sp_uv)) {
    l.image_rotated(sp_cx, sp_cy, 24.0f * s, 24.0f * s, ang, sp_uv, ui4::kWhite);
  } else if (sc.ok()) {
    const int sp_node = sc.find("LOADSPINNER");
    if (sp_node >= 0) sc.model(l, f, sp_node, 1.0f, 0.0f, 0.0f, 0.0f, 0u, {}, -ang);
  }
  ui::TextStyle lw = ui4::style(ui::Font::kB, 11.0f * s, ui4::kOffWhite, ui::Align::kCenter);
  l.text(sp_cx, sp_cy + 28.0f * s, loc_str("ScreenTitles", "IDS_Loading", "LOADING"), lw);
}

// ------------------------------------------------ Virtual On-Screen Keyboard

void OnScreenKeyboard::open(std::string title, std::string initial_text, int max_length, Purpose purpose) {
  open_ = true;
  confirmed_ = false;
  cancelled_ = false;
  title_ = std::move(title);
  text_ = std::move(initial_text);
  max_len_ = std::max(1, max_length);
  if (int(text_.size()) > max_len_) text_.resize(size_t(max_len_));
  purpose_ = purpose;
  shifted_ = (purpose == Purpose::kLicensePlate);
  symbols_ = false;
  row_ = 0;
  col_ = 0;
  blink_t_ = 0.0;
}

std::vector<std::vector<std::string>> OnScreenKeyboard::active_layout() const {
  if (symbols_) {
    return {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
        {"-", "_", "/", ":", ";", "(", ")", "&", "@", "#"},
        {"!", "?", ".", ",", "'", "\"", "+", "=", "*", "%"},
        {"ABC", "SPACE", "BKSP", "CLEAR", "DONE"},
    };
  }
  if (shifted_) {
    return {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
        {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
        {"A", "S", "D", "F", "G", "H", "J", "K", "L", "-"},
        {"SHIFT", "Z", "X", "C", "V", "B", "N", "M", "_", "BKSP"},
        {"SYM", "SPACE", "CLEAR", "CANCEL", "DONE"},
    };
  }
  return {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
      {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "-"},
      {"SHIFT", "z", "x", "c", "v", "b", "n", "m", "_", "BKSP"},
      {"SYM", "SPACE", "CLEAR", "CANCEL", "DONE"},
  };
}

void OnScreenKeyboard::move(int dcol, int drow) {
  if (!open_) return;
  const auto lay = active_layout();
  const int rows = int(lay.size());
  row_ = ((row_ + drow) % rows + rows) % rows;
  const int cols = int(lay[size_t(row_)].size());
  col_ = std::clamp(col_, 0, cols - 1);
  if (dcol != 0) col_ = ((col_ + dcol) % cols + cols) % cols;
}

void OnScreenKeyboard::press_selected() {
  if (!open_) return;
  const auto lay = active_layout();
  if (row_ < 0 || row_ >= int(lay.size())) return;
  const auto& r = lay[size_t(row_)];
  if (col_ < 0 || col_ >= int(r.size())) return;
  const std::string& key = r[size_t(col_)];
  if (key == "SHIFT") toggle_shift();
  else if (key == "SYM" || key == "ABC") {
    toggle_symbols();
    row_ = std::min(row_, int(active_layout().size()) - 1);
    col_ = 0;
  } else if (key == "SPACE") type_char(' ');
  else if (key == "BKSP") backspace();
  else if (key == "CLEAR") text_.clear();
  else if (key == "CANCEL") cancel();
  else if (key == "DONE") confirm();
  else if (key.size() == 1) type_char(key[0]);
}

void OnScreenKeyboard::type_char(char c) {
  if (!open_ || int(text_.size()) >= max_len_) return;
  if (c < 32 || c > 126) return;
  if (purpose_ == Purpose::kLicensePlate) c = char(std::toupper(static_cast<unsigned char>(c)));
  text_.push_back(c);
}

void OnScreenKeyboard::backspace() {
  if (!open_ || text_.empty()) return;
  text_.pop_back();
}

void OnScreenKeyboard::confirm() {
  if (!open_) return;
  confirmed_ = true;
  cancelled_ = false;
  open_ = false;
}

void OnScreenKeyboard::cancel() {
  if (!open_) return;
  cancelled_ = true;
  confirmed_ = false;
  open_ = false;
}

void OnScreenKeyboard::draw(ui::List& l, float screen_w, float screen_h) const {
  if (!open_) return;
  const float s = screen_h / 720.0f;
  // Subtle backdrop scrim (no flat rounded modal!).
  l.rect(0, 0, screen_w, screen_h, ui::rgba(6, 8, 12, 165));

  // Authentic H_MESSAGE_POPUP styling: +3.0 deg tilted #C6C6C6 PILL01 header +
  // PILLCLIPON01 + #0E0E0E 2-tex PANEL01 body + sharp PILL02 keys with #F90064 focus!
  const float pw = 760.0f * s, ph = 428.0f * s;
  const float px0 = 0.5f * (screen_w - pw), py0 = 0.5f * (screen_h - ph) - 10.0f * s;
  const float cx = px0 + 0.5f * pw, cy = py0 + 0.5f * ph;

  {
    ui4::TiltScope tilt(l, cx, cy, ui4::kTiltRad);
    // Drop shadow behind tilted H_MESSAGE_POPUP body.
    l.rect(px0 + 6.0f * s, py0 + 38.0f * s, px0 + pw + 6.0f * s, py0 + ph + 6.0f * s, ui::rgba(0, 0, 0, 125));

    // Body: #0E0E0E 2-tex PANEL01 (or sharp charcoal + BGNOISE01, zero rounded corners).
    std::array<float, 4> panel01{}, noise{};
    l.rect(px0, py0 + 34.0f * s, px0 + pw, py0 + ph, ui4::kCharcoalDark);
    if (ui_texture("DIRTMASKS/PANEL01", panel01) && ui_texture("DIRTMASKS/BGNOISE01", noise)) {
      l.image_masked(px0, py0 + 34.0f * s, px0 + pw, py0 + ph, noise, panel01, ui::rgba(255, 255, 255, 34),
                     {2.0f, 2.0f, 0.0f, 0.0f}, ui::kMaskRepeat);
    } else if (ui_texture("DIRTMASKS/BGNOISE01", noise)) {
      l.image(px0, py0 + 34.0f * s, px0 + pw, py0 + ph, noise, ui::rgba(255, 255, 255, 22));
    }

    // Header bar: #C6C6C6 PILL01 + PILLCLIPON01 (authentic H_MESSAGE_POPUP header).
    const uint32_t kHeaderGrey = ui::rgba(0xC6, 0xC6, 0xC6, 255);
    ui4::pill(l, px0, py0, px0 + pw, py0 + 34.0f * s, kHeaderGrey, ui4::PillKind::kMenu, false);
    std::array<float, 4> clipon{};
    if (ui_texture("DIRTMASKS/PILLCLIPON01", clipon)) {
      l.image(px0 - 6.0f * s, py0 - 4.0f * s, px0 + 28.0f * s, py0 + 38.0f * s, clipon, ui4::kPink);
    } else {
      l.rect(px0, py0, px0 + 6.0f * s, py0 + 34.0f * s, ui4::kPink);
    }

    ui::TextStyle tt = ui4::style(ui::Font::kD, 17.0f * s, ui4::kInk);
    l.text(px0 + 28.0f * s, py0 + 9.0f * s, title_.empty() ? "ENTER TEXT" : title_, tt);

    char lenbuf[32];
    std::snprintf(lenbuf, sizeof lenbuf, "%zu / %d", text_.size(), max_len_);
    ui::TextStyle lt = ui4::style(ui::Font::kB, 13.0f * s, ui4::kInk, ui::Align::kRight);
    l.text(px0 + pw - 22.0f * s, py0 + 11.0f * s, lenbuf, lt);

    // Sharp text input strip (PILL02 charcoal + pink bottom hairline, zero radius).
    const float ix0 = px0 + 24.0f * s, ix1 = px0 + pw - 24.0f * s, iy0 = py0 + 50.0f * s, iy1 = iy0 + 46.0f * s;
    ui4::pill(l, ix0, iy0, ix1, iy1, ui4::kPillGrey, ui4::PillKind::kHud01, false);
    l.rect(ix0, iy1 - 2.0f * s, ix1, iy1, ui4::kPink);
    const bool caret_on = std::fmod(blink_t_, 1.0) < 0.55;
    const std::string shown = text_ + (caret_on ? "_" : "");
    ui::TextStyle it = ui4::style(ui::Font::kB, 22.0f * s, ui4::kWhite, ui::Align::kLeft, true);
    l.text(ix0 + 16.0f * s, iy0 + 11.0f * s, shown, it);

    // Sharp key grid using PILL02 / #F90064 pink active key focus (ZERO rounded corners!).
    const auto lay = active_layout();
    const float gx0 = px0 + 24.0f * s, gw = pw - 48.0f * s;
    const float gy0 = iy1 + 18.0f * s;
    const float gap = 6.0f * s;
    const float key_h = 46.0f * s;
    std::array<float, 4> pill02{};
    const bool has_p02 = ui_texture("DIRTMASKS/PILL02", pill02);
    for (size_t r = 0; r < lay.size(); ++r) {
      const auto& row_keys = lay[r];
      const float kw = (gw - gap * float(row_keys.size() - 1)) / float(row_keys.size());
      const float ky0 = gy0 + float(r) * (key_h + gap);
      for (size_t c = 0; c < row_keys.size(); ++c) {
        const bool sel = (int(r) == row_ && int(c) == col_);
        const bool is_done = (row_keys[c] == "DONE");
        const float kx0 = gx0 + float(c) * (kw + gap);
        const uint32_t bg = sel ? ui4::kPink : is_done ? ui::rgba(36, 118, 68, 240) : ui4::kPillGrey;
        if (has_p02) {
          l.image(kx0, ky0, kx0 + kw, ky0 + key_h, pill02, bg);
        } else {
          l.rect(kx0, ky0, kx0 + kw, ky0 + key_h, bg);
        }
        if (sel) {
          l.rect(kx0, ky0, kx0 + kw, ky0 + 3.0f * s, ui4::kYellow);
          l.frame(kx0, ky0, kx0 + kw, ky0 + key_h, ui4::kWhite, 1.5f * s, 0.0f);
        }
        ui::TextStyle kt = ui4::style(row_keys[c].size() > 1 ? ui::Font::kB : ui::Font::kE,
                                      (row_keys[c].size() > 1 ? 13.0f : 18.0f) * s, ui4::kWhite, ui::Align::kCenter,
                                      true);
        l.text(kx0 + 0.5f * kw, ky0 + 0.5f * (key_h - kt.size), row_keys[c], kt);
      }
    }
  }

  ui4::button_bar(l,
                  {{"A", loc_str("HelpButtons", "IDS_Select", "TYPE")},
                   {"X", "BACKSPACE"},
                   {"START", loc_str("HelpButtons", "IDS_ContinueMenu", "DONE")},
                   {"B", loc_str("HelpButtons", "IDS_Cancel", "CANCEL")}},
                  screen_w, screen_h);
}

}  // namespace carview
