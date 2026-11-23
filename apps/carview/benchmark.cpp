#include "benchmark.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#endif

namespace carview::bench {

namespace {

std::string lower_copy(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string trim_copy(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

double safe_num(double v) { return std::isfinite(v) ? v : 0.0; }

void append_num(std::ostringstream& os, double v, int precision = 4) {
  os << std::fixed << std::setprecision(precision) << safe_num(v);
}

void append_dist_json(std::ostringstream& os, const Distribution& d, const char* indent) {
  os << "{\n";
  os << indent << "  \"avg\": "; append_num(os, d.avg); os << ",\n";
  os << indent << "  \"min\": "; append_num(os, d.min); os << ",\n";
  os << indent << "  \"p50\": "; append_num(os, d.p50); os << ",\n";
  os << indent << "  \"p90\": "; append_num(os, d.p90); os << ",\n";
  os << indent << "  \"p95\": "; append_num(os, d.p95); os << ",\n";
  os << indent << "  \"p99\": "; append_num(os, d.p99); os << ",\n";
  os << indent << "  \"p99_9\": "; append_num(os, d.p99_9); os << ",\n";
  os << indent << "  \"max\": "; append_num(os, d.max); os << "\n";
  os << indent << "}";
}

void append_counter_json(std::ostringstream& os, const DrawCounter& c) {
  os << "{\"avg\": ";
  append_num(os, c.avg, 2);
  os << ", \"peak\": " << c.peak << "}";
}

std::string detect_dominant_phase(const FrameSample& f) {
  const std::pair<const char*, double> phases[] = {
      {"physics", f.physics_ms},
      {"ai_traffic", f.ai_traffic_ms},
      {"world_stream", f.world_stream_ms},
      {"audio_push", f.audio_push_ms},
      {"ui_build", f.ui_build_ms},
      {"pose_cull", f.pose_cull_ms},
      {"cmd_record", f.cmd_record_ms},
      {"submit_wait", f.submit_wait_ms},
  };
  const char* best_name = "unknown";
  double best_val = -1.0;
  for (const auto& [name, val] : phases) {
    if (val > best_val) {
      best_val = val;
      best_name = name;
    }
  }
  return best_name;
}

}  // namespace

bool parse_scenario(std::string_view name, Scenario& out) {
  const std::string k = lower_copy(trim_copy(name));
  if (k == "custom" || k == "default") { out = Scenario::kCustom; return true; }
  if (k == "noon_cruise") { out = Scenario::kNoonCruise; return true; }
  if (k == "midnight_lights") { out = Scenario::kMidnightLights; return true; }
  if (k == "cockpit_mirrors") { out = Scenario::kCockpitMirrors; return true; }
  if (k == "race_pack") { out = Scenario::kRacePack; return true; }
  if (k == "autoshow_studio") { out = Scenario::kAutoshowStudio; return true; }
  if (k == "ui4_heavy") { out = Scenario::kUi4Heavy; return true; }
  if (k == "cold_boot_switch") { out = Scenario::kColdBootSwitch; return true; }
  return false;
}

std::string_view scenario_name(Scenario s) {
  switch (s) {
    case Scenario::kCustom: return "custom";
    case Scenario::kNoonCruise: return "noon_cruise";
    case Scenario::kMidnightLights: return "midnight_lights";
    case Scenario::kCockpitMirrors: return "cockpit_mirrors";
    case Scenario::kRacePack: return "race_pack";
    case Scenario::kAutoshowStudio: return "autoshow_studio";
    case Scenario::kUi4Heavy: return "ui4_heavy";
    case Scenario::kColdBootSwitch: return "cold_boot_switch";
  }
  return "custom";
}

std::string_view scenario_description(Scenario s) {
  switch (s) {
    case Scenario::kCustom: return "Custom CLI benchmark configuration";
    case Scenario::kNoonCruise: return "Daytime Colorado cruise with traffic, streaming, full FX, and HUD";
    case Scenario::kMidnightLights: return "02:00 night Colorado with headlights, bloomed taillights, stars, and traffic";
    case Scenario::kCockpitMirrors: return "Cockpit camera with 3-band mirrors, live gauges, and interior normals";
    case Scenario::kRacePack: return "Multi-car festival race pack with AI physics, skids/particles, and race HUD";
    case Scenario::kAutoshowStudio: return "Autoshow hub with staticcarcubemap.xpr, synced 3D car, and +3 deg UI4 carousel";
    case Scenario::kUi4Heavy: return "Rapid cycling across H_MAP, 925_PAUSE_MENU, H_STREET_RACE_HUB, and H_FIXED_SETTING_LIST";
    case Scenario::kColdBootSwitch: return "Cold boot TTFF plus 5 deterministic car hot-switches";
  }
  return "";
}

std::vector<Scenario> all_scenarios() {
  return {
      Scenario::kNoonCruise,
      Scenario::kMidnightLights,
      Scenario::kCockpitMirrors,
      Scenario::kRacePack,
      Scenario::kAutoshowStudio,
      Scenario::kUi4Heavy,
      Scenario::kColdBootSwitch,
  };
}

ProcessMemory query_process_memory() {
  ProcessMemory out;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    out.rss_bytes = uint64_t(pmc.WorkingSetSize);
    out.peak_rss_bytes = uint64_t(pmc.PeakWorkingSetSize);
  }
#elif defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    out.rss_bytes = uint64_t(info.resident_size);
    out.peak_rss_bytes = uint64_t(info.resident_size_max);
  }
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) == 0 && uint64_t(ru.ru_maxrss) > out.peak_rss_bytes) {
    out.peak_rss_bytes = uint64_t(ru.ru_maxrss);  // bytes on macOS
  }
#else
  std::ifstream st("/proc/self/status");
  std::string line;
  while (std::getline(st, line)) {
    unsigned long long kib = 0;
    if (line.rfind("VmRSS:", 0) == 0 && std::sscanf(line.c_str() + 6, "%llu", &kib) == 1) {
      out.rss_bytes = uint64_t(kib) * 1024ull;
    } else if (line.rfind("VmHWM:", 0) == 0 && std::sscanf(line.c_str() + 6, "%llu", &kib) == 1) {
      out.peak_rss_bytes = uint64_t(kib) * 1024ull;
    }
  }
  if (out.peak_rss_bytes == 0) {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) out.peak_rss_bytes = uint64_t(ru.ru_maxrss) * 1024ull;  // KiB on Linux
  }
  if (out.rss_bytes == 0) out.rss_bytes = out.peak_rss_bytes;
#endif
  return out;
}

