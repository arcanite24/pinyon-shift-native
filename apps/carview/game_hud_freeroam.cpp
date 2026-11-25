#include "game_hud_internal.h"

namespace carview {

namespace {

inline float smoothstep01(float t) {
  const float x = std::clamp(t, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

inline bool boxes_overlap(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1,
                          float pad_x, float pad_y) {
  return (ax0 - pad_x < bx1) && (ax1 + pad_x > bx0) && (ay0 - pad_y < by1) && (ay1 + pad_y > by0);
}

void draw_hexagon_outline(ui::List& l, float cx, float cy, float r, float thickness, uint32_t col) {
  constexpr float kPi = 3.14159265f;
  float vx[6], vy[6];
  for (int i = 0; i < 6; ++i) {
    const float a = -0.5f * kPi + float(i) * (kPi / 3.0f);
    vx[i] = cx + r * std::cos(a);
    vy[i] = cy + r * std::sin(a);
  }
  for (int i = 0; i < 6; ++i) {
    const int j = (i + 1) % 6;
    l.line(vx[i], vy[i], vx[j], vy[j], thickness, col);
  }
}

void compute_world_marker_layout(const WorldMarkerHud* wm, const hud_detail::Layout& g, WorldMarkerLayout& out) {
  out = {};
  if (!wm || !wm->visible) return;
  const float s = g.s;
  float scale = 1.0f;
  float alpha = 1.0f;
  if (!wm->in_range) {
    const double d = std::max(0.0, wm->distance_m);
    if (d > 380.0) return;
    scale = 1.0f - 0.20f * smoothstep01(float((d - 35.0) / 245.0));
    if (d > 160.0) alpha = 1.0f - smoothstep01(float((d - 160.0) / 220.0));
    if (alpha <= 0.03f) return;
  }

  const float mx = wm->in_range ? std::clamp(wm->screen_x, g.L(120), g.w - g.L(300)) : wm->screen_x;
  const float my = wm->in_range ? std::clamp(wm->screen_y, g.T(105), g.h - g.T(175)) : wm->screen_y;
  if (mx < -90.0f * s || mx > g.w + 90.0f * s || my < -70.0f * s || my > g.h + 70.0f * s) return;

  const float hex_r = (wm->in_range ? 18.0f : 14.0f) * s * scale;
  const float tx = mx + hex_r + 9.0f * s * scale;
  ui::TextStyle ts = hud_detail::font(ui::Font::kE, (wm->in_range ? 25.0f : 18.5f) * s * scale, ui4::kWhite);
  hud_detail::fit(wm->title, ts, g.L(360) * scale, 12.0f * s * scale);
  const float tw = ui::List::measure(wm->title, ts);

  float bottom_y = my + std::max(hex_r + 6.0f * s * scale, 0.5f * ts.size + 4.0f * s * scale);
  float right_x = tx + tw + 6.0f * s * scale;
  if (wm->in_range) {
    const std::string ptxt =
        wm->prompt.empty() ? hud_detail::str("Activities", "IDS_EnterNow", "ENTER NOW") : wm->prompt;
    const ui::TextStyle ps = hud_detail::font(ui::Font::kE, 15.0f * s, ui4::kWhite);
    const float pw = ui::List::measure(ptxt, ps);
    const float strip_w = pw + 44.0f * s;
    const float sx0 = std::max(tx + 6.0f * s, tx + tw - strip_w + 4.0f * s);
    right_x = std::max(right_x, sx0 + strip_w);
    bottom_y = std::max(bottom_y, my + 0.48f * ts.size + 29.0f * s);
  } else if (wm->distance_m > 0.0) {
    bottom_y = std::max(bottom_y, my + 0.48f * ts.size + 20.0f * s * scale);
  }

  out.visible = true;
  out.anchor_x = mx;
  out.anchor_y = my;
  out.scale = scale;
  out.alpha = alpha;
  out.x0 = mx - hex_r - 3.0f * s * scale;
  out.y0 = my - std::max(hex_r, 0.58f * ts.size) - 3.0f * s * scale;
  out.x1 = right_x;
  out.y1 = bottom_y;
}

}  // namespace

NameTagLod compute_name_tag_lod(double distance_m) {
  if (distance_m < 0.0) return {1.0f, 1.0f, true};
  const float t_scale = smoothstep01(float((distance_m - 20.0) / 120.0));
  const float scale = 1.0f - 0.22f * t_scale;  // 1.0x close -> 0.78x far
  if (distance_m <= 45.0) return {scale, 1.0f, true};
  if (distance_m >= 160.0) return {scale, 0.0f, false};
  const float t_fade = smoothstep01(float((distance_m - 45.0) / 115.0));
  const float alpha = 1.0f - t_fade;
  return {scale, alpha, alpha > 0.03f};
}

void layout_world_labels(const std::vector<NameTagHud>& tags, const WorldMarkerHud* world_marker, bool in_race,
                         float w, float h, std::vector<ResolvedNameTag>& out_tags, WorldMarkerLayout* out_marker) {
  out_tags.clear();
  const float s = h / 720.0f;
  const hud_detail::Layout g{w, h, s};

  WorldMarkerLayout wm_layout{};
  if (world_marker && !in_race) compute_world_marker_layout(world_marker, g, wm_layout);
  if (out_marker) *out_marker = wm_layout;
  if (tags.empty()) return;

  const float margin_x = 22.0f * s;
  const float top_limit = 22.0f * s;
  const float bottom_limit = h - 104.0f * s;
  const float pad_x = 5.0f * s;
  const float pad_y = 4.0f * s;

  struct Candidate {
    ResolvedNameTag tag;
    double distance_m = -1.0;
    float pill_w = 0.0f;
    float pill_h = 0.0f;
  };
  std::vector<Candidate> cands;
  cands.reserve(tags.size());

  for (size_t i = 0; i < tags.size(); ++i) {
    const auto& t = tags[i];
    if (t.name.empty()) continue;
    if (t.screen_x < 28.0f * s || t.screen_x > w - 28.0f * s || t.screen_y < 26.0f * s || t.screen_y > bottom_limit)
      continue;

    const NameTagLod lod = compute_name_tag_lod(t.distance_m);
    if (!lod.visible) continue;

    Candidate c;
    c.distance_m = t.distance_m;
    c.tag.source_index = i;
    c.tag.orig_x = t.screen_x;
    c.tag.orig_y = t.screen_y;
    c.tag.scale = lod.scale;
    c.tag.alpha = lod.alpha;
    c.tag.leader = t.leader;
    c.tag.driver = t.name;

    // Parse "[<CLASS> <PI>] <DRIVER>" prefix or use explicit NameTagHud class/PI fields.
    if (t.name.size() >= 6 && t.name.front() == '[') {
      const size_t close = t.name.find(']');
      if (close != std::string::npos && close > 2) {
        const std::string inside = t.name.substr(1, close - 1);
        const size_t sp = inside.find(' ');
        if (sp != std::string::npos && sp > 0 && sp + 1 < inside.size()) {
          std::string cls = inside.substr(0, sp);
          std::string pi = inside.substr(sp + 1);
          size_t rest_pos = close + 1;
          while (rest_pos < t.name.size() && t.name[rest_pos] == ' ') ++rest_pos;
          const std::string drv = t.name.substr(rest_pos);
          if (!drv.empty()) {
            c.tag.has_badge = true;
            c.tag.car_class = std::move(cls);
            c.tag.pi_text = std::move(pi);
            c.tag.driver = drv;
          }
        }
      }
    }
    if (!c.tag.has_badge && !t.car_class.empty() && t.pi > 0) {
      c.tag.has_badge = true;
      c.tag.car_class = t.car_class;
      c.tag.pi_text = std::to_string(t.pi);
    }

    const float sc = lod.scale;
    const float pill_h = 17.5f * s * sc;
    ui::TextStyle nts = hud_detail::font(ui::Font::kB, 9.8f * s * sc, ui4::kWhite);
    hud_detail::fit(c.tag.driver, nts, 140.0f * s * sc, 7.6f * s * sc);
    const float name_w = ui::List::measure(c.tag.driver, nts);

    float pill_w = 0.0f;
    if (c.tag.has_badge) {
      const float bh = pill_h - 4.0f * s * sc;
      c.tag.cls_w = (c.tag.car_class.size() > 1 ? 16.5f : 13.5f) * s * sc;
      c.tag.pi_w = std::max(21.0f * s * sc, bh * 1.65f);
      c.tag.badge_w = c.tag.cls_w + c.tag.pi_w;
      pill_w = 4.5f * s * sc + c.tag.badge_w + 6.0f * s * sc + name_w + 7.5f * s * sc;
    } else {
      const float star_w = c.tag.leader ? ui::List::measure("* ", nts) : 0.0f;
      pill_w = 4.0f * s * sc + 5.5f * s * sc + star_w + name_w + 7.5f * s * sc;
    }

    const float half_w = 0.5f * pill_w;
    const float cx = std::clamp(t.screen_x, margin_x + half_w, std::max(margin_x + half_w, w - margin_x - half_w));
    c.pill_w = pill_w;
    c.pill_h = pill_h;
    c.tag.x0 = cx - half_w;
    c.tag.x1 = cx + half_w;
    c.tag.y1 = t.screen_y - 5.0f * s * sc;
    c.tag.y0 = c.tag.y1 - pill_h;
    cands.push_back(std::move(c));
  }

  // Closest cars first (in races cap to 5 closest visible cars so a pack never blocks the track).
  std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
    const bool ha = a.distance_m >= 0.0;
    const bool hb = b.distance_m >= 0.0;
    if (ha && hb) {
      if (std::fabs(a.distance_m - b.distance_m) > 1.5) return a.distance_m < b.distance_m;
    } else if (ha != hb) {
      return ha;
    } else if (std::fabs(a.tag.orig_y - b.tag.orig_y) > 1.0f) {
      return a.tag.orig_y > b.tag.orig_y;
    }
    if (a.tag.leader != b.tag.leader) return a.tag.leader;
    return a.tag.source_index < b.tag.source_index;
  });

  const size_t max_tags = in_race ? 5u : 6u;
  if (cands.size() > max_tags) cands.resize(max_tags);

  struct Obstacle {
    float x0, y0, x1, y1;
    bool is_marker = false;
  };
  std::vector<Obstacle> obstacles;
  obstacles.reserve(cands.size() + 1);
  if (wm_layout.visible) {
    obstacles.push_back({wm_layout.x0, wm_layout.y0, wm_layout.x1, wm_layout.y1, true});
  }

