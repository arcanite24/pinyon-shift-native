#include "profile/profile.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <system_error>

#include "cars/physics.h"
#include "drive/save.h"

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

struct Writer {
  std::vector<uint8_t> out;
  void u8(uint8_t v) { out.push_back(v); }
  void u32(uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
  }
  void u64(uint64_t v) {
    u32(uint32_t(v >> 32));
    u32(uint32_t(v & 0xFFFFFFFFu));
  }
  void bytes(const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); }
  void str(std::string_view s) {
    u32(uint32_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
};

void write_fields(Writer& w, const std::vector<Field>& fields) {
  w.u32(uint32_t(fields.size()));
  for (const auto& f : fields) {
    w.str(f.name);
    w.u32(0x20u);
    w.u32(0u);
    w.u8(uint8_t(f.value.type));
    switch (f.value.type) {
      case Value::kBool:
      case Value::kU8: w.u8(uint8_t(f.value.bits & 0xFFu)); break;
      case Value::kU32:
      case Value::kI32:
      case Value::kFloat: w.u32(uint32_t(f.value.bits & 0xFFFFFFFFu)); break;
      case Value::kU64: w.u64(f.value.bits); break;
      case Value::kStruct: write_fields(w, f.value.fields); break;
    }
  }
}

void write_record(Writer& w, const Record& rec) {
  w.str(rec.name);
  w.str(rec.class_name);
  w.u32(rec.version);
  if (rec.class_name == "CHorizonLocationState") {
    Writer cw;
    cw.u32(uint32_t(rec.children.size()));
    for (const auto& child : rec.children) write_record(cw, child);
    w.u32(uint32_t(cw.out.size()));
    w.bytes(cw.out.data(), cw.out.size());
  } else {
    w.u32(uint32_t(rec.payload.size()));
    w.bytes(rec.payload.data(), rec.payload.size());
  }
}

uint32_t float_bits(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  return u;
}

Field& ensure_field(std::vector<Field>& fields, std::string_view path, Value::Type leaf_type) {
  const auto slash = path.find('/');
  const std::string_view head = path.substr(0, slash);
  for (auto& f : fields) {
    if (f.name == head) {
      if (slash == std::string_view::npos) return f;
      if (f.value.type != Value::kStruct) {
        f.value.type = Value::kStruct;
        f.value.bits = 0;
        f.value.fields.clear();
      }
      return ensure_field(f.value.fields, path.substr(slash + 1), leaf_type);
    }
  }
  Field created;
  created.name = std::string(head);
  if (slash == std::string_view::npos) {
    created.value.type = leaf_type;
    fields.push_back(std::move(created));
    return fields.back();
  }
  created.value.type = Value::kStruct;
  fields.push_back(std::move(created));
  return ensure_field(fields.back().value.fields, path.substr(slash + 1), leaf_type);
}

bool is_valid_sql_ident(std::string_view s) {
  if (s.empty() || s.size() > 128) return false;
  if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return true;
}

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
  if (r.at < bytes.size()) {
    out.trailing_bytes.assign(bytes.begin() + std::ptrdiff_t(r.at), bytes.end());
  }
  return true;
}

