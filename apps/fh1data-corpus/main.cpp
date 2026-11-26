// fh1data-corpus: runs the format readers over the player's whole media
// folder and reports what parses (RF-6 corpus test).
//
//   fh1data-corpus --game-root <dir> [--threads N] [--verbose] [--filter text]
//                  [--all-members]   (also decompress and CRC every other member)
//   fh1data-corpus --dump <archive.zip> <member>        (carbin summary)
//   fh1data-corpus --extract <archive.zip> <member> <out>
//   fh1data-corpus --names <archive.zip> [members-dir]  (directory census)
//   fh1data-corpus --render-models <dir|archive.zip>    (render model census)
//   fh1data-corpus --world-materials <dir|archive.zip> <Colorado_00.pvs>
//                  (WORLD-MATERIALS: materials, UV scales, layouts, textures)
//   fh1data-corpus --props <dir|archive.zip> <Colorado_00.pvs> [x0 z0 x1 z1]
//                  (object table, .pgeo and .pvsz census, placements in a box)
//
// <dir> is the extracted disc root containing media/ (or media itself).
// Exit code 0 when every car archive parses.

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fh1data/archive.h"
#include "fh1data/carbin.h"
#include "fh1data/members.h"
#include "fh1data/placements.h"
#include "fh1data/props.h"
#include "fh1data/rmb.h"
#include "fh1data/world_texture.h"
#include "fh1data/xds.h"

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string extension(const std::string& name) {
  const auto dot = name.find_last_of('.');
  return dot == std::string::npos ? "" : lower(name.substr(dot + 1));
}

std::string basename(const std::string& name) {
  const auto slash = name.find_last_of("/\\");
  return slash == std::string::npos ? name : name.substr(slash + 1);
}

struct Stats {
  uint64_t archives = 0, archives_failed = 0, members = 0;
  std::map<uint16_t, uint64_t> methods;
  uint64_t read_ok = 0, read_failed = 0;
  // carbin
  uint64_t carbin_ok = 0, carbin_header_only = 0, carbin_failed = 0;
  uint64_t stripped_total = 0, stripped_parsed = 0;
  uint64_t parts = 0, subsections = 0, vertices = 0, bad_indices = 0;
  uint64_t car_subsections = 0, car_carbins = 0;
  // xds
  uint64_t xds_ok = 0, xds_base_only = 0, xds_unsupported = 0, xds_failed = 0, xds_levels = 0;
  std::map<std::string, uint64_t> xds_formats;      // "name/dimension" -> count
  std::map<std::string, uint64_t> xds_unsupported_why;
  uint64_t xds_layout_exact = 0, xds_layout_padded = 0;  // payload == / != layout size
  std::map<std::string, uint64_t> xds_padded_shapes;
  // every member (--all-members) and the archive structure
  uint64_t no_local_header = 0, no_extra_offset = 0;
  uint64_t all_read_ok = 0, all_read_failed = 0, all_deflate_skipped = 0;
  uint64_t no_local_read_ok = 0;
  std::map<int, uint64_t> lzx_windows;
  // car archives
  uint64_t car_archives = 0, car_archives_ok = 0;
  std::vector<std::string> failures;

  void merge(const Stats& o) {
    archives += o.archives; archives_failed += o.archives_failed; members += o.members;
    for (auto& [k, v] : o.methods) methods[k] += v;
    read_ok += o.read_ok; read_failed += o.read_failed;
    carbin_ok += o.carbin_ok; carbin_header_only += o.carbin_header_only;
    carbin_failed += o.carbin_failed; stripped_total += o.stripped_total;
    stripped_parsed += o.stripped_parsed; parts += o.parts; subsections += o.subsections;
    vertices += o.vertices; bad_indices += o.bad_indices;
    car_subsections += o.car_subsections; car_carbins += o.car_carbins;
    xds_ok += o.xds_ok; xds_base_only += o.xds_base_only; xds_unsupported += o.xds_unsupported;
    xds_failed += o.xds_failed; xds_levels += o.xds_levels;
    for (auto& [k, v] : o.xds_formats) xds_formats[k] += v;
    for (auto& [k, v] : o.xds_unsupported_why) xds_unsupported_why[k] += v;
    car_archives += o.car_archives; car_archives_ok += o.car_archives_ok;
    xds_layout_exact += o.xds_layout_exact; xds_layout_padded += o.xds_layout_padded;
    for (auto& [k, v] : o.xds_padded_shapes) xds_padded_shapes[k] += v;
    no_local_header += o.no_local_header; no_extra_offset += o.no_extra_offset;
    all_read_ok += o.all_read_ok; all_read_failed += o.all_read_failed;
    all_deflate_skipped += o.all_deflate_skipped; no_local_read_ok += o.no_local_read_ok;
    for (auto& [k, v] : o.lzx_windows) lzx_windows[k] += v;
    failures.insert(failures.end(), o.failures.begin(), o.failures.end());
  }
};

const char* dimension_name(fh1::XdsDimension d) {
  switch (d) {
    case fh1::XdsDimension::k1D: return "1D";
    case fh1::XdsDimension::k2D: return "2D";
    case fh1::XdsDimension::k3D: return "3D";
    case fh1::XdsDimension::kCube: return "cube";
  }
  return "?";
}

