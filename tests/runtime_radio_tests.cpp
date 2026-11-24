#include "audio/audio_engine.h"
#include "audio/runtime_radio.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                   \
  do {                                                                                                \
    if (!(cond)) {                                                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
      ++g_failures;                                                                                   \
    }                                                                                                 \
  } while (0)

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const char* name) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / (std::string("shift_") + name + "_" + std::to_string(now));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void write_text(const std::filesystem::path& p, std::string_view text) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(text.data(), std::streamsize(text.size()));
}

// Writes a valid 16-bit stereo 48 kHz WAV sine wave file.
void write_sine_wav(const std::filesystem::path& p, double freq_hz, int frames, int rate = 48000) {
  std::filesystem::create_directories(p.parent_path());
  const uint16_t channels = 2;
  const uint16_t bits = 16;
  const uint32_t data_bytes = uint32_t(frames) * channels * (bits / 8);
  const uint32_t riff_size = 36 + data_bytes;
  const uint32_t byte_rate = uint32_t(rate) * channels * (bits / 8);
  const uint16_t block_align = channels * (bits / 8);

  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  auto w32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto w16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

  out.write("RIFF", 4);
  w32(riff_size);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  w32(16);
  w16(1);  // PCM
  w16(channels);
  w32(uint32_t(rate));
  w32(byte_rate);
  w16(block_align);
  w16(bits);
  out.write("data", 4);
  w32(data_bytes);

  for (int i = 0; i < frames; ++i) {
    const double s = 0.5 * std::sin(2.0 * 3.14159265358979323846 * freq_hz * double(i) / double(rate));
    const int16_t pcm = int16_t(std::lround(s * 32767.0));
    out.write(reinterpret_cast<const char*>(&pcm), 2);
    out.write(reinterpret_cast<const char*>(&pcm), 2);
  }
}

void test_json_roundtrip_and_mixed_sources() {
  audio::RuntimeRadioConfig cfg;
  cfg.version = 1;

  for (int i = 0; i < 4; ++i) {
    audio::RuntimeStationConfig st;
    st.id = "custom_" + std::to_string(i);
    st.name = "Station \"" + std::to_string(i) + "\" FM";
    st.genre = i == 0 ? "Synthwave" : "Mixed";
    st.accent_color = i == 0 ? "#FF00AA" : "#00E5FF";
    st.logo_png = "logos/st" + std::to_string(i) + ".png";
    st.shuffle = (i % 2 == 0);
    st.enabled = (i != 3);
    st.volume = 0.85f;
    st.sources.push_back({audio::RadioSourceType::kFolder, "C:/Music/Drive", "", ""});
    st.sources.push_back({audio::RadioSourceType::kFile, "C:/Music/intro.wav", "Intro Theme", "Shift"});
    st.sources.push_back({audio::RadioSourceType::kPlaylist, "C:/Music/mix.m3u8", "", ""});
    st.sources.push_back({audio::RadioSourceType::kStream, "https://stream.example.com/live.mp3", "Live FM", "DJ Net"});
    st.sources.push_back(
        {audio::RadioSourceType::kYoutube, "https://www.youtube.com/playlist?list=PL123456", "", "YT Curated"});
    cfg.stations.push_back(std::move(st));
  }

  const std::string json = audio::serialize_runtime_radio_json(cfg);
  audio::RuntimeRadioConfig parsed;
  std::string err;
  CHECK(audio::parse_runtime_radio_json(json, parsed, &err));
  CHECK(parsed.version == 1);
  CHECK(parsed.stations.size() == 4);
  CHECK(parsed.stations[0].id == "custom_0");
  CHECK(parsed.stations[0].name == "Station \"0\" FM");
  CHECK(parsed.stations[0].genre == "Synthwave");
  CHECK(parsed.stations[0].accent_color == "#FF00AA");
  CHECK(parsed.stations[0].logo_png == "logos/st0.png");
  CHECK(parsed.stations[0].shuffle == true);
  CHECK(parsed.stations[0].enabled == true);
  CHECK(parsed.stations[3].enabled == false);
  CHECK(parsed.stations[0].sources.size() == 5);
  CHECK(parsed.stations[0].sources[0].type == audio::RadioSourceType::kFolder);
  CHECK(parsed.stations[0].sources[1].type == audio::RadioSourceType::kFile);
  CHECK(parsed.stations[0].sources[1].title == "Intro Theme");
  CHECK(parsed.stations[0].sources[2].type == audio::RadioSourceType::kPlaylist);
  CHECK(parsed.stations[0].sources[3].type == audio::RadioSourceType::kStream);
  CHECK(parsed.stations[0].sources[4].type == audio::RadioSourceType::kYoutube);
}

