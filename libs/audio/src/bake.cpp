#include "cars/mods.h"
#include "audio/bake.h"
#include "audio/xma_decode.h"
#include "fh1data/fs_walk.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace audio {
namespace {

int run_hidden_command(const std::string& command) {
#ifdef _WIN32
  std::string mutable_cmd = command;
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return int(exit_code);
#else
  return std::system(command.c_str());
#endif
}

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool write_file(const std::filesystem::path& p, const void* data, size_t size) {
  std::ofstream out(p, std::ios::binary);
  out.write(static_cast<const char*>(data), std::streamsize(size));
  return bool(out);
}

std::string pcm_name(const std::string& bank, int index, int channel) {
  return lower_name(bank) + "." + std::to_string(index) + ".c" + std::to_string(channel) + ".s16";
}

std::string quoted(const std::filesystem::path& p) { return "\"" + p.string() + "\""; }

}  // namespace

std::string baked_sound_key(const std::string& media, UpgradeLevels levels) {
  const int in_lv = std::clamp(levels.intake, 0, 3);
  const int ex_lv = std::clamp(levels.exhaust, 0, 3);
  if (in_lv == 0 && ex_lv == 0) return media;
  return media + "_I" + std::to_string(in_lv) + "_E" + std::to_string(ex_lv);
}

std::filesystem::path find_ffmpeg() {
  std::error_code ec;
  if (const char* env = std::getenv("SHIFT_FFMPEG"); env && *env && std::filesystem::exists(env, ec)) return env;
  const char* path = std::getenv("PATH");
#ifdef _WIN32
  const char sep = ';';
  const char* exe = "ffmpeg.exe";
#else
  const char sep = ':';
  const char* exe = "ffmpeg";
#endif
  if (path) {
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, sep))
      if (!dir.empty() && std::filesystem::exists(std::filesystem::path(dir) / exe, ec))
        return std::filesystem::path(dir) / exe;
  }
#ifdef _WIN32
  if (const char* local = std::getenv("LOCALAPPDATA")) {
    const std::filesystem::path packages = std::filesystem::path(local) / "Microsoft" / "WinGet" / "Packages";
    std::filesystem::path found;
    if (std::filesystem::is_directory(packages, ec))
      fh1::walk_tree(packages, [&](const std::filesystem::directory_entry& e) {
        if (e.path().filename() != exe) return true;
        found = fh1::usable_path(e.path());
        return false;
      });
    if (!found.empty()) return found;
  }
#endif
  return {};
}