// Returns false if the item counts against its archive.
bool check_member(const std::string& where, const std::string& name,
                  std::span<const uint8_t> data, bool in_cars, Stats& s) {
  const std::string ext = extension(name);
  if (ext == "carbin") {
    const bool stripped = lower(basename(name)).rfind("stripped_", 0) == 0;
    fh1::Carbin carbin;
    std::string error;
    const bool ok = fh1::parse_carbin(data, carbin, &error);
    if (stripped) {
      ++s.stripped_total;
      if (ok) ++s.stripped_parsed;
      return true;  // a layout this reader does not decode; reported apart
    }
    if (!ok) {
      ++s.carbin_failed;
      s.failures.push_back("carbin " + where + ":" + name + ": " + error);
      return false;
    }
    const size_t bad = fh1::carbin_bad_indices(carbin);
    s.bad_indices += bad;
    if (carbin.parts.empty()) ++s.carbin_header_only; else ++s.carbin_ok;
    s.parts += carbin.parts.size();
    for (const auto& p : carbin.parts) {
      s.subsections += p.subsections.size();
      if (in_cars) s.car_subsections += p.subsections.size();
      s.vertices += p.lod_pool.vertices.size() + p.lod0_pool.vertices.size();
    }
    if (in_cars) ++s.car_carbins;
    if (bad) {
      s.failures.push_back("carbin " + where + ":" + name + ": " + std::to_string(bad) +
                           " indices outside their pool");
      return false;
    }
    return true;
  }
  if (ext == "xds") {
    const fh1::XdsResult r = fh1::decode_xds(data, true);
    s.xds_formats[std::string(fh1::xenos_format_name(r.header.format)) + "(" +
                  std::to_string(r.header.format) + ")/" + dimension_name(r.header.dimension) +
                  (r.header.tiled ? "/tiled" : "/linear")]++;
    if (r.layout_bytes && r.status != fh1::XdsStatus::kFailed) {
      if (r.payload_bytes == r.layout_bytes) {
        ++s.xds_layout_exact;
      } else {
        ++s.xds_layout_padded;
        s.xds_padded_shapes[std::to_string(r.header.width) + "x" + std::to_string(r.header.height) +
                            " levels " + std::to_string(r.header.levels) +
                            (r.header.packed_mips ? " packed" : "") +
                            (r.header.tiled ? "" : " linear") +
                            (r.payload_bytes > r.layout_bytes ? " (payload larger)"
                                                              : " (payload shorter)")]++;
      }
    }
    switch (r.status) {
      case fh1::XdsStatus::kOk:
        ++s.xds_ok;
        s.xds_levels += r.levels.size();
        return true;
      case fh1::XdsStatus::kBaseOnly:
        ++s.xds_base_only;
        s.xds_levels += r.levels.size();
        s.failures.push_back("xds-mips " + where + ":" + name + ": " + r.error);
        return true;
      case fh1::XdsStatus::kUnsupported:
        ++s.xds_unsupported;
        s.xds_unsupported_why[r.error]++;
        return true;
      case fh1::XdsStatus::kFailed:
        ++s.xds_failed;
        s.failures.push_back("xds " + where + ":" + name + ": " + r.error);
        return false;
    }
  }
  return true;
}

bool g_all_members = false;

void check_archive(const fs::path& path, const fs::path& media, Stats& s) {
  const std::string rel = fs::relative(path, media).generic_string();
  const bool in_cars = lower(rel).rfind("cars/", 0) == 0;
  ++s.archives;
  std::string error;
  auto archive = fh1::Archive::open(path, &error);
  if (!archive) {
    ++s.archives_failed;
    s.failures.push_back("archive " + rel + ": " + error);
    if (in_cars) ++s.car_archives;
    return;
  }
  bool all_ok = true;
  s.members += archive->members().size();
  for (const auto& m : archive->members()) {
    s.methods[m.method]++;
    const bool local = archive->has_local_header(m);
    if (!local) ++s.no_local_header;
    if (!m.payload_offset) ++s.no_extra_offset;
    const std::string ext = extension(m.name);
    const bool decoded_type = ext == "carbin" || ext == "xds";
    if (!decoded_type && !g_all_members) continue;
    if (m.method == uint16_t(fh1::ZipMethod::kDeflate)) {
      ++s.all_deflate_skipped;
      continue;
    }
    fh1::ReadResult r = archive->read(m);
    if (r.ok()) {
      ++s.all_read_ok;
      if (!local) ++s.no_local_read_ok;
      if (r.lzx_window_bits) s.lzx_windows[r.lzx_window_bits]++;
    } else {
      ++s.all_read_failed;
    }
    if (!decoded_type) {
      if (!r.ok()) s.failures.push_back("read " + rel + ":" + m.name + ": " + r.error);
      continue;
    }
    if (!r.ok()) {
      ++s.read_failed;
      all_ok = false;
      s.failures.push_back("read " + rel + ":" + m.name + ": " + r.error);
      continue;
    }
    ++s.read_ok;
    all_ok &= check_member(rel, m.name, r.data, in_cars, s);
  }
  if (in_cars) {
    ++s.car_archives;
    if (all_ok) ++s.car_archives_ok;
  }
}

