// JNI bridge connecting SetupBridge.java to libs/setup (XISO extractor, STFS
// installer, Mod & Turnip installer, and in-process on-device baker).
#include <jni.h>

#include <atomic>
#include <filesystem>
#include <sstream>
#include <string>

#include "setup/mod_installer.h"
#include "setup/on_device_baker.h"
#include "setup/stfs.h"
#include "setup/xiso.h"

namespace {

std::atomic<bool> g_cancel_flag{false};

std::string from_jstring(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  if (!chars) return {};
  std::string out(chars);
  env->ReleaseStringUTFChars(s, chars);
  return out;
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20u) out.push_back(c);
        break;
    }
  }
  return out;
}

class JniProgressDispatcher {
 public:
  JniProgressDispatcher(JNIEnv* env, jobject listener) {
    if (!env || !listener) return;
    if (env->GetJavaVM(&vm_) != JNI_OK || !vm_) return;
    listener_ = env->NewGlobalRef(listener);
    jclass cls = env->GetObjectClass(listener);
    if (cls) {
      on_progress_ = env->GetMethodID(cls, "onProgress", "(IIFLjava/lang/String;Ljava/lang/String;)V");
      on_log_ = env->GetMethodID(cls, "onLog", "(Ljava/lang/String;)V");
      env->DeleteLocalRef(cls);
    }
  }

  ~JniProgressDispatcher() {
    if (listener_ && vm_) {
      JNIEnv* env = nullptr;
      bool attached = false;
      if (attach(env, attached) && env) {
        env->DeleteGlobalRef(listener_);
      }
      if (attached) vm_->DetachCurrentThread();
    }
  }

  JniProgressDispatcher(const JniProgressDispatcher&) = delete;
  JniProgressDispatcher& operator=(const JniProgressDispatcher&) = delete;

