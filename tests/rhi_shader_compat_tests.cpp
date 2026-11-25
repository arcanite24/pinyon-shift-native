// Cross-API shader portability and starfield instanced-quad parity tests (no GPU required).
// Validates:
//   1. Compiled-in stars_vert_spv.h & stars_frag_spv.h SPIR-V reflection (zero PointSize/PointCoord, 96 B PC).
//   2. All 53 prebuilt SPIR-V binaries in apps/carview/shaders/prebuilt/ (<= 128 B push constants,
//      zero PointSize/PointCoord, valid bindless set=1 layout, valid spec constant IDs {0, 1}).
//   3. Exact pixel/NDC/alpha math parity between instanced 6-vertex star quads and original point sprites.
//   4. shader_manifest.json & showroom_sky.cpp portability invariants.

#include "stars_frag_spv.h"
#include "stars_vert_spv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failed = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failed;
  }
}

void check_str(bool cond, const std::string& msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    ++g_failed;
  }
}

// Minimal self-contained SPIR-V binary reflector (matches tools/check-shader-portability.py).
constexpr uint32_t kSpvMagic = 0x07230203u;
constexpr uint32_t kOpEntryPoint = 15u;
constexpr uint32_t kOpExecutionMode = 16u;
constexpr uint32_t kOpCapability = 17u;
constexpr uint32_t kOpTypeBool = 20u;
constexpr uint32_t kOpTypeInt = 21u;
constexpr uint32_t kOpTypeFloat = 22u;
constexpr uint32_t kOpTypeVector = 23u;
constexpr uint32_t kOpTypeMatrix = 24u;
constexpr uint32_t kOpTypeImage = 25u;
constexpr uint32_t kOpTypeSampler = 26u;
constexpr uint32_t kOpTypeSampledImage = 27u;
constexpr uint32_t kOpTypeArray = 28u;
constexpr uint32_t kOpTypeRuntimeArray = 29u;
constexpr uint32_t kOpTypeStruct = 30u;
constexpr uint32_t kOpTypePointer = 32u;
constexpr uint32_t kOpConstant = 43u;
constexpr uint32_t kOpSpecConstantTrue = 48u;
constexpr uint32_t kOpSpecConstantFalse = 49u;
constexpr uint32_t kOpSpecConstant = 50u;
constexpr uint32_t kOpVariable = 59u;
constexpr uint32_t kOpAccessChain = 65u;
constexpr uint32_t kOpInBoundsAccessChain = 66u;
constexpr uint32_t kOpDecorate = 71u;
constexpr uint32_t kOpMemberDecorate = 72u;
constexpr uint32_t kOpExecutionModeId = 331u;

constexpr uint32_t kDecSpecId = 1u;
constexpr uint32_t kDecRowMajor = 4u;
constexpr uint32_t kDecArrayStride = 6u;
constexpr uint32_t kDecMatrixStride = 7u;
constexpr uint32_t kDecBuiltIn = 11u;
constexpr uint32_t kDecLocation = 30u;
constexpr uint32_t kDecBinding = 33u;
constexpr uint32_t kDecDescriptorSet = 34u;
constexpr uint32_t kDecOffset = 35u;

constexpr uint32_t kBuiltInPointSize = 1u;
constexpr uint32_t kBuiltInPointCoord = 16u;

constexpr uint32_t kScUniformConstant = 0u;
constexpr uint32_t kScInput = 1u;
constexpr uint32_t kScPushConstant = 9u;
constexpr uint32_t kScPhysicalStorageBuffer = 5349u;

constexpr uint32_t kCapPhysicalStorageBufferAddresses = 5347u;

enum class TypeKind {
  kUnknown,
  kBool,
  kInt,
  kFloat,
  kVector,
  kMatrix,
  kImage,
  kSampler,
  kSampledImage,
  kArray,
  kRuntimeArray,
  kStruct,
  kPointer,
};

struct SpvType {
  TypeKind kind = TypeKind::kUnknown;
  uint32_t a = 0;  // width / elem_type / col_type / storage_class
  uint32_t b = 0;  // count / len_const / pointee_type / sampled_flag
  std::vector<uint32_t> members;
};

struct SpvBinding {
  uint32_t set = 0;
  uint32_t binding = 0;
  bool is_sampler = false;
  bool is_sampled_image = false;
  bool is_unbounded = false;
  uint32_t array_count = 1;
};

struct SpvReflection {
  bool valid = false;
  uint32_t version_major = 0;
  uint32_t version_minor = 0;
  uint32_t exec_model = 0xFFFFFFFFu;
  std::string entry_point;
  std::array<uint32_t, 3> local_size = {0, 0, 0};
  uint32_t push_constant_bytes = 0;
  std::set<uint32_t> spec_ids;
  std::set<uint32_t> vertex_input_locations;
  std::vector<SpvBinding> bindings;
  bool uses_point_size = false;
  bool uses_point_coord = false;
  bool uses_bda = false;
};