std::vector<uint8_t> read_file(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

fs::path find_media(const fs::path& root) {
  if (fs::is_directory(root / "media")) return root / "media";
  if (fs::is_directory(root / "base" / "media")) return root / "base" / "media";
  return root;
}

int dump(const fs::path& zip, const std::string& member) {
  std::string error;
  auto archive = fh1::Archive::open(zip, &error);
  if (!archive) { std::cerr << error << "\n"; return 1; }
  auto r = archive->read(member);
  if (!r.ok()) { std::cerr << r.error << "\n"; return 1; }
  fh1::Carbin c;
  if (!fh1::parse_carbin(r.data, c, &error)) { std::cerr << error << "\n"; return 1; }
  std::printf("head %zu bytes, %zu sections\n", c.head_size, c.parts.size());
  for (const auto& p : c.parts) {
    std::printf("  %s: v%u lod %dx%u lod0 %dx%u subs %zu damage %u offset (%.3f %.3f %.3f)\n",
                p.name.c_str(), p.version, p.lod_pool.count, p.lod_pool.stride,
                p.lod0_pool.count, p.lod0_pool.stride, p.subsections.size(), p.damage_flag,
                p.bounds[0], p.bounds[1], p.bounds[2]);
    for (const auto& s : p.subsections)
      std::printf("      %s lod %d type %u %zux%u\n", s.material.c_str(), s.lod, s.index_type,
                  s.indices.size(), s.index_size);
  }
  return 0;
}

// --names <archive.zip> [members-dir]: the directory as the reader sees it,
// distinct names compared ignoring case, and (with a directory) whether the
// extracted tree holds the same names.
int names(const fs::path& zip, const fs::path& dir) {
  std::string error;
  auto archive = fh1::Archive::open(zip, &error);
  if (!archive) { std::cerr << error << "\n"; return 1; }
  std::map<std::string, std::pair<uint32_t, uint64_t>> first;  // name -> crc, size
  std::set<std::string> spellings;
  uint64_t disagreeing = 0;
  for (const auto& m : archive->members()) {
    spellings.insert(m.name);
    const auto [it, added] = first.emplace(fh1::normalize_member_name(m.name),
                                           std::pair{m.crc32, m.size});
    if (!added && it->second != std::pair{m.crc32, m.size}) ++disagreeing;
  }
  std::printf("%s: %zu directory entries, %zu distinct names (ignoring case), %zu spellings, "
              "%llu repeats with another CRC or size\n",
              zip.filename().string().c_str(), archive->members().size(), first.size(),
              spellings.size(), (unsigned long long)disagreeing);
  if (dir.empty()) return 0;
  std::set<std::string> on_disk;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (e.is_regular_file())
      on_disk.insert(fh1::normalize_member_name(fs::relative(e.path(), dir).generic_string()));
  }
  size_t only_archive = 0, only_disk = 0;
  for (const auto& [name, _] : first) only_archive += !on_disk.count(name);
  for (const auto& name : on_disk) only_disk += !first.count(name);
  std::printf("%s: %zu files; %zu names only in the archive, %zu only on disk\n",
              dir.string().c_str(), on_disk.size(), only_archive, only_disk);
  return only_archive || only_disk ? 1 : 0;
}