void test_folder_scanning() {
  TempDir tmp("radio_scan");
  write_text(tmp.path / "01 - Kavinsky - Nightcall.mp3", "x");
  write_text(tmp.path / "sub" / "Daft Punk - Voyager.FLAC", "x");
  write_text(tmp.path / "sub" / "nested" / "track.ogg", "x");
  write_text(tmp.path / "sub" / "nested" / "track2.m4a", "x");
  write_text(tmp.path / "sub" / "nested" / "track3.opus", "x");
  write_text(tmp.path / "sub" / "tone.wav", "x");
  write_text(tmp.path / "readme.txt", "ignore");
  write_text(tmp.path / "cover.png", "ignore");

  const auto tracks = audio::scan_audio_folder(tmp.path);
  CHECK(tracks.size() == 6);
  bool saw_daft = false;
  for (const auto& t : tracks) {
    CHECK(t.kind == audio::RuntimeTrackKind::kLocalFile);
    if (t.artist == "Daft Punk" && t.title == "Voyager") saw_daft = true;
  }
  CHECK(saw_daft);
}

void test_m3u_and_pls_parsing() {
  TempDir tmp("radio_pl");
  const std::string m3u =
      "\xEF\xBB\xBF#EXTM3U\n"
      "#EXTINF:215,Porter Robinson - Language\n"
      "songs/language.mp3\n"
      "#EXTINF:-1,Nightride FM\n"
      "https://stream.nightride.fm/nightride.m4a\n"
      "https://www.youtube.com/watch?v=dQw4w9WgXcQ\n";
  const auto m3u_tracks = audio::parse_playlist_text(m3u, tmp.path, false);
  CHECK(m3u_tracks.size() == 3);
  CHECK(m3u_tracks[0].kind == audio::RuntimeTrackKind::kLocalFile);
  CHECK(m3u_tracks[0].artist == "Porter Robinson");
  CHECK(m3u_tracks[0].title == "Language");
  CHECK(m3u_tracks[1].kind == audio::RuntimeTrackKind::kStream);
  CHECK(m3u_tracks[1].title == "Nightride FM");
  CHECK(m3u_tracks[2].kind == audio::RuntimeTrackKind::kYoutube);

  const std::string pls =
      "[playlist]\n"
      "NumberOfEntries=2\n"
      "File1=local/track1.wav\n"
      "Title1=Artist A - Song A\n"
      "File2=http://radio.example.org/stream\n"
      "Title2=Example Radio\n";
  const auto pls_tracks = audio::parse_playlist_text(pls, tmp.path, true);
  CHECK(pls_tracks.size() == 2);
  CHECK(pls_tracks[0].kind == audio::RuntimeTrackKind::kLocalFile);
  CHECK(pls_tracks[0].artist == "Artist A");
  CHECK(pls_tracks[0].title == "Song A");
  CHECK(pls_tracks[1].kind == audio::RuntimeTrackKind::kStream);
  CHECK(pls_tracks[1].title == "Example Radio");
}

void test_ytdlp_json_parsing() {
  const std::string playlist_json = R"({
    "id": "PLtest",
    "title": "Horizon Roadtrip",
    "entries": [
      {"id": "vid001", "title": "Madeon - Icarus", "uploader": "Madeon"},
      {"id": "vid002", "title": "Sunset Cruise", "channel": "SynthChannel", "url": "https://www.youtube.com/watch?v=vid002"}
    ]
  })";
  std::vector<audio::RuntimeTrack> tracks;
  std::string err;
  CHECK(audio::parse_ytdlp_flat_playlist_json(playlist_json, tracks, &err));
  CHECK(tracks.size() == 2);
  CHECK(tracks[0].kind == audio::RuntimeTrackKind::kYoutube);
  CHECK(tracks[0].cache_id == "vid001");
  CHECK(tracks[0].artist == "Madeon");
  CHECK(tracks[0].title == "Icarus");
  CHECK(tracks[1].cache_id == "vid002");
  CHECK(tracks[1].artist == "SynthChannel");
  CHECK(tracks[1].title == "Sunset Cruise");

  const std::string single_json = R"({
    "id": "solo99",
    "title": "Single Drive Track",
    "uploader": "Driver",
    "webpage_url": "https://www.youtube.com/watch?v=solo99"
  })";
  CHECK(audio::parse_ytdlp_flat_playlist_json(single_json, tracks, &err));
  CHECK(tracks.size() == 1);
  CHECK(tracks[0].cache_id == "solo99");
  CHECK(tracks[0].title == "Single Drive Track");
}

