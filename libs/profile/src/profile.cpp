#include "profile/profile.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace profile {
namespace {

// A big-endian reader over the body; every read checks the end.
struct Reader {
  std::span<const uint8_t> b;
  size_t at = 0;
  bool ok = true;
  size_t left() const { return at <= b.size() ? b.size() - at : 0; }
  bool need(size_t n) {
    if (left() < n) ok = false;
    return ok;
  }
  uint64_t be(size_t n) {
    if (!need(n)) return 0;
    uint64_t v = 0;
    for (size_t k = 0; k < n; ++k) v = (v << 8) | b[at + k];
    at += n;
    return v;
  }
  uint32_t u32() { return uint32_t(be(4)); }
  std::string str(size_t n) {
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(b.data() + at), n);
    at += n;
    return s;
  }
};

bool read_fields(Reader& r, uint32_t count, std::vector<Field>& out, int depth, std::string* error) {
  if (depth > 16) {
    if (error) *error = "profile body: fields nested too deep";
    return false;
  }
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t len = r.u32();
    if (!r.ok || len == 0 || len > 256) {
      if (error) *error = "profile body: bad field name length at byte " + std::to_string(r.at);
      return false;
    }
    Field f;
    f.name = r.str(len);
    const uint32_t w0 = r.u32(), w1 = r.u32();
    if (!r.ok || w0 != 0x20 || w1 != 0) {
      if (error) *error = "profile body: field " + f.name + " has no 0x20, 0 words";
      return false;
    }
    const uint8_t type = uint8_t(r.be(1));
    switch (type) {
      case Value::kBool:
      case Value::kU8: f.value.bits = r.be(1); break;
      case Value::kU32:
      case Value::kI32:
      case Value::kFloat: f.value.bits = r.be(4); break;
      case Value::kU64: f.value.bits = r.be(8); break;
      case Value::kStruct: {
        const uint32_t n = r.u32();
        f.value.type = Value::kStruct;
        if (!r.ok || !read_fields(r, n, f.value.fields, depth + 1, error)) return false;
        break;
      }
      default:
        if (error) *error = "profile body: field " + f.name + " has type byte " + std::to_string(type);
        return false;
    }
    f.value.type = Value::Type(type);
    if (!r.ok) {
      if (error) *error = "profile body: field " + f.name + " runs past the end";
      return false;
    }
    out.push_back(std::move(f));
  }
  return true;
}

bool read_record(Reader& r, Record& out) {
  out.name = r.str(r.u32());
  out.class_name = r.str(r.u32());
  out.version = r.u32();
  const uint32_t size = r.u32();
  if (!r.need(size)) return false;
  out.payload.assign(r.b.begin() + std::ptrdiff_t(r.at), r.b.begin() + std::ptrdiff_t(r.at + size));
  r.at += size;
  return r.ok;
}

// A parent's payload: a u32 child count, then the children.
bool read_children(Record& parent) {
  Reader c{parent.payload};
  const uint32_t n = c.u32();
  for (uint32_t k = 0; k < n && c.ok; ++k) {
    Record child;
    if (!read_record(c, child)) return false;
    parent.children.push_back(std::move(child));
  }
  return c.ok && c.left() == 0;
}

std::vector<uint8_t> read_file(const std::filesystem::path& p, bool& ok) {
  std::ifstream f(p, std::ios::binary);
  ok = bool(f);
  if (!ok) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
  std::vector<std::string> out;
  std::ifstream f(p);
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

struct Db {
  sqlite3* db = nullptr;
  ~Db() {
    if (db) sqlite3_close(db);
  }
  bool each(const std::string& sql, const std::function<void(sqlite3_stmt*)>& row, std::string* error) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
      if (error) *error = "PlayerDatabase: " + std::string(sqlite3_errmsg(db)) + " (" + sql + ")";
      return false;
    }
    while (sqlite3_step(st) == SQLITE_ROW) row(st);
    sqlite3_finalize(st);
    return true;
  }
};

}  // namespace

int64_t Value::as_int() const {
  switch (type) {
    case kI32: return int64_t(int32_t(uint32_t(bits)));
    case kFloat: return int64_t(as_double());
    default: return int64_t(bits);
  }
}

