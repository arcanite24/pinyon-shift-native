// Our baked car format (`.shiftbake`): one file per car, per wheel or tyre
// model, and one for the shared car textures. Everything a showroom needs is
// decoded once: vertex positions in car space, triangle lists, materials
// sorted into classes with their texture slots, untiled block textures, the
// car's paint, wheel and calliper rows, and livery planes.
//
// Layout: little-endian, written field by field (no struct padding, no
// timestamps, no pointers), so two bakes of the same files are
// byte-identical.
//
//   "SHFTBAKE" | u32 version | u32 kind | car info (kind 1) |
//   u32 model count | models | u32 texture count | textures |
//   u32 livery plane count | planes | "ENDBAKE\0"
//
// Strings are u32 length + bytes. Arrays are u32 count + items.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cars::bake {

inline constexpr uint32_t kVersion = 2;

enum class Kind : uint32_t { kCar = 1, kWheel = 2, kShared = 3 };

// What a model is to the showroom.
enum class Role : uint32_t {
  kExterior = 0,  // <car>.carbin, LOD1-5
  kLod0 = 1,      // <car>_lod0.carbin
  kCockpit = 2,   // <car>_cockpit.carbin
  kCaliper = 3,   // <car>_caliper<W>_LOD0.carbin
  kRotor = 4,     // <car>_rotor<W>_LOD0.carbin
  kWheel = 5,     // a rim model
  kTyre = 6,      // a tyre model
};

// Corner of a calliper or rotor model: LF, RF, LR, RR (from its name).
enum class Corner : uint32_t { kNone = 0, kLF = 1, kRF = 2, kLR = 3, kRR = 4 };

// Material classes, decided at bake time from the technique name and the
// draw lists (FORMAT-CARS "Groups an implementation needs").
enum class MaterialClass : uint32_t {
  kOpaque = 0,    // the slot-0 texel colour
  kPaint = 1,     // paint where the slot-0 alpha is 0, texel elsewhere
  kGlass = 2,     // transparent list: tinted, drawn last
  kLamp = 3,      // lamp parts, drawn unlit (lamps off)
  kInterior = 4,  // samples interior_LOD0 in slot 1
  kGauge = 5,     // gauges (interior, emissive texture where there is one)
  kTyre = 6,      // sidewall and tread
  kRim = 7,       // wheel atlas in slot 0
  kCaliper = 8,   // calliper body, tinted with the calliper colour
  kChrome = 9,    // chrome and polished metal
  kCarbon = 10,   // carbon fibre (Shared carbonFiber in slot 1)
  kHidden = 11,   // spinning-rim cards, not drawn when the wheel is still
};

const char* material_class_name(MaterialClass c);

// 32 bytes as stored: position (car space, metres), the vertex's unit
// quaternion tangent frame (/ 32767), UV0 and UV1 before the transform
// (/ 65535), baked lighting bytes.
struct Vertex {
  std::array<float, 3> position;
  std::array<int16_t, 4> frame;
  std::array<uint16_t, 2> uv0, uv1;
  std::array<uint8_t, 4> lighting;
};

// A texture reference: "" for none, "@atlas" for the paint row's clean
// atlas (LOD0 or LOD1-5 by the model), otherwise "<pack>:<member>" with
// pack car, wheel, tyre or shared, e.g. "car:interior_lod0.xds".
struct Subsection {
  std::string technique;
  MaterialClass material = MaterialClass::kOpaque;
  bool transparent = false;  // from the model's draw lists
  int32_t lod = 0;
  std::array<float, 8> uv_transform{};  // UV0 then UV1: offset, scale for U then V
  std::array<std::string, 2> slots;     // slot 0 and slot 1
  std::vector<uint32_t> indices;        // triangle list into the part's pool for its LOD
};

// A wheel or tyre model's morph target: one car-space position per vertex
// of its pool (FORMAT-CARS, Damage tail: the wheel-sizing targets).
using MorphTarget = std::vector<std::array<float, 3>>;

struct Part {
  std::string name;
  std::array<float, 9> bounds{};  // offset, minimum, maximum
  std::vector<Vertex> lod_vertices;   // LOD1-5 pool
  std::vector<Vertex> lod0_vertices;  // LOD0 pool
  // Wheel and tyre models only: the pools' morph targets, in file order
  // (empty for car models and for pools without targets).
  std::vector<MorphTarget> lod_morphs, lod0_morphs;
  std::vector<Subsection> subsections;
};