bool bake_engine_sound(SoundFiles& files, const std::string& media, const std::filesystem::path& out_dir,
                       const std::filesystem::path& ffmpeg, BakeReport& report, std::string* error,
                       UpgradeLevels levels) {
  auto fail = [&](const std::string& what) {
    if (error) *error = media + ": " + what;
    return false;
  };
  LoadedSound loaded;
  std::string e;
  if (!load_engine_sound(files, media, loaded, &e, levels)) return fail(e);
  report.notes.insert(report.notes.end(), loaded.notes.begin(), loaded.notes.end());
  const std::string key = baked_sound_key(media, levels);
  const std::filesystem::path dir = out_dir / key;
  std::error_code ec;
  std::filesystem::create_directories(dir / "pcm", ec);
  std::ostringstream index;
  // The tuning files, as read.
  auto copy = [&](TuningKind kind, const std::string& file, const std::string& line) -> bool {
    std::string text;
    if (!files.read_tuning(kind, file, text, &e)) return false;
    index << line << ' ' << file << '\n';
    return write_file(dir / file, text.data(), text.size());
  };
  std::string et_file = media + "_ET.xml";
  std::string cmt_file = media + "_CMT.xml";
  if (!copy(TuningKind::kEngine, et_file, "engine")) {
    et_file = "VW_BORAVR6_03_ET.xml";
    cmt_file = "VW_BORAVR6_03_CMT.xml";
    if (!copy(TuningKind::kEngine, et_file, "engine")) return fail("cannot copy the engine tuning");
  }
  if (!copy(TuningKind::kModel, media + "_CMT.xml", "model") && cmt_file != media + "_CMT.xml")
    copy(TuningKind::kModel, cmt_file, "model");
  {
    std::string text;
    XmlNode engine;
    std::vector<HarmonicRef> harmonics;
    EngineSound scratch;
    if (files.read_tuning(TuningKind::kEngine, et_file, text, &e) && parse_xml(text, engine, &e) &&
        read_engine_tuning(engine, scratch, harmonics, &e, levels))
      for (const auto& h : harmonics) copy(TuningKind::kHarmonic, h.file, "harmonic " + h.element);
  }
  // Every sample a loop uses, plus any IntakeChuff sample, once per bank sample.
  std::set<std::pair<std::string, int>> wanted;
  for (const auto& g : loaded.sound.groups)
    for (const auto& l : g.loops) wanted.insert({lower_name(g.bank), l.sample});
  if (loaded.sound.intake_chuff_sample >= 0 && !loaded.sound.intake_chuff_bank.empty()) {
    wanted.insert({lower_name(loaded.sound.intake_chuff_bank), loaded.sound.intake_chuff_sample});
    index << "chuff " << lower_name(loaded.sound.intake_chuff_bank) << ' ' << loaded.sound.intake_chuff_sample << '\n';
  }
  for (const auto& [bank, samples] : loaded.banks) index << "bank " << bank << ' ' << samples.size() << '\n';

  // Read each referenced FSB4 bank into memory once.
  std::map<std::string, std::vector<uint8_t>> bank_bytes;
  struct Job {
    std::string bank;
    int sample = 0;
    FsbSample info;
  };
  std::vector<Job> jobs;
  for (const auto& [bank, sample] : wanted) {
    const auto& samples = loaded.banks.at(bank);
    if (sample < 0 || size_t(sample) >= samples.size()) continue;
    const FsbSample& s = samples[size_t(sample)];
    index << "sample " << bank << ' ' << sample << ' ' << s.channels << ' ' << s.rate << ' ' << s.frames << '\n';
    if (!s.xma()) {
      report.notes.push_back(bank + " " + std::to_string(sample) + ": not XMA, skipped");
      continue;
    }
    if (!bank_bytes.count(bank)) {
      std::vector<uint8_t> data;
      if (!read_file(files.bank(bank), data)) return fail("cannot read " + bank);
      bank_bytes[bank] = std::move(data);
    }
    if (s.offset + s.bytes > bank_bytes[bank].size()) return fail("cannot read " + bank);
    jobs.push_back({bank, sample, s});
  }

  const std::filesystem::path temp = std::filesystem::temp_directory_path(ec) / ("shift-sound-" + key);
  std::filesystem::remove_all(temp, ec);
  std::filesystem::create_directories(temp, ec);

  // Decode XMA2 loops in parallel (up to 4 ffmpeg workers) so on-demand
  // runtime decode finishes in under a second.
  std::atomic<size_t> next_job{0};
  std::mutex err_mu;
  std::string first_err;
  int mismatches = 0;
  int decoded = 0;
  double worst_seam = 0;

  auto worker = [&]() {
    while (true) {
      const size_t idx = next_job.fetch_add(1);
      if (idx >= jobs.size()) break;
      {
        std::lock_guard<std::mutex> lk(err_mu);
        if (!first_err.empty()) break;
      }
      const Job& job = jobs[idx];
      const FsbSample& s = job.info;
      const auto& data = bank_bytes.at(job.bank);
      const auto raw_span = std::span<const uint8_t>(data).subspan(size_t(s.offset), s.bytes);
      const size_t channels = std::max<size_t>(1, s.channels);
      std::vector<int16_t> inter_s16;
      size_t frames = 0;
      std::vector<float> inter_f32;
      int dec_ch = 0, dec_rate = 0;
      if (decode_xma2_interleaved(s, raw_span, inter_f32, dec_ch, dec_rate, nullptr) && !inter_f32.empty()) {
        const size_t eff_ch = std::max<size_t>(1, size_t(dec_ch));
        frames = inter_f32.size() / eff_ch;
        inter_s16.resize(frames * channels, 0);
        for (size_t f = 0; f < frames; ++f) {
          for (size_t c = 0; c < channels; ++c) {
            const float v = inter_f32[f * eff_ch + std::min(c, eff_ch - 1)];
            inter_s16[f * channels + c] = int16_t(std::clamp(std::lround(double(v) * 32767.0), -32768l, 32767l));
          }
        }
      } else {
        if (ffmpeg.empty()) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (first_err.empty()) first_err = "no XMA2 decoder available for " + job.bank + " " + std::to_string(job.sample);
          break;
        }
        const auto wave = xma2_wave(s, raw_span);
        const auto in_path = temp / ("in_" + std::to_string(idx) + ".wav");
        const auto out_path = temp / ("out_" + std::to_string(idx) + ".s16");
        std::error_code lec;
        std::filesystem::remove(out_path, lec);
        if (!write_file(in_path, wave.data(), wave.size())) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (first_err.empty()) first_err = "cannot write " + in_path.string();
          break;
        }
        std::string command = quoted(ffmpeg) + " -hide_banner -nostdin -v error -y -i " + quoted(in_path) +
                              " -f s16le -acodec pcm_s16le " + quoted(out_path);
        if (run_hidden_command(command) != 0) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (first_err.empty()) first_err = "ffmpeg failed on " + job.bank + " " + std::to_string(job.sample);
          break;
        }
        std::vector<uint8_t> pcm;
        read_file(out_path, pcm);
        frames = pcm.size() / (2 * channels);
        inter_s16.resize(frames * channels, 0);
        for (size_t i = 0; i < frames * channels; ++i) {
          inter_s16[i] = int16_t(pcm[2 * i] | (pcm[2 * i + 1] << 8));
        }
      }
      const bool mismatch = frames != s.frames;
      inter_s16.resize(size_t(s.frames) * channels, 0);  // exactly the declared length
      frames = s.frames;
      double seam = 0;
      for (size_t c = 0; c < channels; ++c) {
        std::vector<int16_t> mono(frames);
        for (size_t f = 0; f < frames; ++f) mono[f] = inter_s16[f * channels + c];
        if (frames > 1) seam = std::max(seam, std::fabs(mono.back() - mono.front()) / 32768.0);
        if (!write_file(dir / "pcm" / pcm_name(job.bank, job.sample, int(c)), mono.data(), mono.size() * 2)) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (first_err.empty()) first_err = "cannot write PCM";
          break;
        }
      }
      std::lock_guard<std::mutex> lk(err_mu);
      if (mismatch) ++mismatches;
      worst_seam = std::max(worst_seam, seam);
      ++decoded;
    }
  };

  const size_t threads = std::min<size_t>(4, std::max<size_t>(1, jobs.size()));
  std::vector<std::thread> pool;
  for (size_t t = 0; t + 1 < threads; ++t) pool.emplace_back(worker);
  worker();
  for (auto& th : pool) th.join();

  std::filesystem::remove_all(temp, ec);
  if (!first_err.empty()) return fail(first_err);
  report.samples += decoded;
  report.length_mismatches += mismatches;
  report.worst_seam = std::max(report.worst_seam, worst_seam);
  const std::string text = index.str();
  if (!write_file(dir / "sound.txt", text.data(), text.size())) return fail("cannot write sound.txt");
  return true;
}

