#include "platform_support.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <system_error>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <jni.h>
#include <malloc.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if !defined(__ANDROID__) && (defined(__SSE__) || defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace carview {
namespace {

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

bool resolve_graphics_preset(std::string_view name, GraphicsPresetSpec& out) {
  const std::string p = lower_ascii(name);
  if (p == "ultra") {
    // Tier 3 Enthusiast 4K: 4x MSAA, Full-Res XeGTAO + Bent Normals, Full-Res 12-tap SSCS,
    // Hex-Tiling terrain, 13-tap McGuire Velocity Motion Blur, 3-Cascade D32 PCSS Ultra,
    // Full-Res Hi-Z SSR, Half-Res Volumetric Scattering, Close-Up Car Detail.
    out = {"ultra", 4, false, "high", true, 16.0f, 120, 1.0f,
           modern::SsaoQuality::kUltra, modern::ContactShadows::kUltra, true, modern::VelocityBlur::kHigh,
           modern::ShadowPcss::kUltra, modern::SsrQuality::kFull, modern::VolumetricFog::kHalf, true};
    return true;
  }
  if (p == "high" || p == "quality-60" || p == "performance-120") {
    // Tier 2 Mainstream: 4x MSAA, Half-Res 3-slice XeGTAO + Bent Normals, Half-Res 10-tap SSCS,
    // Hex-Tiling terrain, 9-tap McGuire Velocity Motion Blur, 3-Cascade D32 PCSS High,
    // Half-Res Hi-Z SSR, Quarter-Res Volumetric Scattering, Close-Up Car Detail.
    out = {p, 4, false, "high", true, 16.0f, p == "performance-120" ? 120 : 60, 1.0f,
           modern::SsaoQuality::kHigh, modern::ContactShadows::kHigh, true, modern::VelocityBlur::kLow,
           modern::ShadowPcss::kHigh, modern::SsrQuality::kHalf, modern::VolumetricFog::kQuarter, true};
    return true;
  }
  if (p == "low" || p == "low-spec-60" || p == "balanced-40") {
    // Tier 1 Deck / iGPU / Low: 1x MSAA, Half-Res 2-slice XeGTAO, Half-Res 8-tap SSCS,
    // fast 2-cascade PCF shadows, SSR/Volumetric/Hex-Tiling/VelBlur off.
    out = {p, 1, true, "low", false, 2.0f, p == "balanced-40" ? 40 : 60, 0.5f,
           modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kLow, modern::SsrQuality::kOff, modern::VolumetricFog::kOff, false};
    return true;
  }
  if (p == "smooth-60" || p == "smooth_60") {
    // Android Smooth 60: 2x MSAA, shadows off, quarter-rate reflections, Half-Res 2-slice XeGTAO & 8-tap SSCS.
    out = {"smooth-60", 2, true, "off", false, 4.0f, 60, 0.25f,
           modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kOff, modern::SsrQuality::kOff, modern::VolumetricFog::kOff, false};
    return true;
  }
  if (p == "quality-30" || p == "quality_30") {
    // Android Quality 30: 4x MSAA, shadows low, bloom on, quarter-rate reflections, Quarter-Res Volumetrics.
    out = {"quality-30", 4, false, "low", true, 8.0f, 30, 0.25f,
           modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kLow,
           modern::ShadowPcss::kLow, modern::SsrQuality::kOff, modern::VolumetricFog::kQuarter, false};
    return true;
  }
  if (p == "low-power" || p == "low_power" || p == "ultra-low-30") {
    // Tier 0 Mobile / Deck Low Power: 1x MSAA, shadows off, bloom off, all Phase A/B heavy passes off.
    out = {"low-power", 1, true, "off", false, 2.0f, 30, 0.25f,
           modern::SsaoQuality::kOff, modern::ContactShadows::kOff, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kOff, modern::SsrQuality::kOff, modern::VolumetricFog::kOff, false};
    return true;
  }
  return false;
}

AndroidDriverSelection resolve_android_gpu_driver(std::string_view preference,
                                                  const std::filesystem::path& drivers_dir,
                                                  std::string_view gpu_vendor_hint) {
  AndroidDriverSelection sel;
  const std::string pref = lower_ascii(preference.empty() ? "auto" : preference);
  sel.mode = pref;
  if (pref == "system") {
    sel.summary = "Using Android system Vulkan driver.";
    return sel;
  }

  std::error_code ec;
  auto find_so_in = [&](const std::filesystem::path& dir) -> std::filesystem::path {
    if (!std::filesystem::is_directory(dir, ec)) return {};
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      if (e.path().extension() == ".so") return e.path();
    }
    return {};
  };

