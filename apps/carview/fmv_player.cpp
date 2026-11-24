#include "fmv_player.h"

#include "ui4_chrome.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "audio/bake.h"
#include "audio/xml.h"
#include "fh1data/stringtables.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <objbase.h>
#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
}
#endif

namespace carview {
namespace {

std::mutex& subtitle_resolver_mutex() {
  static std::mutex mu;
  return mu;
}

FmvPlayer::SubtitleResolver& subtitle_resolver_fn() {
  static FmvPlayer::SubtitleResolver fn;
  return fn;
}

std::string upper_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool is_wmv_path(const std::filesystem::path& p) {
  return lower_str(p.extension().string()) == ".wmv";
}

#ifdef _WIN32
struct MfSessionGuard {
  bool com_ok = false;
  bool mf_ok = false;
  MfSessionGuard() {
    const HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_ok = SUCCEEDED(hr_com);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) mf_ok = true;
  }
  ~MfSessionGuard() {
    if (mf_ok) MFShutdown();
    if (com_ok) CoUninitialize();
  }
};

void convert_bgra_to_rgba_scaled(const uint8_t* src, int src_w, int src_h, int src_stride,
                                 uint8_t* dst, int dst_w, int dst_h) {
  const bool flip_y = src_stride < 0;
  const int abs_stride = flip_y ? -src_stride : src_stride;
  const uint8_t* base = flip_y ? (src + int64_t(src_h - 1) * int64_t(abs_stride)) : src;
  for (int y = 0; y < dst_h; ++y) {
    const int sy = (src_h == dst_h) ? y : int((int64_t(y) * src_h) / dst_h);
    const uint8_t* row = flip_y ? (base - int64_t(sy) * int64_t(abs_stride))
                                : (base + int64_t(sy) * int64_t(abs_stride));
    uint8_t* out = dst + size_t(y) * size_t(dst_w) * 4;
    if (src_w == dst_w) {
      for (int x = 0; x < dst_w; ++x) {
        out[4 * x + 0] = row[4 * x + 2];
        out[4 * x + 1] = row[4 * x + 1];
        out[4 * x + 2] = row[4 * x + 0];
        out[4 * x + 3] = 255;
      }
    } else {
      for (int x = 0; x < dst_w; ++x) {
        const int sx = int((int64_t(x) * src_w) / dst_w);
        out[4 * x + 0] = row[4 * sx + 2];
        out[4 * x + 1] = row[4 * sx + 1];
        out[4 * x + 2] = row[4 * sx + 0];
        out[4 * x + 3] = 255;
      }
    }
  }
}

bool mf_create_reader(const std::filesystem::path& wmv_path, IMFSourceReader** out_reader) {
  *out_reader = nullptr;
  IMFAttributes* attrs = nullptr;
  if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
  attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
  const HRESULT hr = MFCreateSourceReaderFromURL(wmv_path.wstring().c_str(), attrs, out_reader);
  attrs->Release();
  return SUCCEEDED(hr) && *out_reader != nullptr;
}

bool mf_probe_wmv(const std::filesystem::path& wmv_path, FmvProbeInfo& out) {
  MfSessionGuard guard;
  if (!guard.mf_ok) return false;
  IMFSourceReader* reader = nullptr;
  if (!mf_create_reader(wmv_path, &reader)) return false;

  IMFMediaType* vtype = nullptr;
  if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &vtype)) && vtype) {
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(MFGetAttributeSize(vtype, MF_MT_FRAME_SIZE, &w, &h))) {
      out.width = int(w);
      out.height = int(h);
    }
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(vtype, MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0) {
      out.fps = double(num) / double(den);
    }
    vtype->Release();
  }

  PROPVARIANT dur_var;
  PropVariantInit(&dur_var);
  if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur_var))) {
    if (dur_var.vt == VT_UI8) out.duration_s = double(dur_var.uhVal.QuadPart) * 1e-7;
    else if (dur_var.vt == VT_I8) out.duration_s = double(dur_var.hVal.QuadPart) * 1e-7;
    PropVariantClear(&dur_var);
  }

  for (DWORD s = 0; s < 32; ++s) {
    IMFMediaType* st = nullptr;
    const HRESULT hr = reader->GetNativeMediaType(s, 0, &st);
    if (hr == MF_E_INVALIDSTREAMNUMBER) break;
    if (SUCCEEDED(hr) && st) {
      GUID major{};
      if (SUCCEEDED(st->GetMajorType(&major)) && major == MFMediaType_Audio) {
        out.has_audio = true;
        ++out.audio_streams;
      }
      st->Release();
    }
  }
  reader->Release();
  return out.width > 0 && out.height > 0;
}

bool mf_decode_frame_wmv(const std::filesystem::path& wmv_path, double time_s, int target_w, int target_h,
                         std::vector<uint8_t>& out_rgba, std::string* error) {
  MfSessionGuard guard;
  if (!guard.mf_ok) {
    if (error) *error = "Media Foundation unavailable";
    return false;
  }
  IMFSourceReader* reader = nullptr;
  if (!mf_create_reader(wmv_path, &reader)) {
    if (error) *error = "Media Foundation failed to open " + wmv_path.string();
    return false;
  }
  reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

  IMFMediaType* rgb_type = nullptr;
  if (FAILED(MFCreateMediaType(&rgb_type))) {
    reader->Release();
    return false;
  }
  rgb_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  rgb_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  const HRESULT set_hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb_type);
  rgb_type->Release();
  if (FAILED(set_hr)) {
    reader->Release();
    if (error) *error = "Media Foundation RGB32 setup failed";
    return false;
  }

  int src_w = target_w, src_h = target_h, stride = target_w * 4;
  IMFMediaType* cur = nullptr;
  if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
      src_w = int(w);
      src_h = int(h);
    }
    UINT32 st_u32 = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st_u32))) {
      stride = int(static_cast<int32_t>(st_u32));
    } else {
      stride = src_w * 4;
    }
    cur->Release();
  }

  if (time_s > 0.0) {
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = LONGLONG(time_s * 1e7);
    reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
  }

  bool got_frame = false;
  for (int attempt = 0; attempt < 30; ++attempt) {
    DWORD flags = 0;
    LONGLONG ts = 0;
    IMFSample* sample = nullptr;
    const HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
      if (sample) sample->Release();
      break;
    }
    if (!sample) continue;
    IMFMediaBuffer* buf = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
      BYTE* data = nullptr;
      DWORD cur_len = 0;
      if (SUCCEEDED(buf->Lock(&data, nullptr, &cur_len)) && data &&
          cur_len >= DWORD(std::abs(stride) * src_h)) {
        out_rgba.resize(size_t(target_w) * size_t(target_h) * 4);
        convert_bgra_to_rgba_scaled(data, src_w, src_h, stride, out_rgba.data(), target_w, target_h);
        got_frame = true;
        buf->Unlock();
      }
      buf->Release();
    }
    sample->Release();
    if (got_frame) break;
  }

  reader->Release();
  if (!got_frame && error) *error = "Media Foundation decoded no video frame";
  return got_frame;
}
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
void convert_yuv420p_to_rgba_scaled(const AVFrame* frame, uint8_t* dst, int dst_w, int dst_h) {
  const int src_w = frame->width;
  const int src_h = frame->height;
  if (src_w <= 0 || src_h <= 0) return;
  const uint8_t* y_plane = frame->data[0];
  const uint8_t* u_plane = frame->data[1];
  const uint8_t* v_plane = frame->data[2];
  const int y_stride = frame->linesize[0];
  const int u_stride = frame->linesize[1];
  const int v_stride = frame->linesize[2];
  for (int y = 0; y < dst_h; ++y) {
    const int sy = (src_h == dst_h) ? y : int((int64_t(y) * src_h) / dst_h);
    const uint8_t* y_row = y_plane + int64_t(sy) * y_stride;
    const uint8_t* u_row = u_plane + int64_t(sy / 2) * u_stride;
    const uint8_t* v_row = v_plane + int64_t(sy / 2) * v_stride;
    uint8_t* out = dst + size_t(y) * size_t(dst_w) * 4;
    for (int x = 0; x < dst_w; ++x) {
      const int sx = (src_w == dst_w) ? x : int((int64_t(x) * src_w) / dst_w);
      const int Y = int(y_row[sx]) - 16;
      const int U = int(u_row[sx / 2]) - 128;
      const int V = int(v_row[sx / 2]) - 128;
      const int c = std::max(0, Y) * 298;
      const int r = (c + 409 * V + 128) >> 8;
      const int g = (c - 100 * U - 208 * V + 128) >> 8;
      const int b = (c + 516 * U + 128) >> 8;
      out[4 * x + 0] = uint8_t(std::clamp(r, 0, 255));
      out[4 * x + 1] = uint8_t(std::clamp(g, 0, 255));
      out[4 * x + 2] = uint8_t(std::clamp(b, 0, 255));
      out[4 * x + 3] = 255;
    }
  }
}

