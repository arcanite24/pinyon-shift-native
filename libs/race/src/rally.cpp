#include "race/rally.h"

#include <algorithm>
#include <cstring>
#include <random>

#include "audio/xml.h"
#include "fh1data/archive.h"
#include "sqlite3.h"

namespace race {

namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

bool read_v4(const std::filesystem::path& state_root, const char* member, std::vector<uint8_t>& out,
             std::string* error) {
  std::string why;
  auto zip = fh1::Archive::open(state_root / "title-update-v4" / "media.zip", &why);
  if (!zip) return fail(error, "title-update-v4/media.zip: " + why);
  auto r = zip->read(member);
  if (!r.ok()) return fail(error, std::string(member) + ": " + r.error);
  out = std::move(r.data);
  return true;
}

std::string_view text(const std::vector<uint8_t>& b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

double StageTrack::best_total() const {
  double s = 0;
  for (double t : best) s += t;
  return s;
}

bool load_stage_track(const std::filesystem::path& state_root, int route_id, StageTrack& out, std::string* error) {
  out = {};
  std::vector<uint8_t> db_bytes;
  if (!read_v4(state_root, "db/gamedb.slt", db_bytes, error)) return false;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) return fail(error, "sqlite");
  auto* copy = static_cast<unsigned char*>(sqlite3_malloc64(db_bytes.size()));
  std::memcpy(copy, db_bytes.data(), db_bytes.size());
  bool ok = sqlite3_deserialize(db, "main", copy, sqlite3_int64(db_bytes.size()), sqlite3_int64(db_bytes.size()),
                                SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE) == SQLITE_OK;
  sqlite3_stmt* st = nullptr;
  if (ok && sqlite3_prepare_v2(db,
                               "select id, Length, StartLine, FinishLine from Tracks where EnvironmentId = 111 and "
                               "RouteId = ? order by id limit 1",
                               -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(st, 1, route_id);
    ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
      out.track_id = sqlite3_column_int64(st, 0);
      out.length = sqlite3_column_double(st, 1);
      out.start_line = sqlite3_column_double(st, 2);
      out.finish_line = sqlite3_column_double(st, 3);
    }
    sqlite3_finalize(st);
  } else {
    ok = false;
  }
  if (ok && sqlite3_prepare_v2(db, "select Split from SplitPoints where TrackId = ? order by Split", -1, &st,
                               nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, out.track_id);
    while (sqlite3_step(st) == SQLITE_ROW) out.splits.push_back(sqlite3_column_double(st, 0));
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  if (!ok) return fail(error, "v4 gamedb: no ColoradoDirt track on route " + std::to_string(route_id));
  out.route_id = route_id;

  std::vector<uint8_t> timings;
  if (!read_v4(state_root, "gametunablesettings/rallytracktimings.xml", timings, error)) return false;
  audio::XmlNode root;
  if (!audio::parse_xml(text(timings), root, error)) return false;
  for (const auto& t : root.children)
    if (audio::iequals(t.name, "Track") && int64_t(t.number("Id")) == out.track_id)
      for (const auto& s : t.children) out.best.push_back(s.number("Time"));
  if (out.best.empty()) return fail(error, "rallytracktimings.xml: no track " + std::to_string(out.track_id));
  return true;
}

bool parse_rally_field(std::string_view xml, RallyField& out, std::string* error) {
  out = {};
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  auto four = [](const audio::XmlNode& n) {
    std::array<std::pair<double, double>, 4> a{};
    size_t k = 0;
    for (const auto& d : n.children)
      if (audio::iequals(d.name, "Difficulty") && k < 4) a[k++] = {d.number("Min"), d.number("Max")};
    return a;
  };
  if (const auto* g = root.child("GlobalDifficulty")) out.global = four(*g);
  if (const auto* o = root.child("TrackDifficultyOverride"))
    for (const auto& t : o->children)
      if (audio::iequals(t.name, "Track")) out.overrides.push_back({int64_t(t.number("Id")), four(t)});
  if (const auto* d = root.child("AIDrivers")) {
    out.skill_variance = d->number("SkillVariance");
    for (const auto& a : d->children)
      if (audio::iequals(a.name, "AIDriver")) out.drivers.push_back({int(a.number("AIPlayerId")), a.number("Skill")});
  }
  if (out.drivers.empty()) return fail(error, "rallyaidifficulty.xml: no AIDriver");
  return true;
}

bool load_rally_field(const std::filesystem::path& state_root, RallyField& out, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!read_v4(state_root, "gametunablesettings/rallyaidifficulty.xml", bytes, error)) return false;
  return parse_rally_field(text(bytes), out, error);
}

std::pair<double, double> RallyField::range(int64_t track_id, int difficulty) const {
  const size_t d = size_t(std::clamp(difficulty, 0, 3));
  for (const auto& [id, a] : overrides)
    if (id == track_id) return a[d];
  return global[d];
}

std::vector<double> field_times(const RallyField& field, const StageTrack& track, int difficulty, uint64_t seed) {
  std::mt19937_64 rng(seed ^ (uint64_t(track.track_id) * 0x9E3779B97F4A7C15ull));
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  const auto [lo, hi] = field.range(track.track_id, difficulty);
  std::vector<double> out;
  out.reserve(field.drivers.size());
  for (const auto& [id, skill] : field.drivers) {
    const double q = std::clamp(skill + u(rng) * field.skill_variance, 0.0, 1.0);
    const double f = lo + (hi - lo) * q;
    out.push_back(f > 0 ? track.best_total() / f : 1e9);
  }
  return out;
}

int rally_place(const std::vector<double>& field, double player_time) {
  int place = 1;
  for (double t : field) place += t < player_time;
  return place;
}

}  // namespace race