std::vector<std::vector<const PcmSample*>> BakedSound::table() const {
  std::vector<std::vector<const PcmSample*>> t;
  for (const auto& g : loaded.sound.groups) {
    t.emplace_back();
    for (const auto& l : g.loops) {
      const auto it = pcm.find({lower_name(g.bank), l.sample, l.channel});
      t.back().push_back(it == pcm.end() ? nullptr : &it->second);
    }
  }
  return t;
}

const PcmSample* BakedSound::intake_chuff() const {
  if (loaded.sound.intake_chuff_sample < 0 || loaded.sound.intake_chuff_bank.empty()) return nullptr;
  const auto it = pcm.find({lower_name(loaded.sound.intake_chuff_bank), loaded.sound.intake_chuff_sample, 0});
  return it == pcm.end() ? nullptr : &it->second;
}

bool load_baked_sound(const std::filesystem::path& out_dir, const std::string& media, BakedSound& out,
                      std::string* error, UpgradeLevels levels) {
  out = {};
  const std::filesystem::path dir = cars::mods::resolve(out_dir / baked_sound_key(media, levels));
  auto fail = [&](const std::string& what) {
    if (error) *error = (dir / "sound.txt").string() + ": " + what;
    return false;
  };
  std::ifstream index(dir / "sound.txt");
  if (!index) return fail("not baked (run soundbake)");
  auto parse = [&](const std::string& file, XmlNode& node) {
    std::vector<uint8_t> bytes;
    std::string e;
    return read_file(dir / file, bytes) &&
           parse_xml(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), node, &e);
  };
  XmlNode engine;
  std::vector<std::pair<std::string, std::string>> harmonics;
  std::string model_file;
  std::string line;
  out.loaded.sound.media = media;
  while (std::getline(index, line)) {
    std::istringstream ls(line);
    std::string kind;
    ls >> kind;
    if (kind == "engine") {
      std::string f;
      ls >> f;
      if (!parse(f, engine)) return fail("cannot read " + f);
    } else if (kind == "model") {
      ls >> model_file;
    } else if (kind == "harmonic") {
      std::string element, f;
      ls >> element >> f;
      harmonics.emplace_back(element, f);
    } else if (kind == "chuff") {
      std::string bank;
      int sample = -1;
      ls >> bank >> sample;
      out.loaded.sound.intake_chuff_bank = bank;
      out.loaded.sound.intake_chuff_sample = sample;
    } else if (kind == "bank") {
      std::string bank;
      size_t count = 0;
      ls >> bank >> count;
      auto& list = out.loaded.banks[bank];
      if (list.size() < count) list.resize(count);
    } else if (kind == "sample") {
      std::string bank;
      int sample = 0, channels = 1, rate = 0;
      size_t frames = 0;
      ls >> bank >> sample >> channels >> rate >> frames;
      auto& list = out.loaded.banks[bank];
      if (list.size() <= size_t(sample)) list.resize(size_t(sample) + 1);
      list[size_t(sample)].channels = uint16_t(channels);
      list[size_t(sample)].rate = rate;
      list[size_t(sample)].frames = uint32_t(frames);
      for (int c = 0; c < channels; ++c) {
        std::vector<uint8_t> bytes;
        if (!read_file(dir / "pcm" / pcm_name(bank, sample, c), bytes)) continue;
        PcmSample p;
        p.rate = rate;
        p.data.resize(bytes.size() / 2);
        for (size_t i = 0; i < p.data.size(); ++i)
          p.data[i] = float(int16_t(bytes[2 * i] | bytes[2 * i + 1] << 8)) / 32768.0f;
        out.pcm[{bank, sample, c}] = std::move(p);
      }
    }
  }
  std::vector<HarmonicRef> refs;
  std::string e;
  if (engine.name.empty() || !read_engine_tuning(engine, out.loaded.sound, refs, &e, levels))
    return fail("no engine tuning");
  for (const auto& [element, f] : harmonics) {
    XmlNode h;
    if (parse(f, h)) read_harmonic(engine, h, element, out.loaded.sound, &e);
  }
  // The loop index rule needs each bank's full sample count ("bank" lines).
  for (auto& g : out.loaded.sound.groups) {
    const auto it = out.loaded.banks.find(lower_name(g.bank));
    if (it == out.loaded.banks.end()) continue;
    std::vector<int> channels;
    for (const auto& s : it->second) channels.push_back(std::max<int>(1, s.channels));
    resolve_loop_indices(g.loops, int(it->second.size()), channels);
  }
  XmlNode model;
  if (!model_file.empty() && parse(model_file, model)) read_model_tuning(model, out.loaded.sound);
  return true;
}

bool decode_engine_sound_runtime(SoundFiles& files, const std::string& media, const std::filesystem::path& cache_dir,
                                 BakedSound& out, std::string* error, UpgradeLevels levels,
                                 const std::filesystem::path& ffmpeg) {
  if (!cache_dir.empty() && load_baked_sound(cache_dir, media, out, nullptr, levels)) return true;
  const std::filesystem::path ff = (ffmpeg.empty() && !has_inprocess_xma2_decoder()) ? find_ffmpeg() : ffmpeg;
  if (ff.empty() && !has_inprocess_xma2_decoder()) {
    if (error) *error = "no baked sound and no XMA2 decoder available for runtime decode";
    return false;
  }
  std::error_code ec;
  const bool ephemeral = cache_dir.empty();
  const std::filesystem::path target =
      ephemeral ? (std::filesystem::temp_directory_path(ec) / ("shift-rt-sound-" + baked_sound_key(media, levels)))
                : cache_dir;
  BakeReport report;
  const bool ok = bake_engine_sound(files, media, target, ff, report, error, levels) &&
                  load_baked_sound(target, media, out, error, levels);
  if (ephemeral) std::filesystem::remove_all(target, ec);
  return ok;
}

}  // namespace audio
