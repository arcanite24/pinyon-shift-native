// Car physics (#1071 P1): the physics world's per-body step.
//
// The physics-world step (base sub_82D4DE30, below CPhysicsWorld slot 22
// sub_82D4E2E0) walks its bodies and calls sub_82D46EF8(world, body, dt,
// ..., r6) for each one, on the simulation thread and from the islands
// jobs on the workers. The step clears a flag at body+2731, runs a body
// update, and then either the dynamics step sub_82D3A528 (body+2756 zero)
// or another update. Everything under it is about half of the physics
// time.
//
// pinyon_shift_physics_body_probe logs the bodies stepped, how often and
// with which dt, beside the cars of the world's car vector, so a body can
// be matched to its car: a car's ICarDynamics (car+0x2D90) sits at
// body+0xAD0. One step a simulation tick: 8.33 ms at 120 fps on the
// desktop, 16.67 ms at 60 fps on the Deck. The player's body always takes
// the dynamics step; AI bodies switch between it and the other branch as
// the race goes (2-6 of 7 on the dynamics step on the Deck).
//
// pinyon_shift_physics_ai_step_interval N > 1 runs an AI body's dynamics
// step on one tick in N with the summed time, staggered so the AI bodies
// share the ticks; the player's body steps every tick.
//
// pinyon_shift_physics_oracle (RF-5, docs/REIMPLEMENTATION_PLAN.md RI-0.5)
// snapshots the player's body at step entry on the first tick at or after a
// route frame, restores (injects) that snapshot at later route frames, and
// writes the body's bytes at every step entry for a number of ticks after
// each, so tools/re/physics-inject-compare.py can check that the trace after
// an injection repeats the one after the snapshot (A/A). The body is the
// 0x5040 bytes the full step prefetches (CCarDynamics, ICarDynamics at
// +0xAD0). Off by default; off, only the cvar check in Enabled() runs.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include "fh1_guest_address.h"
#include "fh1_render_test.h"
#include "guest_native/guest_native.h"
#include "pinyon_shift_diagnostics.h"

REXCVAR_DEFINE_BOOL(pinyon_shift_physics_body_probe, false, "Pinyon Shift",
                    "Diagnostic (#1071): log the physics bodies stepped each second "
                    "beside the world's cars")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(pinyon_shift_physics_ai_step_interval, 1, "Pinyon Shift",
                      "Step AI cars' full dynamics on one simulation tick in N with the "
                      "summed time; the player's car steps every tick (#1071; 1 = every tick)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pinyon_shift_physics_oracle, false, "Pinyon Shift",
                    "Physics oracle (RF-5): snapshot, inject and trace the player's car "
                    "body at the frames the other pinyon_shift_physics_oracle_* cvars give");
REXCVAR_DEFINE_UINT32(pinyon_shift_physics_oracle_snapshot_frame, 0, "Pinyon Shift",
                      "Physics oracle: route frame at which the player's body is snapshot "
                      "(first step at or after it; 0 = the first step)");
REXCVAR_DEFINE_STRING(pinyon_shift_physics_oracle_inject_frames, "", "Pinyon Shift",
                      "Physics oracle: comma-separated route frames at which the snapshot "
                      "is restored into the player's body (empty = never)");
REXCVAR_DEFINE_UINT32(pinyon_shift_physics_oracle_trace_ticks, 240, "Pinyon Shift",
                      "Physics oracle: steps traced after the snapshot and each injection");
REXCVAR_DEFINE_DOUBLE(pinyon_shift_physics_oracle_fixed_dt, 0.0, "Pinyon Shift",
                      "Physics oracle: step the player's body with this dt in seconds "
                      "(0 = the game's dt)");
REXCVAR_DEFINE_STRING(pinyon_shift_physics_oracle_skip, "", "Pinyon Shift",
                      "Physics oracle: body byte ranges a restore leaves alone, as hex "
                      "start-end pairs separated by commas (for example 1800-1804)");
REXCVAR_DEFINE_STRING(pinyon_shift_physics_oracle_path, "", "Pinyon Shift",
                      "Physics oracle: trace file (empty = physics-oracle.bin in the "
                      "route's output directory)");

namespace pinyon_shift::guest_native {
namespace {

constexpr bool kV4 = pinyon_shift::fh1::kTitleUpdateV4;

uint8_t* Host(uint8_t* base, uint32_t address) {
  return base + address + rex::memory::detail::PhysicalHostOffset(address);
}
uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Host(base, address), sizeof(value));
  return std::byteswap(value);
}

bool ProbeEnabled() { return REXCVAR_GET(pinyon_shift_physics_body_probe); }
uint32_t AiInterval() { return REXCVAR_GET(pinyon_shift_physics_ai_step_interval); }
bool OracleEnabled() { return REXCVAR_GET(pinyon_shift_physics_oracle); }
bool Enabled() { return ProbeEnabled() || AiInterval() > 1 || OracleEnabled(); }