std::vector<uint8_t> serialize_body(const Body& body) {
  Writer w;
  write_fields(w, body.fields);
  for (const auto& blk : body.blocks) {
    w.u32(blk.version);
    w.u32(uint32_t(blk.records.size()));
    for (const auto& rec : blk.records) write_record(w, rec);
  }
  if (!body.trailing_bytes.empty()) {
    w.bytes(body.trailing_bytes.data(), body.trailing_bytes.size());
  }
  return w.out;
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

Field* find_mut(std::vector<Field>& fields, std::string_view path) {
  const auto slash = path.find('/');
  const std::string_view head = path.substr(0, slash);
  for (auto& f : fields)
    if (f.name == head) {
      if (slash == std::string_view::npos) return &f;
      return f.value.type == Value::kStruct ? find_mut(f.value.fields, path.substr(slash + 1)) : nullptr;
    }
  return nullptr;
}

void set_field_int(std::vector<Field>& fields, std::string_view path, int64_t value, Value::Type default_type) {
  Field& f = ensure_field(fields, path, default_type);
  switch (f.value.type) {
    case Value::kBool: f.value.bits = value != 0 ? 1u : 0u; break;
    case Value::kU8: f.value.bits = uint64_t(uint8_t(value)); break;
    case Value::kU32: f.value.bits = uint64_t(uint32_t(value)); break;
    case Value::kI32: f.value.bits = uint64_t(uint32_t(int32_t(value))); break;
    case Value::kU64: f.value.bits = uint64_t(value); break;
    case Value::kFloat: f.value.bits = float_bits(float(value)); break;
    case Value::kStruct:
      f.value.type = default_type;
      f.value.fields.clear();
      f.value.bits = uint64_t(uint32_t(value));
      break;
  }
}

void set_field_double(std::vector<Field>& fields, std::string_view path, double value, Value::Type default_type) {
  Field& f = ensure_field(fields, path, default_type);
  if (f.value.type == Value::kFloat) {
    f.value.bits = float_bits(float(value));
  } else {
    set_field_int(fields, path, int64_t(std::llround(value)), default_type);
  }
}

void set_field_bool(std::vector<Field>& fields, std::string_view path, bool value) {
  Field& f = ensure_field(fields, path, Value::kBool);
  if (f.value.type == Value::kFloat) {
    f.value.bits = float_bits(value ? 1.0f : 0.0f);
  } else {
    f.value.bits = value ? 1u : 0u;
  }
}

size_t field_count(const std::vector<Field>& fields, std::string_view path) {
  const Field* f = find(fields, path);
  return f && f->value.type == Value::kStruct ? f->value.fields.size() : 0;
}

// ---- SponsorshipState (CHorizonSponsorshipState v2) ----

namespace {

constexpr size_t kSponsorshipHeader = 20;  // u32 points, 4-byte word, f32, u32 2, u32 child count
constexpr const char* kSponsorChildClass = "CSponsorshipChallengeState";

// The 15 SponsorshipChallenges.xml ids, in the order both reference saves
// store them (ASCII order).
constexpr const char* kSponsorIds[] = {"Air",       "AnySkill", "CleanSpeed", "Daredevil", "Draft",
                                       "Drift",     "EDrift",   "Fame",       "Infection", "King",
                                       "NearMiss",  "Over9000", "Pass",       "SideSwipe", "SlingShot"};

void put_be32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
  out[at] = uint8_t(v >> 24), out[at + 1] = uint8_t(v >> 16), out[at + 2] = uint8_t(v >> 8), out[at + 3] = uint8_t(v);
}

struct SponsorChildSpan {
  std::string name, class_name;
  size_t payload_at = 0, payload_size = 0;
};

// Walks the header and the children; false when the payload does not parse.
bool walk_sponsorship(std::span<const uint8_t> payload, uint32_t& points, std::vector<SponsorChildSpan>& children) {
  Reader r{payload};
  points = r.u32();
  r.u32(), r.u32(), r.u32();
  const uint32_t n = r.u32();
  if (!r.ok || n > 4096) return false;
  for (uint32_t k = 0; k < n; ++k) {
    SponsorChildSpan c;
    c.name = r.str(r.u32());
    c.class_name = r.str(r.u32());
    r.u32();  // version
    const uint32_t size = r.u32();
    if (!r.ok || !r.need(size)) return false;
    c.payload_at = r.at, c.payload_size = size;
    r.at += size;
    children.push_back(std::move(c));
  }
  return r.ok && r.left() == 0;
}

void append_sponsor_child(std::vector<uint8_t>& out, const SponsorshipChallenge& c) {
  Writer w;
  w.str(c.id);
  w.str(kSponsorChildClass);
  w.u32(1);
  w.u32(8);
  w.u32(c.tiers_claimed);
  w.u32(c.count);
  out.insert(out.end(), w.out.begin(), w.out.end());
}

}  // namespace

const SponsorshipChallenge* SponsorshipState::find(std::string_view id) const {
  for (const auto& c : challenges)
    if (c.id == id) return &c;
  return nullptr;
}

bool decode_sponsorship_state(std::span<const uint8_t> payload, SponsorshipState& out) {
  out = SponsorshipState{};
  std::vector<SponsorChildSpan> children;
  if (!walk_sponsorship(payload, out.popularity_points, children)) return false;
  for (const auto& c : children) {
    if (c.class_name != kSponsorChildClass || c.payload_size < 8) continue;
    Reader r{payload.subspan(c.payload_at, c.payload_size)};
    SponsorshipChallenge sc;
    sc.id = c.name;
    sc.tiers_claimed = r.u32();
    sc.count = r.u32();
    out.challenges.push_back(std::move(sc));
  }
  return true;
}