bool ff_probe_wmv(const std::filesystem::path& wmv_path, FmvProbeInfo& out) {
  av_log_set_level(AV_LOG_QUIET);
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, wmv_path.string().c_str(), nullptr, nullptr) < 0 || !fmt) return false;
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  if (fmt->duration > 0) out.duration_s = double(fmt->duration) / double(AV_TIME_BASE);
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    const AVStream* st = fmt->streams[i];
    if (!st || !st->codecpar) continue;
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && out.width == 0) {
      out.width = st->codecpar->width;
      out.height = st->codecpar->height;
      const AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
      if (fr.num > 0 && fr.den > 0) out.fps = double(fr.num) / double(fr.den);
    } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      out.has_audio = true;
      ++out.audio_streams;
    }
  }
  avformat_close_input(&fmt);
  return out.width > 0 && out.height > 0;
}

bool ff_decode_frame_wmv(const std::filesystem::path& wmv_path, double time_s, int target_w, int target_h,
                         std::vector<uint8_t>& out_rgba, std::string* error) {
  av_log_set_level(AV_LOG_QUIET);
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, wmv_path.string().c_str(), nullptr, nullptr) < 0 || !fmt) {
    if (error) *error = "libavformat failed to open " + wmv_path.string();
    return false;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  int vidx = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    if (fmt->streams[i]->codecpar && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      vidx = int(i);
      break;
    }
  }
  if (vidx < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  const AVCodec* dec = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
  AVCodecContext* vctx = dec ? avcodec_alloc_context3(dec) : nullptr;
  if (!vctx || avcodec_parameters_to_context(vctx, fmt->streams[vidx]->codecpar) < 0 ||
      avcodec_open2(vctx, dec, nullptr) < 0) {
    if (vctx) avcodec_free_context(&vctx);
    avformat_close_input(&fmt);
    return false;
  }
  if (time_s > 0.0) {
    av_seek_frame(fmt, -1, int64_t(time_s * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vctx);
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* fr = av_frame_alloc();
  bool got = false;
  while (pkt && fr && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vidx) {
      if (avcodec_send_packet(vctx, pkt) >= 0) {
        while (avcodec_receive_frame(vctx, fr) == 0) {
          out_rgba.resize(size_t(target_w) * size_t(target_h) * 4);
          convert_yuv420p_to_rgba_scaled(fr, out_rgba.data(), target_w, target_h);
          got = true;
          break;
        }
      }
    }
    av_packet_unref(pkt);
    if (got) break;
  }
  if (fr) av_frame_free(&fr);
  if (pkt) av_packet_free(&pkt);
  avcodec_free_context(&vctx);
  avformat_close_input(&fmt);
  if (!got && error) *error = "libavcodec decoded no video frame";
  return got;
}
#endif

std::string read_text_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::filesystem::path find_ffprobe_for(const std::filesystem::path& ffmpeg) {
  if (ffmpeg.empty()) return {};
  std::error_code ec;
  auto probe = ffmpeg.parent_path() /
#ifdef _WIN32
               "ffprobe.exe";
#else
               "ffprobe";
#endif
  if (std::filesystem::exists(probe, ec)) return probe;
  return {};
}

struct PipeProcess {
  FILE* stream = nullptr;
#ifdef _WIN32
  HANDLE process = nullptr;
#endif
};

PipeProcess open_pipe_read(const std::string& cmd) {
  PipeProcess out{};
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return out;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul_in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE nul_err = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = (nul_in != INVALID_HANDLE_VALUE) ? nul_in : nullptr;
  si.hStdOutput = write_pipe;
  si.hStdError = (nul_err != INVALID_HANDLE_VALUE) ? nul_err : nullptr;

  PROCESS_INFORMATION pi{};
  std::string cmd_mut = cmd;
  const BOOL ok = CreateProcessA(nullptr, cmd_mut.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &si, &pi);
  CloseHandle(write_pipe);
  if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
  if (nul_err != INVALID_HANDLE_VALUE) CloseHandle(nul_err);

  if (!ok) {
    CloseHandle(read_pipe);
    return out;
  }
  CloseHandle(pi.hThread);
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(read_pipe), _O_RDONLY | _O_BINARY);
  if (fd < 0) {
    CloseHandle(read_pipe);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    return out;
  }
  out.stream = _fdopen(fd, "rb");
  if (!out.stream) {
    _close(fd);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    return out;
  }
  out.process = pi.hProcess;
#else
  const std::string silent_cmd = cmd + " 2>/dev/null";
  out.stream = popen(silent_cmd.c_str(), "r");
#endif
  return out;
}

void close_pipe(PipeProcess& p, bool abort_early = false) {
#ifdef _WIN32
  if (p.stream) {
    std::fclose(p.stream);
    p.stream = nullptr;
  }
  if (p.process) {
    const DWORD wait_ms = abort_early ? 25u : 2000u;
    if (WaitForSingleObject(p.process, wait_ms) == WAIT_TIMEOUT) {
      TerminateProcess(p.process, 0);
      WaitForSingleObject(p.process, 200u);
    }
    CloseHandle(p.process);
    p.process = nullptr;
  }
#else
  (void)abort_early;
  if (p.stream) {
    pclose(p.stream);
    p.stream = nullptr;
  }
#endif
}

std::string quote_arg(const std::string& s) { return "\"" + s + "\""; }

std::string movie_relpath(FmvMovieId id) {
  switch (id) {
    case FmvMovieId::kT10MsCombined: return "splash_intros/T10_MS_Combined.wmv";
    case FmvMovieId::kMsgsLogo: return "splash_intros/MSGS_Logo.wmv";
    case FmvMovieId::kDolbyCorona: return "splash_intros/Dolby_Corona_Intro.wmv";
    case FmvMovieId::kForzaIntro: return "splash_intros/Forza_Intro.wmv";
    case FmvMovieId::kForzaTone: return "splash_intros/Forza_Tone.wmv";
    case FmvMovieId::kPressStart: return "PressStart.wmv";
    case FmvMovieId::kFmv01Opening: return "FMV_01.wmv";
    case FmvMovieId::kFmv02Ceremony: return "FMV_02.wmv";
    case FmvMovieId::kFmv04Credits: return "FMV_04.wmv";
    case FmvMovieId::kWristbandGet: return "WristbandGet.wmv";
    case FmvMovieId::kRallyBgLoop: return "Splash_Intros/BgLoop.wmv";
    case FmvMovieId::kRallyForzaTone: return "Splash_Intros/Forza_Tone_Rally.wmv";
    case FmvMovieId::kClubIntro: return "Splash_Intros/car_challenges_intro.wmv";
  }
  return "FMV_01.wmv";
}

const char* movie_label(FmvMovieId id) {
  switch (id) {
    case FmvMovieId::kT10MsCombined: return "Turn 10 / Microsoft Studios";
    case FmvMovieId::kMsgsLogo: return "Microsoft Game Studios";
    case FmvMovieId::kDolbyCorona: return "Dolby Digital";
    case FmvMovieId::kForzaIntro: return "Forza Horizon Intro";
    case FmvMovieId::kForzaTone: return "Forza Horizon Tone";
    case FmvMovieId::kPressStart: return "Press Start";
    case FmvMovieId::kFmv01Opening: return "Horizon Festival Opening";
    case FmvMovieId::kFmv02Ceremony: return "Wristband Ceremony";
    case FmvMovieId::kFmv04Credits: return "Credits";
    case FmvMovieId::kWristbandGet: return "Wristband Unlocked";
    case FmvMovieId::kRallyBgLoop: return "Horizon Rally Background";
    case FmvMovieId::kRallyForzaTone: return "Horizon Rally Intro";
    case FmvMovieId::kClubIntro: return "1000 Club Intro";
  }
  return "FMV";
}

}  // namespace