  if (pref != "auto") {
    // Explicit driver folder name or .so path
    std::filesystem::path direct(preference);
    if (std::filesystem::is_regular_file(direct, ec)) {
      sel.driver_so = direct;
      sel.using_custom_driver = true;
      sel.summary = "Using custom Vulkan driver: " + direct.filename().string();
      return sel;
    }
    if (const auto so = find_so_in(drivers_dir / std::string(preference)); !so.empty()) {
      sel.driver_so = so;
      sel.using_custom_driver = true;
      sel.summary = "Using custom Vulkan driver: " + std::string(preference) + " (" + so.filename().string() + ")";
      return sel;
    }
    sel.summary = "Requested driver '" + std::string(preference) + "' not found; falling back to system Vulkan.";
    return sel;
  }

  // AUTO: prefer bundled Turnip on Adreno 7xx/8xx (or when hint mentions adreno/qualcomm)
  const std::string hint = lower_ascii(gpu_vendor_hint);
  const bool prefer_turnip = hint.empty() || hint.find("adreno") != std::string::npos ||
                             hint.find("qualcomm") != std::string::npos;
  if (prefer_turnip && std::filesystem::is_directory(drivers_dir, ec)) {
    for (const auto& e : std::filesystem::directory_iterator(drivers_dir, ec)) {
      if (!e.is_directory(ec)) continue;
      if (lower_ascii(e.path().filename().string()).find("turnip") == std::string::npos) continue;
      if (const auto so = find_so_in(e.path()); !so.empty()) {
        sel.driver_so = so;
        sel.using_custom_driver = true;
        sel.summary = "AUTO selected bundled Mesa Turnip (" + e.path().filename().string() + ").";
        return sel;
      }
    }
  }
  sel.summary = "AUTO selected system Vulkan driver.";
  return sel;
}

VulkanCapabilitySummary evaluate_vulkan_capabilities(std::string_view device_name,
                                                     uint32_t api_version,
                                                     bool dynamic_rendering,
                                                     bool sync2,
                                                     bool portability_subset) {
  VulkanCapabilitySummary s;
  s.api_version = api_version;
  const uint32_t maj = (api_version >> 22) & 0x7Fu;
  const uint32_t min = (api_version >> 12) & 0x3FFu;
  s.has_vulkan_1_1 = (maj > 1) || (maj == 1 && min >= 1);
  s.has_vulkan_1_3 = (maj > 1) || (maj == 1 && min >= 3);
  s.has_dynamic_rendering = dynamic_rendering;
  s.has_sync2 = sync2;
  s.has_portability_subset = portability_subset;
  s.eligible = s.has_vulkan_1_1 && dynamic_rendering && sync2;

  char buf[256];
  std::snprintf(buf, sizeof buf,
                "VULKAN_CAPABILITY_REPORT: device=%.*s api=%u.%u dyn_render=%d sync2=%d portability=%d eligible=%d",
                int(device_name.size()), device_name.data(), maj, min,
                dynamic_rendering ? 1 : 0, sync2 ? 1 : 0, portability_subset ? 1 : 0, s.eligible ? 1 : 0);
  s.report_line = buf;
  return s;
}

PlatformShortcut match_macos_shortcut(uint32_t sdl_keycode, uint16_t sdl_keymod, bool is_macos) {
  if (!is_macos) return PlatformShortcut::kNone;
  const bool cmd = (sdl_keymod & SDL_KMOD_GUI) != 0;
  if (!cmd) return PlatformShortcut::kNone;
  const bool ctrl = (sdl_keymod & SDL_KMOD_CTRL) != 0;
  const bool shift = (sdl_keymod & SDL_KMOD_SHIFT) != 0;

  if (sdl_keycode == SDLK_COMMA && !ctrl && !shift) return PlatformShortcut::kSettings;
  if (sdl_keycode == SDLK_F && ctrl && !shift) return PlatformShortcut::kToggleFullscreen;
  if (sdl_keycode == SDLK_P && shift && !ctrl) return PlatformShortcut::kPhotoMode;
  if (sdl_keycode == SDLK_B && shift && !ctrl) return PlatformShortcut::kBugBookmark;
  if (sdl_keycode == SDLK_T && !ctrl && !shift) return PlatformShortcut::kTrainerOrTime;
  if (sdl_keycode == SDLK_Q && !ctrl && !shift) return PlatformShortcut::kQuit;
  return PlatformShortcut::kNone;
}