SystemHardware probe_system_hardware() {
  SystemHardware hw;
  hw.cpu_logical_cores = std::max(1u, std::thread::hardware_concurrency());

#if defined(__aarch64__) || defined(_M_ARM64)
  hw.cpu_arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  hw.cpu_arch = "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
  hw.cpu_arch = "arm";
#elif defined(__i386__) || defined(_M_IX86)
  hw.cpu_arch = "x86";
#else
  hw.cpu_arch = "unknown";
#endif

#ifdef _WIN32
  hw.os_name = "Windows";
  // Query true Windows build via ntdll!RtlGetVersion (avoids manifest lie from GetVersionEx).
  {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (ntdll) {
      auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
      if (fn && fn(&vi) == 0) {
        char vbuf[64];
        std::snprintf(vbuf, sizeof vbuf, "%lu.%lu.%lu", (unsigned long)vi.dwMajorVersion,
                      (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);
        hw.os_version = vbuf;
      }
    }
    if (hw.os_version.empty()) hw.os_version = "10+";
  }
  {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
      hw.system_ram_mib = double(ms.ullTotalPhys) / (1024.0 * 1024.0);
    }
  }
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  {
    int regs[4] = {};
    __cpuid(regs, int(0x80000000u));
    if (uint32_t(regs[0]) >= 0x80000004u) {
      char brand[64] = {};
      for (int i = 0; i < 3; ++i) {
        __cpuid(regs, int(0x80000002u + i));
        std::memcpy(brand + i * 16, regs, 16);
      }
      hw.cpu_brand = trim_copy(brand);
    }
  }
#endif
  if (hw.cpu_brand.empty()) {
    HKEY hkey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hkey) ==
        ERROR_SUCCESS) {
      char buf[256] = {};
      DWORD sz = sizeof(buf) - 1;
      DWORD type = 0;
      if (RegQueryValueExA(hkey, "ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz) ==
          ERROR_SUCCESS) {
        hw.cpu_brand = trim_copy(buf);
      }
      RegCloseKey(hkey);
    }
  }
#elif defined(__APPLE__)
  hw.os_name = "macOS";
  {
    char ver[128] = {};
    size_t sz = sizeof(ver) - 1;
    if (sysctlbyname("kern.osproductversion", ver, &sz, nullptr, 0) == 0) {
      hw.os_version = trim_copy(ver);
    } else {
      utsname un{};
      if (uname(&un) == 0) hw.os_version = un.release;
    }
  }
  {
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0) == 0) {
      hw.system_ram_mib = double(mem) / (1024.0 * 1024.0);
    }
  }
  {
    char brand[256] = {};
    size_t sz = sizeof(brand) - 1;
    if (sysctlbyname("machdep.cpu.brand_string", brand, &sz, nullptr, 0) == 0) {
      hw.cpu_brand = trim_copy(brand);
    }
  }
#else
#if defined(__ANDROID__)
  hw.os_name = "Android";
#else
  hw.os_name = "Linux";
#endif
  {
    utsname un{};
    if (uname(&un) == 0) hw.os_version = un.release;
  }
  {
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_sz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_sz > 0) {
      hw.system_ram_mib = (double(pages) * double(page_sz)) / (1024.0 * 1024.0);
    }
  }
#if defined(__x86_64__) || defined(__i386__)
  {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) && eax >= 0x80000004u) {
      char brand[64] = {};
      for (unsigned int i = 0; i < 3; ++i) {
        __get_cpuid(0x80000002u + i, &eax, &ebx, &ecx, &edx);
        std::memcpy(brand + i * 16 + 0, &eax, 4);
        std::memcpy(brand + i * 16 + 4, &ebx, 4);
        std::memcpy(brand + i * 16 + 8, &ecx, 4);
        std::memcpy(brand + i * 16 + 12, &edx, 4);
      }
      hw.cpu_brand = trim_copy(brand);
    }
  }
#endif
  if (hw.cpu_brand.empty()) {
    std::ifstream ci("/proc/cpuinfo");
    std::string line;
    while (std::getline(ci, line)) {
      if (line.rfind("model name", 0) == 0 || line.rfind("Hardware", 0) == 0 || line.rfind("Processor", 0) == 0) {
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
          hw.cpu_brand = trim_copy(line.substr(colon + 1));
          if (!hw.cpu_brand.empty()) break;
        }
      }
    }
  }
#endif
  if (hw.cpu_brand.empty()) hw.cpu_brand = hw.cpu_arch + " CPU";
  return hw;
}

std::string format_vulkan_version(uint32_t v) {
  const uint32_t major = (v >> 22) & 0x7Fu;
  const uint32_t minor = (v >> 12) & 0x3FFu;
  const uint32_t patch = v & 0xFFFu;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%u.%u", major, minor, patch);
  return buf;
}

std::string utc_timestamp_iso8601() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buf;
}

double compute_percentile(std::vector<double> values, double q) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double qc = std::clamp(q, 0.0, 1.0);
  const double pos = qc * double(values.size() - 1);
  const size_t lo = size_t(std::floor(pos));
  const size_t hi = std::min(values.size() - 1, lo + 1);
  const double frac = pos - double(lo);
  return values[lo] * (1.0 - frac) + values[hi] * frac;
}