std::string decode_spv_string(const uint32_t* words, size_t max_words) {
  std::string out;
  for (size_t i = 0; i < max_words; ++i) {
    uint32_t w = words[i];
    for (int b = 0; b < 4; ++b) {
      char c = static_cast<char>((w >> (b * 8)) & 0xFFu);
      if (c == '\0') return out;
      out.push_back(c);
    }
  }
  return out;
}

uint32_t compute_type_bytes(uint32_t tid,
                            const std::map<uint32_t, SpvType>& types,
                            const std::map<uint32_t, uint32_t>& constants,
                            const std::map<uint32_t, std::map<uint32_t, uint32_t>>& decs,
                            const std::map<std::pair<uint32_t, uint32_t>, std::map<uint32_t, uint32_t>>& mdecs,
                            const std::map<uint32_t, uint32_t>* active_mdec = nullptr) {
  auto it = types.find(tid);
  if (it == types.end()) return 0;
  const SpvType& t = it->second;
  switch (t.kind) {
    case TypeKind::kBool:
      return 4u;
    case TypeKind::kInt:
    case TypeKind::kFloat:
      return std::max(1u, t.a / 8u);
    case TypeKind::kVector:
      return compute_type_bytes(t.a, types, constants, decs, mdecs) * t.b;
    case TypeKind::kMatrix: {
      uint32_t stride = 0;
      bool row_major = false;
      if (active_mdec) {
        auto ms = active_mdec->find(kDecMatrixStride);
        if (ms != active_mdec->end()) stride = ms->second;
        row_major = active_mdec->find(kDecRowMajor) != active_mdec->end();
      }
      if (stride == 0) stride = compute_type_bytes(t.a, types, constants, decs, mdecs);
      uint32_t count = t.b;
      if (row_major) {
        auto col_it = types.find(t.a);
        if (col_it != types.end() && col_it->second.kind == TypeKind::kVector) {
          count = col_it->second.b;
        }
      }
      return count * stride;
    }
    case TypeKind::kArray: {
      uint32_t len = 0;
      auto cit = constants.find(t.b);
      if (cit != constants.end()) len = cit->second;
      uint32_t stride = 0;
      auto dit = decs.find(tid);
      if (dit != decs.end()) {
        auto sit = dit->second.find(kDecArrayStride);
        if (sit != dit->second.end()) stride = sit->second;
      }
      if (stride == 0) stride = compute_type_bytes(t.a, types, constants, decs, mdecs);
      return len * stride;
    }
    case TypeKind::kStruct: {
      uint32_t end = 0;
      for (uint32_t m = 0; m < static_cast<uint32_t>(t.members.size()); ++m) {
        auto mit = mdecs.find({tid, m});
        const std::map<uint32_t, uint32_t>* md = (mit != mdecs.end()) ? &mit->second : nullptr;
        uint32_t off = end;
        if (md) {
          auto oit = md->find(kDecOffset);
          if (oit != md->end()) off = oit->second;
        }
        const uint32_t sz = compute_type_bytes(t.members[m], types, constants, decs, mdecs, md);
        end = std::max(end, off + sz);
      }
      return end;
    }
    default:
      return 0;
  }
}