double Value::as_double() const {
  if (type == kFloat) {
    const uint32_t u = uint32_t(bits);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  return double(as_int());
}

bool parse_body(std::span<const uint8_t> bytes, Body& out, std::string* error) {
  out = Body{};
  Reader r{bytes};
  const uint32_t top = r.u32();
  if (!r.ok || !read_fields(r, top, out.fields, 0, error)) {
    if (error && error->empty()) *error = "profile body: too short";
    return false;
  }
  // The record blocks, to the end or the 0xBB padding.
  while (r.left() >= 8) {
    const uint32_t peek = uint32_t(r.b[r.at]) << 24 | uint32_t(r.b[r.at + 1]) << 16 | uint32_t(r.b[r.at + 2]) << 8 | r.b[r.at + 3];
    if (peek == 0xBBBBBBBBu) break;
    Block blk;
    blk.version = r.u32();
    const uint32_t n = r.u32();
    for (uint32_t k = 0; k < n; ++k) {
      Record rec;
      if (!read_record(r, rec)) {
        if (error) *error = "profile body: record " + std::to_string(k) + " of block " + std::to_string(out.blocks.size()) + " runs past the end";
        return false;
      }
      if (rec.class_name == "CHorizonLocationState" && !read_children(rec)) {
        if (error) *error = "profile body: " + rec.name + "'s children do not parse";
        return false;
      }
      blk.records.push_back(std::move(rec));
    }
    out.blocks.push_back(std::move(blk));
  }
  return true;
}

const Field* find(const std::vector<Field>& fields, std::string_view path) {
  const auto slash = path.find('/');
  const std::string_view head = path.substr(0, slash);
  for (const auto& f : fields)
    if (f.name == head) {
      if (slash == std::string_view::npos) return &f;
      return f.value.type == Value::kStruct ? find(f.value.fields, path.substr(slash + 1)) : nullptr;
    }
  return nullptr;
}

size_t field_count(const std::vector<Field>& fields, std::string_view path) {
  const Field* f = find(fields, path);
  return f && f->value.type == Value::kStruct ? f->value.fields.size() : 0;
}

size_t Imported::signs_found() const {
  return size_t(std::count_if(signs.begin(), signs.end(), [](const Sign& s) { return s.found; }));
}

size_t Imported::barns_found() const {
  return size_t(std::count_if(barns.begin(), barns.end(), [](const Barn& b) { return b.status == 2; }));
}

const GarageCar* Imported::selected_car() const {
  const GarageCar* lowest = nullptr;
  for (const auto& g : garage) {
    if (g.id == career_car) return &g;
    if (!lowest || g.id < lowest->id) lowest = &g;
  }
  return lowest;
}

bool import_profile(const std::filesystem::path& folder, Imported& out, std::string* error) {
  out = Imported{};
  bool ok = false;
  const auto body = read_file(folder / "ForzaProfile.body", ok);
  if (!ok) {
    if (error) *error = (folder / "ForzaProfile.body").string() + ": missing";
    return false;
  }
  if (!parse_body(body, out.body, error)) return false;
  // VersionFlags (export.json's, from the profile folder's file; missing: base).
  {
    bool have = false;
    const auto j = read_file(folder / "export.json", have);
    const std::string s(j.begin(), j.end());
    const auto at = s.find("\"VersionFlags\"");
    if (at != std::string::npos) {
      const auto colon = s.find(':', at);
      if (colon != std::string::npos) out.version_flags = uint32_t(std::strtoul(s.c_str() + colon + 1, nullptr, 10));
    }
  }
  const auto& fs = out.body.fields;
  out.main_fields = field_count(fs, "Main");
  auto num = [&](const char* name) -> int64_t {
    const Field* f = find(fs, std::string("Main/") + name);
    return f ? f->value.as_int() : 0;
  };
  auto real = [&](const char* name) -> double {
    const Field* f = find(fs, std::string("Main/") + name);
    return f ? f->value.as_double() : 0;
  };
  out.credits = num("Credits"), out.xp = num("XP"), out.wristband_level = num("WristbandLevel");
  out.level = num("Level"), out.career_car = num("CareerCar"), out.upgrade_discount = num("UpgradeDiscount");
  out.total_winnings = num("TotalWinnings"), out.num_races = num("NumRaces");
  out.num_victories = num("NumVictories"), out.num_podiums = num("NumPodiums");
  out.top_speed = real("TopSpeed"), out.distance_driven = real("TotalDistanceDriven");
  out.time_festival = real("TimeInFestivalRaces"), out.time_street = real("TimeInStreetRaces");
  out.time_exhibition = real("TimeInExhibitionRaces");
  out.rally_fields = find(fs, "Main/NumRallies") != nullptr;
  out.num_rallies = num("NumRallies"), out.num_rally_stages = num("NumRallyStages");

  // The records: signs and barns under Colorado, the 1000 Club.
  for (const auto& blk : out.body.blocks)
    for (const auto& rec : blk.records) {
      if (rec.name == "Colorado" && rec.class_name == "CHorizonLocationState") {
        for (const auto& c : rec.children) {
          if (c.class_name == "CFlyerState" && c.payload.size() >= 2)
            out.signs.push_back({c.name, c.payload[0] != 0, c.payload[1] != 0});
          if (c.class_name == "CBarnFindState" && c.payload.size() >= 15) {
            Barn b;
            b.name = c.name;
            b.revealed = c.payload[0] != 0, b.discovered = c.payload[1] != 0, b.collected = c.payload[2] != 0;
            auto f32 = [&](size_t at) {
              const uint32_t u = uint32_t(c.payload[at]) << 24 | uint32_t(c.payload[at + 1]) << 16 |
                                 uint32_t(c.payload[at + 2]) << 8 | c.payload[at + 3];
              float f;
              std::memcpy(&f, &u, 4);
              return f;
            };
            // found first, then the rumour (the rumour is the earlier on every
            // found barn, and 0 found on a rumoured one: ours, from the saves)
            b.found_at = f32(3), b.rumour_at = f32(7);
            b.status = uint32_t(c.payload[11]) << 24 | uint32_t(c.payload[12]) << 16 | uint32_t(c.payload[13]) << 8 | c.payload[14];
            out.barns.push_back(b);
          }
        }
      }
      if (rec.class_name == "CCarChallengeState") {
        Reader r{rec.payload};
        const uint32_t m = r.u32();
        for (uint32_t k = 0; k < m && r.ok; ++k) {
          ClubCar car;
          car.car_id = r.u32();
          const uint32_t c = r.u32();
          for (uint32_t j = 0; j < c && r.ok; ++j) {
            r.u32();  // the challenge's key (an opaque hash)
            const bool a = r.be(1) != 0;
            r.be(1);  // flag B: the online confirmation
            const uint32_t blob = r.u32();
            if (r.need(blob)) r.at += blob;
            ++car.challenges;
            car.done += a;
          }
          if (r.ok) out.club.push_back(car);
        }
      }
    }

  // PlayerDatabase.
  const auto db_path = folder / "PlayerDatabase.sqlite";
  if (!std::filesystem::exists(db_path)) {
    if (error) *error = db_path.string() + ": missing";
    return false;
  }
  Db db;
  if (sqlite3_open_v2(db_path.string().c_str(), &db.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (error) *error = db_path.string() + ": cannot open";
    return false;
  }
  if (!db.each("select * from Career_Garage order by Id",
               [&](sqlite3_stmt* s) {
                 GarageCar g;
                 for (int c = 0; c < sqlite3_column_count(s); ++c)
                   if (sqlite3_column_type(s, c) == SQLITE_INTEGER)
                     g.columns[sqlite3_column_name(s, c)] = sqlite3_column_int64(s, c);
                   else if (sqlite3_column_type(s, c) == SQLITE_FLOAT)
                     g.reals[sqlite3_column_name(s, c)] = sqlite3_column_double(s, c);
                 auto col = [&](const char* k) { return g.columns.count(k) ? g.columns[k] : 0; };
                 g.id = col("Id"), g.car_id = col("CarId"), g.performance_index = g.reals.count("PerformanceIndex") ? g.reals["PerformanceIndex"] : double(col("PerformanceIndex"));
                 g.class_id = col("ClassID"), g.color_index = col("DefaultManufacturerColorIndex");
                 out.garage.push_back(std::move(g));
               },
               error) ||
      !db.each("select GarageId, PartEnum, PartId, ManufacturerId, PricePaid from Career_PurchasedParts",
               [&](sqlite3_stmt* s) {
                 out.parts.push_back({sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2),
                                      sqlite3_column_int64(s, 3), sqlite3_column_int64(s, 4)});
               },
               error) ||
      !db.each("select EventId, Place from Career_EventResults",
               [&](sqlite3_stmt* s) { out.event_results.push_back({sqlite3_column_int64(s, 0), sqlite3_column_int(s, 1)}); },
               error) ||
      !db.each("select RaceId, Place from Career_RaceResults",
               [&](sqlite3_stmt* s) { out.race_results.push_back({sqlite3_column_int64(s, 0), sqlite3_column_int(s, 1)}); },
               error) ||
      !db.each("select EventId from CareerEventUnlocks",
               [&](sqlite3_stmt* s) { out.event_unlocks.push_back(sqlite3_column_int64(s, 0)); }, error) ||
      !db.each("select HubId from CareerHubUnlocks", [&](sqlite3_stmt* s) { out.hub_unlocks.push_back(sqlite3_column_int64(s, 0)); },
               error) ||
      !db.each("select * from LastUsedGarageId",
               [&](sqlite3_stmt* s) { out.last_used_garage_id = sqlite3_column_int64(s, 0); }, error))
    return false;

  // Content packages: listed folders (with a header) and disabled ones.
  out.packages = read_lines(folder / "content-packages.txt");
  // The disabled listing can hold the content-type folder itself
  // (Disabled/00000002, V4_CONTENT), which is not a package.
  for (auto& name : read_lines(folder / "content-disabled.txt"))
    if (name != "00000002" && name != "0000000000000002") out.disabled_packages.push_back(name);
  return true;
}

std::map<std::string, int64_t> summary(const Imported& p) {
  std::map<std::string, int64_t> s;
  s["main_fields"] = int64_t(p.main_fields);
  s["tail_blocks"] = int64_t(p.body.blocks.size());
  for (size_t k = 0; k < p.body.blocks.size(); ++k)
    s["tail_blocks_" + std::to_string(k)] = int64_t(p.body.blocks[k].records.size());
  s["version_flags"] = p.version_flags;
  s["signs"] = int64_t(p.signs.size());
  s["signs_found"] = int64_t(p.signs_found());
  s["barns"] = int64_t(p.barns.size());
  s["barns_found"] = int64_t(p.barns_found());
  s["upgrade_discount"] = p.upgrade_discount;
  s["wristband_level"] = p.wristband_level;
  s["rally_fields"] = p.rally_fields;
  if (p.rally_fields) s["num_rallies"] = p.num_rallies, s["num_rally_stages"] = p.num_rally_stages;
  if (!p.club.empty()) {
    int64_t c = 0, a = 0;
    for (const auto& car : p.club) c += car.challenges, a += car.done;
    s["car_challenge_cars"] = int64_t(p.club.size());
    s["car_challenges"] = c;
    s["car_challenges_flag_a"] = a;
  }
  s["garage"] = int64_t(p.garage.size());
  std::set<int64_t> models, with_parts;
  int64_t max_id = 0;
  for (const auto& g : p.garage) models.insert(g.car_id), max_id = std::max(max_id, g.id);
  s["garage_models"] = int64_t(models.size());
  s["purchased_parts"] = int64_t(p.parts.size());
  for (const auto& part : p.parts) with_parts.insert(part.garage_id);
  s["cars_with_parts"] = int64_t(with_parts.size());
  s["event_results"] = int64_t(p.event_results.size());
  s["events_won"] = std::count_if(p.event_results.begin(), p.event_results.end(), [](const auto& r) { return r.second == 1; });
  s["race_results"] = int64_t(p.race_results.size());
  s["event_unlocks"] = int64_t(p.event_unlocks.size());
  s["hub_unlocks"] = int64_t(p.hub_unlocks.size());
  s["last_used_garage_id_is_max"] = p.last_used_garage_id == max_id;
  bool owned = false;
  for (const auto& g : p.garage) owned = owned || g.id == p.career_car;
  s["selected_car_owned"] = owned;
  s["content_packages"] = int64_t(p.packages.size());
  s["content_packages_disabled"] = int64_t(p.disabled_packages.size());
  return s;
}

}  // namespace profile