std::filesystem::path resolve_portable_data_root(const std::filesystem::path& exe_dir, bool is_macos) {
  std::error_code ec;
  if (exe_dir.empty()) return {};
  std::filesystem::path anchor = exe_dir;
  if (is_macos && exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents") {
    anchor = exe_dir.parent_path().parent_path().parent_path();
  }
  if (std::filesystem::exists(anchor / "portable.txt", ec)) return anchor / "data";
  if (std::filesystem::exists(exe_dir / "portable.txt", ec)) return exe_dir / "data";
  return {};
}

std::filesystem::path resolve_default_save_path(const std::filesystem::path& portable_data_root,
                                                const std::filesystem::path& pref_drive_dir) {
  if (!portable_data_root.empty()) return portable_data_root / "drive" / "save.txt";
  if (!pref_drive_dir.empty()) return pref_drive_dir / "save.txt";
  return {};
}

#ifdef __ANDROID__
namespace {

struct AdpfApi {
  bool loaded = false;
  void* (*get_manager)() = nullptr;
  void* (*create_session)(void*, const int32_t*, size_t, int64_t) = nullptr;
  int (*update_target)(void*, int64_t) = nullptr;
  int (*report_actual)(void*, int64_t) = nullptr;
  void (*close_session)(void*) = nullptr;
};

const AdpfApi& adpf_api() {
  static AdpfApi api = []() {
    AdpfApi a;
    if (void* h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL)) {
      a.get_manager = reinterpret_cast<void* (*)()>(dlsym(h, "APerformanceHint_getManager"));
      a.create_session = reinterpret_cast<void* (*)(void*, const int32_t*, size_t, int64_t)>(
          dlsym(h, "APerformanceHint_createSession"));
      a.update_target =
          reinterpret_cast<int (*)(void*, int64_t)>(dlsym(h, "APerformanceHint_updateTargetWorkDuration"));
      a.report_actual =
          reinterpret_cast<int (*)(void*, int64_t)>(dlsym(h, "APerformanceHint_reportActualWorkDuration"));
      a.close_session = reinterpret_cast<void (*)(void*)>(dlsym(h, "APerformanceHint_closeSession"));
      a.loaded = a.get_manager && a.create_session && a.update_target && a.report_actual && a.close_session;
    }
    return a;
  }();
  return api;
}

int64_t fps_to_target_ns(int target_fps) {
  const int fps = std::clamp(target_fps > 0 ? target_fps : 60, 24, 240);
  // Target 85% of frame budget so Android's scheduler keeps Prime/Performance cores awake.
  return int64_t(850000000LL / fps);
}

}  // namespace

AndroidPerfHintSession::~AndroidPerfHintSession() { close(); }

bool AndroidPerfHintSession::init(int target_fps) {
  close();
  const auto& api = adpf_api();
  if (!api.loaded) return false;
  void* mgr = api.get_manager();
  if (!mgr) return false;
  target_ns_ = fps_to_target_ns(target_fps);
  const int32_t tid = static_cast<int32_t>(gettid());
  session_ = api.create_session(mgr, &tid, 1, target_ns_);
  if (session_) {
    std::fprintf(stderr, "[adpf] active: tid=%d target_fps=%d target_ms=%.2f\n",
                 tid, target_fps, double(target_ns_) / 1e6);
  }
  return session_ != nullptr;
}

void AndroidPerfHintSession::update_target_fps(int target_fps) {
  if (!session_) return;
  const int64_t ns = fps_to_target_ns(target_fps);
  if (ns == target_ns_) return;
  target_ns_ = ns;
  const auto& api = adpf_api();
  if (api.loaded) api.update_target(session_, target_ns_);
}

void AndroidPerfHintSession::report_frame_ns(int64_t actual_work_ns) {
  if (!session_ || actual_work_ns <= 0) return;
  const auto& api = adpf_api();
  if (api.loaded) api.report_actual(session_, actual_work_ns);
}

void AndroidPerfHintSession::close() {
  if (!session_) return;
  const auto& api = adpf_api();
  if (api.loaded) api.close_session(session_);
  session_ = nullptr;
}

void trim_android_memory() {
#ifdef M_DECAY_TIME
  static const bool s_decay_set = (mallopt(M_DECAY_TIME, 1) != 0);
  (void)s_decay_set;
#endif
#ifdef M_PURGE_ALL
  mallopt(M_PURGE_ALL, 0);
#elif defined(M_PURGE)
  mallopt(M_PURGE, 0);
#endif
}