  void progress(int stage, int total_stages, float fraction, const std::string& label,
                const std::string& detail) const {
    if (!vm_ || !listener_ || !on_progress_) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (!attach(env, attached) || !env) return;
    jstring jlabel = env->NewStringUTF(label.c_str());
    jstring jdetail = env->NewStringUTF(detail.c_str());
    env->CallVoidMethod(listener_, on_progress_, static_cast<jint>(stage), static_cast<jint>(total_stages),
                        static_cast<jfloat>(fraction), jlabel, jdetail);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jlabel) env->DeleteLocalRef(jlabel);
    if (jdetail) env->DeleteLocalRef(jdetail);
    if (attached) vm_->DetachCurrentThread();
  }

  void log(const std::string& line) const {
    if (!vm_ || !listener_ || !on_log_) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (!attach(env, attached) || !env) return;
    jstring jline = env->NewStringUTF(line.c_str());
    env->CallVoidMethod(listener_, on_log_, jline);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jline) env->DeleteLocalRef(jline);
    if (attached) vm_->DetachCurrentThread();
  }

 private:
  bool attach(JNIEnv*& env, bool& attached) const {
    attached = false;
    const jint rc = vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_OK) return true;
    if (rc == JNI_EDETACHED) {
      if (vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        attached = true;
        return true;
      }
    }
    return false;
  }

  JavaVM* vm_ = nullptr;
  jobject listener_ = nullptr;
  jmethodID on_progress_ = nullptr;
  jmethodID on_log_ = nullptr;
};

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeInspectLibrary(JNIEnv* env, jclass,
                                                                                      jstring play_dir,
                                                                                      jstring state_dir) {
  const auto status =
      setup::baker::inspect_library(std::filesystem::path(from_jstring(env, play_dir)),
                                    std::filesystem::path(from_jstring(env, state_dir)));
  const std::string json = setup::baker::library_status_to_json(status);
  return env->NewStringUTF(json.c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeExtractIsoFd(JNIEnv* env, jclass, jint fd,
                                                                                    jstring out_game_dir,
                                                                                    jboolean media_only,
                                                                                    jboolean skip_xex,
                                                                                    jobject listener) {
  g_cancel_flag.store(false, std::memory_order_relaxed);
  JniProgressDispatcher disp(env, listener);

  setup::xiso::ExtractOptions opts;
  opts.media_only = (media_only == JNI_TRUE);
  opts.skip_xex = (skip_xex == JNI_TRUE);

  setup::xiso::ExtractReport rep;
  auto cb = [&](const setup::xiso::Progress& p) {
    const float frac =
        p.bytes_total > 0 ? static_cast<float>(static_cast<double>(p.bytes_done) / static_cast<double>(p.bytes_total))
                          : 0.0f;
    disp.progress(0, 1, frac, "Extracting game ISO", p.current_file);
  };

  disp.log("Opening Xbox 360 ISO stream (fd=" + std::to_string(fd) + ")");
  const bool ok = setup::xiso::extract_iso_fd(static_cast<int>(fd), true,
                                              std::filesystem::path(from_jstring(env, out_game_dir)), opts, cb,
                                              &g_cancel_flag, rep);
  if (ok) {
    disp.log("Extracted " + std::to_string(rep.files_extracted) + " files (" + rep.partition.format_label + ")");
  } else {
    disp.log("ISO extraction error: " + rep.error);
  }

  std::ostringstream ss;
  ss << "{"
     << "\"ok\":" << (ok ? "true" : "false") << ","
     << "\"cancelled\":" << (rep.cancelled ? "true" : "false") << ","
     << "\"format\":\"" << json_escape(rep.partition.format_label) << "\","
     << "\"files_extracted\":" << rep.files_extracted << ","
     << "\"files_skipped\":" << rep.files_skipped << ","
     << "\"bytes_written\":" << rep.bytes_written << ","
     << "\"error\":\"" << json_escape(rep.error) << "\""
     << "}";
  return env->NewStringUTF(ss.str().c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeInstallStfsFd(JNIEnv* env, jclass, jint fd,
                                                                                     jstring display_name,
                                                                                     jstring play_dir,
                                                                                     jstring state_dir,
                                                                                     jobject listener) {
  g_cancel_flag.store(false, std::memory_order_relaxed);
  JniProgressDispatcher disp(env, listener);

  setup::stfs::InstallTargets targets;
  targets.play_dir = std::filesystem::path(from_jstring(env, play_dir));
  targets.state_dir = std::filesystem::path(from_jstring(env, state_dir));
  targets.suggested_name = from_jstring(env, display_name);

  setup::stfs::InstallReport rep;
  auto cb = [&](const setup::stfs::Progress& p) {
    const float frac =
        p.bytes_total > 0 ? static_cast<float>(static_cast<double>(p.bytes_done) / static_cast<double>(p.bytes_total))
                          : 0.0f;
    disp.progress(0, 1, frac, "Installing STFS package", p.current_file);
  };

  disp.log("Installing STFS package: " + targets.suggested_name);
  const bool ok = setup::stfs::install_package_fd(static_cast<int>(fd), true, targets, cb, &g_cancel_flag, rep);
  if (ok) {
    disp.log("Installed " + rep.display_name + " (" + std::to_string(rep.files_extracted) + " files)");
  } else {
    disp.log("STFS install error: " + rep.error);
  }

  std::ostringstream ss;
  ss << "{"
     << "\"ok\":" << (ok ? "true" : "false") << ","
     << "\"cancelled\":" << (rep.cancelled ? "true" : "false") << ","
     << "\"package_id\":\"" << json_escape(rep.package_id) << "\","
     << "\"display_name\":\"" << json_escape(rep.display_name) << "\","
     << "\"kind\":\"" << setup::stfs::package_kind_name(rep.kind) << "\","
     << "\"files_extracted\":" << rep.files_extracted << ","
     << "\"error\":\"" << json_escape(rep.error) << "\""
     << "}";
  return env->NewStringUTF(ss.str().c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeInstallModZipFd(JNIEnv* env, jclass, jint fd,
                                                                                       jstring display_name,
                                                                                       jstring mods_dir,
                                                                                       jobject listener) {
  g_cancel_flag.store(false, std::memory_order_relaxed);
  JniProgressDispatcher disp(env, listener);

  setup::mods::InstallModReport rep;
  auto cb = [&](const setup::mods::Progress& p) {
    const float frac =
        p.bytes_total > 0 ? static_cast<float>(static_cast<double>(p.bytes_done) / static_cast<double>(p.bytes_total))
                          : 0.0f;
    disp.progress(0, 1, frac, "Installing mod archive", p.current_file);
  };

  const std::string name = from_jstring(env, display_name);
  disp.log("Installing mod ZIP: " + name);
  const bool ok = setup::mods::install_mod_zip_fd(static_cast<int>(fd), true, name,
                                                  std::filesystem::path(from_jstring(env, mods_dir)), cb,
                                                  &g_cancel_flag, rep);
  if (ok) {
    disp.log("Installed mod '" + rep.mod_name + "' (" + std::to_string(rep.files_extracted) + " files)");
  } else {
    disp.log("Mod install error: " + rep.error);
  }

  std::ostringstream ss;
  ss << "{"
     << "\"ok\":" << (ok ? "true" : "false") << ","
     << "\"cancelled\":" << (rep.cancelled ? "true" : "false") << ","
     << "\"mod_name\":\"" << json_escape(rep.mod_name) << "\","
     << "\"files_extracted\":" << rep.files_extracted << ","
     << "\"error\":\"" << json_escape(rep.error) << "\""
     << "}";
  return env->NewStringUTF(ss.str().c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeSetModEnabled(JNIEnv* env, jclass,
                                                                                     jstring mods_dir,
                                                                                     jstring mod_name,
                                                                                     jboolean enabled) {
  std::string err;
  const bool ok = setup::mods::set_mod_enabled(std::filesystem::path(from_jstring(env, mods_dir)),
                                               from_jstring(env, mod_name), enabled == JNI_TRUE, &err);
  const std::string json = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" + json_escape(err) + "\"}";
  return env->NewStringUTF(json.c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeMoveMod(JNIEnv* env, jclass, jstring mods_dir,
                                                                               jstring mod_name, jint delta) {
  std::string err;
  const bool ok = setup::mods::move_mod(std::filesystem::path(from_jstring(env, mods_dir)), from_jstring(env, mod_name),
                                        static_cast<int>(delta), &err);
  const std::string json = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" + json_escape(err) + "\"}";
  return env->NewStringUTF(json.c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeRemoveMod(JNIEnv* env, jclass, jstring mods_dir,
                                                                                 jstring mod_name) {
  std::string err;
  const bool ok = setup::mods::remove_mod(std::filesystem::path(from_jstring(env, mods_dir)),
                                          from_jstring(env, mod_name), &err);
  const std::string json = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" + json_escape(err) + "\"}";
  return env->NewStringUTF(json.c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeInstallTurnipFd(JNIEnv* env, jclass, jint fd,
                                                                                       jstring file_name,
                                                                                       jstring drivers_dir) {
  setup::mods::TurnipInstallReport rep;
  const bool ok = setup::mods::install_turnip_driver_fd(static_cast<int>(fd), true, from_jstring(env, file_name),
                                                        std::filesystem::path(from_jstring(env, drivers_dir)), rep);
  std::ostringstream ss;
  ss << "{"
     << "\"ok\":" << (ok ? "true" : "false") << ","
     << "\"driver_name\":\"" << json_escape(rep.driver_name) << "\","
     << "\"driver_path\":\"" << json_escape(rep.driver_path.generic_string()) << "\","
     << "\"bytes_written\":" << rep.bytes_written << ","
     << "\"error\":\"" << json_escape(rep.error) << "\""
     << "}";
  return env->NewStringUTF(ss.str().c_str());
}

JNIEXPORT jstring JNICALL Java_net_shiftengine_drive_SetupBridge_nativeRunOnDeviceBake(JNIEnv* env, jclass,
                                                                                       jstring game_dir,
                                                                                       jstring state_dir,
                                                                                       jstring play_dir,
                                                                                       jboolean full_roster,
                                                                                       jboolean resume,
                                                                                       jobject listener) {
  g_cancel_flag.store(false, std::memory_order_relaxed);
  JniProgressDispatcher disp(env, listener);

  setup::baker::BakeOptions opts;
  opts.game_root = std::filesystem::path(from_jstring(env, game_dir));
  opts.state_root = std::filesystem::path(from_jstring(env, state_dir));
  opts.play_dir = std::filesystem::path(from_jstring(env, play_dir));
  opts.mode = (full_roster == JNI_TRUE) ? setup::baker::BakeMode::kFullRoster : setup::baker::BakeMode::kFastStart;
  opts.resume = (resume == JNI_TRUE);
  opts.require_world_index = true;

  setup::baker::BakeReport rep;
  auto prog_cb = [&](const setup::baker::BakeProgress& p) {
    disp.progress(p.stage_index, p.stage_count, p.overall_fraction, p.stage_label, p.detail);
  };
  auto log_cb = [&](const std::string& line) { disp.log(line); };

  const bool ok = setup::baker::run_on_device_bake(opts, prog_cb, log_cb, &g_cancel_flag, rep);

  std::ostringstream ss;
  ss << "{"
     << "\"ok\":" << (ok ? "true" : "false") << ","
     << "\"cancelled\":" << (rep.cancelled ? "true" : "false") << ","
     << "\"cars_baked\":" << rep.cars_baked << ","
     << "\"cars_reused\":" << rep.cars_reused << ","
     << "\"cars_total_available\":" << rep.cars_total_available << ","
     << "\"world_index_ready\":" << (rep.world_index_ready ? "true" : "false") << ","
     << "\"play_txt_written\":" << (rep.play_txt_written ? "true" : "false") << ","
     << "\"elapsed_seconds\":" << rep.elapsed_seconds << ","
     << "\"error\":\"" << json_escape(rep.error) << "\""
     << "}";
  return env->NewStringUTF(ss.str().c_str());
}

JNIEXPORT void JNICALL Java_net_shiftengine_drive_SetupBridge_nativeCancelSetup(JNIEnv*, jclass) {
  g_cancel_flag.store(true, std::memory_order_relaxed);
}

}  // extern "C"
