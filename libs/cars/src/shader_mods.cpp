#include "cars/shader_mods.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace cars::shader_mods {
namespace fs = std::filesystem;
namespace {

Registry g_registry;

std::string lower_ascii(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool read_bytes(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

bool glsl_stage(const std::string& ext) { return ext == ".vert" || ext == ".frag" || ext == ".comp"; }

// glslc or glslangValidator from PATH, or "" when neither runs.
std::string find_compiler() {
#if defined(_WIN32)
  const char* null_dev = " >NUL 2>&1";
#else
  const char* null_dev = " >/dev/null 2>&1";
#endif
  for (const char* c : {"glslc", "glslangValidator"})
    if (std::system((std::string(c) + " --version" + null_dev).c_str()) == 0) return c;
  return {};
}

}  // namespace

bool valid_spirv(std::span<const uint8_t> b) {
  if (b.size() < 20 || b.size() % 4 != 0) return false;
  uint32_t magic;
  std::memcpy(&magic, b.data(), 4);
  return magic == 0x07230203u;
}

void Registry::clear() {
  shaders_.clear();
  sources_.clear();
  refused_.clear();
}

void Registry::scan(const std::vector<fs::path>& mod_dirs_winner_first, const fs::path& cache_dir) {
  clear();
  std::error_code ec;
  std::string compiler;
  bool compiler_probed = false;
  for (const auto& dir : mod_dirs_winner_first) {
    const fs::path root = dir / "shaders";
    if (!fs::is_directory(root, ec)) continue;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(root, ec))
      if (e.is_regular_file(ec)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      const std::string ext = lower_ascii(f.extension().string());
      std::string name;
      std::vector<uint8_t> bytes;
      if (ext == ".spv") {
        name = lower_ascii(f.stem().string());  // world.frag.spv -> world.frag
        if (shaders_.count(name) || refused_.count(name)) continue;
        if (!read_bytes(f, bytes) || !valid_spirv(bytes)) {
          refused_[name] = f.filename().string() + " is not SPIR-V";
          continue;
        }
      } else if (glsl_stage(ext)) {
        name = lower_ascii(f.filename().string());  // world.frag
        if (shaders_.count(name) || refused_.count(name)) continue;
        if (cache_dir.empty()) {
          refused_[name] = "GLSL needs a cache folder";
          continue;
        }
        if (!compiler_probed) compiler = find_compiler(), compiler_probed = true;
        if (compiler.empty()) {
          refused_[name] = "GLSL needs glslc or glslangValidator on PATH (or ship " + name + ".spv)";
          continue;
        }
        fs::create_directories(cache_dir, ec);
        const fs::path out = cache_dir / (dir.filename().string() + "." + name + ".spv");
        const bool stale = !fs::exists(out, ec) || fs::last_write_time(out, ec) < fs::last_write_time(f, ec);
        if (stale) {
          const std::string cmd = compiler == "glslc"
                                      ? "glslc \"" + f.string() + "\" -o \"" + out.string() + "\""
                                      : "glslangValidator -V \"" + f.string() + "\" -o \"" + out.string() + "\"";
          if (std::system(cmd.c_str()) != 0) {
            refused_[name] = f.filename().string() + " did not compile";
            continue;
          }
        }
        if (!read_bytes(out, bytes) || !valid_spirv(bytes)) {
          refused_[name] = f.filename().string() + " compiled to invalid SPIR-V";
          continue;
        }
      } else {
        continue;
      }
      std::vector<uint32_t> words(bytes.size() / 4);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      shaders_[name] = std::move(words);
      sources_[name] = f;
    }
  }
}

std::optional<std::vector<uint32_t>> Registry::find(std::string_view shader) const {
  auto it = shaders_.find(lower_ascii(std::string(shader)));
  if (it == shaders_.end()) return std::nullopt;
  return it->second;
}

Registry& registry() { return g_registry; }

}  // namespace cars::shader_mods