// The world's car vector: [[[0x832DF024] + 4] + 4] + 128/132.
template <typename Visit>
void ForEachCar(uint8_t* base, Visit visit) {
  const uint32_t holder = LoadU32(base, FH1_ADDR(0x832DF024u));
  const uint32_t manager = holder ? LoadU32(base, holder + 4) : 0;
  const uint32_t world = manager ? LoadU32(base, manager + 4) : 0;
  if (!world) return;
  const uint32_t begin = LoadU32(base, world + 128), end = LoadU32(base, world + 132);
  for (uint32_t entry = begin; entry < end && entry - begin < 4 * 64; entry += 4) {
    const uint32_t car = LoadU32(base, entry);
    if (car) visit(car);
  }
}

struct AiBody {
  bool ai = false;
  uint32_t phase = 0;
  uint32_t ticks = 0;
  double carried = 0.0;
};
std::mutex ai_mutex;
std::unordered_map<uint32_t, AiBody> ai_bodies;
uint32_t ai_count = 0;
auto ai_since = std::chrono::steady_clock::now();

// Whether the body is an AI car's (driver not 0), by its ICarDynamics.
bool IsAiBody(uint8_t* base, uint32_t body) {
  bool ai = false;
  ForEachCar(base, [&](uint32_t car) {
    if (LoadU32(base, car + 0x2D90) == body + 0xAD0) ai = int32_t(LoadU32(base, car + 11728)) != 0;
  });
  return ai;
}

// True when this tick's dynamics step of an AI body is skipped.
bool SkipAiStep(PPCContext& ctx, uint8_t* base, uint32_t body) {
  const uint32_t interval = AiInterval();
  std::lock_guard lock(ai_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now - ai_since >= std::chrono::seconds(2)) {
    // Cars come and go between events; look them up again.
    ai_since = now;
    ai_bodies.clear();
    ai_count = 0;
  }
  auto [it, added] = ai_bodies.try_emplace(body);
  AiBody& state = it->second;
  if (added) {
    state.ai = IsAiBody(base, body);
    state.phase = state.ai ? ai_count++ : 0;
  }
  if (!state.ai || interval <= 1) return false;
  if ((state.ticks++ + state.phase) % interval != 0) {
    state.carried += ctx.f1.f64;
    return true;
  }
  ctx.f1.f64 += state.carried;
  state.carried = 0.0;
  return false;
}

struct BodyCount {
  uint32_t calls = 0;
  uint32_t car_path = 0;  // body+2756 zero: the dynamics step
  double dt_sum = 0.0;
};

std::mutex probe_mutex;
std::unordered_map<uint32_t, BodyCount> probe_bodies;
auto probe_since = std::chrono::steady_clock::now();

void EmitProbe(uint8_t* base) {
  // Each car's ICarDynamics (+0x2D90) and driver, to match bodies to cars.
  std::string cars;
  ForEachCar(base, [&](uint32_t car) {
    cars += fmt::format("{}{:08X}:{:08X}:{}", cars.empty() ? "" : "|", car,
                        LoadU32(base, car + 0x2D90), int32_t(LoadU32(base, car + 11728)));
  });
  std::string bodies;
  for (const auto& [body, count] : probe_bodies) {
    bodies += fmt::format("{}{:08X}:{}:{}:{:.5f}", bodies.empty() ? "" : "|", body, count.calls,
                          count.car_path, count.calls ? count.dt_sum / count.calls : 0.0);
  }
  diagnostics::RecordEvent("physics.body_probe",
                           {{"bodies", bodies}, {"count", fmt::format("{}", probe_bodies.size())},
                            {"cars", cars}});
  probe_bodies.clear();
}

// ---------------------------------------------------------------- oracle
// Trace file: a sequence of records, each a host little-endian header and
// the body's raw guest (big-endian) bytes. kind 0 is the window after the
// snapshot, 1 a window after an injection, 2 the one state an injection
// replaced; tick 0 of a window is the state the step at that tick starts
// from (for an injection, the restored state).
constexpr uint32_t kBodySize = 0x5040;
constexpr uint32_t kOracleMagic = 0x544F5350;  // "PSOT"
constexpr uint16_t kOracleVersion = 1;
#pragma pack(push, 1)
struct OracleRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t kind;
  uint32_t window;
  uint32_t tick;
  uint64_t frame;
  double dt;          // the dt this step runs with (after fixed_dt)
  double game_dt;     // the dt the world passed
  uint32_t body;
  uint32_t size;
  uint32_t path;      // body+2756: 0 = full dynamics step
  uint32_t substeps;  // body+0x502C, written by the previous full step
  uint32_t rate[2];   // the substep rate words the full step reads
  uint64_t host_ns;
};
#pragma pack(pop)
static_assert(sizeof(OracleRecord) == 72);