void downmix_interleaved_to_stereo(const float* src, int src_channels, size_t frames, float* dst_stereo) {
  if (!src || !dst_stereo || frames == 0) return;
  const int ch = std::max(1, src_channels);
  if (ch == 2) {
    std::memcpy(dst_stereo, src, frames * 2 * sizeof(float));
    return;
  }
  if (ch == 1) {
    for (size_t i = 0; i < frames; ++i) {
      dst_stereo[2 * i] = src[i];
      dst_stereo[2 * i + 1] = src[i];
    }
    return;
  }
  constexpr float kFold = 0.70710678f;
  if (ch >= 6) {
    // ITU / FMOD 5.1 (FL, FR, C, LFE, BL, BR) -> stereo fold; 7.1 adds SL/SR.
    for (size_t i = 0; i < frames; ++i) {
      const float* f = src + i * size_t(ch);
      float l = f[0] + kFold * f[2] + kFold * f[4];
      float r = f[1] + kFold * f[2] + kFold * f[5];
      if (ch >= 8) {
        l += kFold * f[6];
        r += kFold * f[7];
      }
      dst_stereo[2 * i] = std::clamp(l, -1.0f, 1.0f);
      dst_stereo[2 * i + 1] = std::clamp(r, -1.0f, 1.0f);
    }
    return;
  }
  if (ch == 4) {
    // Quad (FL, FR, BL, BR) -> stereo fold.
    for (size_t i = 0; i < frames; ++i) {
      const float* f = src + i * 4;
      dst_stereo[2 * i] = std::clamp(f[0] + kFold * f[2], -1.0f, 1.0f);
      dst_stereo[2 * i + 1] = std::clamp(f[1] + kFold * f[3], -1.0f, 1.0f);
    }
    return;
  }
  for (size_t i = 0; i < frames; ++i) {
    const float* f = src + i * size_t(ch);
    dst_stereo[2 * i] = f[0];
    dst_stereo[2 * i + 1] = f[1];
  }
}

int FmvStreamDef::audio_stream_id_for(std::string_view language) const {
  if (audio.empty()) return -1;
  const std::string want = upper_str(language.empty() ? "EN" : language);
  for (const auto& t : audio) {
    if (upper_str(t.lang) == want && t.id >= 0) return t.id;
  }
  for (const auto& t : audio) {
    if (upper_str(t.lang) == "EN" && t.id >= 0) return t.id;
  }
  for (const auto& t : audio) {
    if (t.id >= 0) return t.id;
  }
  return -1;
}

int FmvStreamDef::audio_index_for(std::string_view language) const {
  if (const int id = audio_stream_id_for(language); id >= 0) return id;
  if (audio.empty()) return 0;
  const std::string want = upper_str(language.empty() ? "EN" : language);
  for (size_t i = 0; i < audio.size(); ++i) {
    if (upper_str(audio[i].lang) == want) return int(i);
  }
  for (size_t i = 0; i < audio.size(); ++i) {
    if (upper_str(audio[i].lang) == "EN") return int(i);
  }
  return 0;
}

bool parse_fmv_def(std::string_view xml, FmvStreamDef& out) {
  out = FmvStreamDef{};
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, nullptr)) return false;
  const audio::XmlNode* def = audio::iequals(root.name, "StreamDefinition") ? &root : root.child("StreamDefinition");
  if (!def) {
    for (const auto& c : root.children) {
      if (audio::iequals(c.name, "StreamDefinition")) {
        def = &c;
        break;
      }
    }
  }
  if (!def) return false;
  for (const auto& c : def->children) {
    if (audio::iequals(c.name, "Video")) {
      out.video_id = int(c.number("id", -1));
    } else if (audio::iequals(c.name, "Audio")) {
      FmvStreamDef::AudioTrack t;
      t.id = int(c.number("id", -1));
      t.lang = c.attr("lang");
      if (!t.lang.empty() || t.id >= 0) out.audio.push_back(std::move(t));
    }
  }
  return out.video_id >= 0 || !out.audio.empty();
}

bool parse_fmv_subtitles(std::string_view xml, std::vector<FmvSubtitleCue>& out) {
  out.clear();
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, nullptr)) return false;
  const audio::XmlNode* subs = audio::iequals(root.name, "Subtitles") ? &root : root.child("Subtitles");
  if (!subs) return false;
  for (const auto& c : subs->children) {
    const std::string uname = upper_str(c.name);
    if (uname.rfind("SUBTITLE", 0) != 0) continue;
    FmvSubtitleCue cue;
    cue.index = std::atoi(uname.c_str() + 8);
    cue.start_s = c.number("Start", 0.0);
    out.push_back(cue);
  }
  std::sort(out.begin(), out.end(), [](const FmvSubtitleCue& a, const FmvSubtitleCue& b) {
    return a.start_s < b.start_s;
  });
  return !out.empty();
}

FmvPlayer::~FmvPlayer() { close(); }

void FmvPlayer::set_subtitle_resolver(SubtitleResolver fn) {
  std::lock_guard<std::mutex> lk(subtitle_resolver_mutex());
  subtitle_resolver_fn() = std::move(fn);
}

bool FmvPlayer::has_native_wmv_decoder() {
#if defined(_WIN32) || defined(SHIFT_HAVE_FFMPEG_FMV)
  return true;
#else
  return false;
#endif
}

std::vector<FmvCatalogEntry> FmvPlayer::discover_movies(const std::filesystem::path& game_root,
                                                        const std::filesystem::path& state_root) {
  static constexpr std::pair<FmvMovieId, bool> kCatalog[] = {
      {FmvMovieId::kT10MsCombined, true},   {FmvMovieId::kMsgsLogo, true},
      {FmvMovieId::kDolbyCorona, true},     {FmvMovieId::kForzaIntro, true},
      {FmvMovieId::kForzaTone, true},       {FmvMovieId::kPressStart, false},
      {FmvMovieId::kFmv01Opening, false},   {FmvMovieId::kFmv02Ceremony, false},
      {FmvMovieId::kFmv04Credits, false},   {FmvMovieId::kWristbandGet, false},
      {FmvMovieId::kRallyBgLoop, false},    {FmvMovieId::kRallyForzaTone, true},
      {FmvMovieId::kClubIntro, true},
  };
  std::vector<FmvCatalogEntry> out;
  std::error_code ec;
  for (const auto& [id, splash] : kCatalog) {
    FmvCatalogEntry e;
    e.id = id;
    e.name = movie_label(id);
    e.is_boot_splash = splash;
    e.path = resolve_movie(game_root, id, state_root);
    e.exists = !e.path.empty() && std::filesystem::exists(e.path, ec);
    out.push_back(std::move(e));
  }
  return out;
}

std::filesystem::path FmvPlayer::resolve_movie(const std::filesystem::path& game_root, FmvMovieId id,
                                               const std::filesystem::path& state_root) {
  std::error_code ec;
  const std::filesystem::path rel = movie_relpath(id);
  const std::string stem = rel.stem().string();
  const std::string fname_low = lower_str(rel.filename().string());

  if (!game_root.empty()) {
    const auto vid_dir = game_root / "media" / "ui" / "videos";
    const auto mov_dir = game_root / "media" / "movies";
    // Prefer authentic disc WMV or BIK movie if present under media/ui/videos or media/movies.
    const std::filesystem::path candidates[] = {
        vid_dir / rel,
        mov_dir / (stem + ".bik"),
        vid_dir / (stem + ".bik"),
        mov_dir / rel.filename(),
    };
    for (const auto& c : candidates) {
      if (std::filesystem::exists(c, ec)) return c;
    }
    if (id == FmvMovieId::kWristbandGet) {
      const std::filesystem::path fbs[] = {
          vid_dir / "FMV_02.wmv",
          mov_dir / "FMV_02.bik",
          mov_dir / "WristbandGet.bik",
      };
      for (const auto& fb : fbs) {
        if (std::filesystem::exists(fb, ec)) return fb;
      }
    }
  }

  // Search extracted DLC packages under `state_root` (Rally & 1000 Club intro/background WMVs).
  auto scan_dlc_root = [&](const std::filesystem::path& dlc_root) -> std::filesystem::path {
    if (dlc_root.empty() || !std::filesystem::is_directory(dlc_root, ec)) return {};
    for (const auto& pkg : std::filesystem::directory_iterator(dlc_root, ec)) {
      if (!pkg.is_directory(ec)) continue;
      const auto dlczips = pkg.path() / "Media" / "DLCZips";
      if (!std::filesystem::is_directory(dlczips, ec)) continue;
      for (const auto& zdir : std::filesystem::directory_iterator(dlczips, ec)) {
        if (!zdir.is_directory(ec)) continue;
        const auto vdir = zdir.path() / "Media" / "UI" / "Videos";
        const std::filesystem::path direct[] = {
            vdir / rel,
            vdir / "Splash_Intros" / rel.filename(),
            vdir / rel.filename(),
        };
        for (const auto& c : direct) {
          if (std::filesystem::exists(c, ec)) return c;
        }
        // Case-insensitive scan inside Splash_Intros when running on case-sensitive filesystems.
        const auto sdir = vdir / "Splash_Intros";
        if (std::filesystem::is_directory(sdir, ec)) {
          for (const auto& f : std::filesystem::directory_iterator(sdir, ec)) {
            if (lower_str(f.path().filename().string()) == fname_low) return f.path();
          }
        }
      }
    }
    return {};
  };

  if (!state_root.empty()) {
    const std::filesystem::path dlc_roots[] = {
        state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002",
        state_root / "00000002",
        state_root,
    };
    for (const auto& r : dlc_roots) {
      if (auto hit = scan_dlc_root(r); !hit.empty()) return hit;
    }
  }
  return {};
}