  auto hits_any = [&](float x0, float y0, float x1, float y1, bool& hit_marker_only, float& min_obs_y0) -> bool {
    bool hit = false;
    bool hit_car = false;
    min_obs_y0 = y0;
    for (const auto& ob : obstacles) {
      if (boxes_overlap(x0, y0, x1, y1, ob.x0, ob.y0, ob.x1, ob.y1, pad_x, pad_y)) {
        hit = true;
        if (!ob.is_marker) hit_car = true;
        min_obs_y0 = std::min(min_obs_y0, ob.y0);
      }
    }
    hit_marker_only = hit && !hit_car;
    return hit;
  };

  out_tags.reserve(cands.size());
  for (auto& c : cands) {
    auto& tag = c.tag;
    bool marker_only = false;
    float min_obs_y0 = tag.y0;
    if (!hits_any(tag.x0, tag.y0, tag.x1, tag.y1, marker_only, min_obs_y0)) {
      if (tag.y0 >= top_limit) {
        obstacles.push_back({tag.x0, tag.y0, tag.x1, tag.y1, false});
        out_tags.push_back(std::move(tag));
      }
      continue;
    }

    // Bounded gentle nudge search (small upward clearance or modest lateral offset).
    // Never stacks 5-7 boxes into a vertical wall blocking the road!
    const float max_up = (marker_only ? 2.4f : 1.35f) * c.pill_h;
    const float base_x0 = tag.x0, base_y0 = tag.y0, base_x1 = tag.x1, base_y1 = tag.y1;
    const float base_cx = 0.5f * (base_x0 + base_x1);

    // Direction away from the first colliding obstacle center.
    float push_dir = 1.0f;
    for (const auto& ob : obstacles) {
      if (boxes_overlap(base_x0, base_y0, base_x1, base_y1, ob.x0, ob.y0, ob.x1, ob.y1, pad_x, pad_y)) {
        push_dir = (base_cx < 0.5f * (ob.x0 + ob.x1)) ? -1.0f : 1.0f;
        break;
      }
    }

    const float clear_up = std::max(0.0f, base_y1 - (min_obs_y0 - pad_y));
    const std::array<std::pair<float, float>, 9> tries = {{
        {0.0f, -clear_up},
        {push_dir * 32.0f * s, 0.0f},
        {-push_dir * 32.0f * s, 0.0f},
        {push_dir * 58.0f * s, 0.0f},
        {-push_dir * 58.0f * s, 0.0f},
        {push_dir * 36.0f * s, -std::min(clear_up, c.pill_h * 1.15f)},
        {-push_dir * 36.0f * s, -std::min(clear_up, c.pill_h * 1.15f)},
        {0.0f, -std::min(max_up, clear_up + c.pill_h * 0.5f)},
        {push_dir * 76.0f * s, -std::min(max_up, c.pill_h * 0.9f)},
    }};

    bool placed = false;
    for (const auto& [dx, dy] : tries) {
      if (-dy > max_up + 0.5f) continue;
      const float nx0 = base_x0 + dx, nx1 = base_x1 + dx;
      const float ny0 = base_y0 + dy, ny1 = base_y1 + dy;
      if (nx0 < margin_x || nx1 > w - margin_x || ny0 < top_limit || ny1 > bottom_limit) continue;
      bool tmp_mo = false;
      float tmp_y = 0.0f;
      if (!hits_any(nx0, ny0, nx1, ny1, tmp_mo, tmp_y)) {
        tag.x0 = nx0;
        tag.x1 = nx1;
        tag.y0 = ny0;
        tag.y1 = ny1;
        tag.alpha *= 0.88f;  // subtle fade for gently nudged background tag
        obstacles.push_back({tag.x0, tag.y0, tag.x1, tag.y1, false});
        out_tags.push_back(std::move(tag));
        placed = true;
        break;
      }
    }
    (void)placed;  // deeply occluded background tags are gracefully suppressed
  }
}

void render_world_labels_and_nameplates(ui::List& l, const std::vector<NameTagHud>& tags,
                                        const WorldMarkerHud* world_marker, bool in_race, float w, float h,
                                        bool metric_units) {
  const float s = h / 720.0f;
  const hud_detail::Layout g{w, h, s};
  std::vector<ResolvedNameTag> resolved;
  WorldMarkerLayout wm_layout{};
  layout_world_labels(tags, world_marker, in_race, w, h, resolved, &wm_layout);

  // 1. Draw car nameplates back-to-front (farther/nudged first, closest on top).
  for (auto it = resolved.rbegin(); it != resolved.rend(); ++it) {
    const auto& b = *it;
    const float sc = b.scale;
    const float a = std::clamp(b.alpha, 0.0f, 1.0f);
    if (a <= 0.03f) continue;
    const float pill_h = b.y1 - b.y0;

    // Subtle pointer tick anchored directly beneath the pill pointing toward the car roof.
    const float tip_x = std::clamp(b.orig_x, b.x0 + 8.0f * s * sc, b.x1 - 8.0f * s * sc);
    const float tick_h = 3.8f * s * sc;
    const float tick_hw = 3.5f * s * sc;
    const uint32_t accent =
        b.leader ? ui4::kPink : (b.has_badge ? ui4::class_colour_by_name(b.car_class) : ui::rgba(220, 226, 236));
    l.line(tip_x - tick_hw, b.y1, tip_x, b.y1 + tick_h, 1.4f * s * sc, ui::fade(accent, a * 0.85f));
    l.line(tip_x + tick_hw, b.y1, tip_x, b.y1 + tick_h, 1.4f * s * sc, ui::fade(accent, a * 0.85f));

    // Sleek compact translucent FH1 pill backing + subtle top edge sheen + crisp left accent.
    l.rect(b.x0 + 1.0f * s, b.y0 + 1.5f * s, b.x1 + 1.0f * s, b.y1 + 1.5f * s, ui::fade(ui::rgba(0, 0, 0, 96), a));
    l.rect(b.x0, b.y0, b.x1, b.y1, ui::fade(ui::rgba(14, 16, 22, 174), a), 1.5f * s * sc,
           ui::fade(ui::rgba(8, 10, 14, 194), a));
    l.rect(b.x0, b.y0, b.x1, b.y0 + 0.9f * s, ui::fade(ui::rgba(255, 255, 255, 38), a));
    l.rect(b.x0, b.y0, b.x0 + 2.5f * s * sc, b.y1, ui::fade(accent, a * 0.95f));

    ui::TextStyle name_ts =
        hud_detail::font(ui::Font::kB, 9.8f * s * sc, ui::fade(b.leader ? ui4::kYellow : ui4::kWhite, a));
    hud_detail::fit(b.driver, name_ts, 140.0f * s * sc, 7.6f * s * sc);
    float tx = b.x0 + 4.5f * s * sc;
    if (b.has_badge) {
      const float by0 = b.y0 + 2.0f * s * sc;
      const float by1 = b.y1 - 2.0f * s * sc;
      const float bh = by1 - by0;
      const uint32_t cls_col = ui4::class_colour_by_name(b.car_class);
      l.rect(tx, by0, tx + b.cls_w, by1, ui::fade(cls_col, a));
      l.rect(tx + b.cls_w, by0, tx + b.badge_w, by1, ui::fade(ui4::kWhite, a));

      const bool light_cls = (b.car_class == "C" || b.car_class == "c" || b.car_class == "X" || b.car_class == "x");
      ui::TextStyle cs = hud_detail::font(ui::Font::kE, bh * 0.68f, ui::fade(light_cls ? ui4::kInk : ui4::kWhite, a),
                                          ui::Align::kCenter);
      hud_detail::fit(b.car_class, cs, b.cls_w - 1.5f * s * sc, bh * 0.42f);
      l.text(tx + 0.5f * b.cls_w, by0 + 0.5f * (bh - cs.size), b.car_class, cs);

      ui::TextStyle ps = hud_detail::font(ui::Font::kB, bh * 0.64f, ui::fade(ui4::kInk, a), ui::Align::kCenter);
      hud_detail::fit(b.pi_text, ps, b.pi_w - 1.5f * s * sc, bh * 0.40f);
      l.text(tx + b.cls_w + 0.5f * b.pi_w, by0 + 0.5f * (bh - ps.size), b.pi_text, ps);

      tx += b.badge_w + 6.0f * s * sc;
      l.text(tx, b.y0 + 0.5f * (pill_h - name_ts.size), b.driver, name_ts);
    } else {
      tx += 5.5f * s * sc;
      const std::string text = (b.leader ? "* " : "") + b.driver;
      l.text(tx, b.y0 + 0.5f * (pill_h - name_ts.size), text, name_ts);
    }
  }

  // 2. Draw active World Hub / Outpost / Event Marker cleanly on top (guaranteed non-overlapping).
  if (world_marker && wm_layout.visible && !in_race) {
    const WorldMarkerHud& wm = *world_marker;
    const float sc = wm_layout.scale;
    const float a = std::clamp(wm_layout.alpha, 0.0f, 1.0f);
    const float mx = wm_layout.anchor_x;
    const float my = wm_layout.anchor_y;
    const float hex_r = (wm.in_range ? 18.0f : 14.0f) * s * sc;
    const uint32_t rim_col = wm.in_range ? (wm.eligible ? ui4::kPink : ui::rgba(148, 152, 162))
                                         : (wm.is_festival ? ui4::kAmber : ui4::kPink);

    // FH1 hexagonal marker icon with dark translucent interior, crisp white+accent hex rim, and bottom chevron.
    l.circle(mx + 1.2f * s, my + 1.8f * s, hex_r + 1.5f * s * sc, ui::fade(ui::rgba(0, 0, 0, 145), a));
    l.circle(mx, my, hex_r - 1.5f * s * sc, ui::fade(ui::rgba(16, 18, 24, 220), a));
    draw_hexagon_outline(l, mx, my, hex_r, 2.4f * s * sc, ui::fade(ui4::kWhite, a));
    draw_hexagon_outline(l, mx, my, hex_r - 2.6f * s * sc, 1.3f * s * sc, ui::fade(rim_col, a));
    // Compact downward chevron beneath the hexagon pointing to the world trigger.
    const float chev_y = my + hex_r + 2.0f * s * sc;
    l.line(mx - 4.5f * s * sc, chev_y, mx, chev_y + 4.2f * s * sc, 1.8f * s * sc, ui::fade(rim_col, a));
    l.line(mx + 4.5f * s * sc, chev_y, mx, chev_y + 4.2f * s * sc, 1.8f * s * sc, ui::fade(rim_col, a));
    l.text(mx, my - (wm.in_range ? 8.5f : 6.8f) * s * sc, wm.is_festival ? "H" : "!",
           hud_detail::font(ui::Font::kE, (wm.in_range ? 16.5f : 13.0f) * s * sc, ui::fade(ui4::kWhite, a),
                            ui::Align::kCenter));

    // Compact uppercase FH1 tilted title with crisp drop shadow.
    const float tx = mx + hex_r + 9.0f * s * sc;
    hud_detail::Turn t(l, tx + 96.0f * s * sc, my, ui4::kTiltRad * 0.40f);
    ui::TextStyle ts = hud_detail::font(ui::Font::kE, (wm.in_range ? 25.0f : 18.5f) * s * sc, ui::fade(ui4::kWhite, a));
    hud_detail::fit(wm.title, ts, g.L(360) * sc, 12.0f * s * sc);
    const float tw = ui::List::measure(wm.title, ts);
    ui::TextStyle sh = ts;
    sh.color = ui::fade(ui::rgba(0, 0, 0, 195), a);
    l.text(tx + 1.8f * s, my - 0.55f * ts.size + 1.8f * s, wm.title, sh);
    l.text(tx, my - 0.55f * ts.size, wm.title, ts);

    if (wm.in_range) {
      const std::string ptxt =
          wm.prompt.empty() ? hud_detail::str("Activities", "IDS_EnterNow", "ENTER NOW") : wm.prompt;
      ui::TextStyle ps = hud_detail::font(ui::Font::kE, 15.0f * s, ui4::kWhite);
      const float pw = ui::List::measure(ptxt, ps);
      const float strip_w = pw + 44.0f * s;
      const float sx0 = std::max(tx + 6.0f * s, tx + tw - strip_w + 4.0f * s);
      const float sy0 = my + 0.44f * ts.size + 3.0f * s;
      const float sy1 = sy0 + 25.0f * s;
      ui4::brush(l, sx0 - 4.0f * s, sy0, sx0 + strip_w, sy1, wm.eligible ? ui4::kPink : ui::rgba(110, 110, 118, 230));
      l.text(sx0 + 8.0f * s, sy0 + 4.5f * s, ptxt, ps);
      ui4::button_glyph(l, sx0 + pw + 25.0f * s, 0.5f * (sy0 + sy1), 9.5f * s, "X");
    } else if (wm.distance_m > 0.0) {
      const std::string db = format_distance_unit(wm.distance_m, metric_units);
      ui::TextStyle dts = hud_detail::font(ui::Font::kB, 11.2f * s * sc, ui::fade(ui4::kOffWhite, a));
      const float dw = ui::List::measure(db, dts);
      const float dy0 = my + 0.46f * ts.size + 2.5f * s * sc;
      const float dy1 = dy0 + 16.0f * s * sc;
      l.rect(tx - 2.0f * s * sc, dy0, tx + dw + 12.0f * s * sc, dy1, ui::fade(ui::rgba(14, 16, 22, 165), a));
      l.rect(tx - 2.0f * s * sc, dy0, tx + 1.0f * s * sc, dy1, ui::fade(rim_col, a));
      l.text(tx + 5.0f * s * sc, dy0 + 0.5f * (dy1 - dy0 - dts.size), db, dts);
    }
  }
}

void PopularityBadgeAnimator::reset() {
  initialized = false;
  last_rank = 250;
  prev_rank = 250;
  last_chain_points = 0;
  last_banked_at = -1e9;
  anim_progress = 0.0;
  rank_anim_t = 1.0;
  last_time = -1.0;
}

void PopularityBadgeAnimator::update(const SkillChainHud& sk, double time) {
  constexpr double kRankAnimDuration = 1.35;
  const double clamped_target = std::clamp(sk.popularity_progress, 0.0, 1.0);
  if (!initialized || time < last_time - 0.5) {
    initialized = true;
    last_rank = sk.popularity_rank;
    prev_rank = sk.popularity_rank;
    last_chain_points = sk.chain_points;
    last_banked_at = sk.banked_at;
    anim_progress = clamped_target;
    const double since_rank = time - sk.rank_up_at;
    if (since_rank >= 0.0 && since_rank < kRankAnimDuration) {
      rank_anim_t = std::clamp(since_rank / kRankAnimDuration, 0.0, 1.0);
      prev_rank = sk.popularity_rank + 1;
    } else {
      rank_anim_t = 1.0;
    }
    last_time = time;
    return;
  }

  const double dt = std::clamp(time - last_time, 0.0, 0.25);
  last_time = time;

  // Detect rank improvement (FH1 popularity ranks count down: 250 -> 249 -> ... -> 1).
  bool rank_just_started = false;
  if (sk.popularity_rank < last_rank) {
    prev_rank = last_rank;
    last_rank = sk.popularity_rank;
    rank_anim_t = 0.0;
    anim_progress = 0.0;  // sweep freshly into the new rank's ring after flash
    rank_just_started = true;
  } else if (sk.popularity_rank > last_rank) {
    // Save swap / reset to a higher rank number.
    prev_rank = sk.popularity_rank;
    last_rank = sk.popularity_rank;
    rank_anim_t = 1.0;
    anim_progress = clamped_target;
  }

  if (!rank_just_started && rank_anim_t < 1.0) {
    rank_anim_t = std::min(1.0, rank_anim_t + dt / kRankAnimDuration);
  }

  // Smooth exponential interpolation of the radial progress ring (never snaps instantaneously).
  const double rate = (rank_anim_t < 0.35) ? 3.5 : 6.0;
  const double blend = 1.0 - std::exp(-dt * rate);
  anim_progress += (clamped_target - anim_progress) * blend;
  if (std::fabs(clamped_target - anim_progress) < 1e-4) anim_progress = clamped_target;

  last_chain_points = sk.chain_points;
  last_banked_at = sk.banked_at;
}

void PopularityBadgeAnimator::apply(SkillChainHud& sk) const {
  if (!initialized) return;
  sk.prev_popularity_rank = prev_rank;
  sk.pop_anim_progress = std::clamp(anim_progress, 0.0, 1.0);
  sk.pop_rank_anim_t = std::clamp(rank_anim_t, 0.0, 1.0);
}

PopularityBadgeAnimFrame evaluate_popularity_badge_anim(const SkillChainHud& sk, double time) {
  constexpr float kPi = 3.14159265f;
  constexpr double kRankAnimDuration = 1.35;
  PopularityBadgeAnimFrame out;
  out.display_rank = sk.popularity_rank;

  const double since_bank = time - sk.banked_at;
  const double since_rank = time - sk.rank_up_at;
  const double since_skill = time - sk.last_skill_at;

  // 1. Radial progress fill (prefer live interpolated pop_anim_progress, else smooth banking sweep).
  double prog = (sk.pop_anim_progress >= 0.0) ? std::clamp(sk.pop_anim_progress, 0.0, 1.0)
                                              : std::clamp(sk.popularity_progress, 0.0, 1.0);
  if (sk.pop_anim_progress < 0.0 && since_bank >= 0.0 && since_bank < 0.85) {
    const float k = smoothstep01(float(since_bank / 0.85));
    prog *= double(0.25f + 0.75f * k);
  }
  out.radial_progress = float(std::clamp(prog, 0.05, 1.0));

  // 2. Rank-up celebration phase u in [0..1] (1.0 = idle).
  float u = 1.0f;
  if (sk.pop_rank_anim_t >= 0.0) {
    u = std::clamp(float(sk.pop_rank_anim_t), 0.0f, 1.0f);
  } else if (since_rank >= 0.0 && since_rank < kRankAnimDuration) {
    u = std::clamp(float(since_rank / kRankAnimDuration), 0.0f, 1.0f);
  }

  if (u < 0.999f) {
    // Elastic scale pop: 1.0 -> 1.24 -> 1.0.
    if (u <= 0.22f) {
      const float k = u / 0.22f;
      out.disc_scale = 1.0f + 0.24f * std::sin(k * 0.5f * kPi);
    } else {
      const float v = (u - 0.22f) / 0.78f;
      out.disc_scale = 1.0f + 0.24f * std::exp(-5.0f * v) * std::cos(v * 2.0f * kPi * 1.15f) * (1.0f - v);
    }

    // Vertical card-flip / roll transition from old rank to new rank (e.g. 250TH -> 249TH).
    const int old_rank = (sk.prev_popularity_rank > 0 && sk.prev_popularity_rank != sk.popularity_rank)
                             ? sk.prev_popularity_rank
                             : (sk.popularity_rank + 1);
    const float fp = std::clamp((u - 0.08f) / 0.56f, 0.0f, 1.0f);
    out.flip_phase = fp;
    if (u < 0.08f) {
      out.display_rank = old_rank;
    } else if (fp < 0.5f) {
      const float h = fp * 2.0f;
      out.display_rank = old_rank;
      out.text_scale_y = std::max(0.12f, std::cos(h * 0.5f * kPi));
      out.text_offset_y = -9.5f * h;
      out.text_alpha = std::clamp(1.0f - 0.75f * h * h, 0.15f, 1.0f);
    } else if (u < 0.64f) {
      const float h = (fp - 0.5f) * 2.0f;
      out.display_rank = sk.popularity_rank;
      out.text_scale_y = std::max(0.12f, std::sin(h * 0.5f * kPi));
      out.text_offset_y = 9.5f * (1.0f - h);
      out.text_alpha = std::clamp(0.25f + 0.75f * h, 0.25f, 1.0f);
    } else {
      out.display_rank = sk.popularity_rank;
    }

    // Glowing FH1 magenta/gold ring flash sweep.
    const float flash_in = std::sin(std::clamp(u / 0.16f, 0.0f, 1.0f) * 0.5f * kPi);
    const float flash_out = 1.0f - smoothstep01((u - 0.20f) / 0.80f);
    out.ring_flash_alpha = std::clamp(flash_in * flash_out, 0.0f, 1.0f);
    out.ring_flash_angle = -0.5f * kPi + u * (3.0f * kPi);
  }

  // 3. Subtle pulse/glow when an active skill chain is increasing or banking.
  if (sk.active) {
    const float skill_kick =
        (since_skill >= 0.0 && since_skill < 0.45) ? (1.0f - hud_detail::ease_in_time(since_skill, 0.0, 0.45)) : 0.0f;
    const float breath = 0.16f + 0.12f * (0.5f + 0.5f * std::sin(float(time) * 6.5f));
    const float mult_boost = std::min(0.24f, float(std::max(0, sk.multiplier - 1)) * 0.06f);
    out.skill_pulse_alpha = std::clamp(breath + mult_boost + 0.42f * skill_kick, 0.0f, 0.92f);
    out.disc_scale = std::max(out.disc_scale, 1.0f + 0.055f * skill_kick);
  }
  if (since_bank >= 0.0 && since_bank < 1.2) {
    const float bu = float(since_bank / 1.2);
    const float bank_kick = std::sin(std::clamp(bu / 0.18f, 0.0f, 1.0f) * 0.5f * kPi) *
                            (1.0f - smoothstep01((bu - 0.18f) / 0.82f));
    out.skill_pulse_alpha = std::max(out.skill_pulse_alpha, 0.72f * bank_kick);
    out.disc_scale = std::max(out.disc_scale, 1.0f + 0.10f * bank_kick);
  }

  return out;
}

}  // namespace carview