Distribution compute_distribution(const std::vector<double>& values) {
  Distribution d{};
  if (values.empty()) return d;
  std::vector<double> v = values;
  std::sort(v.begin(), v.end());
  double sum = 0.0;
  for (double x : v) sum += x;
  d.avg = sum / double(v.size());
  d.min = v.front();
  d.max = v.back();
  auto at = [&](double q) {
    const double pos = std::clamp(q, 0.0, 1.0) * double(v.size() - 1);
    const size_t lo = size_t(std::floor(pos));
    const size_t hi = std::min(v.size() - 1, lo + 1);
    const double frac = pos - double(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
  };
  d.p50 = at(0.50);
  d.p90 = at(0.90);
  d.p95 = at(0.95);
  d.p99 = at(0.99);
  d.p99_9 = at(0.999);
  return d;
}

double compute_low_fps(const std::vector<double>& frame_times_ms, double fraction) {
  if (frame_times_ms.empty()) return 0.0;
  std::vector<double> v = frame_times_ms;
  std::sort(v.begin(), v.end(), std::greater<double>());
  const size_t count = std::max<size_t>(1, size_t(std::ceil(double(v.size()) * std::clamp(fraction, 1e-6, 1.0))));
  double sum = 0.0;
  for (size_t i = 0; i < std::min(count, v.size()); ++i) sum += v[i];
  const double avg_ms = sum / double(std::min(count, v.size()));
  return avg_ms > 1e-6 ? (1000.0 / avg_ms) : 0.0;
}

BenchmarkReport BenchmarkRecorder::finalize(const SystemHardware& hw, const GpuInfo& gpu,
                                            const FeatureFlags& features, const BuildInfo& build,
                                            const StartupMetrics& startup, const AudioMetrics& audio_in,
                                            const Ui4AtlasMetrics& ui4_atlas, const MemoryMetrics& memory,
                                            const StreamingMetrics& streaming_in, double measured_wall_sec,
                                            double measured_cpu_sec) const {
  BenchmarkReport rep;
  rep.schema_version = kSchemaVersion;
  rep.timestamp_utc = utc_timestamp_iso8601();
  rep.hardware = hw;
  rep.gpu = gpu;
  rep.features = features;
  rep.build = build;
  rep.startup = startup;
  rep.audio = audio_in;
  rep.ui4_atlas = ui4_atlas;
  rep.memory = memory;
  rep.streaming = streaming_in;

  std::vector<const FrameSample*> measured;
  uint32_t warmup_count = 0;
  for (const auto& f : frames_) {
    if (f.is_warmup) ++warmup_count;
    else measured.push_back(&f);
  }
  // If every single frame was tagged warmup (e.g. very short test), fall back to all frames.
  if (measured.empty() && !frames_.empty()) {
    warmup_count = 0;
    for (const auto& f : frames_) measured.push_back(&f);
  }

  const size_t n = measured.size();
  rep.pacing.total_frames = uint32_t(n);
  rep.pacing.warmup_frames = warmup_count;
  rep.pacing.wall_sec = measured_wall_sec;
  if (n >= 2) {
    rep.pacing.sim_sec = std::max(0.0, measured.back()->sim_sec - measured.front()->sim_sec);
  } else if (n == 1) {
    rep.pacing.sim_sec = measured.front()->sim_sec;
  }

  std::vector<double> frame_v, cpu_v, gpu_v;
  std::vector<double> phys_v, ai_v, stream_v, audio_v, ui_v, pose_v, rec_v, wait_v;
  frame_v.reserve(n);
  cpu_v.reserve(n);
  gpu_v.reserve(n);
  phys_v.reserve(n);
  ai_v.reserve(n);
  stream_v.reserve(n);
  audio_v.reserve(n);
  ui_v.reserve(n);
  pose_v.reserve(n);
  rec_v.reserve(n);
  wait_v.reserve(n);

  std::vector<std::string> pass_order;
  std::map<std::string, std::vector<double>> pass_samples;

  auto accum_counter = [](DrawCounter& dst, double& sum, uint64_t val) {
    sum += double(val);
    dst.peak = std::max(dst.peak, val);
  };
  double s_wd = 0, s_wc = 0, s_whiz = 0, s_tr = 0, s_sw = 0, s_sc = 0, s_env = 0, s_mir = 0;
  double s_cd = 0, s_ci = 0, s_ps = 0, s_sk = 0, s_ui = 0, s_td = 0, s_tt = 0;

  for (const FrameSample* f : measured) {
    const double fm = f->frame_ms > 0.0 ? f->frame_ms : (f->cpu_ms + f->submit_wait_ms);
    frame_v.push_back(fm);
    cpu_v.push_back(f->cpu_ms);
    if (f->gpu_ms >= 0.0) gpu_v.push_back(f->gpu_ms);
    phys_v.push_back(f->physics_ms);
    ai_v.push_back(f->ai_traffic_ms);
    stream_v.push_back(f->world_stream_ms);
    audio_v.push_back(f->audio_push_ms);
    ui_v.push_back(f->ui_build_ms);
    pose_v.push_back(f->pose_cull_ms);
    rec_v.push_back(f->cmd_record_ms);
    wait_v.push_back(f->submit_wait_ms);

    for (const auto& [pname, pms] : f->gpu_passes) {
      if (pass_samples.find(pname) == pass_samples.end()) pass_order.push_back(pname);
      pass_samples[pname].push_back(pms);
    }

    accum_counter(rep.geometry.world_draws, s_wd, f->world_draws);
    accum_counter(rep.geometry.world_culled, s_wc, f->world_culled);
    accum_counter(rep.geometry.world_hiz_culled, s_whiz, f->world_hiz_culled);
    accum_counter(rep.geometry.trees_drawn, s_tr, f->trees_drawn);
    accum_counter(rep.geometry.shadow_world_draws, s_sw, f->shadow_world_draws);
    accum_counter(rep.geometry.shadow_cars, s_sc, f->shadow_cars);
    accum_counter(rep.geometry.env_draws, s_env, f->env_draws);
    accum_counter(rep.geometry.mirror_draws, s_mir, f->mirror_draws);
    accum_counter(rep.geometry.car_draws, s_cd, f->car_draws);
    accum_counter(rep.geometry.car_instances, s_ci, f->car_instances);
    accum_counter(rep.geometry.particle_sprites, s_ps, f->particle_sprites);
    accum_counter(rep.geometry.skid_tris, s_sk, f->skid_tris);
    accum_counter(rep.geometry.ui4_shapes, s_ui, f->ui4_shapes);
    accum_counter(rep.geometry.triangles_drawn, s_td, f->triangles_drawn);
    accum_counter(rep.geometry.triangles_total, s_tt, f->triangles_total);
  }

  if (n > 0) {
    const double inv_n = 1.0 / double(n);
    rep.geometry.world_draws.avg = s_wd * inv_n;
    rep.geometry.world_culled.avg = s_wc * inv_n;
    rep.geometry.world_hiz_culled.avg = s_whiz * inv_n;
    rep.geometry.trees_drawn.avg = s_tr * inv_n;
    rep.geometry.shadow_world_draws.avg = s_sw * inv_n;
    rep.geometry.shadow_cars.avg = s_sc * inv_n;
    rep.geometry.env_draws.avg = s_env * inv_n;
    rep.geometry.mirror_draws.avg = s_mir * inv_n;
    rep.geometry.car_draws.avg = s_cd * inv_n;
    rep.geometry.car_instances.avg = s_ci * inv_n;
    rep.geometry.particle_sprites.avg = s_ps * inv_n;
    rep.geometry.skid_tris.avg = s_sk * inv_n;
    rep.geometry.ui4_shapes.avg = s_ui * inv_n;
    rep.geometry.triangles_drawn.avg = s_td * inv_n;
    rep.geometry.triangles_total.avg = s_tt * inv_n;

    rep.pacing.frame_ms = compute_distribution(frame_v);
    rep.pacing.cpu_ms = compute_distribution(cpu_v);
    rep.pacing.has_gpu_timestamps = !gpu_v.empty();
    if (rep.pacing.has_gpu_timestamps) rep.pacing.gpu_ms = compute_distribution(gpu_v);

    rep.pacing.fps_avg = rep.pacing.frame_ms.avg > 1e-6 ? 1000.0 / rep.pacing.frame_ms.avg : 0.0;
    rep.pacing.fps_1pct_low = compute_low_fps(frame_v, 0.01);
    rep.pacing.fps_0_1pct_low = compute_low_fps(frame_v, 0.001);
    rep.pacing.fps_min = rep.pacing.frame_ms.max > 1e-6 ? 1000.0 / rep.pacing.frame_ms.max : 0.0;
    rep.pacing.fps_max = rep.pacing.frame_ms.min > 1e-6 ? 1000.0 / rep.pacing.frame_ms.min : 0.0;

    rep.cpu_phases.physics_ms = compute_distribution(phys_v);
    rep.cpu_phases.ai_traffic_ms = compute_distribution(ai_v);
    rep.cpu_phases.world_stream_ms = compute_distribution(stream_v);
    rep.cpu_phases.audio_push_ms = compute_distribution(audio_v);
    rep.cpu_phases.ui_build_ms = compute_distribution(ui_v);
    rep.cpu_phases.pose_cull_ms = compute_distribution(pose_v);
    rep.cpu_phases.cmd_record_ms = compute_distribution(rec_v);
    rep.cpu_phases.submit_wait_ms = compute_distribution(wait_v);

    rep.audio.main_push_avg_ms = rep.cpu_phases.audio_push_ms.avg;
    rep.audio.main_push_worst_ms = rep.cpu_phases.audio_push_ms.max;

    for (const auto& pname : pass_order) {
      rep.gpu_passes.push_back({pname, compute_distribution(pass_samples[pname])});
    }

    // Stutter & hitch counters.
    const double thresh = rep.pacing.frame_ms.p50 * 1.5;
    rep.stutter.hitch_threshold_ms = thresh;
    uint32_t hitch_free = 0;
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) {
      order[i] = i;
      const double fm = frame_v[i];
      if (fm > 16.6667) ++rep.stutter.frames_over_16_67ms;
      if (fm > 33.3333) ++rep.stutter.frames_over_33_33ms;
      if (fm > 50.0) ++rep.stutter.frames_over_50ms;
      if (fm <= thresh + 1e-9) ++hitch_free;
      if (fm > rep.stutter.worst_hitch_ms) {
        rep.stutter.worst_hitch_ms = fm;
        rep.stutter.worst_hitch_sim_sec = measured[i]->sim_sec;
        rep.stutter.worst_hitch_frame = measured[i]->frame_index;
      }
    }
    rep.stutter.hitch_free_pct = 100.0 * double(hitch_free) / double(n);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return frame_v[a] > frame_v[b]; });
    const size_t top_k = std::min<size_t>(5, n);
    rep.stutter.top5_slowest.reserve(top_k);
    for (size_t k = 0; k < top_k; ++k) {
      const FrameSample* f = measured[order[k]];
      SlowFrameEntry e;
      e.frame_index = f->frame_index;
      e.sim_sec = f->sim_sec;
      e.frame_ms = frame_v[order[k]];
      e.cpu_ms = f->cpu_ms;
      e.gpu_ms = f->gpu_ms;
      e.physics_ms = f->physics_ms;
      e.ai_traffic_ms = f->ai_traffic_ms;
      e.world_stream_ms = f->world_stream_ms;
      e.audio_push_ms = f->audio_push_ms;
      e.ui_build_ms = f->ui_build_ms;
      e.pose_cull_ms = f->pose_cull_ms;
      e.cmd_record_ms = f->cmd_record_ms;
      e.submit_wait_ms = f->submit_wait_ms;
      e.dominant_phase = detect_dominant_phase(*f);
      rep.stutter.top5_slowest.push_back(std::move(e));
    }
  }

  rep.cpu_util.process_cpu_sec = measured_cpu_sec;
  rep.cpu_util.wall_sec = measured_wall_sec;
  rep.cpu_util.logical_cores = std::max(1u, hw.cpu_logical_cores);
  rep.cpu_util.ms_per_frame = n > 0 ? (measured_cpu_sec * 1000.0 / double(n)) : 0.0;
  rep.cpu_util.pct_of_one_core = measured_wall_sec > 1e-9 ? (measured_cpu_sec / measured_wall_sec * 100.0) : 0.0;
  rep.cpu_util.pct_of_all_cores = rep.cpu_util.pct_of_one_core / double(rep.cpu_util.logical_cores);

  if (rep.streaming.staging_upload_mib_per_sec <= 0.0 && measured_wall_sec > 1e-9 &&
      rep.streaming.staging_upload_bytes > 0) {
    rep.streaming.staging_upload_mib_per_sec =
        (double(rep.streaming.staging_upload_bytes) / (1024.0 * 1024.0)) / measured_wall_sec;
  }

  return rep;
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char hex[8];
          std::snprintf(hex, sizeof hex, "\\u%04x", unsigned(c));
          out += hex;
        } else {
          out.push_back(char(c));
        }
        break;
    }
  }
  return out;
}