bool FmvPlayer::probe(const std::filesystem::path& wmv_path, FmvProbeInfo& out) {
  out = FmvProbeInfo{};
  std::error_code ec;
  if (wmv_path.empty() || !std::filesystem::exists(wmv_path, ec)) return false;
#ifdef _WIN32
  if (is_wmv_path(wmv_path) && mf_probe_wmv(wmv_path, out)) return true;
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
  if (ff_probe_wmv(wmv_path, out)) return true;
#endif
  const auto ffmpeg = audio::find_ffmpeg();
  const auto ffprobe = find_ffprobe_for(ffmpeg);
  if (ffprobe.empty()) return false;
  const std::string cmd = quote_arg(ffprobe.string()) +
                          " -v quiet -show_entries stream=codec_type,width,height,r_frame_rate,duration -of default=nw=1 " +
                          quote_arg(wmv_path.string());
  PipeProcess pipe = open_pipe_read(cmd);
  if (!pipe.stream) return false;
  std::string text;
  char buf[512];
  while (size_t n = std::fread(buf, 1, sizeof buf, pipe.stream)) text.append(buf, n);
  close_pipe(pipe, false);

  std::istringstream ss(text);
  std::string line;
  std::string cur_type;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);
    if (k == "codec_type") {
      cur_type = v;
      if (cur_type == "audio") {
        out.has_audio = true;
        ++out.audio_streams;
      }
    } else if (cur_type == "video") {
      if (k == "width") out.width = std::atoi(v.c_str());
      else if (k == "height") out.height = std::atoi(v.c_str());
      else if (k == "duration") {
        const double d = std::strtod(v.c_str(), nullptr);
        if (d > out.duration_s) out.duration_s = d;
      } else if (k == "r_frame_rate") {
        const auto sl = v.find('/');
        if (sl != std::string::npos) {
          const double num = std::strtod(v.substr(0, sl).c_str(), nullptr);
          const double den = std::strtod(v.substr(sl + 1).c_str(), nullptr);
          if (num > 0.0 && den > 0.0) out.fps = num / den;
        } else {
          const double f = std::strtod(v.c_str(), nullptr);
          if (f > 0.0) out.fps = f;
        }
      }
    }
  }
  return out.width > 0 && out.height > 0;
}

bool FmvPlayer::decode_frame_at(const std::filesystem::path& wmv_path, double time_s, int target_w, int target_h,
                                std::vector<uint8_t>& out_rgba, std::string* error) {
  std::error_code ec;
  if (wmv_path.empty() || !std::filesystem::exists(wmv_path, ec)) {
    if (error) *error = "FMV file not found: " + wmv_path.string();
    return false;
  }
  target_w = std::max(16, target_w);
  target_h = std::max(16, target_h);
#ifdef _WIN32
  if (is_wmv_path(wmv_path) && mf_decode_frame_wmv(wmv_path, time_s, target_w, target_h, out_rgba, error)) {
    return true;
  }
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
  if (ff_decode_frame_wmv(wmv_path, time_s, target_w, target_h, out_rgba, error)) {
    return true;
  }
#endif
  const auto ffmpeg = audio::find_ffmpeg();
  if (ffmpeg.empty()) {
    if (error) *error = "ffmpeg executable not found";
    return false;
  }
  char tbuf[32];
  std::snprintf(tbuf, sizeof tbuf, "%.3f", std::max(0.0, time_s));
  const std::string cmd = quote_arg(ffmpeg.string()) + " -hide_banner -nostdin -v quiet -ss " + tbuf + " -i " +
                          quote_arg(wmv_path.string()) + " -map 0:v:0 -vframes 1 -vf scale=" +
                          std::to_string(target_w) + ":" + std::to_string(target_h) + " -pix_fmt rgba -f rawvideo pipe:1";
  PipeProcess pipe = open_pipe_read(cmd);
  if (!pipe.stream) {
    if (error) *error = "failed to launch ffmpeg";
    return false;
  }
  const size_t need = size_t(target_w) * size_t(target_h) * 4;
  out_rgba.resize(need);
  size_t got = 0;
  while (got < need) {
    const size_t n = std::fread(out_rgba.data() + got, 1, need - got, pipe.stream);
    if (n == 0) break;
    got += n;
  }
  close_pipe(pipe, got < need);
  if (got < need) {
    out_rgba.clear();
    if (error) *error = "ffmpeg returned truncated frame (" + std::to_string(got) + "/" + std::to_string(need) + ")";
    return false;
  }
  return true;
}

bool FmvPlayer::open(const std::filesystem::path& wmv_path, std::string_view language, int target_w, int target_h,
                     std::string* error, bool loop) {
  close();
  std::error_code ec;
  if (wmv_path.empty() || !std::filesystem::exists(wmv_path, ec)) {
    if (error) *error = "FMV not found: " + wmv_path.string();
    return false;
  }

#ifdef _WIN32
  const bool use_mf = is_wmv_path(wmv_path);
  const bool use_inproc_ff = false;
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
  const bool use_mf = false;
  const bool use_inproc_ff = true;
#else
  const bool use_mf = false;
  const bool use_inproc_ff = false;
#endif
  std::filesystem::path ffmpeg;
  if (!use_mf && !use_inproc_ff) {
    ffmpeg = audio::find_ffmpeg();
    if (ffmpeg.empty()) {
      if (error) *error = "ffmpeg not found";
      return false;
    }
  }

  FmvProbeInfo info;
  const bool probed = probe(wmv_path, info);
  width_ = std::max(16, target_w);
  height_ = std::max(16, target_h);
  fps_ = probed && info.fps > 1.0 && info.fps <= 120.0 ? info.fps : 24.0;
  duration_s_ = probed && info.duration_s > 0.0 ? info.duration_s : 0.0;
  title_ = wmv_path.stem().string();

  // Companion .def for localized ASF audio stream ID selection (e.g. id="4" or id="13").
  FmvStreamDef def;
  auto def_path = wmv_path;
  def_path.replace_extension(".def");
  if (std::filesystem::exists(def_path, ec)) parse_fmv_def(read_text_file(def_path), def);
  const int audio_stream_id = def.audio_stream_id_for(language);
  const int video_stream_id = def.video_id;

  // Companion subtitles folder: <stem_lower>_subtitles/<LANG>.xml (fallback EN.xml).
  subtitles_.clear();
  std::string stem_low = wmv_path.stem().string();
  for (char& c : stem_low) c = char(std::tolower(static_cast<unsigned char>(c)));
  const auto sub_dir = wmv_path.parent_path() / (stem_low + "_subtitles");
  if (std::filesystem::is_directory(sub_dir, ec)) {
    const std::string lang = upper_str(language.empty() ? "EN" : language);
    auto sub_file = sub_dir / (lang + ".xml");
    if (!std::filesystem::exists(sub_file, ec)) sub_file = sub_dir / "EN.xml";
    if (std::filesystem::exists(sub_file, ec)) parse_fmv_subtitles(read_text_file(sub_file), subtitles_);
  }

  current_frame_.assign(size_t(width_) * size_t(height_) * 4, 0);
  for (size_t i = 3; i < current_frame_.size(); i += 4) current_frame_[i] = 255;

  audio_ring_.clear();
  stop_.store(false, std::memory_order_release);
  video_eof_.store(false, std::memory_order_release);
  audio_eof_.store(false, std::memory_order_release);
  audio_frames_played_.store(0, std::memory_order_release);
  clock_s_ = 0.0;
  finished_ = false;
  paused_.store(false, std::memory_order_release);
  synthetic_ = false;
  frame_serial_ = 1;
  uploaded_serial_ = 0;
  active_.store(true, std::memory_order_release);

#ifdef _WIN32
  if (use_mf) {
    if (loop || (probed && !info.has_audio)) audio_eof_.store(true, std::memory_order_release);
    video_thread_ =
        std::thread(&FmvPlayer::mf_worker, this, wmv_path, video_stream_id, audio_stream_id, width_, height_, loop);
    return true;
  }
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
  if (use_inproc_ff) {
    if (loop || (probed && !info.has_audio)) audio_eof_.store(true, std::memory_order_release);
    video_thread_ =
        std::thread(&FmvPlayer::ff_worker, this, wmv_path, video_stream_id, audio_stream_id, width_, height_, loop);
    return true;
  }
#else
  (void)video_stream_id;
#endif

  const std::string loop_flag = loop ? " -stream_loop -1" : "";
  const std::string vcmd = quote_arg(ffmpeg.string()) + " -hide_banner -nostdin -v quiet" + loop_flag + " -i " +
                           quote_arg(wmv_path.string()) + " -map 0:v:0 -vf scale=" + std::to_string(width_) + ":" +
                           std::to_string(height_) + " -pix_fmt rgba -f rawvideo pipe:1";
  video_thread_ = std::thread(&FmvPlayer::video_worker, this, vcmd, width_, height_, fps_);

  if (!loop && (!probed || info.has_audio)) {
    const std::string audio_map =
        (audio_stream_id >= 0) ? ("0:i:" + std::to_string(audio_stream_id) + "?") : std::string("0:a:0?");
    const std::string acmd = quote_arg(ffmpeg.string()) + " -hide_banner -nostdin -v quiet -i " +
                             quote_arg(wmv_path.string()) + " -map " + audio_map + " -ac 2 -ar " +
                             std::to_string(kAudioRate) + " -f f32le pipe:1";
    audio_thread_ = std::thread(&FmvPlayer::audio_worker, this, acmd);
  } else {
    audio_eof_.store(true, std::memory_order_release);
  }
  return true;
}

