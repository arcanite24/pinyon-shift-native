// Turns a car's, wheel's or the shared folder's files into a Bake.
// Spec: FORMAT-CARS (Car members, Materials, Body atlas, Paint, Livery
// masks, Wheels, Brakes, Lights, glass and shadow); AUTOSHOW_REFERENCE
// (Paint colour; Parts and lighting).
#pragma once

#include <string>

#include "cars/bake_format.h"
#include "cars/database.h"
#include "cars/sources.h"

namespace cars {

// The tyre model every car uses until the tyre choice is known, and the
// tyre archive whose sidewall and tread textures it draws with (the stock
// XKR-S drew TireC with the A textures).
inline constexpr const char* kTyreModel = "tirec";
inline constexpr const char* kTyreTextures = "tirea";

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