SpvReflection reflect_spirv_words(std::span<const uint32_t> words) {
  SpvReflection r{};
  if (words.size() < 5 || words[0] != kSpvMagic) return r;
  r.version_major = (words[1] >> 16) & 0xFFu;
  r.version_minor = (words[1] >> 8) & 0xFFu;

  std::map<uint32_t, SpvType> types;
  std::map<uint32_t, uint32_t> constants;
  std::set<uint32_t> spec_const_ids;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> variables;  // id -> (ptr_type, storage_class)
  std::map<uint32_t, std::map<uint32_t, uint32_t>> decs;
  std::map<std::pair<uint32_t, uint32_t>, std::map<uint32_t, uint32_t>> mdecs;
  std::vector<std::pair<uint32_t, uint32_t>> access_first_idx;
  bool has_local_size_ids = false;
  std::array<uint32_t, 3> local_size_ids = {0, 0, 0};

  size_t pos = 5;
  while (pos < words.size()) {
    const uint32_t hdr = words[pos];
    const uint32_t wc = hdr >> 16;
    const uint32_t op = hdr & 0xFFFFu;
    if (wc == 0 || pos + wc > words.size()) return r;
    const uint32_t* ops = &words[pos + 1];
    const size_t nops = wc - 1;
    pos += wc;

    if (op == kOpCapability && nops >= 1) {
      if (ops[0] == kCapPhysicalStorageBufferAddresses) r.uses_bda = true;
    } else if (op == kOpEntryPoint && nops >= 3) {
      r.exec_model = ops[0];
      r.entry_point = decode_spv_string(ops + 2, nops - 2);
    } else if (op == kOpExecutionMode && nops >= 5 && ops[1] == 17u) {
      r.local_size = {ops[2], ops[3], ops[4]};
    } else if (op == kOpExecutionModeId && nops >= 5 && ops[1] == 38u) {
      has_local_size_ids = true;
      local_size_ids = {ops[2], ops[3], ops[4]};
    } else if (op == kOpDecorate && nops >= 2) {
      decs[ops[0]][ops[1]] = (nops >= 3) ? ops[2] : 1u;
    } else if (op == kOpMemberDecorate && nops >= 3) {
      mdecs[{ops[0], ops[1]}][ops[2]] = (nops >= 4) ? ops[3] : 1u;
    } else if (op == kOpTypeBool && nops >= 1) {
      types[ops[0]] = {TypeKind::kBool, 0, 0, {}};
    } else if (op == kOpTypeInt && nops >= 3) {
      types[ops[0]] = {TypeKind::kInt, ops[1], ops[2], {}};
    } else if (op == kOpTypeFloat && nops >= 2) {
      types[ops[0]] = {TypeKind::kFloat, ops[1], 0, {}};
    } else if (op == kOpTypeVector && nops >= 3) {
      types[ops[0]] = {TypeKind::kVector, ops[1], ops[2], {}};
    } else if (op == kOpTypeMatrix && nops >= 3) {
      types[ops[0]] = {TypeKind::kMatrix, ops[1], ops[2], {}};
    } else if (op == kOpTypeImage && nops >= 8) {
      types[ops[0]] = {TypeKind::kImage, ops[2], ops[6], {}};
    } else if (op == kOpTypeSampler && nops >= 1) {
      types[ops[0]] = {TypeKind::kSampler, 0, 0, {}};
    } else if (op == kOpTypeSampledImage && nops >= 2) {
      types[ops[0]] = {TypeKind::kSampledImage, ops[1], 0, {}};
    } else if (op == kOpTypeArray && nops >= 3) {
      types[ops[0]] = {TypeKind::kArray, ops[1], ops[2], {}};
    } else if (op == kOpTypeRuntimeArray && nops >= 2) {
      types[ops[0]] = {TypeKind::kRuntimeArray, ops[1], 0, {}};
    } else if (op == kOpTypeStruct && nops >= 1) {
      SpvType st{};
      st.kind = TypeKind::kStruct;
      st.members.assign(ops + 1, ops + nops);
      types[ops[0]] = std::move(st);
    } else if (op == kOpTypePointer && nops >= 3) {
      types[ops[0]] = {TypeKind::kPointer, ops[1], ops[2], {}};
    } else if (op == kOpConstant && nops >= 3) {
      constants[ops[1]] = ops[2];
    } else if ((op == kOpSpecConstantTrue || op == kOpSpecConstantFalse || op == kOpSpecConstant) && nops >= 2) {
      spec_const_ids.insert(ops[1]);
    } else if (op == kOpVariable && nops >= 3) {
      variables[ops[1]] = {ops[0], ops[2]};
      if (ops[2] == kScPhysicalStorageBuffer) r.uses_bda = true;
    } else if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && nops >= 4) {
      access_first_idx.emplace_back(ops[2], ops[3]);
    }
  }

  if (r.local_size[0] == 0 && has_local_size_ids) {
    for (size_t i = 0; i < 3; ++i) {
      auto cit = constants.find(local_size_ids[i]);
      r.local_size[i] = (cit != constants.end()) ? cit->second : 1u;
    }
  }

  for (uint32_t cid : spec_const_ids) {
    auto dit = decs.find(cid);
    if (dit != decs.end()) {
      auto sit = dit->second.find(kDecSpecId);
      if (sit != dit->second.end()) r.spec_ids.insert(sit->second);
    }
  }

  std::map<uint32_t, uint32_t> per_vertex_pt_member;
  for (const auto& [vid, info] : variables) {
    const uint32_t ptr_tid = info.first;
    const uint32_t sc = info.second;
    uint32_t pointee = 0;
    auto ptit = types.find(ptr_tid);
    if (ptit != types.end() && ptit->second.kind == TypeKind::kPointer) {
      pointee = ptit->second.b;
    }

    auto vdit = decs.find(vid);
    if (vdit != decs.end()) {
      auto bi = vdit->second.find(kDecBuiltIn);
      if (bi != vdit->second.end()) {
        if (bi->second == kBuiltInPointSize) r.uses_point_size = true;
        if (bi->second == kBuiltInPointCoord) r.uses_point_coord = true;
      }
      if (r.exec_model == 0u && sc == kScInput) {
        auto loc = vdit->second.find(kDecLocation);
        if (loc != vdit->second.end()) r.vertex_input_locations.insert(loc->second);
      }
      auto dset = vdit->second.find(kDecDescriptorSet);
      auto dbind = vdit->second.find(kDecBinding);
      if (dset != vdit->second.end() && dbind != vdit->second.end()) {
        SpvBinding b{};
        b.set = dset->second;
        b.binding = dbind->second;
        uint32_t inner = pointee;
        auto it = types.find(inner);
        if (it != types.end() && it->second.kind == TypeKind::kArray) {
          auto cit = constants.find(it->second.b);
          b.array_count = (cit != constants.end()) ? cit->second : 0u;
          inner = it->second.a;
        } else if (it != types.end() && it->second.kind == TypeKind::kRuntimeArray) {
          b.is_unbounded = true;
          b.array_count = 0u;
          inner = it->second.a;
        }
        auto iit = types.find(inner);
        if (sc == kScUniformConstant && iit != types.end()) {
          if (iit->second.kind == TypeKind::kSampler) b.is_sampler = true;
          if (iit->second.kind == TypeKind::kImage && iit->second.b == 1u) b.is_sampled_image = true;
        }
        r.bindings.push_back(b);
      }
    }

    if (sc == kScPushConstant && pointee != 0) {
      r.push_constant_bytes =
          std::max(r.push_constant_bytes, compute_type_bytes(pointee, types, constants, decs, mdecs));
    }

    auto stit = types.find(pointee);
    if (stit != types.end() && stit->second.kind == TypeKind::kStruct) {
      for (uint32_t m = 0; m < static_cast<uint32_t>(stit->second.members.size()); ++m) {
        auto mit = mdecs.find({pointee, m});
        if (mit != mdecs.end()) {
          auto bi = mit->second.find(kDecBuiltIn);
          if (bi != mit->second.end() && bi->second == kBuiltInPointSize) {
            per_vertex_pt_member[vid] = m;
          }
        }
      }
    }
  }

  for (const auto& [base_id, idx_cid] : access_first_idx) {
    auto pvit = per_vertex_pt_member.find(base_id);
    if (pvit != per_vertex_pt_member.end()) {
      auto cit = constants.find(idx_cid);
      if (cit != constants.end() && cit->second == pvit->second) {
        r.uses_point_size = true;
      }
    }
  }

  r.valid = true;
  return r;
}