bool FmvPlayer::open_or_fallback(const std::filesystem::path& game_root, FmvMovieId id, std::string_view language,
                                 int target_w, int target_h, double fallback_duration_s,
                                 const std::filesystem::path& state_root) {
  movie_id_ = id;
  const auto path = resolve_movie(game_root, id, state_root);
  if (!path.empty() && open(path, language, target_w, target_h, nullptr)) {
    movie_id_ = id;
    return true;
  }
  open_synthetic(std::max(320, target_w), std::max(180, target_h), 30.0, std::max(1.5, fallback_duration_s),
                 movie_label(id));
  movie_id_ = id;
  return true;
}

void FmvPlayer::open_synthetic(int w, int h, double fps, double duration_s, std::string title) {
  close();
  width_ = std::max(16, w);
  height_ = std::max(16, h);
  fps_ = std::clamp(fps, 1.0, 120.0);
  duration_s_ = std::max(0.1, duration_s);
  title_ = std::move(title);
  clock_s_ = 0.0;
  finished_ = false;
  paused_.store(false, std::memory_order_release);
  synthetic_ = true;
  current_frame_.resize(size_t(width_) * size_t(height_) * 4);
  render_synthetic_frame(0.0, current_frame_);
  frame_serial_ = 1;
  uploaded_serial_ = 0;
  active_.store(true, std::memory_order_release);
}

void FmvPlayer::close() {
  active_.store(false, std::memory_order_release);
  stop_.store(true, std::memory_order_release);
  if (video_thread_.joinable()) video_thread_.join();
  if (audio_thread_.joinable()) audio_thread_.join();
  {
    std::lock_guard<std::mutex> lk(video_mu_);
    video_q_.clear();
  }
  audio_ring_.clear();
  finished_ = false;
  paused_.store(false, std::memory_order_release);
  synthetic_ = false;
  subtitles_.clear();
}

void FmvPlayer::skip() {
  if (!active_.load(std::memory_order_acquire)) return;
  close();
  finished_ = true;
}

