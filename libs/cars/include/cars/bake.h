// Turns a car's, wheel's or the shared folder's files into a Bake.
// Spec: FORMAT-CARS (Car members, Materials, Body atlas, Paint, Livery
// masks, Wheels, Brakes, Lights, glass and shadow); AUTOSHOW_REFERENCE
// (Paint colour; Parts and lighting).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cars/bake_format.h"
#include "cars/database.h"
#include "cars/sources.h"
#include "fh1data/carbin.h"

namespace cars {

// The tyre model every car uses until the tyre choice is known, and the
// tyre archive whose sidewall and tread textures it draws with (the stock
// XKR-S drew TireC with the A textures).
inline constexpr const char* kTyreModel = "tirec";
inline constexpr const char* kTyreTextures = "tirea";

// Spinning-rim blur technique classification (cross-faded with wheel0..3.xds at speed).
enum class BlurRimKind : uint8_t {
  kNone = 0,
  kBlurRim = 1,        // blur_rim
  kChromeBlurRim = 2,  // chrome_blur_rim
  kBlurLip = 3,        // blur_lip
  kChromeBlurLip = 4,  // chrome_blur_lip
};

BlurRimKind classify_blur_rim(std::string_view technique);
bool is_blur_rim_technique(std::string_view technique);

// Per-vertex crush weight (0..1) decoded from .carbin damage_flag == 1 HEND3N blocks
// and stored in bake::Vertex::lighting[kCrushWeightLightingChannel] (0..255 unorm,
// exposed as in_light.w in showroom.vert without altering the 32-byte Vertex layout).
inline constexpr size_t kCrushWeightLightingChannel = 3;
inline constexpr float kCrushWeightRefMeters = 0.15f;

float vertex_crush_weight(const bake::Vertex& v);

std::vector<float> decode_carbin_crush_weights(const fh1::CarbinPart& part,
                                               const fh1::CarbinDamageBlock& block,
                                               size_t vertex_count);

bake::MaterialClass classify(const std::string& technique, bool transparent, bake::Role role);

// A car: every non-stripped model, every texture, the livery planes and
// mask, and the car's rows (`row` is the Data_Car row used for paint and
// wheels). Notes go to out.car.notes.
bool bake_car(const FolderRef& folder, const CarRow& row, bake::Bake& out, std::string* error);

// A rim or tyre archive (`tyre` selects the tyre role).
bool bake_wheel(const FolderRef& folder, bool tyre, bake::Bake& out, std::string* error);

// Shared.zip: its textures only.
bool bake_shared(const FolderRef& folder, bake::Bake& out, std::string* error);

// Loads an aftermarket rim or per-car tyre archive on demand from
// <game_root>/media/wheels/<wheel_media>.zip (case-insensitive stem match).
bool load_wheel_from_disc(const std::filesystem::path& game_root, const std::string& wheel_media, bool tyre,
                          bake::Bake& out, std::string* error);

// Loads an upgraded brake rotor or caliper .rmb.carbin from
// <game_root>/media/brakes/ for the given inch size, List_BrakeType ID (1..5),
// and piston count (2/4/6/8).
bool load_brake_model(const std::filesystem::path& game_root, int size_in, int type_id, int pistons, bool is_rotor,
                      bake::Model& out, std::string* error);

// Decodes a 24- or 32-bit TGA (raw or RLE) to RGBA8, top row first.
bool decode_tga(const std::vector<uint8_t>& data, uint32_t& width, uint32_t& height,
                std::vector<uint8_t>& rgba, std::string* error);

}  // namespace cars