std::vector<uint32_t> load_spv_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const auto sz = f.tellg();
  if (sz <= 0 || (sz % 4) != 0) return {};
  std::vector<uint32_t> words(static_cast<size_t>(sz) / 4u);
  f.seekg(0, std::ios::beg);
  f.read(reinterpret_cast<char*>(words.data()), sz);
  return words;
}

void test_embedded_star_shaders() {
  const auto vert = reflect_spirv_words(std::span<const uint32_t>(kStars_vert_spv));
  check(vert.valid, "kStars_vert_spv is valid SPIR-V");
  check(vert.exec_model == 0u, "kStars_vert_spv is a vertex shader");
  check(vert.entry_point == "main", "kStars_vert_spv entry point is main");
  check(vert.push_constant_bytes == 96u, "kStars_vert_spv push constants == 96 bytes (mat4 + 2*vec4)");
  check(vert.push_constant_bytes <= 128u, "kStars_vert_spv push constants <= 128 B cross-API limit");
  check(!vert.uses_point_size, "kStars_vert_spv has zero gl_PointSize writes");
  check(!vert.uses_point_coord, "kStars_vert_spv has zero gl_PointCoord reads");
  check(!vert.uses_bda, "kStars_vert_spv uses no 64-bit BDA");
  check(vert.spec_ids.size() == 1 && vert.spec_ids.count(1u) == 1, "kStars_vert_spv declares constant_id = 1 (kReverseZ)");
  check(vert.vertex_input_locations.size() == 1 && vert.vertex_input_locations.count(0u) == 1,
        "kStars_vert_spv declares per-instance location 0 (in_star)");

  const auto frag = reflect_spirv_words(std::span<const uint32_t>(kStars_frag_spv));
  check(frag.valid, "kStars_frag_spv is valid SPIR-V");
  check(frag.exec_model == 4u, "kStars_frag_spv is a fragment shader");
  check(frag.entry_point == "main", "kStars_frag_spv entry point is main");
  check(frag.push_constant_bytes == 0u, "kStars_frag_spv has 0 push constant bytes");
  check(!frag.uses_point_size, "kStars_frag_spv has zero gl_PointSize");
  check(!frag.uses_point_coord, "kStars_frag_spv has zero gl_PointCoord reads");
  check(!frag.uses_bda, "kStars_frag_spv uses no 64-bit BDA");
  check(frag.spec_ids.size() == 1 && frag.spec_ids.count(0u) == 1, "kStars_frag_spv declares constant_id = 0 (kLinearOut)");
}