void FmvPlayer::push_audio_stereo(const float* stereo, size_t n_floats) {
  if (!stereo || n_floats == 0) return;
  size_t off = 0;
  while (off < n_floats && !stop_.load(std::memory_order_acquire)) {
    const size_t wrote = audio_ring_.write(stereo + off, n_floats - off);
    off += wrote;
    if (off < n_floats) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

int FmvPlayer::current_subtitle_index() const {
  for (size_t i = 0; i < subtitles_.size(); ++i) {
    const double start = subtitles_[i].start_s;
    const double end = (i + 1 < subtitles_.size())
                           ? std::min(subtitles_[i + 1].start_s, start + 6.5)
                           : (start + 5.5);
    if (clock_s_ >= start && clock_s_ < end) return subtitles_[i].index;
  }
  return -1;
}

std::string FmvPlayer::current_subtitle_text() const {
  if (!active_.load(std::memory_order_acquire)) return {};
  int idx = current_subtitle_index();
  if (idx < 0 && synthetic_) {
    idx = int(clock_s_ / 3.0);
  }
  if (idx < 0) return {};

  const char* prefix = nullptr;
  if (movie_id_ == FmvMovieId::kFmv01Opening || title_ == "FMV_01") prefix = "IDS_FMV_01_";
  else if (movie_id_ == FmvMovieId::kFmv02Ceremony || movie_id_ == FmvMovieId::kWristbandGet || title_ == "FMV_02")
    prefix = "IDS_FMV_02_";
  else if (movie_id_ == FmvMovieId::kFmv04Credits || title_ == "FMV_04") prefix = "IDS_FMV_04_";
  else return {};

  // 1. Resolve authentic localized subtitle from `Subtitles.str` (e.g. IDS_FMV_01_0..17).
  SubtitleResolver fn;
  {
    std::lock_guard<std::mutex> lk(subtitle_resolver_mutex());
    fn = subtitle_resolver_fn();
  }
  if (fn) {
    const std::string key = std::string(prefix) + std::to_string(idx);
    const uint32_t packed = (uint32_t(fh1::string_hash("Subtitles")) << 16) | uint32_t(fh1::string_hash(key));
    const std::string ref = "_&" + std::to_string(packed);
    const std::string resolved = fn(ref);
    if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) {
      return resolved;
    }
  }

  // 2. Synthetic / fixture fallback when no StringTables are loaded.
  static const char* kFmv01Lines[] = {
      "SCOTT TYLER: Welcome to Colorado — the Horizon Festival is officially underway!",
      "ALICE HART: Cars, music, and the greatest open roads in America. Only 250 festival passes remain!",
      "SCOTT TYLER: Grab your keys and get to the Horizon Festival entrance before the gates close!",
  };
  static const char* kFmv02Lines[] = {
      "ALICE HART: You made it to Horizon — this wristband opens the roads of Colorado.",
      "ALICE HART: Win races, earn popularity, and climb all the way to Darius Flynt's Horizon Finale!",
  };
  static const char* kFmv04Lines[] = {
      "ALICE HART: Colorado has a new Horizon Champion — thank you for making this the greatest Festival ever!",
      "SCOTT TYLER: Turn up the radio and keep driving — the roads of Horizon Colorado are yours forever!",
  };
  if (movie_id_ == FmvMovieId::kFmv01Opening || title_ == "FMV_01") {
    return kFmv01Lines[size_t(idx) % std::size(kFmv01Lines)];
  }
  if (movie_id_ == FmvMovieId::kFmv02Ceremony || movie_id_ == FmvMovieId::kWristbandGet || title_ == "FMV_02") {
    return kFmv02Lines[size_t(idx) % std::size(kFmv02Lines)];
  }
  return kFmv04Lines[size_t(idx) % std::size(kFmv04Lines)];
}

#ifdef _WIN32
void FmvPlayer::mf_worker(std::filesystem::path wmv_path, int target_video_id, int target_audio_id, int target_w,
                          int target_h, bool loop) {
  MfSessionGuard guard;
  if (!guard.mf_ok) {
    video_eof_.store(true, std::memory_order_release);
    audio_eof_.store(true, std::memory_order_release);
    return;
  }
  IMFSourceReader* reader = nullptr;
  if (!mf_create_reader(wmv_path, &reader)) {
    video_eof_.store(true, std::memory_order_release);
    audio_eof_.store(true, std::memory_order_release);
    return;
  }

  reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);

  // Query each stream's ASF stream identifier (matches `<Audio id="..."/>` / `<Video id="..."/>` in `.def`).
  std::vector<DWORD> asf_stream_ids;
  IMFMediaSource* media_src = nullptr;
  if (SUCCEEDED(reader->GetServiceForStream(MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&media_src))) &&
      media_src) {
    IMFPresentationDescriptor* pd = nullptr;
    if (SUCCEEDED(media_src->CreatePresentationDescriptor(&pd)) && pd) {
      DWORD sd_count = 0;
      if (SUCCEEDED(pd->GetStreamDescriptorCount(&sd_count))) {
        asf_stream_ids.assign(sd_count, DWORD(-1));
        for (DWORD i = 0; i < sd_count; ++i) {
          BOOL sel = FALSE;
          IMFStreamDescriptor* sd = nullptr;
          if (SUCCEEDED(pd->GetStreamDescriptorByIndex(i, &sel, &sd)) && sd) {
            DWORD sid = DWORD(-1);
            if (SUCCEEDED(sd->GetStreamIdentifier(&sid))) asf_stream_ids[i] = sid;
            sd->Release();
          }
        }
      }
      pd->Release();
    }
    media_src->Release();
  }

  DWORD video_stream_idx = DWORD(-1);
  DWORD audio_stream_idx = DWORD(-1);
  DWORD matched_audio_idx = DWORD(-1);
  for (DWORD s = 0; s < 64; ++s) {
    IMFMediaType* st = nullptr;
    const HRESULT hr = reader->GetNativeMediaType(s, 0, &st);
    if (hr == MF_E_INVALIDSTREAMNUMBER) break;
    if (SUCCEEDED(hr) && st) {
      GUID major{};
      if (SUCCEEDED(st->GetMajorType(&major))) {
        const DWORD asf_id = (s < asf_stream_ids.size()) ? asf_stream_ids[s] : DWORD(-1);
        if (major == MFMediaType_Video) {
          if (video_stream_idx == DWORD(-1) || (target_video_id >= 0 && asf_id == DWORD(target_video_id))) {
            video_stream_idx = s;
          }
        } else if (major == MFMediaType_Audio) {
          if (audio_stream_idx == DWORD(-1)) audio_stream_idx = s;
          if (target_audio_id >= 0 && asf_id == DWORD(target_audio_id)) matched_audio_idx = s;
        }
      }
      st->Release();
    }
  }
  if (matched_audio_idx != DWORD(-1)) audio_stream_idx = matched_audio_idx;
  if (video_stream_idx == DWORD(-1)) {
    reader->Release();
    video_eof_.store(true, std::memory_order_release);
    audio_eof_.store(true, std::memory_order_release);
    return;
  }
  reader->SetStreamSelection(video_stream_idx, TRUE);

  IMFMediaType* rgb_type = nullptr;
  if (SUCCEEDED(MFCreateMediaType(&rgb_type))) {
    rgb_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    reader->SetCurrentMediaType(video_stream_idx, nullptr, rgb_type);
    rgb_type->Release();
  }

  int src_w = target_w, src_h = target_h, stride = target_w * 4;
  IMFMediaType* vcur = nullptr;
  if (SUCCEEDED(reader->GetCurrentMediaType(video_stream_idx, &vcur)) && vcur) {
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(MFGetAttributeSize(vcur, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
      src_w = int(w);
      src_h = int(h);
    }
    UINT32 st_u32 = 0;
    if (SUCCEEDED(vcur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st_u32))) {
      stride = int(static_cast<int32_t>(st_u32));
    } else {
      stride = src_w * 4;
    }
    vcur->Release();
  }

  bool have_audio = false;
  int audio_out_channels = 2;
  if (!loop && audio_stream_idx != DWORD(-1)) {
    reader->SetStreamSelection(audio_stream_idx, TRUE);
    IMFMediaType* pcm_type = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&pcm_type))) {
      pcm_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      pcm_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
      pcm_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
      pcm_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
      pcm_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
      pcm_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
      pcm_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * 8);
      if (SUCCEEDED(reader->SetCurrentMediaType(audio_stream_idx, nullptr, pcm_type))) {
        have_audio = true;
        audio_out_channels = 2;
      } else {
        // Fallback for 5.1 WMA Pro streams: decode float at native channel count and downmix in-process.
        IMFMediaType* native_mt = nullptr;
        UINT32 native_ch = 2;
        if (SUCCEEDED(reader->GetNativeMediaType(audio_stream_idx, 0, &native_mt)) && native_mt) {
          native_mt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &native_ch);
          native_mt->Release();
        }
        native_ch = std::clamp<UINT32>(native_ch, 1u, 8u);
        pcm_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, native_ch);
        pcm_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, native_ch * 4u);
        pcm_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * native_ch * 4u);
        if (SUCCEEDED(reader->SetCurrentMediaType(audio_stream_idx, nullptr, pcm_type))) {
          have_audio = true;
          audio_out_channels = int(native_ch);
        } else {
          reader->SetStreamSelection(audio_stream_idx, FALSE);
        }
      }
      pcm_type->Release();
    }
  }
  if (!have_audio) audio_eof_.store(true, std::memory_order_release);

  const size_t dst_bytes = size_t(target_w) * size_t(target_h) * 4;
  std::vector<float> downmix_scratch;
  double pts_offset = 0.0;
  double last_pts = 0.0;
  bool v_eof = false;
  bool a_eof = !have_audio;

  while (!stop_.load(std::memory_order_acquire)) {
    bool v_full = false;
    {
      std::lock_guard<std::mutex> lk(video_mu_);
      v_full = video_q_.size() >= 6;
    }
    const bool a_full = !have_audio || (audio_ring_.available_read() >= size_t(kAudioRate) * 2);
    if (v_full && a_full) {
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      continue;
    }

    // Prefer reading the stream whose queue has room so neither starves.
    DWORD want_stream = MF_SOURCE_READER_ANY_STREAM;
    if (v_full && !a_full && !a_eof) want_stream = audio_stream_idx;
    else if (a_full && !v_full && !v_eof) want_stream = video_stream_idx;

    DWORD actual_stream = 0;
    DWORD flags = 0;
    LONGLONG ts = 0;
    IMFSample* sample = nullptr;
    const HRESULT hr = reader->ReadSample(want_stream, 0, &actual_stream, &flags, &ts, &sample);
    if (FAILED(hr)) {
      if (sample) sample->Release();
      break;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      if (actual_stream == video_stream_idx) {
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType(video_stream_idx, &cur)) && cur) {
          UINT32 w = 0, h = 0;
          if (SUCCEEDED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
            src_w = int(w);
            src_h = int(h);
          }
          UINT32 st_u32 = 0;
          if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st_u32))) {
            stride = int(static_cast<int32_t>(st_u32));
          } else {
            stride = src_w * 4;
          }
          cur->Release();
        }
      } else if (actual_stream == audio_stream_idx) {
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType(audio_stream_idx, &cur)) && cur) {
          UINT32 ch_u32 = 2;
          if (SUCCEEDED(cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch_u32)) && ch_u32 > 0) {
            audio_out_channels = int(ch_u32);
          }
          cur->Release();
        }
      }
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      if (sample) sample->Release();
      if (actual_stream == video_stream_idx) v_eof = true;
      else if (actual_stream == audio_stream_idx) a_eof = true;
      else { v_eof = true; a_eof = true; }
      if (v_eof && a_eof) {
        if (loop) {
          PROPVARIANT pos;
          PropVariantInit(&pos);
          pos.vt = VT_I8;
          pos.hVal.QuadPart = 0;
          if (SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, pos))) {
            PropVariantClear(&pos);
            pts_offset = last_pts + (1.0 / std::max(1.0, fps_));
            v_eof = false;
            a_eof = !have_audio;
            continue;
          }
          PropVariantClear(&pos);
        }
        break;
      }
      continue;
    }
    if (!sample) continue;

    IMFMediaBuffer* buf = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
      BYTE* data = nullptr;
      DWORD cur_len = 0;
      if (SUCCEEDED(buf->Lock(&data, nullptr, &cur_len)) && data && cur_len > 0) {
        if (actual_stream == video_stream_idx && cur_len >= DWORD(std::abs(stride) * src_h)) {
          VideoFrame vf;
          const double raw_pts = std::max(0.0, double(ts) * 1e-7);
          vf.pts = pts_offset + raw_pts;
          last_pts = std::max(last_pts, vf.pts);
          vf.rgba.resize(dst_bytes);
          convert_bgra_to_rgba_scaled(data, src_w, src_h, stride, vf.rgba.data(), target_w, target_h);
          std::lock_guard<std::mutex> lk(video_mu_);
          video_q_.push_back(std::move(vf));
        } else if (have_audio && actual_stream == audio_stream_idx) {
          const size_t n_floats = cur_len / sizeof(float);
          const float* fptr = reinterpret_cast<const float*>(data);
          if (audio_out_channels == 2) {
            push_audio_stereo(fptr, n_floats);
          } else {
            const size_t frames = n_floats / size_t(std::max(1, audio_out_channels));
            downmix_scratch.resize(frames * 2);
            downmix_interleaved_to_stereo(fptr, audio_out_channels, frames, downmix_scratch.data());
            push_audio_stereo(downmix_scratch.data(), downmix_scratch.size());
          }
        }
        buf->Unlock();
      }
      buf->Release();
    }
    sample->Release();
  }

  reader->Release();
  video_eof_.store(true, std::memory_order_release);
  audio_eof_.store(true, std::memory_order_release);
}
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
void FmvPlayer::ff_worker(std::filesystem::path wmv_path, int target_video_id, int target_audio_id, int target_w,
                          int target_h, bool loop) {
  av_log_set_level(AV_LOG_QUIET);
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, wmv_path.string().c_str(), nullptr, nullptr) < 0 || !fmt ||
      avformat_find_stream_info(fmt, nullptr) < 0) {
    if (fmt) avformat_close_input(&fmt);
    video_eof_.store(true, std::memory_order_release);
    audio_eof_.store(true, std::memory_order_release);
    return;
  }

  int vidx = -1, aidx = -1, matched_aidx = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    const AVStream* st = fmt->streams[i];
    if (!st || !st->codecpar) continue;
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (vidx < 0 || (target_video_id >= 0 && st->id == target_video_id)) vidx = int(i);
    } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      if (aidx < 0) aidx = int(i);
      if (target_audio_id >= 0 && st->id == target_audio_id) matched_aidx = int(i);
    }
  }
  if (matched_aidx >= 0) aidx = matched_aidx;

  AVCodecContext* vctx = nullptr;
  if (vidx >= 0) {
    if (const AVCodec* vdec = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id)) {
      vctx = avcodec_alloc_context3(vdec);
      if (!vctx || avcodec_parameters_to_context(vctx, fmt->streams[vidx]->codecpar) < 0 ||
          avcodec_open2(vctx, vdec, nullptr) < 0) {
        if (vctx) avcodec_free_context(&vctx);
      }
    }
  }
  AVCodecContext* actx = nullptr;
  if (!loop && aidx >= 0) {
    if (const AVCodec* adec = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id)) {
      actx = avcodec_alloc_context3(adec);
      if (!actx || avcodec_parameters_to_context(actx, fmt->streams[aidx]->codecpar) < 0 ||
          avcodec_open2(actx, adec, nullptr) < 0) {
        if (actx) avcodec_free_context(&actx);
      }
    }
  }
  if (!actx) audio_eof_.store(true, std::memory_order_release);

  AVPacket* pkt = av_packet_alloc();
  AVFrame* fr = av_frame_alloc();
  std::vector<float> inter_buf;
  std::vector<float> stereo_buf;
  std::vector<float> resamp_buf;
  const size_t dst_bytes = size_t(target_w) * size_t(target_h) * 4;
  double pts_offset = 0.0;
  double last_pts = 0.0;

  auto emit_audio_frame = [&](const AVFrame* af) {
    const int nb = af->nb_samples;
    const int ch = std::max(1, af->channels);
    const int sr = af->sample_rate > 0 ? af->sample_rate : kAudioRate;
    if (nb <= 0) return;
    inter_buf.resize(size_t(nb) * size_t(ch));
    if (af->format == AV_SAMPLE_FMT_FLTP) {
      for (int i = 0; i < nb; ++i)
        for (int c = 0; c < ch; ++c)
          inter_buf[size_t(i) * size_t(ch) + size_t(c)] =
              reinterpret_cast<const float*>(af->extended_data[c])[i];
    } else if (af->format == AV_SAMPLE_FMT_FLT) {
      std::memcpy(inter_buf.data(), af->data[0], inter_buf.size() * sizeof(float));
    } else if (af->format == AV_SAMPLE_FMT_S16P) {
      for (int i = 0; i < nb; ++i)
        for (int c = 0; c < ch; ++c)
          inter_buf[size_t(i) * size_t(ch) + size_t(c)] =
              float(reinterpret_cast<const int16_t*>(af->extended_data[c])[i]) / 32768.0f;
    } else if (af->format == AV_SAMPLE_FMT_S16) {
      const auto* src = reinterpret_cast<const int16_t*>(af->data[0]);
      for (size_t i = 0; i < inter_buf.size(); ++i) inter_buf[i] = float(src[i]) / 32768.0f;
    } else {
      std::fill(inter_buf.begin(), inter_buf.end(), 0.0f);
    }
    stereo_buf.resize(size_t(nb) * 2);
    downmix_interleaved_to_stereo(inter_buf.data(), ch, size_t(nb), stereo_buf.data());
    if (sr == kAudioRate) {
      push_audio_stereo(stereo_buf.data(), stereo_buf.size());
    } else {
      const size_t out_n = std::max<size_t>(1, size_t(std::llround(double(nb) * double(kAudioRate) / double(sr))));
      resamp_buf.resize(out_n * 2);
      const double step = double(nb) / double(out_n);
      for (size_t i = 0; i < out_n; ++i) {
        const double pos = double(i) * step;
        const size_t k0 = std::min(size_t(nb - 1), size_t(pos));
        const size_t k1 = std::min(size_t(nb - 1), k0 + 1);
        const float frac = float(pos - double(k0));
        resamp_buf[2 * i] = stereo_buf[2 * k0] + (stereo_buf[2 * k1] - stereo_buf[2 * k0]) * frac;
        resamp_buf[2 * i + 1] = stereo_buf[2 * k0 + 1] + (stereo_buf[2 * k1 + 1] - stereo_buf[2 * k0 + 1]) * frac;
      }
      push_audio_stereo(resamp_buf.data(), resamp_buf.size());
    }
  };

  while (vctx && pkt && fr && !stop_.load(std::memory_order_acquire)) {
    bool v_full = false;
    {
      std::lock_guard<std::mutex> lk(video_mu_);
      v_full = video_q_.size() >= 6;
    }
    const bool a_full = !actx || (audio_ring_.available_read() >= size_t(kAudioRate) * 2);
    if (v_full && a_full) {
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      continue;
    }
    if (av_read_frame(fmt, pkt) < 0) {
      if (loop) {
        if (av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
          avcodec_flush_buffers(vctx);
          if (actx) avcodec_flush_buffers(actx);
          pts_offset = last_pts + (1.0 / std::max(1.0, fps_));
          continue;
        }
      }
      break;
    }
    if (pkt->stream_index == vidx) {
      if (avcodec_send_packet(vctx, pkt) >= 0) {
        while (avcodec_receive_frame(vctx, fr) == 0) {
          VideoFrame vf;
          const int64_t best_ts = (fr->best_effort_timestamp != AV_NOPTS_VALUE) ? fr->best_effort_timestamp : 0;
          const double raw_pts = std::max(0.0, double(best_ts) * av_q2d(fmt->streams[vidx]->time_base));
          vf.pts = pts_offset + raw_pts;
          last_pts = std::max(last_pts, vf.pts);
          vf.rgba.resize(dst_bytes);
          convert_yuv420p_to_rgba_scaled(fr, vf.rgba.data(), target_w, target_h);
          std::lock_guard<std::mutex> lk(video_mu_);
          video_q_.push_back(std::move(vf));
        }
      }
    } else if (actx && pkt->stream_index == aidx) {
      if (avcodec_send_packet(actx, pkt) >= 0) {
        while (avcodec_receive_frame(actx, fr) == 0) emit_audio_frame(fr);
      }
    }
    av_packet_unref(pkt);
  }

  if (fr) av_frame_free(&fr);
  if (pkt) av_packet_free(&pkt);
  if (actx) avcodec_free_context(&actx);
  if (vctx) avcodec_free_context(&vctx);
  avformat_close_input(&fmt);
  video_eof_.store(true, std::memory_order_release);
  audio_eof_.store(true, std::memory_order_release);
}
#endif