uint32_t popularity_points_from_sponsorship_state(std::span<const uint8_t> payload) {
  SponsorshipState st;
  return decode_sponsorship_state(payload, st) ? st.popularity_points : 0;
}

std::vector<uint8_t> encode_sponsorship_state(std::span<const uint8_t> payload, const SponsorshipState& st) {
  uint32_t points = 0;
  std::vector<SponsorChildSpan> children;
  std::vector<uint8_t> out;
  if (!payload.empty() && !walk_sponsorship(payload, points, children)) {
    // A layout this reader does not know (another version): kept as is.
    return std::vector<uint8_t>(payload.begin(), payload.end());
  }
  if (payload.empty()) {
    // A fresh record: the reference saves' header words and every id.
    std::vector<SponsorshipChallenge> all;
    for (const char* id : kSponsorIds) {
      SponsorshipChallenge c;
      c.id = id;
      if (const SponsorshipChallenge* have = st.find(id)) c = *have;
      all.push_back(std::move(c));
    }
    for (const auto& c : st.challenges)
      if (std::none_of(all.begin(), all.end(), [&](const SponsorshipChallenge& a) { return a.id == c.id; }))
        all.push_back(c);
    Writer w;
    w.u32(st.popularity_points);
    w.u32(0);
    w.u32(0);  // f32 0.0
    w.u32(2);
    w.u32(uint32_t(all.size()));
    out = std::move(w.out);
    for (const auto& c : all) append_sponsor_child(out, c);
    return out;
  }
  out.assign(payload.begin(), payload.end());
  put_be32(out, 0, st.popularity_points);
  std::set<std::string> seen;
  for (const auto& c : children) {
    seen.insert(c.name);
    if (c.class_name != kSponsorChildClass || c.payload_size < 8) continue;
    if (const SponsorshipChallenge* want = st.find(c.name)) {
      put_be32(out, c.payload_at, want->tiers_claimed);
      put_be32(out, c.payload_at + 4, want->count);
    }
  }
  uint32_t n = uint32_t(children.size());
  for (const auto& c : st.challenges)
    if (!seen.count(c.id)) {
      append_sponsor_child(out, c);
      seen.insert(c.id);
      ++n;
    }
  put_be32(out, kSponsorshipHeader - 4, n);  // the child count
  return out;
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
  // VersionFlags (export.json's, from the profile folder's file; fallback to top-level field; missing: base).
  {
    bool have = false;
    const auto j = read_file(folder / "export.json", have);
    if (have) {
      const std::string s(j.begin(), j.end());
      const auto at = s.find("\"VersionFlags\"");
      if (at != std::string::npos) {
        const auto colon = s.find(':', at);
        if (colon != std::string::npos) out.version_flags = uint32_t(std::strtoul(s.c_str() + colon + 1, nullptr, 10));
      }
    } else if (const Field* vf = find(out.body.fields, "VersionFlags")) {
      out.version_flags = uint32_t(vf->value.as_int());
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
  out.last_rewarded_level = num("LastRewardedLevel");
  // Main has no popularity or driver XP field: popularity is read from the
  // SponsorshipState record below.
  out.max_skills_score = num("MaxSkillsScore"), out.max_skills_car = num("MaxSkillsCarOrdinal");
  out.top_speed = real("TopSpeed"), out.distance_driven = real("TotalDistanceDriven");
  out.time_festival = real("TimeInFestivalRaces"), out.time_street = real("TimeInStreetRaces");
  out.time_exhibition = real("TimeInExhibitionRaces");
  out.rally_fields = find(fs, "Main/NumRallies") != nullptr;
  out.num_rallies = num("NumRallies"), out.num_rally_stages = num("NumRallyStages");
  out.num_rally_victories = num("NumRallyVictories"), out.rally_winnings = num("RallyWinnings");
  out.rally_xp = num("RallyXP"), out.rally_level = num("RallyWristbandLevel");
  if (const Field* sn = find(fs, "Main/SatNav")) out.satnav_on = sn->value.as_int() != 0;
  if (const Field* ft = find(fs, "Main/FastTravelAnywhere")) out.fast_travel_anywhere = ft->value.as_int() != 0;
  if (const Field* cd = find(fs, "Main/CoDriver")) {
    out.has_codriver = true;
    out.codriver_on = cd->value.as_int() != 0;
  }

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
          if (c.class_name == "CRoadDiscovery" && c.payload.size() >= 4) {
            Reader rr{c.payload};
            out.road_discovery_nodes = rr.u32();
            const size_t nwords = (c.payload.size() - 4) / 4;
            out.road_discovery_words.resize(nwords);
            for (size_t wi = 0; wi < nwords; ++wi) out.road_discovery_words[wi] = rr.u32();
          }
          if (c.class_name == "CFirstTimeCareerState" && c.payload.size() >= 5) {
            Reader fr{c.payload};
            out.has_first_time_career = true;
            out.first_time_career_flag = uint8_t(fr.be(1));
            out.first_time_career_stage = fr.u32();
          }
          if (c.class_name == "CGasStationState" && !c.payload.empty() && c.payload[0] != 0) {
            out.outposts_discovered.push_back(c.name);
          }
          if (c.class_name == "CMissionState" && c.payload.size() >= 2 && (c.payload[0] != 0 || c.payload[1] != 0)) {
            out.pr_stunts_completed.push_back(c.name);
          }
          // Speed traps (CSpeedCameraState) and speed zones (retail CAverageSpeedState,
          // with CSpeedZoneState retained as secondary fallback).
          const bool is_speed_trap = (c.class_name == "CSpeedCameraState");
          const bool is_speed_zone = (c.class_name == "CAverageSpeedState" || c.class_name == "CSpeedZoneState");
          if ((is_speed_trap || is_speed_zone) && c.payload.size() >= 5) {
            auto f32 = [&](size_t at) {
              const uint32_t u = uint32_t(c.payload[at]) << 24 | uint32_t(c.payload[at + 1]) << 16 |
                                 uint32_t(c.payload[at + 2]) << 8 | c.payload[at + 3];
              float f = 0.0f;
              std::memcpy(&f, &u, 4);
              return double(f);
            };
            auto valid_speed = [](double v) { return std::isfinite(v) && v > 1.0 && v < 400.0; };
            double val = 0.0;
            if (c.payload.size() >= 8 && valid_speed(f32(4))) val = f32(4);
            else if (c.payload.size() >= 6 && valid_speed(f32(2))) val = f32(2);
            else if (valid_speed(f32(1))) val = f32(1);
            if (valid_speed(val)) {
              if (is_speed_trap) out.speed_traps.emplace_back(c.name, val);
              else out.speed_zones.emplace_back(c.name, val);
            }
          }
        }
      }
      if (!out.has_sponsorship && rec.name == "SponsorshipState" && rec.class_name == "CHorizonSponsorshipState") {
        SponsorshipState st;
        if (decode_sponsorship_state(rec.payload, st)) {
          out.has_sponsorship = true;
          out.popularity = double(popularity_points_from_sponsorship_state(rec.payload));
          out.sponsors = st.challenges;
          if (const SponsorshipChallenge* fame = st.find("Fame")) out.fame_tiers_claimed = fame->tiers_claimed;
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
            const uint32_t key = r.u32();  // an opaque hash of the definition
            const bool a = r.be(1) != 0;
            const bool b_flag = r.be(1) != 0;  // flag B: the online confirmation
            const uint32_t blob = r.u32();
            std::vector<int> prog_vals;
            std::vector<uint8_t> raw_blob;
            if (r.need(blob)) {
              const size_t start_at = r.at;
              raw_blob.assign(r.b.begin() + std::ptrdiff_t(start_at), r.b.begin() + std::ptrdiff_t(start_at + blob));
              // Retail 1000 Club save_progress nodes are serialized with an 8-byte stride.
              if (blob >= 8 && (blob % 8) == 0 && blob <= 256) {
                for (uint32_t off = 0; off < blob; off += 8) {
                  const uint32_t hi = uint32_t(r.b[start_at + off]) << 24 | uint32_t(r.b[start_at + off + 1]) << 16 |
                                      uint32_t(r.b[start_at + off + 2]) << 8 | r.b[start_at + off + 3];
                  const uint32_t lo = uint32_t(r.b[start_at + off + 4]) << 24 |
                                      uint32_t(r.b[start_at + off + 5]) << 16 | uint32_t(r.b[start_at + off + 6]) << 8 |
                                      r.b[start_at + off + 7];
                  const uint64_t u64 = (uint64_t(hi) << 32) | uint64_t(lo);
                  if (u64 < 1000000ull) {
                    prog_vals.push_back(int(u64));
                  } else if (hi < 1000000u && lo == 0u) {
                    prog_vals.push_back(int(hi));
                  } else if (lo < 1000000u) {
                    prog_vals.push_back(int(lo));
                  }
                }
              }
              r.at = start_at + blob;
            }
            ++car.challenges;
            car.done += a;
            car.keys.emplace_back(key, a);
            car.flags_b.push_back(b_flag);
            car.progress.push_back(std::move(prog_vals));
            car.raw_blobs.push_back(std::move(raw_blob));
          }
          if (r.ok) out.club.push_back(std::move(car));
        }
        if (r.ok && r.left() > 0) {
          out.club_trailing.assign(rec.payload.begin() + std::ptrdiff_t(r.at), rec.payload.end());
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
                 for (int c = 0; c < sqlite3_column_count(s); ++c) {
                   const int ctype = sqlite3_column_type(s, c);
                   const char* cname = sqlite3_column_name(s, c);
                   if (!cname) continue;
                   if (ctype == SQLITE_INTEGER)
                     g.columns[cname] = sqlite3_column_int64(s, c);
                   else if (ctype == SQLITE_FLOAT)
                     g.reals[cname] = sqlite3_column_double(s, c);
                   else if (ctype == SQLITE_TEXT) {
                     const auto* txt = sqlite3_column_text(s, c);
                     if (txt) g.texts[cname] = reinterpret_cast<const char*>(txt);
                   }
                 }
                 auto col = [&](const char* k) -> int64_t { return g.columns.count(k) ? g.columns[k] : 0; };
                 auto real_col = [&](const char* k) -> double {
                   if (g.reals.count(k)) return g.reals[k];
                   if (g.columns.count(k)) return double(g.columns[k]);
                   return 0.0;
                 };
                 g.id = col("Id"), g.car_id = col("CarId"), g.performance_index = real_col("PerformanceIndex");
                 g.class_id = col("ClassID"), g.color_index = col("DefaultManufacturerColorIndex");
                 g.history.distance_driven = real_col("DistanceDriven");
                 g.history.time_driven = real_col("TimeDriven");
                 g.history.total_winnings = col("TotalWinnings");
                 g.history.total_repairs = col("TotalRepairs");
                 g.history.num_races = col("NumRaces");
                 g.history.num_victories = col("NumVictories");
                 g.history.num_podiums = col("NumPodiums");
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

bool export_forza_profile(const Imported& p, const std::filesystem::path& out_folder, std::string* error,
                          const std::filesystem::path& template_db) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(out_folder, ec);
  if (ec) {
    if (error) *error = "cannot create " + out_folder.string() + ": " + ec.message();
    return false;
  }

  // 1. Serialize and atomically write ForzaProfile.body.
  const std::vector<uint8_t> body_bytes = serialize_body(p.body);
  if (!drive::atomic_write_file(out_folder / "ForzaProfile.body", body_bytes.data(), body_bytes.size(), error)) {
    return false;
  }

  // 2. Write PlayerDatabase.sqlite via temp file + SQLite transaction + atomic replace.
  const fs::path db_dst = out_folder / "PlayerDatabase.sqlite";
  const fs::path db_tmp = out_folder / "PlayerDatabase.sqlite.tmp";
  fs::remove(db_tmp, ec);
  const bool use_template = !template_db.empty() && fs::is_regular_file(template_db, ec);
  if (use_template) {
    fs::copy_file(template_db, db_tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      if (error) *error = "cannot copy template database " + template_db.string() + ": " + ec.message();
      return false;
    }
  }
  {
    Db db;
    auto fail_db = [&](const std::string& msg) {
      if (db.db) {
        sqlite3_close(db.db);
        db.db = nullptr;
      }
      std::error_code rm_ec;
      fs::remove(db_tmp, rm_ec);
      if (error) *error = msg;
      return false;
    };
    if (sqlite3_open_v2(db_tmp.string().c_str(), &db.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
      return fail_db("PlayerDatabase: cannot create " + db_tmp.string());
    }
    auto exec = [&](const std::string& sql) -> bool {
      char* errmsg = nullptr;
      const int rc = sqlite3_exec(db.db, sql.c_str(), nullptr, nullptr, &errmsg);
      if (rc != SQLITE_OK) {
        std::string msg = "PlayerDatabase: ";
        msg += errmsg ? errmsg : sqlite3_errmsg(db.db);
        if (errmsg) sqlite3_free(errmsg);
        if (error) *error = msg;
        return false;
      }
      return true;
    };
    if (!exec("PRAGMA journal_mode=DELETE;") || !exec("BEGIN IMMEDIATE;")) return fail_db(error ? *error : "");

    if (!use_template) {
      std::string create_garage =
          "CREATE TABLE Career_Garage ("
          "Id INTEGER PRIMARY KEY, CarId INTEGER, PerformanceIndex REAL, ClassID INTEGER, "
          "PartsValue INTEGER, DefaultManufacturerColorIndex INTEGER, LiveryFileName TEXT, "
          "DistanceDriven REAL, TimeDriven REAL, TotalWinnings INTEGER, TotalRepairs INTEGER, "
          "NumVictories INTEGER, NumPodiums INTEGER, NumRaces INTEGER";
      for (const auto& slot : cars::upgrade_slots()) {
        create_garage += ", ";
        create_garage += slot.column;
        create_garage += " INTEGER";
      }
      create_garage += ", WheelStyle INTEGER);";
      if (!exec(create_garage) ||
          !exec("CREATE TABLE Career_PurchasedParts (GarageId INTEGER, PartEnum INTEGER, PartId INTEGER, "
                "ManufacturerId INTEGER, PricePaid INTEGER);") ||
          !exec("CREATE TABLE Career_EventResults (EventId INTEGER, Place INTEGER);") ||
          !exec("CREATE TABLE Career_RaceResults (RaceId INTEGER, Place INTEGER);") ||
          !exec("CREATE TABLE CareerEventUnlocks (EventId INTEGER);") ||
          !exec("CREATE TABLE CareerHubUnlocks (HubId INTEGER);") ||
          !exec("CREATE TABLE LastUsedGarageId (Id INTEGER);")) {
        return fail_db(error ? *error : "");
      }
    }

    // Discover Career_Garage columns and add any extra valid columns present in `p.garage`.
    std::vector<std::string> garage_cols;
    std::set<std::string> garage_col_set;
    if (!db.each("PRAGMA table_info(Career_Garage)",
                 [&](sqlite3_stmt* s) {
                   const auto* cname = sqlite3_column_text(s, 1);
                   if (cname) {
                     std::string col(reinterpret_cast<const char*>(cname));
                     garage_cols.push_back(col);
                     garage_col_set.insert(col);
                   }
                 },
                 error)) {
      return fail_db(error ? *error : "");
    }
    auto ensure_garage_col = [&](const std::string& col, const char* sql_type) -> bool {
      if (garage_col_set.count(col)) return true;
      if (!is_valid_sql_ident(col)) return true;
      if (!exec("ALTER TABLE Career_Garage ADD COLUMN " + col + " " + sql_type + ";")) return false;
      garage_cols.push_back(col);
      garage_col_set.insert(col);
      return true;
    };
    for (const auto& g : p.garage) {
      for (const auto& kv : g.columns)
        if (!ensure_garage_col(kv.first, "INTEGER")) return fail_db(error ? *error : "");
      for (const auto& kv : g.reals)
        if (!ensure_garage_col(kv.first, "REAL")) return fail_db(error ? *error : "");
      for (const auto& kv : g.texts)
        if (!ensure_garage_col(kv.first, "TEXT")) return fail_db(error ? *error : "");
    }

    // Preserve RivalsBeaten from template Career_EventResults when present.
    std::map<int64_t, int64_t> prev_rivals;
    bool has_rivals_col = false;
    (void)db.each("PRAGMA table_info(Career_EventResults)",
                  [&](sqlite3_stmt* s) {
                    const auto* cname = sqlite3_column_text(s, 1);
                    if (cname && std::string_view(reinterpret_cast<const char*>(cname)) == "RivalsBeaten")
                      has_rivals_col = true;
                  },
                  nullptr);
    if (has_rivals_col) {
      (void)db.each("SELECT EventId, RivalsBeaten FROM Career_EventResults",
                    [&](sqlite3_stmt* s) { prev_rivals[sqlite3_column_int64(s, 0)] = sqlite3_column_int64(s, 1); },
                    nullptr);
    }

    if (!exec("DELETE FROM Career_Garage;") || !exec("DELETE FROM Career_PurchasedParts;") ||
        !exec("DELETE FROM Career_EventResults;") || !exec("DELETE FROM Career_RaceResults;") ||
        !exec("DELETE FROM CareerEventUnlocks;") || !exec("DELETE FROM CareerHubUnlocks;") ||
        !exec("DELETE FROM LastUsedGarageId;")) {
      return fail_db(error ? *error : "");
    }

    // Insert Career_Garage rows.
    if (!p.garage.empty() && !garage_cols.empty()) {
      std::string ins = "INSERT INTO Career_Garage (";
      std::string ph;
      for (size_t i = 0; i < garage_cols.size(); ++i) {
        if (i) ins += ", ", ph += ", ";
        ins += garage_cols[i];
        ph += "?";
      }
      ins += ") VALUES (" + ph + ");";
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db.db, ins.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        return fail_db("PlayerDatabase: " + std::string(sqlite3_errmsg(db.db)));
      }
      for (const auto& g : p.garage) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        for (size_t i = 0; i < garage_cols.size(); ++i) {
          const int idx = int(i + 1);
          const std::string& col = garage_cols[i];
          if (col == "Id") {
            sqlite3_bind_int64(st, idx, g.id);
          } else if (col == "CarId") {
            sqlite3_bind_int64(st, idx, g.car_id);
          } else if (col == "PerformanceIndex") {
            sqlite3_bind_double(st, idx, g.performance_index);
          } else if (col == "ClassID") {
            sqlite3_bind_int64(st, idx, g.class_id);
          } else if (col == "DefaultManufacturerColorIndex") {
            sqlite3_bind_int64(st, idx, g.color_index);
          } else if (auto it_t = g.texts.find(col); it_t != g.texts.end()) {
            sqlite3_bind_text(st, idx, it_t->second.c_str(), int(it_t->second.size()), SQLITE_TRANSIENT);
          } else if (auto it_r = g.reals.find(col); it_r != g.reals.end()) {
            sqlite3_bind_double(st, idx, it_r->second);
          } else if (auto it_c = g.columns.find(col); it_c != g.columns.end()) {
            sqlite3_bind_int64(st, idx, it_c->second);
          } else {
            sqlite3_bind_null(st, idx);
          }
        }
        if (sqlite3_step(st) != SQLITE_DONE) {
          const std::string msg = "PlayerDatabase: insert Career_Garage: " + std::string(sqlite3_errmsg(db.db));
          sqlite3_finalize(st);
          return fail_db(msg);
        }
      }
      sqlite3_finalize(st);
    }

    // Insert Career_PurchasedParts.
    if (!p.parts.empty()) {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(
              db.db,
              "INSERT INTO Career_PurchasedParts (GarageId, PartEnum, PartId, ManufacturerId, PricePaid) VALUES (?,?,?,?,?);",
              -1, &st, nullptr) != SQLITE_OK) {
        return fail_db("PlayerDatabase: " + std::string(sqlite3_errmsg(db.db)));
      }
      for (const auto& part : p.parts) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, part.garage_id);
        sqlite3_bind_int64(st, 2, part.part_enum);
        sqlite3_bind_int64(st, 3, part.part_id);
        sqlite3_bind_int64(st, 4, part.manufacturer_id);
        sqlite3_bind_int64(st, 5, part.price_paid);
        if (sqlite3_step(st) != SQLITE_DONE) {
          const std::string msg = "PlayerDatabase: insert Career_PurchasedParts: " + std::string(sqlite3_errmsg(db.db));
          sqlite3_finalize(st);
          return fail_db(msg);
        }
      }
      sqlite3_finalize(st);
    }

    // Insert Career_EventResults.
    if (!p.event_results.empty()) {
      const char* sql = has_rivals_col
                            ? "INSERT INTO Career_EventResults (EventId, Place, RivalsBeaten) VALUES (?,?,?);"
                            : "INSERT INTO Career_EventResults (EventId, Place) VALUES (?,?);";
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db.db, sql, -1, &st, nullptr) != SQLITE_OK) {
        return fail_db("PlayerDatabase: " + std::string(sqlite3_errmsg(db.db)));
      }
      for (const auto& [eid, place] : p.event_results) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, eid);
        sqlite3_bind_int(st, 2, place);
        if (has_rivals_col) {
          auto it = prev_rivals.find(eid);
          sqlite3_bind_int64(st, 3, it != prev_rivals.end() ? it->second : 0);
        }
        if (sqlite3_step(st) != SQLITE_DONE) {
          const std::string msg = "PlayerDatabase: insert Career_EventResults: " + std::string(sqlite3_errmsg(db.db));
          sqlite3_finalize(st);
          return fail_db(msg);
        }
      }
      sqlite3_finalize(st);
    }

    // Insert Career_RaceResults.
    if (!p.race_results.empty()) {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db.db, "INSERT INTO Career_RaceResults (RaceId, Place) VALUES (?,?);", -1, &st, nullptr) !=
          SQLITE_OK) {
        return fail_db("PlayerDatabase: " + std::string(sqlite3_errmsg(db.db)));
      }
      for (const auto& [rid, place] : p.race_results) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, rid);
        sqlite3_bind_int(st, 2, place);
        if (sqlite3_step(st) != SQLITE_DONE) {
          const std::string msg = "PlayerDatabase: insert Career_RaceResults: " + std::string(sqlite3_errmsg(db.db));
          sqlite3_finalize(st);
          return fail_db(msg);
        }
      }
      sqlite3_finalize(st);
    }

    // Insert CareerEventUnlocks and CareerHubUnlocks.
    auto insert_ids = [&](const char* sql, const std::vector<int64_t>& ids) -> bool {
      if (ids.empty()) return true;
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db.db, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = "PlayerDatabase: " + std::string(sqlite3_errmsg(db.db));
        return false;
      }
      for (int64_t id : ids) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) != SQLITE_DONE) {
          if (error) *error = "PlayerDatabase: " + std::string(sqlite3_errmsg(db.db));
          sqlite3_finalize(st);
          return false;
        }
      }
      sqlite3_finalize(st);
      return true;
    };
    if (!insert_ids("INSERT INTO CareerEventUnlocks (EventId) VALUES (?);", p.event_unlocks) ||
        !insert_ids("INSERT INTO CareerHubUnlocks (HubId) VALUES (?);", p.hub_unlocks)) {
      return fail_db(error ? *error : "");
    }

    // Insert LastUsedGarageId.
    {
      int64_t last_id = p.last_used_garage_id;
      for (const auto& g : p.garage) last_id = std::max(last_id, g.id);
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db.db, "INSERT INTO LastUsedGarageId VALUES (?);", -1, &st, nullptr) != SQLITE_OK) {
        return fail_db("PlayerDatabase: " + std::string(sqlite3_errmsg(db.db)));
      }
      sqlite3_bind_int64(st, 1, last_id);
      if (sqlite3_step(st) != SQLITE_DONE) {
        const std::string msg = "PlayerDatabase: insert LastUsedGarageId: " + std::string(sqlite3_errmsg(db.db));
        sqlite3_finalize(st);
        return fail_db(msg);
      }
      sqlite3_finalize(st);
    }

    if (!exec("COMMIT;")) return fail_db(error ? *error : "");
    const int close_rc = sqlite3_close(db.db);
    db.db = nullptr;
    if (close_rc != SQLITE_OK) return fail_db("PlayerDatabase: failed to close " + db_tmp.string());
  }

  if (!drive::atomic_replace_file(db_tmp, db_dst, error)) return false;

  // 3. Write export.json and optional content package lists atomically.
  const std::string export_json = "{\"VersionFlags\": " + std::to_string(p.version_flags) + "}\n";
  if (!drive::atomic_write_file(out_folder / "export.json", export_json, error)) return false;

  if (!p.packages.empty()) {
    std::string lines;
    for (const auto& pkg : p.packages) lines += pkg + "\n";
    if (!drive::atomic_write_file(out_folder / "content-packages.txt", lines, error)) return false;
  }
  if (!p.disabled_packages.empty()) {
    std::string lines;
    for (const auto& pkg : p.disabled_packages) lines += pkg + "\n";
    if (!drive::atomic_write_file(out_folder / "content-disabled.txt", lines, error)) return false;
  }
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
  s["level"] = p.level;
  s["max_skills_score"] = p.max_skills_score;
  s["max_skills_car"] = p.max_skills_car;
  if (p.has_sponsorship) {
    s["popularity_points"] = int64_t(std::llround(p.popularity));
    s["fame_tiers_claimed"] = p.fame_tiers_claimed;
    s["sponsor_challenges"] = int64_t(p.sponsors.size());
  }
  s["rally_fields"] = p.rally_fields;
  if (p.rally_fields) {
    s["num_rallies"] = p.num_rallies, s["num_rally_stages"] = p.num_rally_stages;
    s["num_rally_victories"] = p.num_rally_victories, s["rally_winnings"] = p.rally_winnings;
    s["rally_xp"] = p.rally_xp, s["rally_level"] = p.rally_level;
  }
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