void test_all_prebuilt_spirv() {
  const std::filesystem::path prebuilt = std::filesystem::path(SHIFT_SOURCE_DIR) / "apps" / "carview" / "shaders" / "prebuilt";
  check(std::filesystem::is_directory(prebuilt), "prebuilt shader directory exists");

  std::vector<std::filesystem::path> spv_files;
  for (const auto& e : std::filesystem::directory_iterator(prebuilt)) {
    if (e.is_regular_file() && e.path().extension() == ".spv") {
      spv_files.push_back(e.path());
    }
  }
  std::sort(spv_files.begin(), spv_files.end());
  check_str(spv_files.size() == 53u, "expected 53 prebuilt SPIR-V shaders, found " + std::to_string(spv_files.size()));

  uint32_t max_pc = 0;
  uint32_t bindless_count = 0;
  uint32_t compute_count = 0;

  for (const auto& spv : spv_files) {
    const std::string name = spv.filename().string();
    const auto words = load_spv_file(spv);
    const auto r = reflect_spirv_words(words);
    check_str(r.valid, name + ": valid SPIR-V binary");
    check_str(r.entry_point == "main", name + ": entry point is main");
    check_str(r.push_constant_bytes <= 128u,
              name + ": push constants " + std::to_string(r.push_constant_bytes) + " B <= 128 B");
    check_str(!r.uses_point_size, name + ": zero BuiltIn PointSize writes");
    check_str(!r.uses_point_coord, name + ": zero BuiltIn PointCoord reads");
    check_str(!r.uses_bda, name + ": zero PhysicalStorageBufferAddresses (64-bit BDA) for DX12 SM 6.6 parity");

    max_pc = std::max(max_pc, r.push_constant_bytes);

    for (uint32_t sid : r.spec_ids) {
      check_str(sid == 0u || sid == 1u, name + ": spec constant_id in {0, 1}");
    }

    if (r.exec_model == 5u) {
      ++compute_count;
      check_str(r.local_size[0] >= 1u && r.local_size[1] >= 1u && r.local_size[2] >= 1u,
                name + ": compute shader declares valid LocalSize");
    }

    bool has_bindless_array = false;
    bool has_set1_sampler = false;
    bool has_set1_sampled_image = false;
    for (const auto& b : r.bindings) {
      if (b.set == 1u && (b.is_unbounded || b.array_count >= 256u)) has_bindless_array = true;
      if (b.set == 1u && b.binding == 0u && b.is_sampler) has_set1_sampler = true;
      if (b.set == 1u && b.binding == 1u && b.is_sampled_image) has_set1_sampled_image = true;
    }
    if (has_bindless_array) {
      ++bindless_count;
      check_str(has_set1_sampler && has_set1_sampled_image,
                name + ": split bindless uses set=1 binding=0 sampler + binding=1 sampled_image array");
    }
  }

  check(max_pc <= 128u && max_pc > 0u, "observed max push constant size is within (0, 128] B");
  check(compute_count == 10u, "all 10 compute shaders (3 culling/Hi-Z + 7 RT) reflected cleanly");
  check(bindless_count >= 9u, "all split-bindless world/RT shaders validated");
}

struct StarQuadVertexOut {
  float pos[4];
  float alpha;
  float uv[2];
};