void FmvPlayer::video_worker(std::string cmd, int w, int h, double fps) {
  PipeProcess pipe = open_pipe_read(cmd);
  if (!pipe.stream) {
    video_eof_.store(true, std::memory_order_release);
    return;
  }
  const size_t frame_bytes = size_t(w) * size_t(h) * 4;
  int64_t idx = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lk(video_mu_);
      if (video_q_.size() >= 6) {
        // Yield briefly while player consumes frames.
        goto wait_slot;
      }
    }
    {
      VideoFrame vf;
      vf.pts = double(idx) / fps;
      vf.rgba.resize(frame_bytes);
      size_t got = 0;
      while (got < frame_bytes && !stop_.load(std::memory_order_acquire)) {
        const size_t n = std::fread(vf.rgba.data() + got, 1, frame_bytes - got, pipe.stream);
        if (n == 0) break;
        got += n;
      }
      if (got < frame_bytes) break;
      {
        std::lock_guard<std::mutex> lk(video_mu_);
        video_q_.push_back(std::move(vf));
      }
      ++idx;
      continue;
    }
  wait_slot:
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  close_pipe(pipe, stop_.load(std::memory_order_acquire));
  video_eof_.store(true, std::memory_order_release);
}

void FmvPlayer::audio_worker(std::string cmd) {
  PipeProcess pipe = open_pipe_read(cmd);
  if (!pipe.stream) {
    audio_eof_.store(true, std::memory_order_release);
    return;
  }
  constexpr size_t kChunkFloats = 2048 * 2;
  float buf[kChunkFloats];
  while (!stop_.load(std::memory_order_acquire)) {
    if (audio_ring_.available_read() >= size_t(kAudioRate) * 2 || audio_ring_.available_write() < kChunkFloats) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    const size_t n = std::fread(buf, sizeof(float), kChunkFloats, pipe.stream);
    if (n == 0) break;
    push_audio_stereo(buf, n);
  }
  close_pipe(pipe, stop_.load(std::memory_order_acquire));
  audio_eof_.store(true, std::memory_order_release);
}

