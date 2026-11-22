// Vulkan shader overrides (ours, Slice 13): SPIR-V (or GLSL compiled with
// glslc / glslangValidator from PATH) in a mod's shaders/ folder that the
// renderer uses instead of its built-in shader of the same name.
//
//   shaders/<name>.<stage>.spv   e.g. world.frag.spv replaces world.frag
//   shaders/<name>.<stage>       GLSL source (vert, frag, comp), compiled
//                                once into <mods root>/.shader-cache/
//
// The first mod in the load order with a valid file wins. A file that is
// not SPIR-V (magic 0x07230203, whole 32-bit words) is refused and listed.
// An override must keep the built-in's interface (inputs, outputs,
// descriptor bindings and push constants); the renderer cannot check that
// beyond the driver's own validation. Changes take effect at the next start.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cars::shader_mods {

bool valid_spirv(std::span<const uint8_t> bytes);

class Registry {
 public:
  // Scans the mods (the winner first). `cache_dir` holds compiled GLSL
  // (empty: GLSL is refused).
  void scan(const std::vector<std::filesystem::path>& mod_dirs_winner_first,
            const std::filesystem::path& cache_dir = {});
  void clear();
  // The override for a built-in shader ("world.frag"), as SPIR-V words.
  std::optional<std::vector<uint32_t>> find(std::string_view shader) const;
  size_t size() const { return shaders_.size(); }
  const std::map<std::string, std::string>& refused() const { return refused_; }
  const std::map<std::string, std::filesystem::path>& sources() const { return sources_; }

 private:
  std::map<std::string, std::vector<uint32_t>> shaders_;
  std::map<std::string, std::filesystem::path> sources_;
  std::map<std::string, std::string> refused_;
};

Registry& registry();

}  // namespace cars::shader_mods