std::string to_json(const BenchmarkReport& r) {
  std::ostringstream os;
  auto str = [&](std::string_view v) { os << '"' << json_escape(v) << '"'; };
  auto bstr = [&](bool v) { os << (v ? "true" : "false"); };
  auto hex_id = [](uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%04x", v);
    return std::string(buf);
  };

  os << "{\n";
  os << "  \"schema_version\": " << r.schema_version << ",\n";
  os << "  \"timestamp_utc\": "; str(r.timestamp_utc); os << ",\n";

  // Hardware
  os << "  \"hardware\": {\n";
  os << "    \"os_name\": "; str(r.hardware.os_name); os << ",\n";
  os << "    \"os_version\": "; str(r.hardware.os_version); os << ",\n";
  os << "    \"cpu_arch\": "; str(r.hardware.cpu_arch); os << ",\n";
  os << "    \"cpu_logical_cores\": " << r.hardware.cpu_logical_cores << ",\n";
  os << "    \"cpu_brand\": "; str(r.hardware.cpu_brand); os << ",\n";
  os << "    \"system_ram_mib\": "; append_num(os, r.hardware.system_ram_mib, 1); os << "\n";
  os << "  },\n";

  // GPU
  os << "  \"gpu\": {\n";
  os << "    \"device_name\": "; str(r.gpu.device_name); os << ",\n";
  os << "    \"vendor_id\": "; str(hex_id(r.gpu.vendor_id)); os << ",\n";
  os << "    \"device_id\": "; str(hex_id(r.gpu.device_id)); os << ",\n";
  os << "    \"device_type\": "; str(r.gpu.device_type); os << ",\n";
  os << "    \"vulkan_api_version\": "; str(r.gpu.api_version); os << ",\n";
  os << "    \"vulkan_api_version_raw\": " << r.gpu.api_version_raw << ",\n";
  os << "    \"driver_version_raw\": " << r.gpu.driver_version_raw << ",\n";
  os << "    \"driver_name\": "; str(r.gpu.driver_name); os << ",\n";
  os << "    \"driver_info\": "; str(r.gpu.driver_info); os << ",\n";
  os << "    \"is_moltenvk\": "; bstr(r.gpu.is_moltenvk); os << ",\n";
  os << "    \"is_turnip\": "; bstr(r.gpu.is_turnip); os << ",\n";
  os << "    \"portability_subset\": "; bstr(r.gpu.portability_subset); os << ",\n";
  os << "    \"has_timestamps\": "; bstr(r.gpu.has_timestamps); os << ",\n";
  os << "    \"vram_total_mib\": "; append_num(os, r.gpu.vram_total_mib, 1); os << ",\n";
  os << "    \"vram_budget_mib\": "; append_num(os, r.gpu.vram_budget_mib, 1); os << "\n";
  os << "  },\n";

  // Features
  os << "  \"features\": {\n";
  os << "    \"gpu_driven\": "; bstr(r.features.gpu_driven); os << ",\n";
  os << "    \"hiz\": "; bstr(r.features.hiz); os << ",\n";
  os << "    \"bindless\": "; bstr(r.features.bindless); os << ",\n";
  os << "    \"vrs_supported\": "; bstr(r.features.vrs_supported); os << ",\n";
  os << "    \"vrs_active\": "; bstr(r.features.vrs_active); os << ",\n";
  os << "    \"hdr10_supported\": "; bstr(r.features.hdr10_supported); os << ",\n";
  os << "    \"scrgb_supported\": "; bstr(r.features.scrgb_supported); os << ",\n";
  os << "    \"hdr_mode\": "; str(r.features.hdr_mode); os << ",\n";
  os << "    \"upscaler\": "; str(r.features.upscaler); os << ",\n";
  os << "    \"frame_gen\": "; str(r.features.frame_gen); os << ",\n";
  os << "    \"aa_mode\": "; str(r.features.aa_mode); os << ",\n";
  os << "    \"ssao\": "; str(r.features.ssao); os << ",\n";
  os << "    \"shadows\": "; str(r.features.shadows); os << ",\n";
  os << "    \"vsync\": "; str(r.features.vsync); os << ",\n";
  os << "    \"msaa\": " << r.features.msaa << ",\n";
  os << "    \"anisotropy\": "; append_num(os, r.features.anisotropy, 1); os << ",\n";
  os << "    \"render_scale\": " << r.features.render_scale << ",\n";
  os << "    \"base_540p\": "; bstr(r.features.base_540p); os << ",\n";
  os << "    \"z_prepass\": "; bstr(r.features.z_prepass); os << ",\n";
  os << "    \"reverse_z\": "; bstr(r.features.reverse_z); os << ",\n";
  os << "    \"bloom\": "; bstr(r.features.bloom); os << ",\n";
  os << "    \"full_fx\": "; bstr(r.features.full_fx); os << ",\n";
  os << "    \"bench_audio\": "; bstr(r.features.bench_audio); os << "\n";
  os << "  },\n";

  // Build
  os << "  \"build\": {\n";
  os << "    \"git_commit\": "; str(r.build.git_commit); os << ",\n";
  os << "    \"build_config\": "; str(r.build.build_config); os << ",\n";
  os << "    \"preset\": "; str(r.build.preset); os << ",\n";
  os << "    \"device_label\": "; str(r.build.device_label); os << ",\n";
  os << "    \"scenario\": "; str(r.build.scenario); os << ",\n";
  os << "    \"output_width\": " << r.build.output_width << ",\n";
  os << "    \"output_height\": " << r.build.output_height << ",\n";
  os << "    \"render_width\": " << r.build.render_width << ",\n";
  os << "    \"render_height\": " << r.build.render_height << ",\n";
  os << "    \"target_seconds\": "; append_num(os, r.build.target_seconds, 2); os << ",\n";
  os << "    \"warmup_seconds\": "; append_num(os, r.build.warmup_seconds, 2); os << "\n";
  os << "  },\n";

  // Startup
  os << "  \"startup_ms\": {\n";
  os << "    \"vulkan_init_ms\": "; append_num(os, r.startup.vulkan_init_ms, 2); os << ",\n";
  os << "    \"pipeline_compile_ms\": "; append_num(os, r.startup.pipeline_compile_ms, 2); os << ",\n";
  os << "    \"db_catalog_load_ms\": "; append_num(os, r.startup.db_catalog_load_ms, 2); os << ",\n";
  os << "    \"audio_load_ms\": "; append_num(os, r.startup.audio_load_ms, 2); os << ",\n";
  os << "    \"car_bake_load_ms\": "; append_num(os, r.startup.car_bake_load_ms, 2); os << ",\n";
  os << "    \"world_load_ms\": "; append_num(os, r.startup.world_load_ms, 2); os << ",\n";
  os << "    \"ttff_ms\": "; append_num(os, r.startup.ttff_ms, 2); os << ",\n";
  double sw_sum = 0.0, sw_max = 0.0;
  for (const auto& sw : r.startup.car_switches) {
    sw_sum += sw.total_ms;
    sw_max = std::max(sw_max, sw.total_ms);
  }
  os << "    \"car_switch_avg_ms\": ";
  append_num(os, r.startup.car_switches.empty() ? 0.0 : sw_sum / double(r.startup.car_switches.size()), 2);
  os << ",\n";
  os << "    \"car_switch_max_ms\": "; append_num(os, sw_max, 2); os << ",\n";
  os << "    \"car_switches\": [";
  for (size_t i = 0; i < r.startup.car_switches.size(); ++i) {
    const auto& sw = r.startup.car_switches[i];
    if (i > 0) os << ",";
    os << "\n      {\"car\": "; str(sw.car);
    os << ", \"load_ms\": "; append_num(os, sw.load_ms, 2);
    os << ", \"upload_ms\": "; append_num(os, sw.upload_ms, 2);
    os << ", \"total_ms\": "; append_num(os, sw.total_ms, 2); os << "}";
  }
  if (!r.startup.car_switches.empty()) os << "\n    ";
  os << "]\n";
  os << "  },\n";

  // Pacing
  os << "  \"pacing\": {\n";
  os << "    \"total_frames\": " << r.pacing.total_frames << ",\n";
  os << "    \"warmup_frames\": " << r.pacing.warmup_frames << ",\n";
  os << "    \"wall_sec\": "; append_num(os, r.pacing.wall_sec, 4); os << ",\n";
  os << "    \"sim_sec\": "; append_num(os, r.pacing.sim_sec, 4); os << ",\n";
  os << "    \"fps_avg\": "; append_num(os, r.pacing.fps_avg, 2); os << ",\n";
  os << "    \"fps_1pct_low\": "; append_num(os, r.pacing.fps_1pct_low, 2); os << ",\n";
  os << "    \"fps_0_1pct_low\": "; append_num(os, r.pacing.fps_0_1pct_low, 2); os << ",\n";
  os << "    \"fps_min\": "; append_num(os, r.pacing.fps_min, 2); os << ",\n";
  os << "    \"fps_max\": "; append_num(os, r.pacing.fps_max, 2); os << ",\n";
  os << "    \"frame_ms\": "; append_dist_json(os, r.pacing.frame_ms, "    "); os << ",\n";
  os << "    \"cpu_ms\": "; append_dist_json(os, r.pacing.cpu_ms, "    "); os << ",\n";
  os << "    \"has_gpu_timestamps\": "; bstr(r.pacing.has_gpu_timestamps); os << ",\n";
  os << "    \"gpu_ms\": "; append_dist_json(os, r.pacing.gpu_ms, "    "); os << "\n";
  os << "  },\n";

  // CPU phases
  os << "  \"cpu_phases_ms\": {\n";
  os << "    \"physics_ms\": "; append_dist_json(os, r.cpu_phases.physics_ms, "    "); os << ",\n";
  os << "    \"ai_traffic_ms\": "; append_dist_json(os, r.cpu_phases.ai_traffic_ms, "    "); os << ",\n";
  os << "    \"world_stream_ms\": "; append_dist_json(os, r.cpu_phases.world_stream_ms, "    "); os << ",\n";
  os << "    \"audio_push_ms\": "; append_dist_json(os, r.cpu_phases.audio_push_ms, "    "); os << ",\n";
  os << "    \"ui_build_ms\": "; append_dist_json(os, r.cpu_phases.ui_build_ms, "    "); os << ",\n";
  os << "    \"pose_cull_ms\": "; append_dist_json(os, r.cpu_phases.pose_cull_ms, "    "); os << ",\n";
  os << "    \"cmd_record_ms\": "; append_dist_json(os, r.cpu_phases.cmd_record_ms, "    "); os << ",\n";
  os << "    \"submit_wait_ms\": "; append_dist_json(os, r.cpu_phases.submit_wait_ms, "    "); os << "\n";
  os << "  },\n";

  // GPU passes
  os << "  \"gpu_passes_ms\": {";
  for (size_t i = 0; i < r.gpu_passes.size(); ++i) {
    if (i > 0) os << ",";
    os << "\n    "; str(r.gpu_passes[i].first); os << ": ";
    append_dist_json(os, r.gpu_passes[i].second, "    ");
  }
  if (!r.gpu_passes.empty()) os << "\n  ";
  os << "},\n";

  // Audio
  os << "  \"audio\": {\n";
  os << "    \"enabled\": "; bstr(r.audio.enabled); os << ",\n";
  os << "    \"blocks_rendered\": " << r.audio.blocks_rendered << ",\n";
  os << "    \"avg_block_ms\": "; append_num(os, r.audio.avg_block_ms, 4); os << ",\n";
  os << "    \"p95_block_ms\": "; append_num(os, r.audio.p95_block_ms, 4); os << ",\n";
  os << "    \"worst_block_ms\": "; append_num(os, r.audio.worst_block_ms, 4); os << ",\n";
  os << "    \"stalls\": " << r.audio.stalls << ",\n";
  os << "    \"main_push_avg_ms\": "; append_num(os, r.audio.main_push_avg_ms, 4); os << ",\n";
  os << "    \"main_push_worst_ms\": "; append_num(os, r.audio.main_push_worst_ms, 4); os << "\n";
  os << "  },\n";

  // Stutter
  os << "  \"stutter\": {\n";
  os << "    \"frames_over_16_67ms\": " << r.stutter.frames_over_16_67ms << ",\n";
  os << "    \"frames_over_33_33ms\": " << r.stutter.frames_over_33_33ms << ",\n";
  os << "    \"frames_over_50ms\": " << r.stutter.frames_over_50ms << ",\n";
  os << "    \"hitch_threshold_ms\": "; append_num(os, r.stutter.hitch_threshold_ms, 4); os << ",\n";
  os << "    \"hitch_free_pct\": "; append_num(os, r.stutter.hitch_free_pct, 2); os << ",\n";
  os << "    \"worst_hitch_ms\": "; append_num(os, r.stutter.worst_hitch_ms, 4); os << ",\n";
  os << "    \"worst_hitch_sim_sec\": "; append_num(os, r.stutter.worst_hitch_sim_sec, 4); os << ",\n";
  os << "    \"worst_hitch_frame\": " << r.stutter.worst_hitch_frame << ",\n";
  os << "    \"top5_slowest_frames\": [";
  for (size_t i = 0; i < r.stutter.top5_slowest.size(); ++i) {
    const auto& s = r.stutter.top5_slowest[i];
    if (i > 0) os << ",";
    os << "\n      {\"frame_index\": " << s.frame_index
       << ", \"sim_sec\": "; append_num(os, s.sim_sec, 3);
    os << ", \"frame_ms\": "; append_num(os, s.frame_ms, 3);
    os << ", \"cpu_ms\": "; append_num(os, s.cpu_ms, 3);
    os << ", \"gpu_ms\": "; append_num(os, s.gpu_ms, 3);
    os << ", \"physics_ms\": "; append_num(os, s.physics_ms, 3);
    os << ", \"ai_traffic_ms\": "; append_num(os, s.ai_traffic_ms, 3);
    os << ", \"world_stream_ms\": "; append_num(os, s.world_stream_ms, 3);
    os << ", \"audio_push_ms\": "; append_num(os, s.audio_push_ms, 3);
    os << ", \"ui_build_ms\": "; append_num(os, s.ui_build_ms, 3);
    os << ", \"pose_cull_ms\": "; append_num(os, s.pose_cull_ms, 3);
    os << ", \"cmd_record_ms\": "; append_num(os, s.cmd_record_ms, 3);
    os << ", \"submit_wait_ms\": "; append_num(os, s.submit_wait_ms, 3);
    os << ", \"dominant_phase\": "; str(s.dominant_phase); os << "}";
  }
  if (!r.stutter.top5_slowest.empty()) os << "\n    ";
  os << "]\n";
  os << "  },\n";

  // Geometry
  os << "  \"geometry\": {\n";
  os << "    \"world_draws\": "; append_counter_json(os, r.geometry.world_draws); os << ",\n";
  os << "    \"world_culled\": "; append_counter_json(os, r.geometry.world_culled); os << ",\n";
  os << "    \"world_hiz_culled\": "; append_counter_json(os, r.geometry.world_hiz_culled); os << ",\n";
  os << "    \"trees_drawn\": "; append_counter_json(os, r.geometry.trees_drawn); os << ",\n";
  os << "    \"shadow_world_draws\": "; append_counter_json(os, r.geometry.shadow_world_draws); os << ",\n";
  os << "    \"shadow_cars\": "; append_counter_json(os, r.geometry.shadow_cars); os << ",\n";
  os << "    \"env_draws\": "; append_counter_json(os, r.geometry.env_draws); os << ",\n";
  os << "    \"mirror_draws\": "; append_counter_json(os, r.geometry.mirror_draws); os << ",\n";
  os << "    \"car_draws\": "; append_counter_json(os, r.geometry.car_draws); os << ",\n";
  os << "    \"car_instances\": "; append_counter_json(os, r.geometry.car_instances); os << ",\n";
  os << "    \"particle_sprites\": "; append_counter_json(os, r.geometry.particle_sprites); os << ",\n";
  os << "    \"skid_tris\": "; append_counter_json(os, r.geometry.skid_tris); os << ",\n";
  os << "    \"ui4_shapes\": "; append_counter_json(os, r.geometry.ui4_shapes); os << ",\n";
  os << "    \"triangles_drawn\": "; append_counter_json(os, r.geometry.triangles_drawn); os << ",\n";
  os << "    \"triangles_total\": "; append_counter_json(os, r.geometry.triangles_total); os << "\n";
  os << "  },\n";

  // UI4 Atlas
  os << "  \"ui4_atlas\": {\n";
  os << "    \"atlas_size\": " << r.ui4_atlas.atlas_size << ",\n";
  os << "    \"glyph_rows_used\": " << r.ui4_atlas.glyph_rows_used << ",\n";
  os << "    \"persistent_rows_used\": " << r.ui4_atlas.persistent_rows_used << ",\n";
  os << "    \"transient_rows_used\": " << r.ui4_atlas.transient_rows_used << ",\n";
  os << "    \"utilization_pct\": "; append_num(os, r.ui4_atlas.utilization_pct, 2); os << ",\n";
  os << "    \"transient_evictions\": " << r.ui4_atlas.transient_evictions << ",\n";
  os << "    \"glyph_count\": " << r.ui4_atlas.glyph_count << ",\n";
  os << "    \"image_count\": " << r.ui4_atlas.image_count << "\n";
  os << "  },\n";

  // Memory
  os << "  \"memory\": {\n";
  os << "    \"rss_current_mib\": "; append_num(os, r.memory.rss_current_mib, 2); os << ",\n";
  os << "    \"rss_peak_mib\": "; append_num(os, r.memory.rss_peak_mib, 2); os << ",\n";
  os << "    \"vma_blocks\": " << r.memory.vma_blocks << ",\n";
  os << "    \"vma_allocs\": " << r.memory.vma_allocs << ",\n";
  os << "    \"vma_used_mib\": "; append_num(os, r.memory.vma_used_mib, 2); os << ",\n";
  os << "    \"vma_reserved_mib\": "; append_num(os, r.memory.vma_reserved_mib, 2); os << ",\n";
  os << "    \"vram_usage_mib\": "; append_num(os, r.memory.vram_usage_mib, 2); os << ",\n";
  os << "    \"vram_budget_mib\": "; append_num(os, r.memory.vram_budget_mib, 2); os << ",\n";
  os << "    \"vram_total_mib\": "; append_num(os, r.memory.vram_total_mib, 2); os << "\n";
  os << "  },\n";

  // Streaming
  os << "  \"streaming\": {\n";
  os << "    \"resident_textures\": " << r.streaming.resident_textures << ",\n";
  os << "    \"resident_textures_mib\": "; append_num(os, r.streaming.resident_textures_mib, 2); os << ",\n";
  os << "    \"texture_uploads\": " << r.streaming.texture_uploads << ",\n";
  os << "    \"texture_evictions\": " << r.streaming.texture_evictions << ",\n";
  os << "    \"texture_reuploads\": " << r.streaming.texture_reuploads << ",\n";
  os << "    \"retired_pending\": " << r.streaming.retired_pending << ",\n";
  os << "    \"descriptor_pools\": " << r.streaming.descriptor_pools << ",\n";
  os << "    \"staging_upload_bytes\": " << r.streaming.staging_upload_bytes << ",\n";
  os << "    \"staging_upload_mib_per_sec\": "; append_num(os, r.streaming.staging_upload_mib_per_sec, 2); os << "\n";
  os << "  },\n";

  // CPU util
  os << "  \"cpu_util\": {\n";
  os << "    \"process_cpu_sec\": "; append_num(os, r.cpu_util.process_cpu_sec, 4); os << ",\n";
  os << "    \"wall_sec\": "; append_num(os, r.cpu_util.wall_sec, 4); os << ",\n";
  os << "    \"ms_per_frame\": "; append_num(os, r.cpu_util.ms_per_frame, 4); os << ",\n";
  os << "    \"pct_of_one_core\": "; append_num(os, r.cpu_util.pct_of_one_core, 2); os << ",\n";
  os << "    \"pct_of_all_cores\": "; append_num(os, r.cpu_util.pct_of_all_cores, 2); os << ",\n";
  os << "    \"logical_cores\": " << r.cpu_util.logical_cores << "\n";
  os << "  }\n";
  os << "}\n";
  return os.str();
}