void FmvPlayer::render_synthetic_frame(double t, std::vector<uint8_t>& rgba) const {
  const float phase = float(std::fmod(t * 0.5, 1.0));
  for (int y = 0; y < height_; ++y) {
    const float v = float(y) / float(std::max(1, height_ - 1));
    for (int x = 0; x < width_; ++x) {
      const float u = float(x) / float(std::max(1, width_ - 1));
      const float band = 0.5f + 0.5f * std::sin((u * 6.28318f) + phase * 6.28318f);
      uint8_t* p = &rgba[(size_t(y) * size_t(width_) + size_t(x)) * 4];
      // Horizon sunset pink/amber gradient with moving scan band.
      p[0] = uint8_t(std::clamp(int((0.18f + 0.72f * (1.0f - v) + 0.10f * band) * 255.0f), 0, 255));
      p[1] = uint8_t(std::clamp(int((0.06f + 0.25f * (1.0f - v) * band) * 255.0f), 0, 255));
      p[2] = uint8_t(std::clamp(int((0.15f + 0.45f * v + 0.15f * band) * 255.0f), 0, 255));
      p[3] = 255;
    }
  }
}

bool FmvPlayer::update(double dt, bool audio_synced) {
  if (!active_.load(std::memory_order_acquire)) return false;
  if (paused_.load(std::memory_order_acquire)) return true;

  if (synthetic_) {
    clock_s_ += std::max(0.0, dt);
    if (clock_s_ >= duration_s_) {
      close();
      finished_ = true;
      return false;
    }
    render_synthetic_frame(clock_s_, current_frame_);
    ++frame_serial_;
    return true;
  }

  const int64_t played = audio_frames_played_.load(std::memory_order_acquire);
  if (audio_synced && played > 0 && !audio_eof_.load(std::memory_order_acquire)) {
    const double a_clock = double(played) / double(kAudioRate);
    // Keep clock tightly coupled to audio clock without jumping backwards.
    clock_s_ = std::max(clock_s_, a_clock);
  } else {
    clock_s_ += std::max(0.0, dt);
  }

  bool popped = false;
  bool q_empty = false;
  {
    std::lock_guard<std::mutex> lk(video_mu_);
    while (!video_q_.empty() && video_q_.front().pts <= clock_s_ + (0.5 / fps_)) {
      current_frame_ = std::move(video_q_.front().rgba);
      video_q_.pop_front();
      popped = true;
    }
    q_empty = video_q_.empty();
  }
  if (popped) ++frame_serial_;

  if (q_empty && video_eof_.load(std::memory_order_acquire)) {
    close();
    finished_ = true;
    return false;
  }
  return true;
}

int FmvPlayer::read_audio(float* out, int frames) {
  if (!out || frames <= 0 || !active_.load(std::memory_order_acquire) ||
      paused_.load(std::memory_order_acquire)) {
    return 0;
  }
  if (synthetic_) {
    const double base = double(audio_frames_played_.fetch_add(frames, std::memory_order_relaxed));
    for (int i = 0; i < frames; ++i) {
      const float s = 0.08f * std::sin(float((base + i) * 2.0 * 3.14159265 * 440.0 / kAudioRate));
      out[2 * i] = s;
      out[2 * i + 1] = s;
    }
    return frames;
  }
  // Lock-free SPSC read: safe on TIME_CRITICAL audio render callback with zero mutexes.
  const size_t avail_frames = audio_ring_.available_read() / 2;
  const int take = std::min(frames, int(avail_frames));
  if (take <= 0) return 0;
  const size_t got_floats = audio_ring_.read(out, size_t(take) * 2);
  const int got_frames = int(got_floats / 2);
  if (got_frames > 0) audio_frames_played_.fetch_add(got_frames, std::memory_order_relaxed);
  return got_frames;
}

void FmvPlayer::draw(ui::List& l, float screen_w, float screen_h, std::string_view subtitle_text,
                     bool show_skip_prompt, float hold_skip_progress) const {
  if (!active_.load(std::memory_order_acquire) || width_ <= 0 || height_ <= 0 ||
      current_frame_.size() < size_t(width_) * size_t(height_) * 4) {
    return;
  }
  // Letterbox background.
  l.rect(0, 0, screen_w, screen_h, ui::rgba(0, 0, 0, 255));

  std::array<float, 4> uv{};
  bool ok = false;
  if (frame_serial_ != uploaded_serial_) {
    ok = ui::atlas_update("fmv.frame", uint32_t(width_), uint32_t(height_), current_frame_, uv);
    if (ok) uploaded_serial_ = frame_serial_;
  } else {
    ok = ui::atlas_find("fmv.frame", uv);
  }
  if (ok) {
    const float video_aspect = float(width_) / float(height_);
    const float scr_aspect = screen_w / std::max(1.0f, screen_h);
    float draw_w = screen_w, draw_h = screen_h;
    if (scr_aspect > video_aspect) {
      draw_w = screen_h * video_aspect;
    } else {
      draw_h = screen_w / video_aspect;
    }
    const float x0 = 0.5f * (screen_w - draw_w);
    const float y0 = 0.5f * (screen_h - draw_h);
    l.image(x0, y0, x0 + draw_w, y0 + draw_h, uv);
  }

  const float s = screen_h / 720.0f;
  // Cinema letterbox bars & smooth fade-in / fade-out envelope.
  const float bar_h = 34.0f * s;
  l.rect(0, 0, screen_w, bar_h, ui::rgba(0, 0, 0, 235));
  l.rect(0, screen_h - bar_h, screen_w, screen_h, ui::rgba(0, 0, 0, 235));

  if (synthetic_ && !title_.empty()) {
    ui4::title_banner(l, s, 68.0f * s, 62.0f * s, title_, "HORIZON FESTIVAL CUTSCENE", true, 640.0f * s);
  }

  const std::string resolved_sub = subtitle_text.empty() ? current_subtitle_text() : std::string(subtitle_text);
  if (!resolved_sub.empty()) {
    const float pad_y = 602.0f * s;
    ui::TextStyle ts = ui4::style(ui::Font::kA, 17.5f * s, ui4::kWhite, ui::Align::kCenter, true);
    const float sw = std::clamp(ui::List::measure(resolved_sub, ts) + 56.0f * s, 320.0f * s, screen_w * 0.80f);
    // Sharp FH1 HUDPILL01 subtitle strip (ZERO 4px rounded boxes!).
    ui4::pill(l, 0.5f * (screen_w - sw), pad_y - 6.0f * s, 0.5f * (screen_w + sw), pad_y + 32.0f * s,
              ui4::kCharcoalDark, ui4::PillKind::kHud01);
    l.rect(0.5f * (screen_w - sw), pad_y - 6.0f * s, 0.5f * (screen_w - sw) + 4.0f * s, pad_y + 32.0f * s, ui4::kPink);
    l.text(screen_w * 0.5f, pad_y + 4.0f * s, resolved_sub, ts);
  }

  if (show_skip_prompt) {
    // Authentic BUTTON_BAR_SCENE skip prompt + optional hold-to-skip progress fill.
    const float hp = std::clamp(hold_skip_progress, 0.0f, 1.0f);
    ui4::button_bar(l, {{"A", hp > 0.01f ? "SKIPPING..." : "HOLD / PRESS TO SKIP"}}, screen_w, screen_h, 70.0f);
    if (hp > 0.01f) {
      const float bx0 = screen_w - 260.0f * s, bx1 = screen_w - 64.0f * s;
      const float by0 = screen_h - 36.0f * s, by1 = screen_h - 28.0f * s;
      l.rect(bx0, by0, bx1, by1, ui::rgba(255, 255, 255, 55));
      l.rect(bx0, by0, bx0 + (bx1 - bx0) * hp, by1, ui4::kPink);
    }
  }

  // Smooth fade-in and fade-out veil on full-length cutscenes (never blacks out t=0 thumbnails/tests).
  float fade = 0.0f;
  if (duration_s_ >= 3.0) {
    if (clock_s_ > 0.001 && clock_s_ < 0.55) fade = std::max(fade, float(1.0 - clock_s_ / 0.55));
    if (clock_s_ > duration_s_ - 0.55) {
      fade = std::max(fade, float((clock_s_ - (duration_s_ - 0.55)) / 0.55));
    }
  }
  fade = std::clamp(fade, 0.0f, 1.0f);
  if (fade > 0.01f) {
    l.rect(0, 0, screen_w, screen_h, ui::rgba(0, 0, 0, uint8_t(std::lround(fade * 255.0f))));
  }
}

}  // namespace carview
