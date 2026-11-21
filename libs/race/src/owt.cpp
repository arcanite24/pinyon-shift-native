#include "race/owt.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "fh1data/archive.h"

namespace race {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float bef(const uint8_t* p) {
  const uint32_t u = be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

constexpr size_t kHeader = 32, kPoint = 48, kTrailer = 16;

}  // namespace

float AiLinePoint::half_width() const { return std::sqrt(lx * lx + ly * ly + lz * lz); }

bool parse_owt(std::span<const uint8_t> b, AiLineFile& out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (b.size() < kHeader + kTrailer) return fail("owt: shorter than a header and trailer");
  if (std::memcmp(b.data(), "OWTM", 4) != 0) return fail("owt: no OWTM magic");
  const uint32_t n = be32(b.data() + 16);
  if (b.size() != kHeader + size_t(n) * kPoint + kTrailer) return fail("owt: size is not header + points + trailer");
  const uint8_t* t = b.data() + kHeader + size_t(n) * kPoint;
  if (std::memcmp(t, "OWTM", 4) != 0) return fail("owt: no trailer magic");
  out.version = be32(b.data() + 4);
  out.closed = be32(b.data() + 20) != 0;
  out.points.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* p = b.data() + kHeader + size_t(i) * kPoint;
    AiLinePoint& q = out.points[i];
    q.x = bef(p), q.y = bef(p + 4), q.z = bef(p + 8);
    q.lx = bef(p + 16), q.ly = bef(p + 20), q.lz = bef(p + 24);
    q.offset = bef(p + 32);
  }
  return true;
}

bool load_owt(const std::filesystem::path& game_root, int route_id, AiLineFile& out, std::string* error) {
  auto zip = fh1::Archive::open(game_root / "media" / "aiopenworld.zip", error);
  if (!zip) return false;
  char name[64];
  std::snprintf(name, sizeof name, "colorado/Ribbon_00/route_%03d.owt", route_id);
  auto r = zip->read(name);
  if (!r.ok()) {
    if (error) *error = std::string(name) + ": " + r.error;
    return false;
  }
  return parse_owt(r.data, out, error);
}

bool load_owt_file(const std::filesystem::path& path, AiLineFile& out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse_owt(bytes, out, error);
}

std::vector<std::string> owt_members(const std::filesystem::path& game_root, std::string* error) {
  std::vector<std::string> out;
  auto zip = fh1::Archive::open(game_root / "media" / "aiopenworld.zip", error);
  if (!zip) return out;
  for (const auto& m : zip->members())
    if (m.name.size() > 4 && m.name.compare(m.name.size() - 4, 4, ".owt") == 0) out.push_back(m.name);
  return out;
}

namespace {
template <class F>
double length_of(const AiLineFile& l, F at) {
  const size_t n = l.points.size();
  double s = 0;
  for (size_t i = 0; i + 1 < n + (l.closed ? 1 : 0); ++i) {
    float ax, ay, az, bx, by, bz;
    at(l.points[i], ax, ay, az);
    at(l.points[(i + 1) % n], bx, by, bz);
    s += std::sqrt(double(bx - ax) * (bx - ax) + double(by - ay) * (by - ay) + double(bz - az) * (bz - az));
  }
  return s;
}
}  // namespace

double centre_length(const AiLineFile& l) {
  return length_of(l, [](const AiLinePoint& p, float& x, float& y, float& z) { x = p.x, y = p.y, z = p.z; });
}
double racing_length(const AiLineFile& l) {
  return length_of(l, [](const AiLinePoint& p, float& x, float& y, float& z) {
    x = p.race_x(), y = p.race_y(), z = p.race_z();
  });
}

}  // namespace race