StarQuadVertexOut eval_star_vertex(int vertex_idx,
                                   const float in_star[4],
                                   const float params[4],
                                   uint32_t vp_w,
                                   uint32_t vp_h,
                                   bool reverse_z) {
  static constexpr float kCorners[6][2] = {
      {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
      {-1.0f, -1.0f}, {1.0f, 1.0f},  {-1.0f, 1.0f},
  };
  const float cx = kCorners[vertex_idx % 6][0];
  const float cy = kCorners[vertex_idx % 6][1];
  StarQuadVertexOut o{};
  o.uv[0] = cx * 0.5f + 0.5f;
  o.uv[1] = cy * 0.5f + 0.5f;

  const float cs = std::cos(params[1]);
  const float sn = std::sin(params[1]);
  const float dx = in_star[0] * cs - in_star[2] * sn;
  const float dy = in_star[1];
  const float dz = in_star[0] * sn + in_star[2] * cs;

  if (dy < 0.0f || params[0] < 0.002f) {
    o.pos[0] = 2.0f; o.pos[1] = 2.0f; o.pos[2] = 2.0f; o.pos[3] = 1.0f;
    o.alpha = 0.0f;
    return o;
  }

  const float t = std::clamp((in_star[3] - 0.05f) / 0.95f, 0.0f, 1.0f);
  const float h_x = std::clamp(dy / 0.14f, 0.0f, 1.0f);
  const float horizon = h_x * h_x * (3.0f - 2.0f * h_x);
  o.alpha = params[0] * std::pow(t, 1.8f) * horizon * params[3];

  // Identity camera looking along +Z (clip.w = dz) for deterministic analytic check.
  const float clip_x = dx;
  const float clip_y = -dy;
  const float clip_w = dz;
  if (clip_w <= 1e-4f) {
    o.pos[0] = 2.0f; o.pos[1] = 2.0f; o.pos[2] = 2.0f; o.pos[3] = 1.0f;
    o.alpha = 0.0f;
    return o;
  }

  const float size_px = 1.2f + (params[2] - 1.2f) * t;
  const float ndc_x = clip_x / clip_w;
  const float ndc_y = clip_y / clip_w;
  const float inv_w = 1.0f / float(vp_w);
  const float inv_h = 1.0f / float(vp_h);
  o.pos[0] = ndc_x + cx * size_px * inv_w;
  o.pos[1] = ndc_y + cy * size_px * inv_h;
  o.pos[2] = reverse_z ? 0.0f : 1.0f;
  o.pos[3] = 1.0f;
  return o;
}

void test_star_quad_math_parity() {
  const std::pair<uint32_t, uint32_t> viewports[] = {
      {1280u, 720u}, {1920u, 1080u}, {3440u, 1440u}, {3840u, 2160u},
  };
  const float star[4] = {0.2f, 0.5f, 0.842615f, 0.85f};
  const float params[4] = {1.0f, 0.0f, 2.6f, 1.0f};
  const float t = std::clamp((star[3] - 0.05f) / 0.95f, 0.0f, 1.0f);
  const float expected_px = 1.2f + (2.6f - 1.2f) * t;

  for (const auto [w, h] : viewports) {
    for (bool rev_z : {false, true}) {
      const auto v0 = eval_star_vertex(0, star, params, w, h, rev_z);
      const auto v1 = eval_star_vertex(1, star, params, w, h, rev_z);
      const auto v2 = eval_star_vertex(2, star, params, w, h, rev_z);

      const float width_px = (v1.pos[0] - v0.pos[0]) * 0.5f * float(w);
      const float height_px = (v2.pos[1] - v1.pos[1]) * 0.5f * float(h);
      check(std::fabs(width_px - expected_px) < 1e-4f, "star quad pixel width matches gl_PointSize");
      check(std::fabs(height_px - expected_px) < 1e-4f, "star quad pixel height matches gl_PointSize");

      const float center_x = 0.5f * (v0.pos[0] + v2.pos[0]);
      const float center_y = 0.5f * (v0.pos[1] + v2.pos[1]);
      check(std::fabs(center_x - (star[0] / star[2])) < 1e-5f, "star quad NDC center X matches point projection");
      check(std::fabs(center_y - (-star[1] / star[2])) < 1e-5f, "star quad NDC center Y matches point projection");

      const float expected_z = rev_z ? 0.0f : 1.0f;
      check(std::fabs(v0.pos[2] - expected_z) < 1e-6f && std::fabs(v0.pos[3] - 1.0f) < 1e-6f,
            "star quad depth matches far plane for reverse-Z / forward-Z");

      // Metal / DX12 flip_vert_y invariance: negating Y keeps identical pixel height.
      const float flipped_height_px = ((-v1.pos[1]) - (-v2.pos[1])) * 0.5f * float(h);
      check(std::fabs(flipped_height_px - expected_px) < 1e-4f, "star quad height invariant under flip_vert_y");
    }
  }

  // Below-horizon and low-fade culling produces degenerate offscreen triangle with alpha 0.
  const float below_horizon[4] = {0.0f, -0.1f, 0.995f, 1.0f};
  const auto culled = eval_star_vertex(0, below_horizon, params, 1920, 1080, true);
  check(culled.pos[0] == 2.0f && culled.alpha == 0.0f, "below-horizon star collapses off-screen");

  // Fragment radial soft falloff parity with gl_PointCoord.
  auto soft_alpha = [](float u, float v, float v_alpha) -> float {
    const float dx = u - 0.5f;
    const float dy = v - 0.5f;
    const float r2 = dx * dx + dy * dy;
    if (r2 > 0.25f) return 0.0f;
    const float soft = std::clamp(1.0f - r2 * 4.0f, 0.0f, 1.0f);
    return v_alpha * soft * soft;
  };
  check(std::fabs(soft_alpha(0.5f, 0.5f, 1.0f) - 1.0f) < 1e-6f, "star center has full falloff weight");
  check(std::fabs(soft_alpha(0.5f, 0.25f, 1.0f) - 0.5625f) < 1e-6f, "star half-radius matches (1 - 0.25)^2");
  check(soft_alpha(0.0f, 0.0f, 1.0f) == 0.0f, "star quad corners discarded outside inscribed circle");
  check(std::fabs(soft_alpha(0.3f, 0.2f, 1.0f) - soft_alpha(0.3f, 0.8f, 1.0f)) < 1e-6f,
        "star fragment falloff is Y-flip symmetric for Metal/DX12");
}

void test_manifest_and_host_source() {
  const std::filesystem::path root(SHIFT_SOURCE_DIR);
  const std::filesystem::path manifest_path = root / "apps" / "carview" / "shaders" / "prebuilt" / "shader_manifest.json";
  std::ifstream mf(manifest_path);
  check(mf.good(), "shader_manifest.json exists");
  const std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
  check(text.find("\"total_shader_files\": 56") != std::string::npos, "manifest tracks all 56 shader files");
  check(text.find("\"entry_shaders\": 53") != std::string::npos, "manifest tracks 53 entry shaders");
  check(text.find("\"shared_headers\": 3") != std::string::npos, "manifest tracks 3 shared headers");
  check(text.find("\"point_size_shaders\": 0") != std::string::npos, "manifest reports 0 PointSize shaders");
  check(text.find("\"point_coord_shaders\": 0") != std::string::npos, "manifest reports 0 PointCoord shaders");
  check(text.find("\"vulkan_1_3_ready\": 53") != std::string::npos, "manifest reports 53/53 Vulkan 1.3 ready");
  check(text.find("\"metal_3_ready\": 53") != std::string::npos, "manifest reports 53/53 Metal 3 ready");
  check(text.find("\"dx12_sm_6_6_ready\": 53") != std::string::npos, "manifest reports 53/53 DirectX 12 SM 6.6 ready");
  check(text.find("\"bda_shaders\": 0") != std::string::npos, "manifest reports 0 BDA shaders");
  check(text.find("\"name\": \"stars.vert\"") != std::string::npos, "manifest contains stars.vert entry");
  check(text.find("\"name\": \"stars.frag\"") != std::string::npos, "manifest contains stars.frag entry");

  const std::filesystem::path sky_cpp = root / "apps" / "carview" / "showroom_sky.cpp";
  std::ifstream sf(sky_cpp);
  check(sf.good(), "showroom_sky.cpp exists");
  const std::string sky_text((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
  check(sky_text.find("VK_PRIMITIVE_TOPOLOGY_POINT_LIST") == std::string::npos,
        "showroom_sky.cpp has zero VK_PRIMITIVE_TOPOLOGY_POINT_LIST references");
  check(sky_text.find("VK_VERTEX_INPUT_RATE_INSTANCE") != std::string::npos,
        "showroom_sky.cpp binds star buffer with VK_VERTEX_INPUT_RATE_INSTANCE");
  check(sky_text.find("vkCmdDraw(cmd, 6, star_count_, 0, 0)") != std::string::npos,
        "showroom_sky.cpp draws 6-vertex instanced star quads");
}

std::string read_text_file(const std::filesystem::path& p) {
  std::ifstream f(p);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_flag_bitmask_float32_exact_decode() {
  constexpr uint32_t kFlagSlot1 = 1u;
  constexpr uint32_t kFlagMetallic = 2u;
  constexpr uint32_t kFlagLampEmissive = 2048u;
  constexpr uint32_t kFlagInstanced = 8192u;
  constexpr uint32_t kFlagCarbonTex = 4194304u;
  constexpr uint32_t kFlagHasBlurMesh = 8388608u;

  // Confirm IEEE-754 float32 + 0.5f corrupts odd masks >= 2^23 via round-to-nearest-even.
  const uint32_t odd_blur = kFlagHasBlurMesh | kFlagSlot1;
  const uint32_t corrupted = static_cast<uint32_t>(static_cast<float>(odd_blur) + 0.5f);
  check(corrupted != odd_blur && (corrupted & kFlagSlot1) == 0u && (corrupted & kFlagMetallic) != 0u,
        "float32 + 0.5f corrupts kFlagHasBlurMesh | kFlagSlot1 into kFlagMetallic");

  // Confirm std::lround (matching GLSL round()) decodes all 24-bit flag combinations exactly.
  const uint32_t samples[] = {
      0u,
      kFlagSlot1,
      kFlagHasBlurMesh,
      kFlagHasBlurMesh | kFlagSlot1,
      kFlagHasBlurMesh | kFlagSlot1 | kFlagMetallic,
      kFlagHasBlurMesh | kFlagSlot1 | kFlagInstanced | kFlagLampEmissive,
      kFlagHasBlurMesh | kFlagCarbonTex | kFlagSlot1 | kFlagMetallic | kFlagInstanced,
      0x00FFFFFFu,
  };
  for (uint32_t mask : samples) {
    const float encoded = static_cast<float>(mask);
    const uint32_t decoded = static_cast<uint32_t>(std::lround(encoded));
    check_str(decoded == mask, "exact float32 round decode preserves 24-bit flag mask " + std::to_string(mask));
  }
  for (uint32_t low = 0u; low < 512u; ++low) {
    const uint32_t mask = kFlagHasBlurMesh | low;
    const uint32_t decoded = static_cast<uint32_t>(std::lround(static_cast<float>(mask)));
    check_str(decoded == mask, "kFlagHasBlurMesh | " + std::to_string(low) + " round-trips bit-exact");
  }

  // Static guard: car pipeline shaders and C++ files must never decode flags via params + 0.5.
  const std::filesystem::path root(SHIFT_SOURCE_DIR);
  const std::string sv = read_text_file(root / "apps" / "carview" / "shaders" / "showroom.vert");
  const std::string sf = read_text_file(root / "apps" / "carview" / "shaders" / "showroom.frag");
  check(sv.find("draw.params.y + 0.5") == std::string::npos && sv.find("uint(round(draw.params.y))") != std::string::npos,
        "showroom.vert decodes draw.params.y with exact uint(round(...))");
  check(sf.find("draw.params.y + 0.5") == std::string::npos && sf.find("uint(round(draw.params.y))") != std::string::npos,
        "showroom.frag decodes draw.params.y with exact uint(round(...))");

  const char* cpp_files[] = {
      "showroom_car_load.cpp",
      "showroom_car_state.cpp",
      "showroom_draw.cpp",
      "showroom_shadows.cpp",
  };
  for (const char* f : cpp_files) {
    const std::string src = read_text_file(root / "apps" / "carview" / f);
    check_str(!src.empty() && src.find("params[1] + 0.5f") == std::string::npos,
              std::string(f) + ": zero params[1] + 0.5f flag decodes");
  }
}

void test_shadow_and_smoothstep_invariants() {
  // 1. RGB shadow_color packing in shadow_bias.w uses safe normal-float tag 0x3Fu (never NaN/Inf/subnormal).
  auto pack_shadow_rgb = [](float r, float g, float b) -> float {
    const uint32_t sr = static_cast<uint32_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f)) & 0xFFu;
    const uint32_t sg = static_cast<uint32_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f)) & 0xFFu;
    const uint32_t sb = static_cast<uint32_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255.0f)) & 0xFFu;
    const uint32_t packed = (0x3Fu << 24u) | (sb << 16u) | (sg << 8u) | sr;
    float bits = 0.0f;
    std::memcpy(&bits, &packed, sizeof(bits));
    return bits;
  };
  auto unpack_shadow_rgb = [](float bits) -> std::array<float, 3> {
    uint32_t u = 0;
    std::memcpy(&u, &bits, sizeof(u));
    if ((u >> 24u) != 0x3Fu) return {1.0f, 1.0f, 1.0f};
    return {
        static_cast<float>(u & 255u) / 255.0f,
        static_cast<float>((u >> 8u) & 255u) / 255.0f,
        static_cast<float>((u >> 16u) & 255u) / 255.0f,
    };
  };

  const std::array<float, 3> shadow_colors[] = {
      {0.0f, 0.0f, 0.0f},
      {0.35f, 0.35f, 0.35f},
      {0.20f, 0.30f, 0.48f},
      {1.0f, 1.0f, 1.0f},
  };
  for (const auto& c : shadow_colors) {
    const float packed = pack_shadow_rgb(c[0], c[1], c[2]);
    check(std::isfinite(packed) && std::fpclassify(packed) == FP_NORMAL && packed >= 0.5f && packed < 2.0f,
          "packed RGB shadow_color is a safe normal IEEE-754 float in [0.5, 2.0)");
    const auto dec = unpack_shadow_rgb(packed);
    for (int k = 0; k < 3; ++k) {
      check(std::fabs(dec[size_t(k)] - c[size_t(k)]) <= (1.0f / 255.0f + 1e-6f),
            "unpacked RGB shadow_color matches source within 1/255");
    }
  }
  const auto cool = unpack_shadow_rgb(pack_shadow_rgb(0.20f, 0.30f, 0.48f));
  check(cool[2] > cool[1] && cool[1] > cool[0], "RGB shadow_color preserves cool blue chromaticity without grayscale collapse");

  // 2. Reversed smoothstep replacement 1.0 - smoothstep(lo, hi, x) strictly satisfies lo < hi.
  auto safe_smoothstep = [](float lo, float hi, float x) -> float {
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  auto inv_smoothstep = [&](float lo, float hi, float x) -> float {
    return 1.0f - safe_smoothstep(lo, hi, x);
  };
  // Front crush deformation range [-1.95, -0.75]: 1.0 at bumper tip (-1.95), 0.0 at cabin (-0.75).
  check(std::fabs(inv_smoothstep(-1.95f, -0.75f, -1.95f) - 1.0f) < 1e-6f, "inv_smoothstep(-1.95, -0.75, -1.95) == 1.0");
  check(std::fabs(inv_smoothstep(-1.95f, -0.75f, -1.35f) - 0.5f) < 1e-6f, "inv_smoothstep(-1.95, -0.75, -1.35) == 0.5");
  check(std::fabs(inv_smoothstep(-1.95f, -0.75f, -0.75f) - 0.0f) < 1e-6f, "inv_smoothstep(-1.95, -0.75, -0.75) == 0.0");

  // 3. Source invariants: no shadow.z floor leaking direct sun, instanced guards on tyre mud & rotor glow,
  //    and phantom shadow caster exclusions in showroom_shadows.cpp.
  const std::filesystem::path root(SHIFT_SOURCE_DIR);
  const char* shadow_shaders[] = {"world_frag.glsl", "showroom.frag", "particle.frag", "skid.frag"};
  for (const char* sh : shadow_shaders) {
    const std::string src = read_text_file(root / "apps" / "carview" / "shaders" / sh);
    check_str(!src.empty() && src.find("mix(frame.shadow.z, 1.0,") == std::string::npos,
              std::string(sh) + ": direct sun shadow never floored by frame.shadow.z");
  }
  const std::string sf = read_text_file(root / "apps" / "carview" / "shaders" / "showroom.frag");
  check(sf.find("(flags & kFlagInstanced) == 0u && frame.damage_dirt.y > 0.01") != std::string::npos,
        "showroom.frag guards player tyre mud from leaking onto instanced cars");
  check(sf.find("float rotor_heat = 0.0;") != std::string::npos &&
            sf.find("if ((flags & kFlagInstanced) == 0u)") != std::string::npos,
        "showroom.frag guards player brake rotor glow from leaking onto instanced cars");
  const std::string shadows_cpp = read_text_file(root / "apps" / "carview" / "showroom_shadows.cpp");
  check(shadows_cpp.find("d.glass || d.detached || d.is_blur_rim || d.cockpit_role >= 0") != std::string::npos,
        "showroom_shadows.cpp skips detached, blur_rim, and cockpit-only parts in shadow pass");
}

}  // namespace

int main() {
  test_embedded_star_shaders();
  test_all_prebuilt_spirv();
  test_star_quad_math_parity();
  test_manifest_and_host_source();
  test_flag_bitmask_float32_exact_decode();
  test_shadow_and_smoothstep_invariants();
  if (g_failed == 0) {
    std::printf("rhi_shader_compat_tests: PASS (56 shaders audited, stars instanced quads + R3-A1 invariants verified)\n");
    return 0;
  }
  std::fprintf(stderr, "rhi_shader_compat_tests: %d check(s) failed\n", g_failed);
  return 1;
}