// --render-models <dir|archive.zip> [--objects]: parses every *.rmb.bin to
// its last index list and prints the census the format spec quotes;
// --objects also lists every mesh object (model, name, the model's header
// box, its submeshes' effects and triangles).
int render_models(const fs::path& source, bool list_objects = false) {
  std::unique_ptr<fh1::Archive> archive;
  std::vector<fs::path> files;
  std::vector<const fh1::ArchiveMember*> members;
  if (fs::is_directory(source)) {
    for (const auto& e : fs::recursive_directory_iterator(source)) {
      const std::string n = lower(e.path().filename().string());
      if (e.is_regular_file() && n.size() > 8 && n.compare(n.size() - 8, 8, ".rmb.bin") == 0)
        files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
  } else {
    std::string error;
    archive = fh1::Archive::open(source, &error);
    if (!archive) { std::cerr << error << "\n"; return 1; }
    std::set<std::string> seen;
    for (const auto& m : archive->members()) {
      const std::string n = fh1::normalize_member_name(m.name);
      if (n.size() > 8 && n.compare(n.size() - 8, 8, ".rmb.bin") == 0 && seen.insert(n).second)
        members.push_back(&m);
    }
  }
  const size_t total = archive ? members.size() : files.size();
  uint64_t ok = 0, failed = 0, bytes = 0, geometry_bytes = 0, objects = 0, submeshes = 0;
  uint64_t vertices = 0, indices = 0, triangles = 0, outside_box = 0, odd_constants = 0;
  uint64_t world = 0, local = 0;
  uint32_t min_objects = UINT32_MAX, max_objects = 0;
  std::map<uint32_t, uint64_t> strides, primitives, index_sizes, trailers;
  std::vector<std::string> failures, wide_lists;
  for (size_t i = 0; i < total; ++i) {
    std::string name;
    std::vector<uint8_t> data;
    if (archive) {
      name = members[i]->name;
      auto r = archive->read(*members[i]);
      if (!r.ok()) {
        ++failed;
        failures.push_back(name + ": read: " + r.error);
        continue;
      }
      data = std::move(r.data);
    } else {
      name = files[i].filename().string();
      data = read_file(files[i]);
    }
    fh1::RenderModel model;
    std::string error;
    bytes += data.size();
    if (!fh1::parse_render_model(data, model, &error)) {
      ++failed;
      failures.push_back(name + ": " + error);
      continue;
    }
    ++ok;
    geometry_bytes += model.geometry_end;
    const uint32_t n = uint32_t(model.objects.size());
    min_objects = std::min(min_objects, n);
    max_objects = std::max(max_objects, n);
    objects += n;
    bool constants = model.header_words == std::array<uint32_t, 4>{4, 4, 0, 1};
    for (int k = 0; k < 16; ++k) constants &= model.matrix[k] == (k % 5 == 0 ? 1.0f : 0.0f);
    ++(fh1::rmb_is_world_space(model) ? world : local);
    for (const auto& o : model.objects) {
      vertices += o.vertex_count;
      strides[o.stride]++;
      constants &= o.vertex_format == 3;
      for (uint32_t v = 0; v < o.vertex_count; ++v) {
        const auto p = o.position(v);
        for (int a = 0; a < 3; ++a) {
          const float slack = 1e-3f * std::max(1.0f, std::fabs(p[a]));
          if (!(p[a] >= model.box_min[a] - slack && p[a] <= model.box_max[a] + slack)) {
            ++outside_box;
            break;
          }
        }
      }
      for (const auto& s : o.submeshes) {
        ++submeshes;
        indices += s.indices.size();
        primitives[s.primitive]++;
        index_sizes[s.index_size]++;
        trailers[s.trailer]++;
        triangles += fh1::rmb_triangle_list(s).size() / 3;
        constants &= s.colour == std::array<float, 4>{1, 1, 1, 1};
        if (s.index_size == 4)
          wide_lists.push_back(name + " " + o.name + ": " + std::to_string(o.vertex_count) +
                               " vertices, primitive " + std::to_string(s.primitive) + ", " +
                               std::to_string(s.indices.size()) + " indices");
      }
    }
    if (!constants) ++odd_constants;
    if (list_objects) {
      std::string ignored;
      fh1::parse_render_materials(data, model, &ignored);
      for (const auto& o : model.objects) {
        std::string effects;
        size_t tris = 0;
        for (const auto& s : o.submeshes) {
          tris += fh1::rmb_triangle_list(s).size() / 3;
          std::string e = s.material_index < model.materials.size() &&
                                  model.materials[s.material_index].effect < model.effects.size()
                              ? model.effects[model.materials[s.material_index].effect]
                              : std::string("-");
          if (effects.find(e) == std::string::npos) effects += (effects.empty() ? "" : ",") + e;
        }
        std::printf("object %s %s box %.0f %.0f %.0f %.0f %.0f %.0f tris %zu %s\n", name.c_str(), o.name.c_str(),
                    model.box_min[0], model.box_min[1], model.box_min[2], model.box_max[0], model.box_max[1],
                    model.box_max[2], tris, effects.c_str());
      }
    }
  }
  auto histogram = [](const char* what, const std::map<uint32_t, uint64_t>& h) {
    std::printf("%s:", what);
    for (const auto& [k, v] : h) std::printf(" %u (%llu)", k, (unsigned long long)v);
    std::printf("\n");
  };
  std::printf("render models: %zu found, %llu parsed to their last index list, %llu failed\n",
              total, (unsigned long long)ok, (unsigned long long)failed);
  std::printf("mesh objects: %llu (%u to %u per model); submeshes %llu; vertices %llu; "
              "indices %llu; triangles %llu\n",
              (unsigned long long)objects, min_objects == UINT32_MAX ? 0 : min_objects, max_objects,
              (unsigned long long)submeshes,
              (unsigned long long)vertices, (unsigned long long)indices,
              (unsigned long long)triangles);
  histogram("vertex strides (objects)", strides);
  histogram("primitive types (submeshes)", primitives);
  histogram("index sizes (submeshes)", index_sizes);
  histogram("trailers (submeshes)", trailers);
  for (const auto& w : wide_lists) std::printf("32-bit list: %s\n", w.c_str());
  std::printf("geometry: %llu of %llu bytes (%.1f %%) up to the last index list\n",
              (unsigned long long)geometry_bytes, (unsigned long long)bytes,
              bytes ? 100.0 * double(geometry_bytes) / double(bytes) : 0.0);
  std::printf("vertices outside their model's header box: %llu\n",
              (unsigned long long)outside_box);
  std::printf("models with a constant field off its usual value: %llu\n",
              (unsigned long long)odd_constants);
  std::printf("world-space models: %llu; local-space (box within %.0f m of the origin in X/Z): "
              "%llu\n",
              (unsigned long long)world, double(fh1::kRmbLocalRadius), (unsigned long long)local);
  for (size_t i = 0; i < failures.size() && i < 20; ++i)
    std::printf("FAIL %s\n", failures[i].c_str());
  return failed ? 1 : 0;
}

// --props <dir|archive.zip> <Colorado_00.pvs> [x0 z0 x1 z1]: the object
// table, every kind-7 .pgeo tile and every .pvsz zone, and, with a box,
// the placements whose file-frame translation lies inside it (inclusive).
int props(const fs::path& source_path, const fs::path& pvs_path, const double* box) {
  std::string error;
  fh1::MemberSource source;
  if (!source.open(source_path, &error)) { std::cerr << error << "\n"; return 1; }
  const auto pvs_bytes = read_file(pvs_path);
  fh1::PvsObjectTable table;
  if (!fh1::parse_object_table(pvs_bytes, table, &error)) {
    std::cerr << pvs_path.string() << ": " << error << "\n";
    return 1;
  }
  std::set<uint32_t> missing_members;
  size_t objects_missing = 0;
  for (const auto& o : table.objects)
    if (!source.contains(fh1::PropModels::member_name(o.member))) {
      ++objects_missing;
      missing_members.insert(o.member);
    }
  std::printf("object table: version %u; %u 28-byte records at %zu, %zu shader names, %zu objects "
              "at %zu (ends at %zu of %zu bytes); %zu objects name %zu member numbers with no "
              "render model in the source\n",
              table.version, table.record_count, table.record_table_offset, table.shaders.size(),
              table.objects.size(), table.object_table_offset,
              table.object_table_offset + 4 + table.objects.size() * 18, pvs_bytes.size(),
              objects_missing, missing_members.size());

  fh1::PropWorld world;
  if (!fh1::load_props(source, world, &error)) { std::cerr << error << "\n"; return 1; }
  std::printf(".pgeo: %llu tiles; by kind:", (unsigned long long)world.pgeo_tiles);
  for (const auto& [k, n] : world.pgeo_kinds) std::printf(" %u (%llu)", k, (unsigned long long)n);
  std::printf("\n  kind 7: %llu parsed to their stated size, %llu failed; %llu model records, "
              "%llu groups, %llu instances\n",
              (unsigned long long)world.pgeo_parsed, (unsigned long long)world.pgeo_failed,
              (unsigned long long)world.pgeo_models, (unsigned long long)world.pgeo_groups,
              (unsigned long long)world.pgeo_instances);
  std::printf("  header sizes:");
  for (const auto& [k, n] : world.pgeo_header_sizes)
    std::printf(" 0x%zx (%llu)", k, (unsigned long long)n);
  std::printf("; model records with consecutive IDs %llu, with an empty list 0 %llu; tiles whose "
              "trailing block is 40 x the groups' counts %llu\n",
              (unsigned long long)world.pgeo_consecutive_records,
              (unsigned long long)world.pgeo_records_without_list0,
              (unsigned long long)world.pgeo_trailing_matches);
  std::set<uint32_t> distinct;
  size_t out_of_table = 0;
  for (const auto& p : world.placements) {
    if (p.store == fh1::PropStore::kPvsz) distinct.insert(p.object_id);
    if (p.has_object && p.object_id >= table.objects.size()) ++out_of_table;
  }
  std::printf(".pvsz: %llu zones, %llu parsed, %llu failed; %llu records; %llu zones with more than "
              "one start that ends exactly, %llu where the most rigid start is not the first\n",
              (unsigned long long)world.pvsz_zones, (unsigned long long)world.pvsz_parsed,
              (unsigned long long)world.pvsz_failed, (unsigned long long)world.pvsz_records,
              (unsigned long long)world.pvsz_multi_start,
              (unsigned long long)world.pvsz_not_first_start);
  std::printf("  %zu distinct objects (largest ID %llu, %llu IDs flagged); records differing from "
              "the object's record in another zone: %llu (first-start rule: %llu)\n",
              distinct.size(), (unsigned long long)world.pvsz_max_object,
              (unsigned long long)world.pvsz_flagged_ids, (unsigned long long)world.pvsz_conflicts,
              (unsigned long long)world.pvsz_first_conflicts);
  std::printf("placements naming an object ID outside the table: %zu\n", out_of_table);
  for (size_t i = 0; i < world.failures.size() && i < 20; ++i)
    std::printf("FAIL %s\n", world.failures[i].c_str());

  if (box) {
    fh1::PropModels models(source);
    size_t in_box[2] = {0, 0}, unresolved[2] = {0, 0};
    std::set<uint32_t> lod00_members;
    for (const auto& p : world.placements) {
      const auto& t = p.placement.offset;
      if (!(t[0] >= box[0] && t[0] <= box[2] && t[2] >= box[1] && t[2] <= box[3])) continue;
      const int s = p.store == fh1::PropStore::kPgeo ? 0 : 1;
      ++in_box[s];
      const int64_t member = p.has_object && p.object_id < table.objects.size()
                                 ? models.lod_member(table.objects[p.object_id].member, 0)
                                 : -1;
      if (member < 0) ++unresolved[s];
      else lod00_members.insert(uint32_t(member));
    }
    std::printf("box x %.0f..%.0f z %.0f..%.0f: %zu .pgeo instances (%zu name no render model), "
                "%zu .pvsz objects (%zu name no render model); %zu distinct LOD00 models\n",
                box[0], box[2], box[1], box[3], in_box[0], unresolved[0], in_box[1],
                unresolved[1], lod00_members.size());
  }
  return world.pgeo_failed || world.pvsz_failed ? 1 : 0;
}

}  // namespace

