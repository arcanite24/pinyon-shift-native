// FH1 UI scenes (media/UI.zip Scenes/ui4/<name>.fbf + .bsg): the static
// layout of a screen. The FBF holds the native objects (groups, text boxes,
// models, materials, textures), the BSG the scene graph (one 96-byte node
// per drawable or group: parent, object, position, rotation, scale, bounds,
// opacity). The BGF holds the items, contracts and the timeline (slides,
// curves, actions; docs/UI_SCENE_ANIMATION.md). Text content and list rows
// are set at run time by the title's UI code and are not here.
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
  // Version >= 1007 59-byte text effect extension: drop shadow enable, RGBA, (dx, dy).
  bool text_has_shadow = false;
  std::array<float, 4> text_shadow_colour{0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 2> text_shadow_offset{0.0f, 0.0f};
  // Base text colour from companion BGF item's textcolor.r/g/b/a (0..1), when present.
  bool has_text_colour = false;
  std::array<float, 4> text_colour{1.0f, 1.0f, 1.0f, 1.0f};
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

// The scene's timeline (docs/UI_SCENE_ANIMATION.md). The runtime is Anark
// Gameface's: every component (the scene root and each wrapper item) has
// slides, a "Master Slide" first; a slide sets its elements' properties when
// it starts, plays its curves over its time range and holds the actions its
// events run (go to a slide, set a property). Names are hashed with
// `ui_name_hash`; attributes are its low 27 bits (`ui_attr_hash`), with the
// value's type in the tag's top 5 bits.
constexpr uint32_t ui_name_hash(std::string_view s) {
  uint32_t h = 0;
  for (unsigned char c : s) h = (h + c) * 65599u;
  return h;
}
constexpr uint32_t ui_attr_hash(std::string_view s) { return ui_name_hash(s) & 0x07FFFFFFu; }

namespace ui_attr {
inline constexpr uint32_t kOpacity = ui_attr_hash("opacity");    // percent, 0..100
inline constexpr uint32_t kEndTime = ui_attr_hash("endtime");    // ms, int
inline constexpr uint32_t kStartTime = ui_attr_hash("starttime");
inline constexpr uint32_t kPositionX = ui_attr_hash("position.x");  // scene units, y up
inline constexpr uint32_t kPositionY = ui_attr_hash("position.y");
inline constexpr uint32_t kPositionZ = ui_attr_hash("position.z");
inline constexpr uint32_t kScaleX = ui_attr_hash("scale.x");
inline constexpr uint32_t kScaleY = ui_attr_hash("scale.y");
inline constexpr uint32_t kScaleZ = ui_attr_hash("scale.z");
inline constexpr uint32_t kRotationX = ui_attr_hash("rotation.x");  // radians
inline constexpr uint32_t kRotationY = ui_attr_hash("rotation.y");
inline constexpr uint32_t kRotationZ = ui_attr_hash("rotation.z");
inline constexpr uint32_t kPivotX = ui_attr_hash("pivot.x");
inline constexpr uint32_t kPivotY = ui_attr_hash("pivot.y");
inline constexpr uint32_t kDiffuseR = ui_attr_hash("diffuse.r");  // material, 0..255
inline constexpr uint32_t kDiffuseG = ui_attr_hash("diffuse.g");
inline constexpr uint32_t kDiffuseB = ui_attr_hash("diffuse.b");
inline constexpr uint32_t kDiffuseA = ui_attr_hash("diffuse.a");
inline constexpr uint32_t kTextColorR = ui_attr_hash("textcolor.r");  // text, 0..255
inline constexpr uint32_t kTextColorG = ui_attr_hash("textcolor.g");
inline constexpr uint32_t kTextColorB = ui_attr_hash("textcolor.b");
inline constexpr uint32_t kTextColorA = ui_attr_hash("textcolor.a");
inline constexpr uint32_t kPositionU = ui_attr_hash("positionu");  // texture offset
inline constexpr uint32_t kPositionV = ui_attr_hash("positionv");
}  // namespace ui_attr

// A tagged property value: the attribute (27 bits), its type (the tag's top 5
// bits; their low 3: 1 int, 3 float, 5 string) and the raw 32-bit value.
struct UiBgfValue {
  uint32_t attr = 0;
  uint8_t type = 0;
  uint32_t raw = 0;
  float f() const;
  float number() const;  // an int's value, else the float's
};
// A tagged raw value as a number (an int's value, else the float's).
float ui_bgf_number(uint32_t tag, uint32_t raw);

// A key of an animation curve. The value between key i and i + 1 is the
// cubic Bezier (value_i, c1_value_i, c2_value_i, value_i+1) at s = the
// normalised time. The control times are 0 in every retail key, which the
// title's evaluator (82F2EF08) takes as s linear in time.
struct UiBgfCurveKey {
  float time_ms = 0, value = 0;
  float c1_time = 0, c1_value = 0;
  float c2_time = 0, c2_value = 0;
};

// An animation curve: one attribute of one element (BGF item index).
struct UiBgfCurve {
  uint32_t item = 0;
  uint32_t attr = 0;
  bool flag = true;      // header byte 8 (1 in every retail curve)
  bool dynamic = false;  // header byte 9: the first key starts from the current value
  std::vector<UiBgfCurveKey> keys;
};

// An action: when `event` reaches its owner, run `handler` on `target`.
struct UiBgfAction {
  uint32_t owner = 0;    // the item that holds it (a contract or behaviour)
  uint32_t event = 0;    // the event's name hash (compare 27 bits)
  uint32_t source = 0, target = 0;  // item indexes
  uint32_t handler = 0;  // kGotoSlide, kSetProperty, ...
  uint32_t arg_tag = 0;  // GOTO: the slide name hash; SET: the attribute tag; GOTO_TIME: ms
  uint32_t arg = 0;      // SET: the value
};
namespace ui_handler {
inline constexpr uint32_t kGotoSlide = 0x1195C23Bu;    // arg_tag: slide name hash
inline constexpr uint32_t kSetProperty = 0x7E4906FDu;  // arg_tag: attribute tag, arg: value
inline constexpr uint32_t kGotoTime = 0x5B2FA69Cu;     // arg_tag: ms (the pause menu's SIZEn)
inline constexpr uint32_t kPause = 0x4F15F91Eu;        // paired with kGotoTime (probable)
inline constexpr uint32_t kPlay = 0x573BA2B8u;         // no argument (probable)
}  // namespace ui_handler

// What a slide does with one element: whether it is active (on), the values
// set when the slide starts, and the curves and actions it lists (`enabled`
// false: listed but not run in this slide; the Master Slide lists every one
// disabled).
struct UiBgfSlideRef {
  bool action = false;
  bool enabled = false;
  uint16_t index = 0;  // into UiBgfDocument::curves or ::actions
};
struct UiBgfSlideTrack {
  uint32_t item = 0;
  bool active = false;
  std::vector<UiBgfValue> values;
  std::vector<UiBgfSlideRef> refs;
};

// A slide. `flags` is the stored byte: bits 0..1 the repetitions (0 loop
// forever, 1 play once), bit 2 starts paused (the game sets its time), bit 3
// set on nearly every play-once slide (effect unknown, Q183), bit 4 plays
// through to the next slide at its end.
struct UiBgfSlide {
  std::string name;
  uint32_t component = 0;  // the owning component's item index (0: the scene)
  uint8_t flags = 0;
  uint32_t start_ms = 0, end_ms = 0;
  std::vector<UiBgfSlideTrack> tracks;
  bool loops() const { return (flags & 3u) == 0; }
  bool paused() const { return (flags & 4u) != 0; }
  bool play_through() const { return (flags & 16u) != 0; }
  bool master() const { return name == "Master Slide"; }
  float duration_ms() const { return float(end_ms) - float(start_ms); }
};

struct UiBgfDocument {
  uint32_t element_count = 0;
  uint32_t wrapper_count = 0;
  uint32_t property_count = 0;
  std::vector<std::string> identities;
  std::vector<UiBgfItem> items;
  std::vector<UiContract> contracts;
  std::vector<UiBgfRelation> relations;
  // The timeline: false when its sections did not read to their
  // declarations (the layout above still holds).
  bool has_timeline = false;
  std::vector<UiBgfCurve> curves;
  std::vector<UiBgfAction> actions;
  std::vector<UiBgfSlide> slides;
  bool has_contract(std::string_view type) const;
  std::vector<const UiContract*> find_contracts(std::string_view type) const;
  // The slides of a component in file order (its Master Slide first).
  std::vector<int> slides_of(uint32_t component) const;
  // A component's slide by name (exact, case-sensitive); -1: none.
  int find_slide(uint32_t component, std::string_view name) const;
  // The item whose FBF object id is `object` (kinds 1..7); -1: none.
  int item_of_object(uint32_t object) const;
};

struct UiScene {
  uint32_t version = 0;
  std::vector<UiObject> objects;  // in file order
  std::vector<UiNode> nodes;
  // The pool's ids that are not nodes (materials, textures).
  std::vector<uint32_t> pool;
  // FBF geometry descriptors (unit quads and non-quad meshes).
  std::vector<UiGeometry> geometries;
  // Optional BGF document (items, contracts, the timeline).
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
// Reads the BGF document: identity pool, items, contracts, relations and the
// timeline (curves, actions, slides).
bool parse_ui_bgf(std::span<const uint8_t> data, UiBgfDocument& out, std::string* error);
// Populates kText UiObject base text_colour from the parsed BGF's kind==4 items.
void apply_ui_bgf_defaults(UiScene& out);

class Archive;
// Loads Scenes/ui4/<name>.fbf / .bsg (and .bgf when present) from a UI.zip:
// the app's scene resolver and the scene corpus tests share it.
bool load_ui_scene(Archive& ui_zip, const std::string& name, UiScene& out, std::string* error);

// A curve's value at `time_ms` (the slide's time), as the title evaluates
// it (82F2EF08): before the first key its value, after the last the last;
// between keys the cubic Bezier on the values with s linear in time.
float evaluate_bgf_curve(const UiBgfCurve& curve, float time_ms);
// A dynamic curve started from `start` (82F2D4B0): the first key takes the
// value and its control values keep their place between it and the next.
void restart_bgf_curve(UiBgfCurve& curve, float start);

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
  // The group's own attributes: how many of its tips one load shows
  // (`tipcount`) and whether they are picked at random or in file order
  // (`order="sequential"`).
  int group_tip_count = 1;
  bool group_sequential = false;
};
bool parse_loading_defs_xml(std::string_view xml, std::vector<UiLoadingTip>& out);

}  // namespace fh1