bool write_json(const std::filesystem::path& path, const BenchmarkReport& report, std::string& error) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    error = "cannot write benchmark JSON: " + path.string();
    return false;
  }
  const std::string data = to_json(report);
  out.write(data.data(), std::streamsize(data.size()));
  if (!out) {
    error = "failed writing benchmark JSON: " + path.string();
    return false;
  }
  return true;
}

std::string to_csv(const std::vector<FrameSample>& frames) {
  // Collect ordered union of GPU pass column names.
  std::vector<std::string> pass_names;
  for (const auto& f : frames) {
    for (const auto& [pname, _] : f.gpu_passes) {
      if (std::find(pass_names.begin(), pass_names.end(), pname) == pass_names.end()) {
        pass_names.push_back(pname);
      }
    }
  }
  std::ostringstream os;
  os << "frame,warmup,sim_sec,wall_sec,frame_ms,cpu_ms,gpu_ms,"
        "physics_ms,ai_traffic_ms,world_stream_ms,audio_push_ms,ui_build_ms,pose_cull_ms,cmd_record_ms,submit_wait_ms,"
        "world_draws,world_culled,world_hiz_culled,trees_drawn,shadow_world_draws,shadow_cars,"
        "env_draws,mirror_draws,car_draws,car_instances,particle_sprites,skid_tris,ui4_shapes,"
        "triangles_drawn,triangles_total,rss_mib";
  for (const auto& pn : pass_names) {
    std::string col = pn;
    for (char& c : col) {
      if (c == ' ' || c == ',') c = '_';
    }
    os << ",gpu_" << col << "_ms";
  }
  os << "\n";
  os << std::fixed << std::setprecision(4);
  for (const auto& f : frames) {
    os << f.frame_index << ',' << (f.is_warmup ? 1 : 0) << ','
       << safe_num(f.sim_sec) << ',' << safe_num(f.wall_sec) << ','
       << safe_num(f.frame_ms) << ',' << safe_num(f.cpu_ms) << ',' << safe_num(f.gpu_ms) << ','
       << safe_num(f.physics_ms) << ',' << safe_num(f.ai_traffic_ms) << ',' << safe_num(f.world_stream_ms) << ','
       << safe_num(f.audio_push_ms) << ',' << safe_num(f.ui_build_ms) << ',' << safe_num(f.pose_cull_ms) << ','
       << safe_num(f.cmd_record_ms) << ',' << safe_num(f.submit_wait_ms) << ','
       << f.world_draws << ',' << f.world_culled << ',' << f.world_hiz_culled << ',' << f.trees_drawn << ','
       << f.shadow_world_draws << ',' << f.shadow_cars << ','
       << f.env_draws << ',' << f.mirror_draws << ',' << f.car_draws << ',' << f.car_instances << ','
       << f.particle_sprites << ',' << f.skid_tris << ',' << f.ui4_shapes << ','
       << f.triangles_drawn << ',' << f.triangles_total << ',' << safe_num(f.rss_mib);
    for (const auto& pn : pass_names) {
      double val = 0.0;
      for (const auto& [name, ms] : f.gpu_passes) {
        if (name == pn) {
          val = ms;
          break;
        }
      }
      os << ',' << safe_num(val);
    }
    os << "\n";
  }
  return os.str();
}

