// FH1 UI scenes (media/UI.zip Scenes/ui4/<name>.fbf + .bsg): the static
// layout of a screen. The FBF holds the native objects (groups, text boxes,
// models, materials, textures), the BSG the scene graph (one 96-byte node
// per drawable or group: parent, object, position, rotation, scale, bounds,
// opacity). Text content, list rows, states and animations are set at run
// time by the title's UI code and are not here.
// Spec: FORMAT-UISCENES (FBF kinds and bodies, BSG node layout, units:
// pixels of a 1280x720 canvas, origin at the centre, y up; unit quads of
// +-50 sized by 100 x scale; texture paths and their archives).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fh1 {

enum class UiObjectKind : uint32_t {
  kGroup = 0, kText = 1, kCamera = 2, kLayer = 3, kModel = 4, kMaterial = 5, kLight = 6, kTexture = 7,
};

struct UiObject {
  UiObjectKind kind = UiObjectKind::kGroup;
  uint32_t id = 0;
  std::string name;
  // kText.
  std::string font;  // lower case, as the scene names it
  uint32_t text_size = 0, text_align = 0;
  uint32_t text_valign = 1, text_mode = 2;
  std::array<float, 2> text_spacing{0.0f, 0.0f};
  float text_width = 0;  // version 1008; 0 otherwise
  // kModel.
  uint32_t geometry = 0;
  // kMaterial: the colour (probable).
  std::array<float, 4> colour{1, 1, 1, 1};
  // kTexture: the owning material's id, the path, the texture matrix
  // (16 floats as stored), and the 52-byte sampler tail (13 words).
  uint32_t owner = 0;
  std::string path;
  std::array<float, 16> matrix{};
  float tex_rotation = 0.0f;
  float tex_u_offset = 0.0f, tex_v_offset = 0.0f;
  float tex_u_scale = 1.0f, tex_v_scale = 1.0f;
  float tex_pivot_u = 0.5f, tex_pivot_v = 0.5f;
  uint32_t tex_flag = 0;
  uint32_t tex_address_mode = 4;  // 4: transparent-border crop, 3: clamp/wrap
  std::array<uint32_t, 4> tex_sampler_enums{};
};

struct UiNode {
  int parent = -1;     // node index; parents precede children
  uint32_t object = 0;  // FBF object id
  std::array<float, 3> position{}, rotation{}, scale{1, 1, 1};
  std::array<float, 3> bounds_min{}, bounds_max{};
  float opacity = 1;
  bool drawable = false;
  // Resolved: the object's index in UiScene::objects (-1: none), and the
  // node on the canvas: centre (pixels, y up from the canvas centre),
  // z depth (BSG painter order), rotation about z (radians), scale, and
  // opacity times the parents'.
  int object_index = -1;
  float x = 0, y = 0, z = 0, angle = 0, sx = 1, sy = 1, sz = 1, alpha = 1;
};

struct UiGeometry {
  uint32_t index = 0;
  std::array<float, 3> bounds_min{};
  std::array<float, 3> bounds_max{};
  uint32_t gpu_offset = 0;
  uint32_t gpu_fetch_word = 0;
  uint32_t extra_offset = 0;
  bool is_unit_quad = true;
  // 2D local-space vertices (x, y) extracted for non-quad meshes (or empty for unit quads).
  std::vector<std::array<float, 2>> vertices;
};

struct UiBgfItem {
  uint32_t name_hash = 0;
  uint32_t parent_hash = 0;
  uint32_t value = 0;  // FBF native object id for kinds 1..7; 0 for contracts (kind 8)
  uint8_t kind = 0;
  uint8_t flags = 0;
  bool is_wrapper = false;
  std::vector<std::pair<uint32_t, uint32_t>> properties;  // (attribute hash, raw value)
};

struct UiContract {
  int item_index = -1;
  uint32_t owner_hash = 0;
  std::string type;  // e.g. "SUPER_STACKER", "PAUSE_MENU_BUTTON", "HUD_COMBINED_NOTIFICATION"
  std::string initial_state;  // e.g. "SHOWN", "HIDDEN" (property attribute 0x083BF5F8)
  std::vector<std::string> bindings;
  std::vector<std::string> events;
};

struct UiBgfRelation {
  uint32_t parent = 0;
  std::vector<uint32_t> children;
};

enum class UiBgfPropTarget : uint8_t {
  kUnknown = 0,
  kPosX,
  kPosY,
  kScaleX,
  kScaleY,
  kRotation,
  kColorR,
  kColorG,
  kColorB,
  kColorRgba,
  kAlpha,
};

// Clean-room FNV-1a 32-bit property attribute hashes recognized by classify_bgf_property().
namespace bgf_attr {
inline constexpr uint32_t fnv1a_32(std::string_view s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}
inline constexpr uint32_t kPosX = fnv1a_32("position.x");
inline constexpr uint32_t kPosY = fnv1a_32("position.y");
inline constexpr uint32_t kScaleX = fnv1a_32("scale.x");
inline constexpr uint32_t kScaleY = fnv1a_32("scale.y");
inline constexpr uint32_t kRotation = fnv1a_32("rotation");
inline constexpr uint32_t kColorR = fnv1a_32("color.r");
inline constexpr uint32_t kColorG = fnv1a_32("color.g");
inline constexpr uint32_t kColorB = fnv1a_32("color.b");
inline constexpr uint32_t kColorRgba = fnv1a_32("color");
inline constexpr uint32_t kAlpha = fnv1a_32("alpha");
}  // namespace bgf_attr