void enable_arm64_fp_flush_to_zero() {
#if defined(__aarch64__)
  uint64_t fpcr = 0;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= (1ull << 24);  // FZ: flush subnormals to zero
  __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

void configure_android_render_thread_qos() {
  setpriority(PRIO_PROCESS, 0, -4);
  const long ncpus = sysconf(_SC_NPROCESSORS_CONF);
  if (ncpus == 8) {
    // Snapdragon 8 Gen 2 (Odin 2): CPUs 0..2 are efficiency (Cortex-A510),
    // CPUs 3..6 are performance (A715/A710), CPU 7 is prime (Cortex-X3).
    // Keep the render thread on performance + prime cores so it never migrates
    // onto slow LITTLE cores under load.
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int cpu = 3; cpu < 8; ++cpu) CPU_SET(cpu, &mask);
    sched_setaffinity(0, sizeof(mask), &mask);
  }
}

void set_android_refresh_rate_hz(int desired_hz) {
  static int s_last_hz = -1;
  const int bucket = (desired_hz >= 110) ? 120 : ((desired_hz >= 80) ? 90 : 60);
  if (bucket == s_last_hz) return;
  s_last_hz = bucket;
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto act = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !act) return;
  jclass cls = env->GetObjectClass(act);
  if (cls) {
    if (jmethodID mid = env->GetMethodID(cls, "setPreferredRefreshRateHz", "(I)V")) {
      env->CallVoidMethod(act, mid, static_cast<jint>(bucket));
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(act);
}
#else
AndroidPerfHintSession::~AndroidPerfHintSession() = default;
bool AndroidPerfHintSession::init(int) { return false; }
void AndroidPerfHintSession::update_target_fps(int) {}
void AndroidPerfHintSession::report_frame_ns(int64_t) {}
void AndroidPerfHintSession::close() {}
void trim_android_memory() {}
void enable_arm64_fp_flush_to_zero() {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
  // Enable SSE Flush-To-Zero (bit 15) and Denormals-Are-Zero (bit 6) on x86/x64
  // so physics/audio/lighting loops never suffer 100-cycle subnormal traps.
  _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
}
void configure_android_render_thread_qos() {
#ifdef _WIN32
  static bool s_win_qos_configured = false;
  if (s_win_qos_configured) return;
  s_win_qos_configured = true;

  // 1. Disable Windows 11 EcoQoS / Efficiency Mode throttling for the game process.
  struct WinProcessPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
  };
  constexpr ULONG kPowerThrottlingCurrentVersion = 1;
  constexpr ULONG kPowerThrottlingExecutionSpeed = 0x1;
  constexpr ULONG kPowerThrottlingIgnoreTimerResolution = 0x4;
  WinProcessPowerThrottlingState throttle{};
  throttle.Version = kPowerThrottlingCurrentVersion;
  throttle.ControlMask = kPowerThrottlingExecutionSpeed | kPowerThrottlingIgnoreTimerResolution;
  throttle.StateMask = 0;  // Disable execution throttling and preserve 1 ms timer resolution.
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttle, sizeof(throttle));

  // 2. Request 1 ms system timer resolution for sub-millisecond sleep/pacing precision.
  if (HMODULE winmm = LoadLibraryW(L"winmm.dll")) {
    using TimeBeginPeriodFn = UINT(WINAPI*)(UINT);
    if (auto fn = reinterpret_cast<TimeBeginPeriodFn>(GetProcAddress(winmm, "timeBeginPeriod"))) {
      fn(1);
    }
  }

  // 3. Register the render thread with MMCSS "Games" profile so Windows prioritizes it
  //    above background streaming/decompression threads and boosts priority cleanly.
  if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
    using AvSetMmThreadCharacteristicsWFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    using AvSetMmThreadPriorityFn = BOOL(WINAPI*)(HANDLE, int);
    if (auto set_char =
            reinterpret_cast<AvSetMmThreadCharacteristicsWFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))) {
      DWORD task_idx = 0;
      if (HANDLE h = set_char(L"Games", &task_idx)) {
        if (auto set_prio = reinterpret_cast<AvSetMmThreadPriorityFn>(GetProcAddress(avrt, "AvSetMmThreadPriority"))) {
          set_prio(h, 1);  // AVRT_PRIORITY_HIGH
        }
      }
    }
  }
#endif
}
void set_android_refresh_rate_hz(int) {}
#endif

}  // namespace carview