bool write_csv(const std::filesystem::path& path, const std::vector<FrameSample>& frames, std::string& error) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    error = "cannot write benchmark CSV: " + path.string();
    return false;
  }
  const std::string data = to_csv(frames);
  out.write(data.data(), std::streamsize(data.size()));
  if (!out) {
    error = "failed writing benchmark CSV: " + path.string();
    return false;
  }
  return true;
}

std::string format_console_summary(const BenchmarkReport& r) {
  std::ostringstream os;
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "bench[%s]: %s (%s %s), %ux%u (render %ux%u), preset %s, MSAA %u, %u frames (+%u warmup) in %.2fs wall\n",
                r.build.scenario.c_str(), r.gpu.device_name.c_str(), r.hardware.os_name.c_str(),
                r.hardware.cpu_arch.c_str(), r.build.output_width, r.build.output_height,
                r.build.render_width, r.build.render_height, r.build.preset.c_str(), r.features.msaa,
                r.pacing.total_frames, r.pacing.warmup_frames, r.pacing.wall_sec);
  os << buf;
  std::snprintf(buf, sizeof buf,
                "bench: FPS avg %.1f | 1%% low %.1f | 0.1%% low %.1f | min %.1f | max %.1f | hitch-free %.1f%% (>16.7ms: %u, >33.3ms: %u, >50ms: %u)\n",
                r.pacing.fps_avg, r.pacing.fps_1pct_low, r.pacing.fps_0_1pct_low, r.pacing.fps_min, r.pacing.fps_max,
                r.stutter.hitch_free_pct, r.stutter.frames_over_16_67ms, r.stutter.frames_over_33_33ms,
                r.stutter.frames_over_50ms);
  os << buf;
  std::snprintf(buf, sizeof buf,
                "bench: frame ms avg %.2f p50 %.2f p90 %.2f p95 %.2f p99 %.2f p99.9 %.2f max %.2f\n",
                r.pacing.frame_ms.avg, r.pacing.frame_ms.p50, r.pacing.frame_ms.p90, r.pacing.frame_ms.p95,
                r.pacing.frame_ms.p99, r.pacing.frame_ms.p99_9, r.pacing.frame_ms.max);
  os << buf;
  std::snprintf(buf, sizeof buf,
                "bench: CPU ms avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f (phys %.2f, ai %.2f, stream %.2f, audio %.2f, ui %.2f, pose %.2f, rec %.2f, wait %.2f)\n",
                r.pacing.cpu_ms.avg, r.pacing.cpu_ms.p50, r.pacing.cpu_ms.p95, r.pacing.cpu_ms.p99, r.pacing.cpu_ms.max,
                r.cpu_phases.physics_ms.avg, r.cpu_phases.ai_traffic_ms.avg, r.cpu_phases.world_stream_ms.avg,
                r.cpu_phases.audio_push_ms.avg, r.cpu_phases.ui_build_ms.avg, r.cpu_phases.pose_cull_ms.avg,
                r.cpu_phases.cmd_record_ms.avg, r.cpu_phases.submit_wait_ms.avg);
  os << buf;
  if (r.pacing.has_gpu_timestamps) {
    std::snprintf(buf, sizeof buf,
                  "bench: GPU ms avg %.2f p50 %.2f p90 %.2f p95 %.2f p99 %.2f p99.9 %.2f max %.2f\n",
                  r.pacing.gpu_ms.avg, r.pacing.gpu_ms.p50, r.pacing.gpu_ms.p90, r.pacing.gpu_ms.p95,
                  r.pacing.gpu_ms.p99, r.pacing.gpu_ms.p99_9, r.pacing.gpu_ms.max);
    os << buf;
  } else {
    os << "bench: no GPU timestamps on this queue\n";
  }
  if (!r.gpu_passes.empty()) {
    os << "bench: GPU passes (p50 ms):";
    for (const auto& [name, dist] : r.gpu_passes) {
      std::snprintf(buf, sizeof buf, " %s %.2f;", name.c_str(), dist.p50);
      os << buf;
    }
    os << "\n";
  }
  std::snprintf(buf, sizeof buf,
                "bench: startup TTFF %.1f ms (vk %.1f, pipe %.1f, db %.1f, audio %.1f, car %.1f, world %.1f)\n",
                r.startup.ttff_ms, r.startup.vulkan_init_ms, r.startup.pipeline_compile_ms,
                r.startup.db_catalog_load_ms, r.startup.audio_load_ms, r.startup.car_bake_load_ms,
                r.startup.world_load_ms);
  os << buf;
  if (!r.startup.car_switches.empty()) {
    os << "bench: car switches:";
    for (const auto& sw : r.startup.car_switches) {
      std::snprintf(buf, sizeof buf, " %s %.1fms;", sw.car.c_str(), sw.total_ms);
      os << buf;
    }
    os << "\n";
  }
  if (r.audio.enabled) {
    std::snprintf(buf, sizeof buf,
                  "bench: audio %llu blocks, avg %.3f ms, p95 %.3f ms, worst %.3f ms, stalls %llu (main push worst %.3f ms)\n",
                  (unsigned long long)r.audio.blocks_rendered, r.audio.avg_block_ms, r.audio.p95_block_ms,
                  r.audio.worst_block_ms, (unsigned long long)r.audio.stalls, r.audio.main_push_worst_ms);
    os << buf;
  }
  std::snprintf(buf, sizeof buf,
                "bench: mem RSS %.1f MiB (peak %.1f MiB) | VRAM %.1f/%.1f MiB (VMA %u blocks, %u allocs) | tex %zu (%.1f MiB, up %llu, evict %llu, reup %llu, retired %zu)\n",
                r.memory.rss_current_mib, r.memory.rss_peak_mib, r.memory.vma_used_mib, r.memory.vma_reserved_mib,
                r.memory.vma_blocks, r.memory.vma_allocs, r.streaming.resident_textures,
                r.streaming.resident_textures_mib, (unsigned long long)r.streaming.texture_uploads,
                (unsigned long long)r.streaming.texture_evictions, (unsigned long long)r.streaming.texture_reuploads,
                r.streaming.retired_pending);
  os << buf;
  std::snprintf(buf, sizeof buf,
                "bench: CPU util %.2fs / %.2fs wall (%.2f ms/f, %.0f%% of 1 core, %.1f%% of %u cores) | UI4 shapes %.0f (peak %llu), atlas %.1f%% (evictions %llu)\n",
                r.cpu_util.process_cpu_sec, r.cpu_util.wall_sec, r.cpu_util.ms_per_frame,
                r.cpu_util.pct_of_one_core, r.cpu_util.pct_of_all_cores, r.cpu_util.logical_cores,
                r.geometry.ui4_shapes.avg, (unsigned long long)r.geometry.ui4_shapes.peak,
                r.ui4_atlas.utilization_pct, (unsigned long long)r.ui4_atlas.transient_evictions);
  os << buf;
  return os.str();
}

}  // namespace carview::bench
