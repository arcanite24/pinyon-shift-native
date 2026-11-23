// Crisp native TrueType text renderer for shift-launcher.
// Loads system UI sans-serif & monospace fonts via stb_truetype and uploads
// anti-aliased glyphs into carview::ui's shared atlas so Vulkan rendering and
// headless CPU rasterization produce identical razor-sharp typography.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ui.h"

namespace launcher {

enum class FontFace : int {
  kRegular = 0,
  kSemiBold = 1,
  kMono = 2,
};

// Initializes native system fonts (safe to call multiple times).
// Returns true if at least the regular TrueType font loaded.
bool init_launcher_fonts();

// Returns true when system TrueType fonts are active (otherwise falls back to carview::ui).
bool launcher_fonts_ready();

// Exact pixel width of a single-line UTF-8 string.
float measure_text(FontFace face, float px_size, std::string_view text);

// Truncates `text` with "..." so its rendered width fits within `max_width` pixels.
std::string ellipsize_text(FontFace face, float px_size, std::string_view text, float max_width);

// Draws single-line UTF-8 text. `y` is the top of the em box (use vertical centering helper
// for buttons/rows). When `max_width > 0`, automatically ellipsizes to stay inside `max_width`.
// Returns the rendered width in pixels.
float draw_text(carview::ui::List& list, float x, float y, std::string_view text, FontFace face,
                float px_size, uint32_t color, carview::ui::Align align = carview::ui::Align::kLeft,
                float max_width = 0.0f);

// Word-wraps `text` into up to `max_lines` lines within `max_width`, ellipsizing the final line
// if text remains. Returns total height consumed.
float draw_wrapped_text(carview::ui::List& list, float x, float y, float max_width, float line_height,
                        int max_lines, std::string_view text, FontFace face, float px_size, uint32_t color);

}  // namespace launcher