struct Model {
  std::string name;  // lower-case member name
  Role role = Role::kExterior;
  Corner corner = Corner::kNone;
  // The header's first word, and for kind 3 (rims and tyres) the header
  // floats at bytes 32, 36, 40 and 44: x maximum, the design +x face, x
  // minimum, the design -x face (FORMAT-CARS, Header).
  uint32_t header_kind = 0;
  std::array<float, 4> header_bounds{};
  std::vector<Part> parts;
};

enum class TextureFormat : uint32_t { kRgba8 = 0, kBc1 = 1, kBc2 = 2, kBc3 = 3, kBc4 = 4, kBc5 = 5, kR8 = 6 };

struct TextureLevel {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> data;  // rows of blocks (or texels), top first
};

struct Texture {
  std::string name;  // lower-case member name, e.g. "nodamage_lod0.xds"
  TextureFormat format = TextureFormat::kRgba8;
  std::array<uint8_t, 4> swizzle{0, 1, 2, 3};  // per R, G, B, A: 0-3 a component, 4 zero, 5 one
  std::vector<TextureLevel> levels;
};

struct Colour {
  int64_t id = 0, sequence = 0;
  bool metallic = false;
  uint32_t rgb = 0;             // sRGB, r x 65536 + g x 256 + b
  std::string atlas_lod0;       // resolved member names ("" when unresolved)
  std::string atlas_lod;
  // A Combo_Colors_Exceptions row (same car and Sequence) with HasStripe 1:
  // the stripe colour, sRGB like rgb.
  bool has_stripe = false;
  uint32_t stripe_rgb = 0;
};

struct LiveryPlane {
  std::string name;  // Front, Back, Top, Left, Right, Wing
  bool valid = false;
  float left = 0, right = 0, top = 0, bottom = 0;  // pixels from the sheet's centre
  float x_origin = 0, y_origin = 0, x_scale = 1, y_scale = 1, rotation = 0;
  std::string x_axis, y_axis;
};

struct CarInfo {
  std::string media_name, display_name, origin;
  int64_t car_id = 0;
  std::vector<Colour> colours;  // by Sequence; the showroom paints colours[0]
  std::string wheel_media;      // lower case
  std::string tyre_media;       // lower case (TireC until the tyre choice is known)
  std::string tyre_texture_media;  // lower case (TireA: the sidewall and tread textures)
  std::array<int64_t, 2> tyre_width_mm{}, tyre_aspect{}, rim_inches{};  // front, rear
  std::array<float, 2> ride_height{};  // FrontStockRideHeight, RearStockRideHeight
  // Data_CarBody (row car id x 1000): ModelFrontTrackOuter and
  // ModelRearTrackOuter, ModelWheelbase, BottomCenterWheelbasePosY.
  std::array<float, 2> track_outer{};
  float wheelbase = 0, body_bottom_y = 0;
  bool has_body = false;
  // The model-to-body pose (FORMAT-CARS, Wheels): body = R (model) + (0,
  // lift, 0), R = [[1,0,0],[0,cos p,sin p],[0,sin p,-cos p]] (z mirrored
  // and pitched by p).
  float lift = 0, pitch = 0;
  bool has_track_offsets = false;  // CarTrackOffsets rows (not applied: logged)
  std::array<uint32_t, 2> caliper_rgb{};    // front, rear (unconverted)
  std::array<float, 2> caliper_angle{};
  // Wheel centres in the model's frame, LF, RF, LR, RR: x from the track
  // and tyre width, y the tyre's outer radius, z from CarPartPositions
  // PartId 5, 6, 8, 7 (body frame, +Z forward), mapped by the inverse pose.
  std::array<std::array<float, 3>, 4> wheel_centres{};
  // The same centres in the body frame.
  std::array<std::array<float, 3>, 4> wheel_centres_body{};
  std::array<std::array<float, 3>, 10> part_positions{};
  bool has_livery = false;
  int64_t wing_mask = 0;
  std::vector<std::string> notes;  // what the bake could not resolve or had to guess
};

struct Bake {
  Kind kind = Kind::kCar;
  CarInfo car;  // kind kCar only
  std::vector<Model> models;
  std::vector<Texture> textures;
  std::vector<LiveryPlane> livery;
};

std::vector<uint8_t> encode(const Bake& bake);
bool decode(const std::vector<uint8_t>& bytes, Bake& out, std::string* error);
bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string* error);
bool read_file(const std::filesystem::path& path, Bake& out, std::string* error);

}  // namespace cars::bake