// Maps a BGF attribute hash and/or channel tag byte to a semantic property target.
UiBgfPropTarget classify_bgf_property(uint32_t attr_hash, uint32_t raw_tag = 0);

struct UiBgfStateRecord {
  uint32_t state_hash = 0;
  uint32_t target_item = 0;
  uint32_t attr_hash = 0;
  uint32_t value_type = 0;
  uint32_t raw_value = 0;
  float value = 0.0f;
  uint32_t extra = 0;
  UiBgfPropTarget target = UiBgfPropTarget::kUnknown;
};

struct UiBgfStyle {
  uint32_t owner_item = 0;
  uint16_t state_count = 0;
  uint32_t style_id = 0;
  std::vector<UiBgfStateRecord> states;
};

struct UiBgfKeyframe {
  uint32_t target_item = 0;
  uint32_t attr_hash = 0;
  float time = 0.0f;
  float value = 0.0f;
  uint32_t raw_tag = 0;
  uint32_t raw_value = 0;
  UiBgfPropTarget target = UiBgfPropTarget::kUnknown;
};

struct UiBgfTrack {
  uint32_t owner_item = 0;
  std::vector<UiBgfKeyframe> keys;
  uint32_t target_item = 0;
  uint32_t attr_hash = 0;
  UiBgfPropTarget target = UiBgfPropTarget::kUnknown;
};

struct UiBgfAnimKey {
  float time = 0.0f;
  float value = 0.0f;
  uint32_t raw_value = 0;
};

struct UiBgfAnimChannel {
  uint32_t attr_hash = 0;
  uint8_t channel_tag = 0;
  UiBgfPropTarget target = UiBgfPropTarget::kUnknown;
  std::vector<UiBgfAnimKey> keys;
  std::vector<uint32_t> events;
};

struct UiBgfAnimation {
  uint32_t owner_item = 0;
  uint32_t track_count = 0;
  uint32_t key_count = 0;
  uint32_t event_count = 0;
  uint32_t name_hash = 0;
  float duration = 0.0f;
  uint8_t flags = 0;
  std::vector<UiBgfAnimChannel> channels;
};

struct UiBgfDocument {
  uint32_t element_count = 0;
  uint32_t wrapper_count = 0;
  uint32_t property_count = 0;
  std::vector<std::string> identities;
  std::vector<UiBgfItem> items;
  std::vector<UiContract> contracts;
  std::vector<UiBgfRelation> relations;
  std::vector<UiBgfStyle> styles;
  std::vector<UiBgfTrack> tracks;
  std::vector<UiBgfAnimation> animations;
  bool has_contract(std::string_view type) const;
  std::vector<const UiContract*> find_contracts(std::string_view type) const;
};

struct UiScene {
  uint32_t version = 0;
  std::vector<UiObject> objects;  // in file order
  std::vector<UiNode> nodes;
  // The pool's ids that are not nodes (materials, textures).
  std::vector<uint32_t> pool;
  // FBF geometry descriptors (unit quads and non-quad meshes).
  std::vector<UiGeometry> geometries;
  // Optional BGF document (contracts, states, tracks, animations).
  UiBgfDocument bgf;
  bool has_bgf = false;
  int find_object(uint32_t id) const;
  const UiGeometry* geometry_of(int node_index) const;
};

// Reads the FBF's objects and geometry records (unit quads + non-quad meshes).
bool parse_ui_fbf(std::span<const uint8_t> data, UiScene& out, std::string* error);
// Reads the BSG's nodes and pool, links them to the objects and resolves
// the canvas transforms. Call after parse_ui_fbf.
bool parse_ui_bsg(std::span<const uint8_t> data, UiScene& out, std::string* error);
// Reads the BGF document: identity pool, items, contracts, relations, styles,
// tracks/keyframes, and animations.
bool parse_ui_bgf(std::span<const uint8_t> data, UiBgfDocument& out, std::string* error);

// Evaluates a BGF keyframe track or animation channel linearly at `time_s`.
float evaluate_bgf_track(const UiBgfTrack& track, float time_s, float fallback = 0.0f);
float evaluate_bgf_anim_channel(const UiBgfAnimChannel& ch, float time_s, float fallback = 0.0f);

// SUPER_STACKER reflow: computes packed Y (or X) positions for `count` visible
// rows starting at `origin` with pitch `step` (negative for top-down in scene y-up
// or positive for y-down canvas coordinates).
std::vector<float> reflow_stacker(float origin, float step, int count);

// The texture archive member for a scene's texture path: `horizon` true
// for Horizon.zip, else Textures.zip; the member name ends .xds.
std::string ui_texture_member(const std::string& path, bool& horizon);

// Font mapping from media/ui/Fonts.zip/fontmap.xml: maps FBF font names
// (case-insensitive, stripping any style suffix after the first space) to
// canonical target names ("A", "B", "C", "D", "E", "SYM", "DG1".."DG5").
struct UiFontMap {
  std::unordered_map<std::string, std::string> mappings;  // lower(fontname) -> upper(target)
  std::string resolve(std::string_view font_name) const;
};
bool parse_fontmap_xml(std::string_view xml, UiFontMap& out);

// Loading tips parsed from media/ui/LoadingDefs.xml.
struct UiLoadingTip {
  std::string group;        // e.g. "freeroam", "festivalevent", "paintshop", "workshop"
  std::string title_key;    // e.g. "Tips:IDS_PopularityTitle"
  std::string message_key;  // e.g. "Tips:IDS_PopularityTip"
  int layout = 1;
  float display_time = 8.0f;
};
bool parse_loading_defs_xml(std::string_view xml, std::vector<UiLoadingTip>& out);

}  // namespace fh1