void test_hot_reload_merge_and_playback() {
  TempDir tmp("radio_live");
  const auto music_dir = tmp.path / "music";
  const auto wav1 = music_dir / "Artist One - Tone 440.wav";
  const auto wav2 = music_dir / "Artist Two - Tone 880.wav";
  write_sine_wav(wav1, 440.0, 24000);
  write_sine_wav(wav2, 880.0, 24000);

  const auto play_cfg_path = tmp.path / "play" / "radio_stations.json";
  const auto user_cfg_path = tmp.path / "user" / "radio_stations.json";
  const auto cache_dir = tmp.path / "user" / "radio_cache";

  // Initial play config with 2 stations; user overrides station 1.
  audio::RuntimeRadioConfig play_cfg;
  play_cfg.stations.push_back(
      {"st_play", "Play Station", "Rock", "#FF0000", "", false, true, 1.0f, {{audio::RadioSourceType::kFile, wav1.string(), "", ""}}});
  play_cfg.stations.push_back(
      {"st_override", "Old Name", "Pop", "#00FF00", "", false, true, 1.0f, {{audio::RadioSourceType::kFile, wav1.string(), "", ""}}});
  CHECK(audio::save_runtime_radio_config(play_cfg_path, play_cfg));

  audio::RuntimeRadioConfig user_cfg;
  user_cfg.stations.push_back({"st_override", "User Override FM", "Electronic", "#00AAFF", "", false, true, 1.0f,
                               {{audio::RadioSourceType::kFolder, music_dir.string(), "", ""}}});
  CHECK(audio::save_runtime_radio_config(user_cfg_path, user_cfg));

  audio::RuntimeRadioManager mgr;
  mgr.configure(user_cfg_path, play_cfg_path, cache_dir);
  CHECK(mgr.reload());
  auto enabled = mgr.enabled_stations();
  CHECK(enabled.size() == 2);
  CHECK(enabled[0].config.id == "st_play");
  CHECK(enabled[1].config.id == "st_override");
  CHECK(enabled[1].config.name == "User Override FM");
  CHECK(enabled[1].tracks.size() == 2);

  // No change yet -> poll_updates() returns false.
  CHECK(!mgr.poll_updates());

  // Hot-reload: add a third station via file edit (sleep briefly so mtime advances on Windows).
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  user_cfg.stations.push_back({"st_third", "Third Station", "Jazz", "#FFAA00", "", true, true, 0.9f,
                               {{audio::RadioSourceType::kFile, wav2.string(), "Jazz Cut", "Quartet"}}});
  CHECK(audio::save_runtime_radio_config(user_cfg_path, user_cfg));
  CHECK(mgr.poll_updates());
  enabled = mgr.enabled_stations();
  CHECK(enabled.size() == 3);
  CHECK(enabled[2].config.name == "Third Station");

  // Playback + shuffle/advance + AudioEngine Bus::kMusic offline rendering.
  audio::AudioEngine engine(48000);
  auto player = std::make_unique<audio::RuntimeRadioPlayer>(48000, 42, /*non_blocking=*/false);
  player->set_stations(enabled, cache_dir);
  player->select_station(1);  // User Override FM (2 WAV tracks, sequential)
  CHECK(player->active());
  CHECK(player->current_title_view() == "Tone 440");
  CHECK(player->current_artist_view() == "Artist One");

  player->next_track();
  CHECK(player->current_title_view() == "Tone 880");
  CHECK(player->current_artist_view() == "Artist Two");

  player->prev_track();
  CHECK(player->current_title_view() == "Tone 440");

  engine.set_runtime_radio(std::move(player));
  std::vector<float> pcm(4800 * 2, 0.0f);
  engine.render_offline(pcm.data(), 4800);
  double energy = 0.0;
  for (float s : pcm) energy += double(s) * double(s);
  CHECK(energy > 1.0);

  const auto tel = engine.telemetry();
  CHECK(tel.runtime_radio_active);
  CHECK(tel.runtime_radio_station == 1);
  CHECK(std::string(tel.runtime_radio_title.data()) == "Tone 440");
  CHECK(std::string(tel.runtime_radio_artist.data()) == "Artist One");

  // Runtime CRUD: upsert + remove.
  audio::RuntimeStationConfig added{"st_crud", "CRUD FM", "LoFi", "#123456", "", true, true, 1.0f, {}};
  CHECK(mgr.upsert_station(added));
  CHECK(mgr.enabled_stations().size() == 4);
  CHECK(mgr.remove_station("st_crud"));
  CHECK(mgr.enabled_stations().size() == 3);
}

}  // namespace

int main() {
  test_json_roundtrip_and_mixed_sources();
  test_folder_scanning();
  test_m3u_and_pls_parsing();
  test_ytdlp_json_parsing();
  test_hot_reload_merge_and_playback();
  if (g_failures == 0) {
    std::printf("runtime_radio_tests: ALL PASSED\n");
    return 0;
  }
  std::fprintf(stderr, "runtime_radio_tests: %d FAILURE(S)\n", g_failures);
  return 1;
}