// --world-materials <dir|bin.zip> <Colorado_00.pvs>: the material records,
// uvOffsetScale writes, vertex layouts, texture records and every texture
// file, with the counts WORLD-MATERIALS quotes.
std::string texture_file(uint32_t id, const char* suffix) {
  char name[32];
  std::snprintf(name, sizeof name, "_0x%08x%s", id, suffix);
  return name;
}

int world_materials(const fs::path& source_path, const fs::path& pvs_path) {
  fh1::MemberSource source;
  std::string error;
  if (!source.open(source_path, &error)) { std::cerr << error << "\n"; return 1; }
  fh1::PvsObjectTable table;
  if (!fh1::parse_object_table(read_file(pvs_path), table, &error)) { std::cerr << error << "\n"; return 1; }
  std::map<uint32_t, uint64_t> types;
  for (const auto& t : table.textures) ++types[t.type];
  std::printf("texture records: %zu (", table.textures.size());
  for (const auto& [t, n] : types) std::printf(" type %u: %llu", t, (unsigned long long)n);
  std::printf(" ); model lists: %zu%s%s\n", table.model_textures.size(),
              table.texture_list_error.empty() ? "" : ", error: ", table.texture_list_error.c_str());
  // Every texture file.
  uint64_t bix = 0, bix_exact = 0, bix_decoded = 0, caff = 0, caff_exact = 0, caff_decoded = 0, cutouts = 0;
  std::map<std::string, uint64_t> formats;
  std::vector<std::string> bad;
  for (const auto& t : table.textures) {
    if (t.type != fh1::kPvsTextureBix && t.type != fh1::kPvsTextureCaff) continue;
    fh1::WorldTexture tex;
    bool exact = false, ok = false;
    std::vector<uint8_t> a, b;
    if (t.type == fh1::kPvsTextureBix) {
      if (!source.read(texture_file(t.id, ".bix"), a, &error) || !source.read(texture_file(t.id, "_b.bix"), b, &error))
        continue;
      ++bix;
      ok = fh1::decode_bix(a, b, tex, &error, &exact);
      bix_exact += exact, bix_decoded += ok && tex.blocks.levels.size() == tex.levels;
    } else {
      if (!source.read(texture_file(t.id, ".bin"), a, &error)) continue;
      ++caff;
      ok = fh1::decode_caff(a, tex, &error, &exact);
      caff_exact += exact, caff_decoded += ok && tex.blocks.levels.size() == tex.levels;
    }
    if (ok) {
      ++formats[std::string(fh1::xenos_format_name(tex.format.format)) + (tex.format.gamma ? " gamma" : " linear") +
                (tex.caff ? " caff" : " bix")];
      cutouts += fh1::world_texture_has_cutout(tex);
    }
    if ((!ok || !exact) && bad.size() < 12)
      bad.push_back(texture_file(t.id, t.type == fh1::kPvsTextureBix ? ".bix" : ".bin") + " " +
                    std::to_string(tex.width) + "x" + std::to_string(tex.height) + " levels " +
                    std::to_string(tex.levels) + (ok ? " size differs" : " " + error));
  }
  std::printf(".bix pairs: %llu, %llu at the layout's sizes, %llu with every level\n", (unsigned long long)bix,
              (unsigned long long)bix_exact, (unsigned long long)bix_decoded);
  std::printf("CAFF: %llu, %llu at the layout's sizes, %llu with every level\n", (unsigned long long)caff,
              (unsigned long long)caff_exact, (unsigned long long)caff_decoded);
  std::printf("with cut-out alpha: %llu\n", (unsigned long long)cutouts);
  for (const auto& [f, n] : formats) std::printf("  %-28s %llu\n", f.c_str(), (unsigned long long)n);
  for (const auto& b : bad) std::printf("  not exact: %s\n", b.c_str());
  // Every render model.
  uint64_t models = 0, failed = 0, materials = 0, submeshes = 0, with_uv = 0, without_uv = 0, unknown_layout = 0;
  uint64_t layout_known = 0, bad_material = 0;
  std::map<std::string, uint64_t> slot_outcomes, unknown;
  std::vector<std::string> no_uv;
  for (const auto& name : source.with_suffix(".rmb.bin")) {
    std::vector<uint8_t> data;
    if (!source.read(name, data, &error)) { ++failed; continue; }
    fh1::RenderModel m;
    if (!fh1::parse_render_model(data, m, &error) || !fh1::parse_render_materials(data, m, &error)) {
      ++failed;
      if (failed < 5) std::printf("  %s: %s\n", name.c_str(), error.c_str());
      continue;
    }
    ++models;
    materials += m.materials.size();
    size_t n = 0;
    for (const auto& o : m.objects) n += o.submeshes.size();
    submeshes += n;
    if (m.uv_offset_scale.empty()) {
      ++without_uv;
      if (no_uv.size() < 6) no_uv.push_back(name);
    } else {
      ++with_uv;
    }
    // <track>out.NNNNN.rmb.bin (any track's prefix, the hub rooms' too).
    const size_t dot = name.find('.');
    const uint32_t member = dot != std::string::npos ? uint32_t(std::strtoul(name.c_str() + dot + 1, nullptr, 10)) : 0;
    const std::vector<uint32_t>* list =
        member >= 1 && member - 1 < table.model_textures.size() ? &table.model_textures[member - 1] : nullptr;
    for (const auto& o : m.objects)
      for (const auto& s : o.submeshes) {
        if (s.material_index >= m.materials.size()) { ++bad_material; continue; }
        const auto& mat = m.materials[s.material_index];
        const auto layout = fh1::rmb_vertex_layout(m.effects[mat.effect], o.stride);
        if (layout.known) ++layout_known;
        else if (++unknown_layout, unknown.size() < 20) ++unknown[m.effects[mat.effect] + " " + std::to_string(o.stride)];
      }
    for (const auto& mat : m.materials)
      for (int32_t v : mat.slots) {
        std::string outcome;
        if (v == -2) outcome = "-2 no texture";
        else if (v == -1) outcome = "-1 run time";
        else if (!list || size_t(v) >= list->size()) outcome = "past the model's list";
        else if ((*list)[size_t(v)] >= table.textures.size()) outcome = "past the records";
        else {
          const auto& t = table.textures[(*list)[size_t(v)]];
          if (t.type == fh1::kPvsTextureBix)
            outcome = source.contains(texture_file(t.id, ".bix")) ? "type 4, .bix present" : "type 4, missing";
          else if (t.type == fh1::kPvsTextureCaff)
            outcome = source.contains(texture_file(t.id, ".bin")) ? "type 0, .bin present" : "type 0, missing";
          else outcome = "type " + std::to_string(t.type);
        }
        ++slot_outcomes[outcome];
      }
  }
  std::printf("render models: %llu parsed, %llu failed; %llu materials for %llu submeshes (%llu without one)\n",
              (unsigned long long)models, (unsigned long long)failed, (unsigned long long)materials,
              (unsigned long long)submeshes, (unsigned long long)bad_material);
  std::printf("uvOffsetScale: %llu models with one per submesh, %llu with none\n", (unsigned long long)with_uv,
              (unsigned long long)without_uv);
  for (const auto& n : no_uv) std::printf("  none: %s\n", n.c_str());
  std::printf("vertex layouts: %llu submeshes known, %llu not\n", (unsigned long long)layout_known,
              (unsigned long long)unknown_layout);
  for (const auto& [k, n] : unknown) std::printf("  unknown: %s (%llu)\n", k.c_str(), (unsigned long long)n);
  std::printf("texture slots:\n");
  for (const auto& [k, n] : slot_outcomes) std::printf("  %-24s %llu\n", k.c_str(), (unsigned long long)n);
  return failed ? 1 : 0;
}

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.size() == 3 && args[0] == "--dump") return dump(args[1], args[2]);
  if ((args.size() == 2 || args.size() == 3) && args[0] == "--names")
    return names(args[1], args.size() == 3 ? fs::path(args[2]) : fs::path());
  if ((args.size() == 2 || (args.size() == 3 && args[2] == "--objects")) && args[0] == "--render-models")
    return render_models(args[1], args.size() == 3);
  if (args.size() == 3 && args[0] == "--world-materials") return world_materials(args[1], args[2]);
  if ((args.size() == 3 || args.size() == 7) && args[0] == "--props") {
    double box[4];
    for (int k = 0; k < 4 && args.size() == 7; ++k) box[k] = std::stod(args[3 + k]);
    return props(args[1], args[2], args.size() == 7 ? box : nullptr);
  }
  if (args.size() == 4 && args[0] == "--extract") {
    std::string error;
    auto archive = fh1::Archive::open(args[1], &error);
    if (!archive) { std::cerr << error << "\n"; return 1; }
    auto r = archive->read(args[2]);
    if (!r.ok()) { std::cerr << r.error << "\n"; return 1; }
    std::ofstream(args[3], std::ios::binary).write(reinterpret_cast<const char*>(r.data.data()),
                                                   std::streamsize(r.data.size()));
    return 0;
  }
  fs::path root;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  bool verbose = false;
  std::string filter;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--game-root" && i + 1 < args.size()) root = args[++i];
    else if (args[i] == "--threads" && i + 1 < args.size()) threads = std::stoul(args[++i]);
    else if (args[i] == "--verbose") verbose = true;
    else if (args[i] == "--all-members") g_all_members = true;
    else if (args[i] == "--filter" && i + 1 < args.size()) filter = lower(args[++i]);
    else {
      std::cerr << "usage: fh1data-corpus --game-root <dir> [--threads N] [--verbose] [--filter text]\n"
                   "                      [--all-members]\n"
                   "       fh1data-corpus --dump <archive.zip> <member>\n"
                   "       fh1data-corpus --extract <archive.zip> <member> <out>\n"
                   "       fh1data-corpus --names <archive.zip> [members-dir]\n"
                   "       fh1data-corpus --render-models <dir|archive.zip>\n"
                   "       fh1data-corpus --world-materials <dir|archive.zip> <Colorado_00.pvs>\n"
                   "       fh1data-corpus --props <dir|archive.zip> <Colorado_00.pvs> [x0 z0 x1 z1]\n";
      return 2;
    }
  }
  if (root.empty()) { std::cerr << "--game-root is required\n"; return 2; }
  const fs::path media = find_media(root);
  std::vector<fs::path> zips, loose;
  for (const auto& entry : fs::recursive_directory_iterator(media)) {
    if (!entry.is_regular_file()) continue;
    const std::string rel = lower(fs::relative(entry.path(), media).generic_string());
    if (!filter.empty() && rel.find(filter) == std::string::npos) continue;
    const std::string ext = extension(entry.path().filename().string());
    if (ext == "zip") zips.push_back(entry.path());
    else if (ext == "carbin" || ext == "xds") loose.push_back(entry.path());
  }
  // Largest first so the long archives do not finish last.
  std::sort(zips.begin(), zips.end(), [](const fs::path& a, const fs::path& b) {
    return fs::file_size(a) > fs::file_size(b);
  });
  const auto start = std::chrono::steady_clock::now();
  std::atomic<size_t> next{0};
  std::vector<Stats> per_thread(threads);
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      for (size_t i; (i = next.fetch_add(1)) < zips.size();) check_archive(zips[i], media, per_thread[t]);
    });
  }
  for (auto& th : pool) th.join();
  Stats s;
  for (const auto& p : per_thread) s.merge(p);
  Stats loose_stats;
  for (const auto& path : loose) {
    const auto data = read_file(path);
    check_member("(loose)", fs::relative(path, media).generic_string(), data, false, loose_stats);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::printf("media: %s\n", media.string().c_str());
  std::printf("archives: %llu opened, %llu failed; members %llu\n",
              (unsigned long long)(s.archives - s.archives_failed),
              (unsigned long long)s.archives_failed, (unsigned long long)s.members);
  std::printf("  by method:");
  for (auto& [m, n] : s.methods) std::printf(" %u=%llu", m, (unsigned long long)n);
  std::printf("\n");
  std::printf("carbin/xds members read (decompressed + CRC): %llu ok, %llu failed\n",
              (unsigned long long)s.read_ok, (unsigned long long)s.read_failed);
  auto print_carbin = [](const char* label, const Stats& x) {
    std::printf("%s carbin: %llu ok, %llu header-only, %llu failed; stripped twins %llu "
                "(not decoded, %llu happen to parse)\n",
                label, (unsigned long long)x.carbin_ok, (unsigned long long)x.carbin_header_only,
                (unsigned long long)x.carbin_failed, (unsigned long long)x.stripped_total,
                (unsigned long long)x.stripped_parsed);
    std::printf("  parts %llu, sub-sections %llu, vertices %llu, indices outside pool %llu\n",
                (unsigned long long)x.parts, (unsigned long long)x.subsections,
                (unsigned long long)x.vertices, (unsigned long long)x.bad_indices);
  };
  print_carbin("archived", s);
  std::printf("  in cars/: %llu carbins, %llu sub-sections\n", (unsigned long long)s.car_carbins,
              (unsigned long long)s.car_subsections);
  print_carbin("loose", loose_stats);
  auto print_xds = [](const char* label, const Stats& x) {
    std::printf("%s xds: %llu ok (all levels), %llu base only, %llu unsupported, %llu failed; "
                "%llu levels decoded\n",
                label, (unsigned long long)x.xds_ok, (unsigned long long)x.xds_base_only,
                (unsigned long long)x.xds_unsupported, (unsigned long long)x.xds_failed,
                (unsigned long long)x.xds_levels);
    for (auto& [k, v] : x.xds_formats) std::printf("    %-48s %llu\n", k.c_str(), (unsigned long long)v);
    for (auto& [k, v] : x.xds_unsupported_why)
      std::printf("  unsupported: %-38s %llu\n", k.c_str(), (unsigned long long)v);
  };
  print_xds("archived", s);
  print_xds("loose", loose_stats);
  std::printf("xds layout vs payload: %llu exact, %llu differ\n",
              (unsigned long long)(s.xds_layout_exact + loose_stats.xds_layout_exact),
              (unsigned long long)(s.xds_layout_padded + loose_stats.xds_layout_padded));
  for (auto& [k, v] : s.xds_padded_shapes)
    std::printf("    %-48s %llu\n", k.c_str(), (unsigned long long)v);
  std::printf("members without a local header: %llu; without extra field 0x1123: %llu\n",
              (unsigned long long)s.no_local_header, (unsigned long long)s.no_extra_offset);
  std::printf("%s members read: %llu ok (%llu of them without a local header), %llu failed, "
              "%llu deflated skipped\n",
              g_all_members ? "all" : "carbin/xds", (unsigned long long)s.all_read_ok,
              (unsigned long long)s.no_local_read_ok, (unsigned long long)s.all_read_failed,
              (unsigned long long)s.all_deflate_skipped);
  std::printf("  LZX window that decoded each XMem member:");
  for (auto& [k, v] : s.lzx_windows)
    std::printf(" %d KiB=%llu", (1 << k) / 1024, (unsigned long long)v);
  std::printf("\n");
  std::printf("car archives: %llu of %llu parse completely\n", (unsigned long long)s.car_archives_ok,
              (unsigned long long)s.car_archives);
  std::printf("time: %.1f s on %u threads\n", seconds, threads);
  std::vector<std::string> failures = s.failures;
  failures.insert(failures.end(), loose_stats.failures.begin(), loose_stats.failures.end());
  std::sort(failures.begin(), failures.end());
  const size_t shown = verbose ? failures.size() : std::min<size_t>(failures.size(), 25);
  if (!failures.empty()) std::printf("problems (%zu, showing %zu):\n", failures.size(), shown);
  for (size_t i = 0; i < shown; ++i) std::printf("  %s\n", failures[i].c_str());
  return s.car_archives_ok == s.car_archives && s.archives_failed == 0 ? 0 : 1;
}