namespace carview::hud_detail {

void world_marker_hud(ui::List& l, const WorldMarkerHud& wm, const Layout& g, bool metric_units) {
  render_world_labels_and_nameplates(l, {}, &wm, false, g.w, g.h, metric_units);
}

// (g) 947_HUD/PopularityBar & 947_HUD/DrivingSkill (exact match to out/oracle/ui/race-hud.png):
// Animated silver-white POPULARITYBARBACKGROUND disc with smooth interpolated radial ring,
// elastic rank-up scale pop + vertical card-flip transition + magenta/gold flash sweep,
// and subtle active skill / banking pulse glow.
void skill_chain_hud(ui::List& l, const SkillChainHud& sk, const Layout& g, double time) {
  const double since_bank = time - sk.banked_at;
  const double since_lost = time - sk.lost_at;
  const double since_rank = time - sk.rank_up_at;
  const bool show_bank = since_bank >= 0 && since_bank < 3.5;
  const bool show_lost = since_lost >= 0 && since_lost < 2.2;
  const bool show_rank = since_rank >= 0 && since_rank < 4.0;
  const bool rank_anim_active = (sk.pop_rank_anim_t >= 0.0 && sk.pop_rank_anim_t < 0.999);
  if (!sk.active && !show_bank && !show_lost && !show_rank && !rank_anim_active) return;

  const PopularityBadgeAnimFrame anim = evaluate_popularity_badge_anim(sk, time);
  const float s = g.s;
  const float bs = s * anim.disc_scale;
  const float cx = g.C(640), cy = g.T(62);
  const float r = 34.0f * bs;
  constexpr uint32_t kSkillYellow = ui4::kYellow;
  constexpr uint32_t kRankGold = ui4::kAmber;

  // 0. Expanding outer glow halo during active skill chain / banking pulse or rank-up flash.
  if (anim.ring_flash_alpha > 0.02f) {
    const float expand = (1.0f - anim.ring_flash_alpha) * 14.0f * s;
    l.circle(cx, cy, r + 8.0f * s + expand, ui::fade(ui4::kPink, anim.ring_flash_alpha * 0.24f));
    l.arc(cx, cy, r + 4.0f * s + expand * 0.65f, 3.2f * s, 0.0f, 6.28318f,
          ui::fade(kRankGold, anim.ring_flash_alpha * 0.55f));
  } else if (anim.skill_pulse_alpha > 0.02f) {
    l.circle(cx, cy, r + (4.5f + 3.5f * anim.skill_pulse_alpha) * s,
             ui::fade(kSkillYellow, anim.skill_pulse_alpha * 0.22f));
  }

  // 1. 947_HUD/PopularityBar textures: shadow -> opaque silver-white centre disc ->
  //    POPULARITYBARBACKGROUND rim -> smooth radial fill -> magenta/gold flash sweep -> highlight.
  std::array<float, 4> psh{}, pbg{}, phi{};
  if (ui_texture("HUD/POPULARITYBAR/POPULARITYBARSHADOW", psh)) {
    l.image(cx - 42.0f * bs, cy - 42.0f * bs, cx + 42.0f * bs, cy + 42.0f * bs, psh, rgba(0, 0, 0, 165));
  }
  l.circle(cx, cy, r - 1.5f * bs, rgba(226, 228, 232, 236));
  l.circle(cx, cy, r - 6.5f * bs, rgba(242, 244, 248, 246));
  if (ui_texture("HUD/POPULARITYBAR/POPULARITYBARBACKGROUND", pbg)) {
    l.image(cx - 38.0f * bs, cy - 38.0f * bs, cx + 38.0f * bs, cy + 38.0f * bs, pbg, ui4::kWhite);
  }

  // Smoothly interpolated radial progress ring along the outer groove.
  const float sweep = anim.radial_progress * 6.28318f;
  const uint32_t ring_col = (show_rank || anim.ring_flash_alpha > 0.1f) ? ui4::kPink : kSkillYellow;
  l.arc(cx, cy, r - 4.0f * bs, 4.8f * bs, -1.5708f, -1.5708f + sweep, ring_col, true);

  // Subtle leading tip pulse when skill chain is active or banking.
  if (anim.skill_pulse_alpha > 0.05f && anim.ring_flash_alpha <= 0.05f) {
    const float head_a = -1.5708f + sweep;
    const float tail_a = std::max(-1.5708f, head_a - 0.55f);
    l.arc(cx, cy, r - 4.0f * bs, 5.4f * bs, tail_a, head_a, ui::fade(ui4::kWhite, anim.skill_pulse_alpha * 0.45f), true);
  }

  // Glowing FH1 magenta/gold ring flash sweep on rank-up celebration.
  if (anim.ring_flash_alpha > 0.02f) {
    const float fa = anim.ring_flash_alpha;
    const float a0 = anim.ring_flash_angle - 1.55f;
    const float a1 = anim.ring_flash_angle;
    l.arc(cx, cy, r - 4.0f * bs, 6.2f * bs, a0, a1, ui::fade(ui4::kPink, fa * 0.85f), true);
    l.arc(cx, cy, r - 4.0f * bs, 4.2f * bs, a1 - 0.72f, a1, ui::fade(kRankGold, fa * 0.95f), true);
    l.arc(cx, cy, r - 4.0f * bs, 2.4f * bs, a1 - 0.24f, a1, ui::fade(ui4::kWhite, fa * 0.98f), true);
  }

  if (ui_texture("HUD/POPULARITYBAR/POPULARITYBARHIGHLIGHT", phi)) {
    l.image(cx - 38.0f * bs, cy - 38.0f * bs, cx + 38.0f * bs, cy + 38.0f * bs, phi,
            rgba(255, 255, 255, uint8_t(110 + int(anim.ring_flash_alpha * 95.0f))));
  }

  // BLACK italic rank number with inline superscript suffix ("250TH" -> "249TH" vertical flip/roll).
  char rbuf[16];
  std::snprintf(rbuf, sizeof rbuf, "%d", anim.display_rank);
  const char* suf = place_suffix(anim.display_rank);
  const float base_rsz = (anim.display_rank >= 100 ? 15.5f : 19.0f) * bs;
  const float base_ssz = (anim.display_rank >= 100 ? 8.8f : 10.5f) * bs;
  const float rsz_y = std::max(2.0f * bs, base_rsz * anim.text_scale_y);
  const float ssz_y = std::max(1.5f * bs, base_ssz * anim.text_scale_y);
  const uint32_t rank_col = ui::fade(ui4::kBrushBlack, anim.text_alpha);
  TextStyle num_ts = font(ui::Font::kE, rsz_y, rank_col);
  TextStyle suf_ts = font(ui::Font::kE, ssz_y, rank_col);
  // Preserve horizontal width while foreshortening vertically for authentic card-flip feel.
  const float flip_stretch = 1.0f / std::max(0.32f, anim.text_scale_y);
  num_ts.stretch = flip_stretch;
  suf_ts.stretch = flip_stretch;
  const float nw = ui::List::measure(rbuf, num_ts);
  const float sw = ui::List::measure(suf, suf_ts);
  const float total_w = nw + sw + 1.0f * bs;
  const float tx0 = cx - 0.5f * total_w;
  const float ty = cy - 0.52f * num_ts.size + anim.text_offset_y * bs;
  l.text(tx0, ty, rbuf, num_ts);
  l.text(tx0 + nw + 1.0f * bs, ty + 1.5f * bs * anim.text_scale_y, suf, suf_ts);

  // 2. 947_HUD/DrivingSkill: #DAFF00 score + white X1.0 + clean drop-shadowed skill callout
  //    (optional CRISPTAPESKILLS + SKILLICONSET, ZERO dark rectangle behind the skill name!).
  if (sk.active) {
    char pts_buf[32], mult_buf[16];
    std::snprintf(pts_buf, sizeof pts_buf, "%s", thousands(sk.chain_points).c_str());
    std::snprintf(mult_buf, sizeof mult_buf, " X%d.0", sk.multiplier);
    TextStyle pts_ts = font(ui::Font::kE, 26 * s, kSkillYellow);
    TextStyle mul_ts = font(ui::Font::kE, 22 * s, ui4::kWhite);
    const float pw = ui::List::measure(pts_buf, pts_ts);
    const float mw = ui::List::measure(mult_buf, mul_ts);
    const float sx0 = cx - 0.5f * (pw + mw);

    // Subtle CRISPTAPESKILLS/CIRCLEFLASH glow when chaining high scores.
    std::array<float, 4> flash_uv{}, icon_uv{};
    if (ui_texture("HUD/DRIVINGSKILLS/CIRCLEFLASH", flash_uv) && (sk.multiplier >= 2 || anim.skill_pulse_alpha > 0.35f)) {
      const float fa = std::clamp(0.18f + 0.25f * anim.skill_pulse_alpha, 0.15f, 0.45f);
      l.image(cx - 28 * bs, cy - 28 * bs, cx + 28 * bs, cy + 28 * bs, flash_uv, ui::fade(kSkillYellow, fa));
    }

    shadowed(l, sx0, g.T(102), pts_buf, pts_ts, s);
    shadowed(l, sx0 + pw, g.T(105), mult_buf, mul_ts, s);

    if (!sk.last_skill.empty()) {
      char pval[24];
      std::snprintf(pval, sizeof pval, " %d", sk.last_points);
      TextStyle sn_ts = font(ui::Font::kE, 14.5f * s, ui4::kWhite);
      TextStyle sp_ts = font(ui::Font::kE, 14.5f * s, kSkillYellow);
      const float snw = ui::List::measure(sk.last_skill, sn_ts);
      const float spw = ui::List::measure(pval, sp_ts);
      const float icon_sz = ui_texture("HUD/DRIVINGSKILLS/SKILLICONSET", icon_uv) ? 24.0f * s : 0.0f;
      const float row_w = (icon_sz > 0 ? (icon_sz + 6 * s) : 0.0f) + snw + spw;
      float rx = cx - 0.5f * row_w;
      if (icon_sz > 0) {
        // Hud/DrivingSkills/SkillIconSet.xds is a 512x256 (8x4 grid of 64x64) white-outlined icon sheet.
        const float u_span = icon_uv[2] - icon_uv[0];
        const float v_span = icon_uv[3] - icon_uv[1];
        const float aspect = (v_span > 1e-6f) ? (u_span / v_span) : 2.0f;
        const int cols = (aspect >= 1.5f) ? 8 : 4;
        const int rows = 4;
        std::string sk_up = sk.last_skill;
        for (char& ch : sk_up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        int col = 0, row = 1;  // default: skill badge (0,1)
        if (sk_up.find("AIR") != std::string::npos || sk_up.find("JUMP") != std::string::npos) {
          col = 4;
          row = 2;  // winged flying car
        } else if (sk_up.find("SPEED") != std::string::npos) {
          col = 2;
          row = 2;  // speeding car
        } else if (sk_up.find("NEAR MISS") != std::string::npos || sk_up.find("CLEAN") != std::string::npos ||
                   sk_up.find("PASS") != std::string::npos) {
          col = 4;
          row = 0;  // cars passing close
        } else if (sk_up.find("DRIFT") != std::string::npos || sk_up.find("180") != std::string::npos) {
          col = 2;
          row = 0;  // spinning drift arrow
        } else if (sk_up.find("BURN") != std::string::npos) {
          col = 5;
          row = 0;  // burning wheel
        } else if (sk_up.find("DRAFT") != std::string::npos || sk_up.find("SLIP") != std::string::npos) {
          col = 2;
          row = 1;  // slingshot
        } else if (sk_up.find("WRECK") != std::string::npos || sk_up.find("SMASH") != std::string::npos) {
          col = 3;
          row = 1;  // explosion
        }
        col = std::clamp(col, 0, cols - 1);
        row = std::clamp(row, 0, rows - 1);
        const float du = u_span / float(cols);
        const float dv = v_span / float(rows);
        const float pad_u = du * 0.04f;
        const float pad_v = dv * 0.04f;
        l.image(rx, g.T(124), rx + icon_sz, g.T(124) + icon_sz,
                {icon_uv[0] + col * du + pad_u, icon_uv[1] + row * dv + pad_v,
                 icon_uv[0] + (col + 1) * du - pad_u, icon_uv[1] + (row + 1) * dv - pad_v},
                ui4::kWhite);
        rx += icon_sz + 6 * s;
      }
      shadowed(l, rx, g.T(130), sk.last_skill, sn_ts, s);
      shadowed(l, rx + snw, g.T(130), pval, sp_ts, s);
    }
  } else if (show_rank) {
    const float a = 1.0f - clamp01((since_rank - 3.0) / 1.0);
    Turn t(l, cx, g.T(114));
    char rl[64];
    std::snprintf(rl, sizeof rl, "POPULARITY RANK %d%s!", sk.popularity_rank, place_suffix(sk.popularity_rank));
    const TextStyle ts = font(ui::Font::kE, 22 * s, ui::fade(kText, a), Align::kCenter);
    const float w = ui::List::measure(rl, ts) + 34 * s;
    ui4::brush(l, cx - 0.5f * w, g.T(100), cx + 0.5f * w, g.T(132), ui::fade(ui4::kPink, a));
    l.text(cx, g.T(106), rl, ts);
  } else if (show_bank) {
    const float a = 1.0f - clamp01((since_bank - 2.6) / 0.9);
    const std::string bl = "+" + thousands(sk.banked_total) + " BANKED";
    shadowed(l, cx, g.T(104), bl, font(ui::Font::kE, 24 * s, ui::fade(kSkillYellow, a), Align::kCenter), s);
  } else if (show_lost) {
    const float a = 1.0f - clamp01((since_lost - 1.5) / 0.7);
    shadowed(l, cx, g.T(104), "SKILL CHAIN LOST", font(ui::Font::kE, 22 * s, ui::fade(kRed, a), Align::kCenter), s);
  }
}


void ceremony_overlay(ui::List& l, const CeremonyHud& c, const Layout& g) {
  const float s = g.s;
  l.rect(0, 0, g.w, g.h, rgba(0, 0, 0, 155));
  const uint32_t bc = band_colour(c.band_colour);
  {
    Turn t(l, g.L(340), g.T(185));
    brush(l, g.L(95), g.T(150), g.L(610), g.T(218), kBrush);
    TextStyle hs = font(ui::Font::kE, 44 * s, kText);
    fit(c.headline, hs, g.L(490), 20 * s);
    l.text(g.L(115), g.T(160), c.headline, hs);
  }
  std::array<float, 4> uv;
  if (!c.band_colour.empty() && ui_texture("WRISTBANDS/" + c.band_colour + "XL", uv)) {
    l.image_rotated(g.L(300), g.T(292), 255 * s, 99 * s, -0.06f, uv);
  }
  {
    Turn t(l, g.L(410), g.T(392));
    brush(l, g.L(88), g.T(354), g.L(740), g.T(432), bc);
    TextStyle bs = font(ui::Font::kE, 48 * s, kInk);
    fit(c.band_name, bs, g.L(620), 20 * s);
    l.text(g.L(112), g.T(368), c.band_name, bs);
  }
  if (!c.subtitle.empty()) {
    Turn t(l, g.L(340), g.T(465));
    l.rect(g.L(105), g.T(445), g.L(640), g.T(486), kLight);
    l.text(g.L(122), g.T(456), c.subtitle, font(ui::Font::kB, 17 * s, kDark));
  }
  if (!c.bonus_car.empty()) {
    Turn t(l, g.L(340), g.T(518));
    brush(l, g.L(105), g.T(495), g.L(660), g.T(542), kPink);
    const std::string bc_txt = "BONUS CAR ADDED TO GARAGE: " + c.bonus_car;
    TextStyle cs = font(ui::Font::kB, 16 * s, kText);
    cs.keep_case = true;
    fit(bc_txt, cs, g.L(530), 10 * s);
    l.text(g.L(122), g.T(509), bc_txt, cs);
  }
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
}

// (j) Interactive 1:1 FH1 H_MAP HUD overlay:
// Center MAP_CROSSHAIR + #CF2169 MAP_LOCKON, clean hover MAPPILLSMALL pill,
// compact non-intrusive top-left filter bar & roads counter (NEVER a giant
// permanent list blocking the right third of Colorado!), PlayerBar top-right,
// and BUTTON_BAR_SCENE + road legend at bottom.
void map_overlay_hud(ui::List& l, const MapOverlayHud& mo, const Layout& g, bool has_player_bar, bool metric_units) {
  const float s = g.s;
  constexpr uint32_t kMapPink = rgba(0xCF, 0x21, 0x69);
  const bool hovering_marker = !mo.title.empty() && mo.title != "COLORADO MAP";

  // 1. Center H_MAP crosshair reticle: MAP/MASKS/MAP_CROSSHAIR + #CF2169 MAP_LOCKON.
  {
    const float cx = 0.5f * g.w, cy = 0.5f * g.h;
    std::array<float, 4> ch_uv{}, lo_uv{};
    if (ui_texture("MAP/MASKS/MAP_CROSSHAIR", ch_uv)) {
      l.image(cx - 56 * s, cy - 56 * s, cx + 56 * s, cy + 56 * s, ch_uv, rgba(255, 255, 255, 235));
    } else {
      l.arc(cx, cy, 30 * s, 2.0f * s, 0.0f, 6.28318f, rgba(255, 255, 255, 220));
      l.line(cx - 44 * s, cy, cx - 18 * s, cy, 2.0f * s, ui4::kWhite);
      l.line(cx + 18 * s, cy, cx + 44 * s, cy, 2.0f * s, ui4::kWhite);
      l.line(cx, cy - 44 * s, cx, cy - 18 * s, 2.0f * s, ui4::kWhite);
      l.line(cx, cy + 18 * s, cx, cy + 44 * s, 2.0f * s, ui4::kWhite);
    }
    if (hovering_marker) {
      if (ui_texture("MAP/MASKS/MAP_LOCKON", lo_uv)) {
        l.image(cx - 42 * s, cy - 42 * s, cx + 42 * s, cy + 42 * s, lo_uv, kMapPink);
      } else {
        l.arc(cx, cy, 22 * s, 2.5f * s, 0.0f, 6.28318f, kMapPink);
      }
      // Compact hover MAPPILLSMALL tooltip placed cleanly to the left of the reticle.
      TextStyle hts = font(ui::Font::kE, 14 * s, ui4::kWhite);
      fit(mo.title, hts, 260 * s, 10 * s);
      const float hw = std::clamp(ui::List::measure(mo.title, hts) + 32 * s, 128 * s, 292 * s);
      const float hx0 = cx - 48 * s - hw, hy0 = cy - 15 * s;
      ui4::pill(l, hx0, hy0, hx0 + hw, hy0 + 30 * s, ui4::kBrushBlack, ui4::PillKind::kMapSmall);
      l.rect(hx0 + hw - 4 * s, hy0, hx0 + hw, hy0 + 30 * s, kMapPink);
      l.text(hx0 + 12 * s, hy0 + 0.5f * (30 * s - hts.size), mo.title, hts);
    } else if (mo.distance_m >= 0.0) {
      // Subtle compact distance pill beside crosshair when panning over open road.
      const std::string db = format_distance_unit(mo.distance_m, metric_units);
      TextStyle dts = font(ui::Font::kB, 11.5f * s, ui4::kWhite);
      const float dw = ui::List::measure(db, dts) + 22 * s;
      const float dx0 = cx + 36 * s, dy0 = cy - 12 * s;
      ui4::pill(l, dx0, dy0, dx0 + dw, dy0 + 24 * s, ui::rgba(14, 16, 20, 205), ui4::PillKind::kMapSmall);
      l.text(dx0 + 11 * s, dy0 + 6 * s, db, dts);
    }
  }

  // 1b. Subtle Colorado world region labels projected directly onto the World Map.
  if (mo.scale_mpp > 0.01) {
    struct RegionLabel {
      const char* name;
      double x, z;
    };
    static constexpr RegionLabel kRegions[] = {
        {"HORIZON FESTIVAL", -1380.0, -180.0},
        {"RED ROCK", -3200.0, -2400.0},
        {"CARSON", -2100.0, 1450.0},
        {"BOULDER", 1700.0, -3400.0},
        {"FINLEY DAM", 2050.0, 950.0},
        {"EAGLE RIDGE", -550.0, -4100.0},
        {"BEAUMONT", 2450.0, -900.0},
        {"GLADSTONE", 900.0, 2100.0},
        {"MONTELLO", -3400.0, 800.0},
        {"GOLF COURSE", -420.0, 1250.0},
    };
    const float half_w = 0.5f * g.w, half_h = 0.5f * g.h;
    const float inv_mpp = float(1.0 / mo.scale_mpp);
    const float lbl_sz = std::clamp(11.5f * s * float(std::sqrt(std::clamp(mo.zoom, 0.8, 3.0))), 10.0f * s, 16.5f * s);
    for (const auto& reg : kRegions) {
      const float sx = half_w + float(reg.x - mo.center_x) * inv_mpp;
      const float sy = half_h - float(reg.z - mo.center_z) * inv_mpp;
      if (sx < g.L(90) || sx > g.w - g.L(90) || sy < g.T(72) || sy > g.h - g.T(84)) continue;
      TextStyle rsh = font(ui::Font::kE, lbl_sz, rgba(0, 0, 0, 175), Align::kCenter);
      TextStyle rts = font(ui::Font::kE, lbl_sz, rgba(255, 255, 255, 192), Align::kCenter);
      l.text(sx + 1.4f * s, sy + 1.4f * s, reg.name, rsh);
      l.text(sx, sy, reg.name, rts);
    }
  }

  // 2. Compact non-intrusive top-left H_MAP filter bar & roads discovery pill
  //    (cycles cleanly via LB / RB without covering the right third of Colorado!).
  {
    const float fx0 = g.L(48);
    const float fy0 = g.T(32);
    const std::string active_name =
        (!mo.filter_tabs.empty() && mo.active_filter >= 0 && mo.active_filter < int(mo.filter_tabs.size()))
            ? mo.filter_tabs[size_t(mo.active_filter)]
            : std::string("ALL");
    const std::string f_txt = "FILTER: " + active_name;
    TextStyle fts = font(ui::Font::kE, 13.0f * s, ui4::kWhite);
    const float f_pill_w = std::max(180 * s, ui::List::measure(f_txt, fts) + 92 * s);
    ui4::pill(l, fx0, fy0, fx0 + f_pill_w, fy0 + 28 * s, ui4::kBrushBlack, ui4::PillKind::kHud01);
    l.rect(fx0, fy0, fx0 + 4 * s, fy0 + 28 * s, kMapPink);
    ui4::button_glyph(l, fx0 + 24 * s, fy0 + 14 * s, 9.5f * s, "LB");
    l.text(fx0 + 46 * s, fy0 + 7.5f * s, f_txt, fts);
    ui4::button_glyph(l, fx0 + f_pill_w - 22 * s, fy0 + 14 * s, 9.5f * s, "RB");

    // Compact Roads Discovered pill right beside filter bar.
    char rb[64];
    std::snprintf(rb, sizeof rb, "ROADS %d / %d", mo.roads_discovered, mo.roads_total);
    TextStyle rts = font(ui::Font::kB, 12.0f * s, ui4::kWhite);
    const float rx0 = fx0 + f_pill_w + 10 * s;
    const float rw = std::max(134 * s, ui::List::measure(rb, rts) + 26 * s);
    ui4::pill(l, rx0, fy0, rx0 + rw, fy0 + 28 * s, ui4::kPillGrey, ui4::PillKind::kHud01);
    l.text(rx0 + 12 * s, fy0 + 6.5f * s, rb, rts);
    const float rfrac = mo.roads_total > 0 ? float(std::clamp(double(mo.roads_discovered) / mo.roads_total, 0.0, 1.0)) : 0.0f;
    l.rect(rx0 + 4 * s, fy0 + 23 * s, rx0 + rw - 4 * s, fy0 + 26.5f * s, rgba(255, 255, 255, 32));
    if (rfrac > 0.0f) l.rect(rx0 + 4 * s, fy0 + 23 * s, rx0 + 4 * s + (rw - 8 * s) * rfrac, fy0 + 26.5f * s, ui4::kGreen);

    if (mo.treasure_map) {
      const float tx0 = rx0 + rw + 10 * s;
      ui4::pill(l, tx0, fy0, tx0 + 150 * s, fy0 + 28 * s, ui4::kAmber, ui4::PillKind::kHud01);
      l.text(tx0 + 75 * s, fy0 + 7.5f * s, "TREASURE MAP", font(ui::Font::kE, 11.5f * s, ui4::kInk, Align::kCenter));
    }
  }

  // 3. Compact contextual right-hand PILL26PX detail card ONLY when hovering an icon/location
  //    (zero permanent wall blocking the map when browsing roads!).
  if (hovering_marker) {
    const float px0 = g.R(908), px1 = g.R(1214);
    float ry = has_player_bar ? g.T(130) : g.T(36);
    std::array<float, 4> pill26_uv{}, fest_uv{};
    const bool has_pill26 = ui_texture("KITPARTS/PILL26PX", pill26_uv);
    auto draw_map_row = [&](float y, float h_px, uint32_t col) {
      if (has_pill26) l.image(px0, y, px1, y + h_px, pill26_uv, col);
      else ui4::pill(l, px0, y, px1, y + h_px, col, ui4::PillKind::kHud01);
    };
    draw_map_row(ry, 32 * s, kMapPink);
    float hdr_tx = px0 + 12 * s;
    if (ui_texture("KITPARTS/FESTIVALMAPICON", fest_uv)) {
      l.image(px0 + 8 * s, ry + 3 * s, px0 + 34 * s, ry + 29 * s, fest_uv, ui4::kWhite);
      hdr_tx = px0 + 40 * s;
    }
    TextStyle hts = font(ui::Font::kE, 15.5f * s, ui4::kWhite);
    fit(mo.title, hts, px1 - hdr_tx - 12 * s, 10 * s);
    l.text(hdr_tx, ry + 0.5f * (32 * s - hts.size), mo.title, hts);
    ry += 34 * s;

    if (!mo.subtitle.empty()) {
      draw_map_row(ry, 26 * s, ui4::kPillGrey);
      TextStyle ss = font(ui::Font::kB, 12.0f * s, ui4::kWhite);
      fit(mo.subtitle, ss, (px1 - px0) - 24 * s, 9 * s);
      l.text(px0 + 12 * s, ry + 6.5f * s, mo.subtitle, ss);
      ry += 28 * s;
    }
    if (mo.distance_m >= 0.0 || mo.gps_active) {
      draw_map_row(ry, 26 * s, ui4::kPillGrey);
      if (mo.distance_m >= 0.0) {
        const std::string db = "DISTANCE  " + format_distance_unit(mo.distance_m, metric_units);
        l.text(px0 + 12 * s, ry + 6.5f * s, db, font(ui::Font::kB, 12.0f * s, ui4::kYellow));
      }
      if (mo.gps_active) {
        l.rect(px1 - 86 * s, ry + 3 * s, px1 - 6 * s, ry + 23 * s, ui4::kGreen);
        l.text(px1 - 46 * s, ry + 7 * s, "GPS SET", font(ui::Font::kE, 10.5f * s, ui4::kInk, Align::kCenter));
      }
    }
  }

  // 4. Crisp Pause Map Legend panel (compact 2-column FH1 legend tucked cleanly above bottom-left button bar).
  {
    const float lx0 = g.L(48), lx1 = g.L(328);
    const float ly0 = g.B(506), ly1 = g.B(642);
    ui4::panel(l, lx0, ly0, lx1, ly1, s, rgba(14, 16, 22, 218), 0, kMapPink, true);
    ui4::pill(l, lx0 + 8 * s, ly0 + 6 * s, lx1 - 8 * s, ly0 + 26 * s, ui::rgba(0xBF, 0xBF, 0xBF, 238),
              ui4::PillKind::kMenu);
    l.text(lx0 + 16 * s, ly0 + 10.5f * s, "MAP LEGEND", font(ui::Font::kD, 11.5f * s, ui4::kInk));
    l.text(lx1 - 14 * s, ly0 + 11.0f * s, "COLORADO", font(ui::Font::kE, 10.0f * s, kMapPink, Align::kRight));

    struct LegendEntry {
      const char* label;
      uint32_t col;
      bool is_line;
      bool is_new_tag;
    };
    static constexpr LegendEntry kLegend[8] = {
        {"FESTIVAL EVENT", rgba(249, 0, 100), false, false},
        {"OUTPOST HUB", rgba(255, 146, 36), false, false},
        {"STREET HUB", rgba(44, 140, 232), false, false},
        {"PR STUNT", rgba(218, 255, 0), false, false},
        {"BARN / SIGN", rgba(246, 206, 78), false, false},
        {"NEW EVENT", rgba(249, 0, 100), false, true},
        {"GPS ROUTE", rgba(74, 238, 97), true, false},
        {"DRIVEN ROAD", rgba(252, 244, 214), true, false},
    };
    const float col_w = (lx1 - lx0 - 20 * s) * 0.5f;
    for (int i = 0; i < 8; ++i) {
      const int col = i % 2;
      const int row = i / 2;
      const float ex = lx0 + 14 * s + float(col) * col_w;
      const float ey = ly0 + 34 * s + float(row) * 25.0f * s;
      const auto& item = kLegend[i];
      if (item.is_line) {
        l.rect(ex, ey + 6 * s, ex + 16 * s, ey + 10.5f * s, item.col);
      } else if (item.is_new_tag) {
        l.rect(ex, ey + 2.5f * s, ex + 16 * s, ey + 13.5f * s, item.col);
        l.text(ex + 8 * s, ey + 4.5f * s, "N", font(ui::Font::kE, 7.5f * s, ui4::kWhite, Align::kCenter));
      } else {
        l.circle(ex + 8 * s, ey + 8 * s, 5.5f * s, item.col);
        l.circle(ex + 8 * s, ey + 8 * s, 2.2f * s, ui4::kWhite);
      }
      l.text(ex + 22 * s, ey + 3.0f * s, item.label, font(ui::Font::kB, 10.2f * s, ui4::kOffWhite));
    }
  }

  // 5. Authentic BUTTON_BAR_SCENE bottom help strip + driven/undriven legend pill.
  ui4::button_bar(
      l,
      {{"A", mo.action_hint.empty() ? "FAST TRAVEL" : mo.action_hint},
       {"X", mo.gps_hint.empty() ? "SET GPS ROUTE" : mo.gps_hint},
       {"LB", "FILTER"},
       {"LT", "ZOOM"},
       {"B", str("HelpButtons", "IDS_Back", "BACK")}},
      g.w, g.h, 48.0f);

  const float ly0 = g.B(654), ly1 = g.B(682);
  ui4::pill(l, g.R(992), ly0, g.R(1232), ly1, ui4::kPillGrey, ui4::PillKind::kMenu);
  l.rect(g.R(1008), ly0 + 11 * s, g.R(1030), ly0 + 17 * s, rgba(252, 244, 214));
  l.text(g.R(1036), ly0 + 8 * s, "DRIVEN", font(ui::Font::kB, 11.5f * s, ui4::kWhite));
  l.rect(g.R(1110), ly0 + 11 * s, g.R(1132), ly0 + 17 * s, rgba(108, 112, 122));
  l.text(g.R(1138), ly0 + 8 * s, "UNDRIVEN", font(ui::Font::kB, 11.5f * s, ui4::kDim));
}

// (j2) 947_HUD/MiniMap/SatNavCommands: slim HUDPILL01 turn-by-turn strip above minimap.
void gps_turn_hud(ui::List& l, const GpsHud& gps, const Layout& g, bool metric_units) {
  if (!should_show_gps_turn_banner(gps)) return;
  const float s = g.s;
  const float x0 = g.L(64), y0 = g.B(376), x1 = g.L(296), y1 = g.B(408);
  ui4::pill(l, x0, y0, x1, y1, ui4::kPillGrey, ui4::PillKind::kHud01);
  l.rect(x0, y0, x0 + 28 * s, y1, ui4::kGreen);
  // Crisp vector chevron/arrow SatNav turn icon (replaces ASCII ^/<</U).
  {
    const float ax = x0 + 14.0f * s, ay = 0.5f * (y0 + y1);
    const float lw = 2.6f * s;
    const uint32_t ink = ui4::kInk;
    auto head = [&](float hx, float hy, float dx, float dy) {
      const float px = -dy, py = dx;
      l.line(hx, hy, hx - (dx * 4.5f - px * 3.8f) * s, hy - (dy * 4.5f - py * 3.8f) * s, lw, ink);
      l.line(hx, hy, hx - (dx * 4.5f + px * 3.8f) * s, hy - (dy * 4.5f + py * 3.8f) * s, lw, ink);
    };
    switch (gps.turn_kind) {
      case 2:  // slight left
        l.line(ax + 2.0f * s, ay + 8.0f * s, ax + 2.0f * s, ay + 1.0f * s, lw, ink);
        l.line(ax + 2.0f * s, ay + 1.0f * s, ax - 4.5f * s, ay - 6.0f * s, lw, ink);
        head(ax - 4.5f * s, ay - 6.0f * s, -0.707f, -0.707f);
        break;
      case 3:  // left
      case 4:  // sharp left
        l.line(ax + 4.0f * s, ay + 8.0f * s, ax + 4.0f * s, ay - 2.0f * s, lw, ink);
        l.line(ax + 4.0f * s, ay - 2.0f * s, ax - 6.0f * s, ay - (gps.turn_kind == 4 ? -3.0f : 2.0f) * s, lw, ink);
        head(ax - 6.0f * s, ay - (gps.turn_kind == 4 ? -3.0f : 2.0f) * s, -1.0f, 0.0f);
        break;
      case 5:  // slight right
        l.line(ax - 2.0f * s, ay + 8.0f * s, ax - 2.0f * s, ay + 1.0f * s, lw, ink);
        l.line(ax - 2.0f * s, ay + 1.0f * s, ax + 4.5f * s, ay - 6.0f * s, lw, ink);
        head(ax + 4.5f * s, ay - 6.0f * s, 0.707f, -0.707f);
        break;
      case 6:  // right
      case 7:  // sharp right
        l.line(ax - 4.0f * s, ay + 8.0f * s, ax - 4.0f * s, ay - 2.0f * s, lw, ink);
        l.line(ax - 4.0f * s, ay - 2.0f * s, ax + 6.0f * s, ay - (gps.turn_kind == 7 ? -3.0f : 2.0f) * s, lw, ink);
        head(ax + 6.0f * s, ay - (gps.turn_kind == 7 ? -3.0f : 2.0f) * s, 1.0f, 0.0f);
        break;
      case 8:  // U-turn
        l.line(ax + 4.0f * s, ay + 8.0f * s, ax + 4.0f * s, ay - 2.0f * s, lw, ink);
        l.arc(ax, ay - 2.0f * s, 4.0f * s, lw, 3.14159f, 6.28318f, ink, true);
        l.line(ax - 4.0f * s, ay - 2.0f * s, ax - 4.0f * s, ay + 6.0f * s, lw, ink);
        head(ax - 4.0f * s, ay + 6.0f * s, 0.0f, 1.0f);
        break;
      case 9:  // arrived
        l.circle(ax, ay, 6.5f * s, ink);
        l.circle(ax, ay, 3.0f * s, ui4::kGreen);
        break;
      default:  // straight
        l.line(ax, ay + 8.0f * s, ax, ay - 7.0f * s, lw, ink);
        head(ax, ay - 7.0f * s, 0.0f, -1.0f);
        break;
    }
  }

  const bool show_dist = should_show_gps_turn_distance(gps);
  const std::string line1 = gps.turn_text.empty() ? std::string("FOLLOW ROUTE") : gps.turn_text;
  TextStyle ts = font(ui::Font::kE, 13.5f * s, ui4::kWhite);
  fit(line1, ts, (x1 - x0) - (show_dist ? 96.0f : 48.0f) * s, 9.5f * s);
  l.text(x0 + 36 * s, y0 + 9 * s, line1, ts);
  if (show_dist) {
    const double rounded_m =
        gps.dist_to_turn_m >= 1000.0 ? gps.dist_to_turn_m : std::max(10.0, std::round(gps.dist_to_turn_m / 10.0) * 10.0);
    const std::string dt_buf = format_distance_unit(rounded_m, metric_units);
    l.text(x1 - 10 * s, y0 + 9 * s, dt_buf, font(ui::Font::kB, 12.5f * s, ui4::kGreen, Align::kRight));
  }
}

// (j3) 947_HUD/Missions (top-right, [956,30..1214,126]): slim HUDPILL01 rows for active
// Outpost PR Stunt (replaces the giant 430x114 top-left dark box!).
void pr_stunt_hud(ui::List& l, const PrStuntHud& ps, const Layout& g) {
  if (!ps.active) return;
  const float s = g.s;
  const float x0 = g.R(948), x1 = g.R(1214);
  float ry = g.T(32);
  const uint32_t accent = ps.in_zone ? ui4::kGreen : ui4::kPink;

  // Row 1: #F90064 / #46DC6E slim HUDPILL01 header + countdown timer.
  ui4::pill(l, x0, ry, x1, ry + 26 * s, accent, ui4::PillKind::kHud01);
  const std::string badge = ps.kind_badge.empty() ? std::string("PR STUNT") : ps.kind_badge;
  l.text(x0 + 12 * s, ry + 6 * s, badge, font(ui::Font::kE, 13.5f * s, ui4::kWhite));
  if (ps.time_remaining_s > 0.0) {
    l.text(x1 - 12 * s, ry + 5 * s, race_clock(ps.time_remaining_s),
           font(ui::Font::kE, 15.0f * s, ps.time_remaining_s < 15.0 ? ui4::kYellow : ui4::kWhite, Align::kRight));
  }
  ry += 28 * s;

  // Row 2: #212121 slim HUDPILL01 stunt title.
  ui4::pill(l, x0, ry, x1, ry + 24 * s, ui4::kPillGrey, ui4::PillKind::kHud01);
  TextStyle ts = font(ui::Font::kB, 13.0f * s, ui4::kWhite);
  fit(ps.title, ts, (x1 - x0) - 22 * s, 9.5f * s);
  l.text(x0 + 12 * s, ry + 5 * s, ps.title, ts);
  ry += 26 * s;

  // Row 3: Status & sharp progress fill strip.
  if (!ps.status_line.empty() || ps.progress > 0.0) {
    ui4::pill(l, x0, ry, x1, ry + 26 * s, ui4::kPillGrey, ui4::PillKind::kHud02);
    if (!ps.status_line.empty()) {
      TextStyle ss = font(ui::Font::kB, 11.5f * s, ps.in_zone ? ui4::kGreen : ui4::kYellow);
      fit(ps.status_line, ss, (x1 - x0) - 22 * s, 8.5f * s);
      l.text(x0 + 12 * s, ry + 4 * s, ps.status_line, ss);
    }
    const float bx0 = x0 + 10 * s, bx1 = x1 - 10 * s, by = ry + 19 * s;
    l.rect(bx0, by, bx1, by + 4 * s, rgba(255, 255, 255, 34));
    const float p = float(std::clamp(ps.progress, 0.0, 1.0));
    if (p > 0.0f) l.rect(bx0, by, bx0 + (bx1 - bx0) * p, by + 4 * s, accent);
  }
}


float speed_activity_hud(ui::List& l, const freeroam::SpeedActivityHud& sh, const Layout& g, bool metric_units,
                         float lift_canvas) {
  const float s = g.s;
  if (sh.flash_alpha > 0.01f) {
    l.rect(0, 0, g.w, g.h, rgba(255, 255, 255, uint8_t(std::clamp(sh.flash_alpha * 255.0f, 0.0f, 220.0f))));
  }
  const float cx = 0.5f * g.w;
  float used = 0.0f;
  if (sh.zone_active) {
    const float x0 = cx - 190 * s, x1 = cx + 190 * s, y0 = g.B(584 - lift_canvas);
    ui4::pill(l, x0, y0, x1, y0 + 24 * s, ui4::kBlue, ui4::PillKind::kHud01);
    l.text(cx, y0 + 5 * s, sh.active_zone_id + "  •  SPEED ZONE", font(ui::Font::kE, 12.5f * s, ui4::kWhite, Align::kCenter));
    ui4::pill(l, x0, y0 + 26 * s, x1, y0 + 56 * s, ui4::kPillGrey, ui4::PillKind::kHud02);
    const std::string avg =
        "AVG " + format_speed_unit(sh.live_avg_mph, metric_units, true) + "   •   " +
        format_distance_unit(sh.zone_distance_m, metric_units);
    l.text(cx, y0 + 33 * s, avg, font(ui::Font::kE, 17 * s, ui4::kWhite, Align::kCenter));
    used = std::max(used, 66.0f);
  }
  if (sh.banner_active) {
    const float extra_lift = sh.zone_active ? 68.0f : 0.0f;
    const float x0 = cx - 220 * s, x1 = cx + 220 * s, y0 = g.B(564 - lift_canvas - extra_lift),
                y1 = g.B(656 - lift_canvas - extra_lift);
    Turn t(l, cx, 0.5f * (y0 + y1), ui4::kTiltRad * 0.5f);
    ui4::brush(l, x0, y0, x1, y0 + 28 * s, ui4::kBrushBlack);
    l.text(cx, y0 + 6 * s, sh.banner_title + (sh.banner_new_pb ? "  •  NEW PERSONAL BEST!" : ""),
           font(ui::Font::kB, 13.5f * s, sh.banner_new_pb ? ui4::kYellow : ui4::kWhite, Align::kCenter));
    ui4::brush(l, x0 + 14 * s, y0 + 28 * s, x1 - 14 * s, y1, sh.banner_is_zone ? ui4::kPink : ui4::kBlue, true);
    const std::string spd =
        format_speed_unit(sh.banner_speed_mph, metric_units, true) + (sh.banner_is_zone ? " AVG" : "");
    const std::string best = "PERSONAL BEST  " + format_speed_unit(sh.banner_best_mph, metric_units, true);
    l.text(cx, y0 + 34 * s, spd, font(ui::Font::kE, 28 * s, ui4::kWhite, Align::kCenter));
    l.text(cx, y0 + 68 * s, best, font(ui::Font::kB, 11 * s, rgba(255, 255, 255, 230), Align::kCenter));
    used = extra_lift + 100.0f;
  }
  return used;
}

// 947_HUD/RaceEncounter: bottom-center Head-to-Head challenge prompt, live bar, and finish banner.
float h2h_hud(ui::List& l, const freeroam::HeadToHeadHud& hh, const Layout& g, bool metric_units, float lift_canvas) {
  const float s = g.s;
  const float cx = 0.5f * g.w;
  float used = 0.0f;
  if (hh.prompt_visible && !hh.active) {
    const float x0 = cx - 230 * s, x1 = cx + 230 * s, y0 = g.B(576 - lift_canvas);
    ui4::brush(l, x0 + 16 * s, y0, x1 - 16 * s, y0 + 26 * s, ui4::kPink);
    l.text(cx, y0 + 5 * s, "HEAD-TO-HEAD CHALLENGE  [X]", font(ui::Font::kE, 14.5f * s, ui4::kWhite, Align::kCenter));
    ui4::pill(l, x0, y0 + 28 * s, x1, y0 + 54 * s, ui4::kPillGrey, ui4::PillKind::kHud01);
    char sub[128];
    std::snprintf(sub, sizeof sub, "VS %s [%s %d]  •  WAGER %s CR", hh.prompt_driver.c_str(),
                  hh.prompt_car_class.c_str(), hh.prompt_pi, thousands(hh.prompt_wager).c_str());
    l.text(cx, y0 + 34 * s, sub, font(ui::Font::kB, 12.5f * s, ui4::kYellow, Align::kCenter));
    used = std::max(used, 64.0f);
  }
  if (hh.active) {
    const float x0 = cx - 230 * s, x1 = cx + 230 * s, y0 = g.B(584 - lift_canvas);
    ui4::pill(l, x0, y0, x1, y0 + 26 * s, ui4::kPink, ui4::PillKind::kHud01);
    char hdr[96], stats[160];
    std::snprintf(hdr, sizeof hdr, "HEAD-TO-HEAD  VS  %s", hh.opponent_name.c_str());
    l.text(cx, y0 + 6 * s, hdr, font(ui::Font::kE, 14.5f * s, ui4::kWhite, Align::kCenter));
    ui4::pill(l, x0, y0 + 28 * s, x1, y0 + 60 * s, ui4::kPillGrey, ui4::PillKind::kHud02);
    const std::string rem_s = format_distance_unit(hh.player_remaining_m, metric_units);
    const std::string gap_s = format_gap_unit(hh.gap_m, metric_units);
    std::snprintf(stats, sizeof stats, "%d%s / 2  •  %s LEFT  •  GAP %s  •  %s CR", hh.place,
                  place_suffix(hh.place), rem_s.c_str(), gap_s.c_str(), thousands(hh.wager_credits).c_str());
    l.text(cx, y0 + 34 * s, stats,
           font(ui::Font::kB, 12.5f * s, hh.place == 1 ? ui4::kGreen : ui4::kYellow, Align::kCenter));
    const float bx0 = x0 + 12 * s, bx1 = x1 - 12 * s, by = y0 + 52 * s;
    l.rect(bx0, by, bx1, by + 4 * s, rgba(255, 255, 255, 35));
    l.rect(bx0, by, bx0 + float(hh.progress) * (bx1 - bx0), by + 4 * s, ui4::kPink);
    used = std::max(used, 68.0f);
  }
  if (hh.finish_banner) {
    const float x0 = cx - 230 * s, x1 = cx + 230 * s, y0 = g.B(564 - lift_canvas), y1 = g.B(638 - lift_canvas);
    Turn t(l, cx, 0.5f * (y0 + y1), ui4::kTiltRad * 0.5f);
    ui4::brush(l, x0, y0, x1, y1, hh.finish_won ? ui4::kPink : ui4::kBrushBlack, true);
    l.text(cx, y0 + 10 * s, hh.finish_won ? "HEAD-TO-HEAD VICTORY!" : "HEAD-TO-HEAD DEFEAT",
           font(ui::Font::kE, 26 * s, kText, Align::kCenter));
    char detail[96];
    if (hh.finish_won)
      std::snprintf(detail, sizeof detail, "BEAT %s   •   +%s CR  +1,000 FAME", hh.finish_opponent.c_str(),
                    thousands(hh.finish_credits).c_str());
    else std::snprintf(detail, sizeof detail, "%s REACHED THE FINISH FIRST", hh.finish_opponent.c_str());
    l.text(cx, y0 + 44 * s, detail, font(ui::Font::kB, 13 * s, ui4::kYellow, Align::kCenter));
    used = std::max(used, 84.0f);
  }
  return used;
}

// 947_HUD/CombinedNotification: bottom-center Sponsor Notification (lifted cleanly above active subtitles/banners).
bool progression_hud(ui::List& l, const DriveHud& st, const Layout& g, float lift_canvas) {
  const float s = g.s;
  const double age = st.time - st.sponsor_at;
  if (st.sponsor_title.empty() || age < 0.0 || age >= 5.5) return false;

  const float fade_a = ease_in_time(age, 0.0, 0.25) * (1.0f - clamp01((age - 4.9) / 0.6));
  const float cx = 0.5f * g.w;
  const float x0 = cx - 196 * s, x1 = cx + 196 * s, y0 = g.B(570 - lift_canvas), y1 = g.B(656 - lift_canvas);

  // Pick matching SPONSORLOGOTILE/*.xds by brand substring, else HorizonLogo/RedBull.
  std::string up = st.sponsor_title;
  for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
  std::string logo_key = "SPONSORLOGOTILE/HORIZONLOGO";
  static constexpr std::pair<const char*, const char*> kBrands[] = {
      {"REDBULL", "SPONSORLOGOTILE/REDBULL"},   {"RED BULL", "SPONSORLOGOTILE/REDBULL"},
      {"OAKLEY", "SPONSORLOGOTILE/OAKLEY"},     {"GOPRO", "SPONSORLOGOTILE/GOPRO"},
      {"HKS", "SPONSORLOGOTILE/HKS"},           {"NIKE", "SPONSORLOGOTILE/NIKE"},
      {"ADIDAS", "SPONSORLOGOTILE/ADIDAS"},     {"BOSE", "SPONSORLOGOTILE/BOSE"},
      {"PUMA", "SPONSORLOGOTILE/PUMA"},         {"ROCKSTAR", "SPONSORLOGOTILE/ROCKSTAR"},
      {"PIONEER", "SPONSORLOGOTILE/PIONEER"},   {"GSHOCK", "SPONSORLOGOTILE/GSHOCK"},
      {"DIESEL", "SPONSORLOGOTILE/DIESEL"},     {"NIXON", "SPONSORLOGOTILE/NIXON"},
      {"ZOO", "SPONSORLOGOTILE/ZOOYORK"},
  };
  for (const auto& [needle, path] : kBrands) {
    if (up.find(needle) != std::string::npos) {
      logo_key = path;
      break;
    }
  }

  // Top #DAFF00 tilted brush banner dynamically sized to wrap header text cleanly.
  {
    const char* hdr_txt = "SPONSOR CHALLENGE UNLOCKED!";
    TextStyle hts = font(ui::Font::kE, 12.5f * s, ui::fade(ui4::kInk, fade_a), Align::kCenter);
    const float hbw = std::max(240 * s, ui::List::measure(hdr_txt, hts) + 46 * s);
    Turn bt(l, cx, y0 + 12 * s, ui4::kTiltRad * 0.4f);
    ui4::brush(l, cx - 0.5f * hbw, y0, cx + 0.5f * hbw, y0 + 24 * s, ui::fade(ui4::kYellow, fade_a));
    l.text(cx, y0 + 5 * s, hdr_txt, hts);
  }

  // Dark HUDPANEL02 backing strip + white square SPONSORLOGOTILE badge on the left.
  std::array<float, 4> panel_uv{}, logo_uv{};
  if (ui_texture("DIRTMASKS/HUD/HUDPANEL02", panel_uv)) {
    l.image_masked(x0, y0 + 24 * s, x1, y1, panel_uv, panel_uv, ui::fade(ui4::kPillGrey, fade_a),
                   {1.0f, 1.0f, 0.0f, 0.0f}, ui::kMaskBorderTransparent);
  } else {
    ui4::panel(l, x0, y0 + 24 * s, x1, y1, s, ui::fade(ui4::kCharcoalDark, fade_a), 0, ui::fade(ui4::kYellow, fade_a),
               true);
  }
  const float tile_sz = 50 * s;
  const float lx0 = x0 + 12 * s, ly0 = y0 + 30 * s;
  l.rect(lx0, ly0, lx0 + tile_sz, ly0 + tile_sz, ui::fade(ui4::kWhite, fade_a));
  if (ui_texture(logo_key, logo_uv) || ui_texture("SPONSORLOGOTILE/OAKLEY", logo_uv) ||
      ui_texture("SPONSORLOGOTILE/BOSE", logo_uv)) {
    l.image(lx0 + 2 * s, ly0 + 2 * s, lx0 + tile_sz - 2 * s, ly0 + tile_sz - 2 * s, logo_uv,
            ui::fade(ui4::kWhite, fade_a));
  }
  const float tx = lx0 + tile_sz + 14 * s;
  TextStyle stt = font(ui::Font::kE, 16 * s, ui::fade(ui4::kWhite, fade_a));
  fit(st.sponsor_title, stt, x1 - tx - 14 * s, 10 * s);
  TextStyle stdt = font(ui::Font::kB, 12.5f * s, ui::fade(ui4::kYellow, fade_a));
  fit(st.sponsor_detail, stdt, x1 - tx - 14 * s, 9 * s);
  l.text(tx, y0 + 34 * s, st.sponsor_title, stt);
  l.text(tx, y0 + 56 * s, st.sponsor_detail, stdt);
  return true;
}


}  // namespace carview::hud_detail