struct Oracle {
  std::mutex mutex;
  bool parsed = false;
  std::vector<uint64_t> inject_frames;
  size_t next_inject = 0;
  std::vector<std::pair<uint32_t, uint32_t>> skip;
  bool have_snapshot = false;
  uint32_t snapshot_body = 0;
  std::vector<uint8_t> snapshot;
  uint16_t kind = 0;
  uint32_t window = 0;
  uint32_t tick = 0;
  uint32_t ticks_left = 0;
  std::FILE* file = nullptr;
  bool file_failed = false;
};
Oracle oracle;

std::vector<std::string> SplitCommas(const std::string& text) {
  std::vector<std::string> items;
  size_t at = 0;
  while (at <= text.size()) {
    const size_t comma = text.find(',', at);
    const size_t end = comma == std::string::npos ? text.size() : comma;
    if (end > at) items.push_back(text.substr(at, end - at));
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return items;
}

// The player's body (driver 0), or 0.
uint32_t PlayerBody(uint8_t* base) {
  uint32_t body = 0;
  ForEachCar(base, [&](uint32_t car) {
    const uint32_t dyn = LoadU32(base, car + 0x2D90);
    if (!body && dyn && int32_t(LoadU32(base, car + 11728)) == 0) body = dyn - 0xAD0;
  });
  return body;
}

void OracleWrite(uint8_t* base, uint32_t body, uint64_t frame, double dt, double game_dt) {
  if (!oracle.file && !oracle.file_failed) {
    std::filesystem::path path = std::string(REXCVAR_GET(pinyon_shift_physics_oracle_path));
    if (path.empty()) {
      path = (fh1_render_test::Enabled() ? fh1_render_test::OutputDirectory()
                                         : std::filesystem::path{}) /
             "physics-oracle.bin";
    }
    oracle.file = std::fopen(path.string().c_str(), "wb");
    oracle.file_failed = !oracle.file;
    diagnostics::RecordEvent("physics.oracle_file",
                             {{"path", path.string()}, {"ok", oracle.file ? "1" : "0"}});
  }
  if (!oracle.file) return;
  OracleRecord record{};
  record.magic = kOracleMagic;
  record.version = kOracleVersion;
  record.kind = oracle.kind;
  record.window = oracle.window;
  record.tick = oracle.tick;
  record.frame = frame;
  record.dt = dt;
  record.game_dt = game_dt;
  record.body = body;
  record.size = kBodySize;
  record.path = LoadU32(base, body + 2756);
  record.substeps = LoadU32(base, body + 0x502C);
  // The full step's substep rate words, [0x834AA248 + 0x170] and +0x174
  // (the second when body+0x5018 is 1); base disc only, no v4 mapping yet.
  constexpr uint32_t kRateTable = FH1_BASE_ONLY_ADDR(0x834AA248u);
  if constexpr (kRateTable != 0) {
    record.rate[0] = LoadU32(base, kRateTable + 0x170);
    record.rate[1] = LoadU32(base, kRateTable + 0x174);
  }
  record.host_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
  std::fwrite(&record, sizeof(record), 1, oracle.file);
  std::fwrite(Host(base, body), 1, kBodySize, oracle.file);
}

// Restores the snapshot into the body; returns how many changed words it
// left alone (the skip ranges). Pointer-looking words are not singled
// out: the body heap sits at 0x41xxxxxx, the bit patterns of floats 8-32.
uint32_t OracleRestore(uint8_t* base, uint32_t body, std::string& skipped_offsets) {
  uint8_t* host = Host(base, body);
  uint32_t skipped = 0;
  for (uint32_t offset = 0; offset < kBodySize; offset += 4) {
    uint32_t now, then;
    std::memcpy(&now, host + offset, 4);
    std::memcpy(&then, oracle.snapshot.data() + offset, 4);
    if (now == then) continue;
    bool skip = false;
    for (const auto& [first, last] : oracle.skip) skip |= offset >= first && offset < last;
    if (skip) {
      ++skipped;
      if (skipped_offsets.size() < 400) {
        skipped_offsets += fmt::format("{}{:X}", skipped_offsets.empty() ? "" : ",", offset);
      }
      continue;
    }
    std::memcpy(host + offset, &then, 4);
  }
  return skipped;
}

void OracleStep(PPCContext& ctx, uint8_t* base, uint32_t body) {
  std::lock_guard lock(oracle.mutex);
  if (!oracle.parsed) {
    oracle.parsed = true;
    for (const auto& item : SplitCommas(REXCVAR_GET(pinyon_shift_physics_oracle_inject_frames))) {
      oracle.inject_frames.push_back(std::strtoull(item.c_str(), nullptr, 10));
    }
    std::sort(oracle.inject_frames.begin(), oracle.inject_frames.end());
    for (const auto& item : SplitCommas(REXCVAR_GET(pinyon_shift_physics_oracle_skip))) {
      const size_t dash = item.find('-');
      if (dash == std::string::npos) continue;
      oracle.skip.emplace_back(uint32_t(std::strtoul(item.substr(0, dash).c_str(), nullptr, 16)),
                               uint32_t(std::strtoul(item.substr(dash + 1).c_str(), nullptr, 16)));
    }
  }
  if (body != PlayerBody(base)) return;
  const uint64_t frame = fh1_render_test::Enabled() ? fh1_render_test::CurrentFrame() : 0;
  const double game_dt = ctx.f1.f64;
  const double fixed = REXCVAR_GET(pinyon_shift_physics_oracle_fixed_dt);
  if (fixed > 0.0) ctx.f1.f64 = fixed;

  if (!oracle.have_snapshot) {
    if (frame < REXCVAR_GET(pinyon_shift_physics_oracle_snapshot_frame)) return;
    oracle.have_snapshot = true;
    oracle.snapshot_body = body;
    oracle.snapshot.assign(Host(base, body), Host(base, body) + kBodySize);
    oracle.kind = 0;
    oracle.window = 0;
    oracle.tick = 0;
    oracle.ticks_left = REXCVAR_GET(pinyon_shift_physics_oracle_trace_ticks) + 1;
    diagnostics::RecordEvent("physics.oracle_snapshot",
                             {{"frame", fmt::format("{}", frame)},
                              {"body", fmt::format("{:08X}", body)},
                              {"dt", fmt::format("{:.6f}", ctx.f1.f64)}});
  } else if (oracle.next_inject < oracle.inject_frames.size() &&
             frame >= oracle.inject_frames[oracle.next_inject]) {
    ++oracle.next_inject;
    if (body != oracle.snapshot_body) {
      diagnostics::RecordEvent("physics.oracle_inject_skipped",
                               {{"frame", fmt::format("{}", frame)}, {"reason", "another body"}});
    } else {
      // The state the restore replaces, as a kind 2 record: the negative
      // control (a restore that did nothing would trace on from here).
      ++oracle.window;
      oracle.kind = 2;
      oracle.tick = 0;
      OracleWrite(base, body, frame, ctx.f1.f64, game_dt);
      std::string skipped_offsets;
      const uint32_t skipped = OracleRestore(base, body, skipped_offsets);
      oracle.kind = 1;
      oracle.tick = 0;
      oracle.ticks_left = REXCVAR_GET(pinyon_shift_physics_oracle_trace_ticks) + 1;
      diagnostics::RecordEvent("physics.oracle_inject",
                               {{"frame", fmt::format("{}", frame)},
                                {"window", fmt::format("{}", oracle.window)},
                                {"skipped_words", fmt::format("{}", skipped)},
                                {"skipped", skipped_offsets}});
    }
  }
  if (oracle.ticks_left) {
    OracleWrite(base, body, frame, ctx.f1.f64, game_dt);
    ++oracle.tick;
    if (--oracle.ticks_left == 0 && oracle.file) std::fflush(oracle.file);
  }
}

bool BodyStepNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t body = ctx.r4.u32;
  if (body && OracleEnabled()) OracleStep(ctx, base, body);
  const bool car_path = body && LoadU32(base, body + 2756) == 0;
  if (car_path && AiInterval() > 1 && SkipAiStep(ctx, base, body)) return true;
  if (!ProbeEnabled()) return false;
  std::lock_guard lock(probe_mutex);
  auto& count = probe_bodies[body];
  ++count.calls;
  count.car_path += car_path;
  count.dt_sum += ctx.f1.f64;
  const auto now = std::chrono::steady_clock::now();
  if (now - probe_since >= std::chrono::seconds(1)) {
    probe_since = now;
    EmitProbe(base);
  }
  return false;  // the original step runs
}

Replacement body_step_replacement = {
    .name = "physics.body_step",
    .guest_address = kV4 ? 0x82E08FF8u : 0x82D46EF8u,
    .guest_code_size = 148,
    .guest_code_fnv1a = kV4 ? 0xA4510033DE362EC0ull : 0xF542A29CC13785D8ull,
    .returns = kReturnNone,
    .enabled = Enabled,
    .native = BodyStepNative,
    .written = nullptr,
};

}  // namespace
}  // namespace pinyon_shift::guest_native

PINYON_GUEST_NATIVE(82D46EF8, 82E08FF8, ::pinyon_shift::guest_native::body_step_replacement)
