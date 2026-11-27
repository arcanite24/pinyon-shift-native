// Cutscene library: synthetic CutsceneManager documents written here (no
// game data). Covers the parser, the key interpolation rule, fades, event
// firing, looping/hold and the world framing conventions.
#include <cmath>
#include <cstdio>
#include <string>

#include "cutscene/cutscene.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

const char* kDoc = R"(<?xml version="1.0"?>
<CutsceneManager>
  <!-- a cutscene with only events and no Cams element -->
  <Cutscene name="EventsOnly" PlaybackMode="Normal" Duration="5.0" MirrorForRightHandDrive="0" LoopFrom="0.0">
    <Events>
      <Event time="1.0">
        <EventTrigger name="A" id="CCutsceneAudioTrigger" group="G/H" event="Whoosh" timeLimit="-1.0"/>
      </Event>
    </Events>
  </Cutscene>
  <Cutscene name="Test_Scene" PlaybackMode="Normal" Duration="6.0" MirrorForRightHandDrive="0" LoopFrom="4.0">
    <Cams CutQueueType="Cutscene" NumCams="2">
      <Cam Version="3" Group="0" Type="Animateable" StartCut="0.0" TargetName="Node_A" duration="2.0" DisableCSM="1" ApplyMotionBlur="1">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" TimeAnimType="AffectsPath" IsInCockpit="0" ConstVelCamSpeed="0" EaseIn="0" EaseOut="0" NumKeys="2">
          <Key Time="0.0" PosX="0" PosY="1" PosZ="-10" Yaw="0" Pitch="0" Roll="0" FOV="40" TimeC0="0"/>
          <Key Time="2.0" PosX="4" PosY="1" PosZ="-10" Yaw="90" Pitch="10" Roll="0" FOV="60" TimeC0="0"/>
        </Anim>
        <PostEffectsAnim DOFMode="Manual" MotionBlurAmount="0.25" NumKeys="2">
          <Key Time="0.0" FocalDistance="2" FocalLength="5" FocalMaxBlur="7" Saturation="0" Exposure="0" Brightness="0" Contrast="0" Sepia="0" Blur="0"/>
          <Key Time="1.0" FocalDistance="4" FocalLength="5" FocalMaxBlur="3" Saturation="0" Exposure="0.5" Brightness="0" Contrast="0" Sepia="0" Blur="0"/>
        </PostEffectsAnim>
        <FadeInOptions Duration="1.0" HoldDuration="0.5" CurveType="S" Color.x="0" Color.y="0" Color.z="0" Color.w="0"/>
        <FadeOutOptions Duration="0.0"/>
        <CameraPhysics NumLayers="0"/>
      </Cam>
      <Cam Version="3" Group="0" Type="Animateable" StartCut="2.0" TargetName="" duration="4.0">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" TimeAnimType="AffectsPath" IsInCockpit="1" NumKeys="5">
          <Key Time="0.0" PosX="0" PosY="0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="50" TimeC0="0"/>
          <Key Time="0.5" PosX="1" PosY="0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="50" TimeC0="0"/>
          <Key Time="1.5" PosX="2" PosY="0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="50" TimeC0="0"/>
          <Key Time="2.0" PosX="3" PosY="0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="50" TimeC0="0"/>
          <Key Time="4.0" PosX="4" PosY="2" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="50" TimeC0="0"/>
        </Anim>
        <PostEffectsAnim DOFMode="Off" MotionBlurAmount="0" NumKeys="0"/>
        <FadeInOptions Duration="0.0"/>
        <FadeOutOptions Duration="1.0" CurveType="Linear" Color.x="1" Color.y="1" Color.z="1" Color.w="0"/>
      </Cam>
    </Cams>
    <Events>
      <Event time="0.0">
        <EventTrigger name="Node_A" id="CCutsceneCameraNodeTrigger" x="100" y="5" z="200" rotation="90" parent="" snapToGround="0"/>
      </Event>
      <Event time="3.0">
        <EventTrigger name="PlacePlayerCar_0" id="CCutscenePlaceCarTrigger" x="1" y="2" z="3" rotation="-45" timeTillRelease="0" initialSpeed="100"/>
      </Event>
      <Event time="1.5">
        <EventTrigger name="" id="CCutsceneVoiceOverTrigger" event="Hello" exitCutsceneOnFinish="1"/>
        <EventTrigger name="" id="CSomethingNew" foo="bar"/>
      </Event>
      <Event time="5.0">
        <EventTrigger name="Snap" id="CCutsceneAudioMixSnapshotTrigger" snapshot="FreeRoam"/>
      </Event>
    </Events>
  </Cutscene>
  <Cutscene name="Parts" PlaybackMode="Normal" Duration="4.0" MirrorForRightHandDrive="0" LoopFrom="0.0">
    <Cams CutQueueType="Cutscene" NumCams="2">
      <Cam Version="3" Group="0" Type="Animateable" StartCut="0.0" TargetName="" duration="2.0">
        <Anim PosAnimType="PartSpace" RotAnimType="CarSpace" TimeAnimType="AffectsPath" IsInCockpit="0" NumKeys="2">
          <Key Time="0.0" PosX="0" PosY="0" PosZ="1" Yaw="0" Pitch="0" Roll="0" FOV="40" TimeC0="0" CarPart="headlightL"/>
          <Key Time="2.0" PosX="0" PosY="0" PosZ="1" Yaw="0" Pitch="0" Roll="0" FOV="40" TimeC0="0" CarPart="headlightL"/>
        </Anim>
      </Cam>
      <Cam Version="3" Group="0" Type="Animateable" StartCut="2.0" TargetName="Late_Node" duration="2.0">
        <Anim PosAnimType="PartSpace" RotAnimType="CarSpace" TimeAnimType="AffectsPath" IsInCockpit="0" NumKeys="2">
          <Key Time="0.0" PosX="0" PosY="0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="40" TimeC0="0" CarPart="wheelRR"/>
          <Key Time="2.0" PosX="0" PosY="0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="40" TimeC0="0" CarPart="wheelRR"/>
        </Anim>
      </Cam>
    </Cams>
    <Events>
      <Event time="3.0">
        <EventTrigger name="Late_Node" id="CCutsceneCameraNodeTrigger" x="50" y="0" z="0" rotation="0"/>
      </Event>
    </Events>
  </Cutscene>
</CutsceneManager>
)";

void test_parse() {
  cutscene::Library lib;
  std::string err;
  check(cutscene::parse_cutscenes(kDoc, lib, &err), "parses the synthetic document");
  check(lib.cutscenes.size() == 3, "three cutscenes");
  const auto* eo = lib.find("eventsonly");
  check(eo && eo->cams.empty() && eo->declared_cams == 0 && eo->events.size() == 1, "events-only cutscene keeps its event");
  const auto* cs = lib.find("Test_Scene");
  check(cs != nullptr, "find is case-insensitive");
  if (!cs) return;
  check(near(cs->duration, 6.0) && near(cs->loop_from, 4.0), "duration and loop-from");
  check(cs->mode == cutscene::PlaybackMode::kNormal, "Normal playback");
  check(cs->declared_cams == 2 && cs->cams.size() == 2, "two cams");
  const auto& c0 = cs->cams[0];
  check(c0.target_name == "Node_A" && c0.disable_csm && c0.apply_motion_blur, "cam flags");
  check(c0.keys.size() == 2 && near(c0.keys[1].yaw, 90) && near(c0.keys[1].fov, 60), "keys read");
  check(c0.dof == cutscene::DofMode::kManual && near(c0.motion_blur, 0.25) && c0.post_keys.size() == 2, "post keys read");
  check(near(c0.fade_in.duration, 1.0) && near(c0.fade_in.hold, 0.5) && c0.fade_in.curve == cutscene::CurveType::kS,
        "fade-in options");
  check(cs->cams[1].in_cockpit && near(cs->cams[1].fade_out.color[0], 1.0f), "cockpit flag and white fade-out");
  check(cs->events.size() == 5, "five event triggers");
  check(cs->events[0].type == cutscene::EventType::kCameraNode && near(cs->events[0].time, 0.0), "events sorted by time");
  check(cs->events.back().type == cutscene::EventType::kMixSnapshot && cs->events.back().attr("snapshot") == "FreeRoam",
        "mix snapshot attributes");
  bool saw_other = false, saw_vo = false;
  for (const auto& e : cs->events) {
    if (e.type == cutscene::EventType::kOther) saw_other = e.attr("foo") == "bar";
    if (e.type == cutscene::EventType::kVoiceOver) saw_vo = e.number("exitCutsceneOnFinish") == 1.0;
  }
  check(saw_other && saw_vo, "unknown triggers are kept, VO exit flag read");
  check(cs->cam_at(0.0) == 0 && cs->cam_at(1.99) == 0 && cs->cam_at(2.0) == 1 && cs->cam_at(99.0) == 1, "cam_at");

  cutscene::Library bad;
  check(!cutscene::parse_cutscenes("<ActivityManager/>", bad, &err), "rejects a non-cutscene root");
}

void test_interpolation() {
  cutscene::Library lib;
  std::string err;
  cutscene::parse_cutscenes(kDoc, lib, &err);
  const auto* cs = lib.find("Test_Scene");
  if (!cs) return;
  // Two keys: the time map and the spline are linear.
  const auto& two = cs->cams[0].keys;
  check(near(cutscene::key_parameter(two, 0.5), 0.25), "two keys: linear time map");
  const auto p = cutscene::sample_pose(cs->cams[0], 1.0);
  check(near(p.pos[0], 2.0) && near(p.yaw, 45.0) && near(p.fov, 50.0), "two keys: linear pose at the midpoint");
  // Five keys, uneven times: exact at the keys, monotonic in between.
  const auto& five = cs->cams[1].keys;
  for (size_t k = 0; k < five.size(); ++k)
    check(near(cutscene::key_parameter(five, five[k].time), double(k), 1e-9), "time map hits every key exactly");
  double prev = -1;
  bool mono = true;
  for (double t = 0; t <= 4.0; t += 0.01) {
    const double u = cutscene::key_parameter(five, t);
    if (u < prev - 1e-12) mono = false;
    prev = u;
  }
  check(mono, "time map is monotonic");
  check(near(cutscene::key_parameter(five, -1.0), 0.0) && near(cutscene::key_parameter(five, 9.0), 4.0),
        "time map clamps to the key range");
  // The path passes through each key and, for collinear evenly spaced x, is
  // linear in the key index (Catmull-Rom reproduces linear data).
  for (size_t k = 0; k < five.size(); ++k) {
    const auto q = cutscene::pose_at_parameter(five, double(k));
    check(near(q.pos[0], five[k].pos[0]) && near(q.pos[1], five[k].pos[1]), "spline passes through the keys");
  }
  check(near(cutscene::pose_at_parameter(five, 1.5).pos[0], 1.5), "Catmull-Rom is linear on linear data");
  // Interior tangents 0.5*(next-prev); last tangent one-sided: y on the last
  // segment (0,0,0,0,2) at f = 0.5 is h10*m0 + h01*2 + h11*m1 with m0 = 1,
  // m1 = 2: 0.125 + 1.0 - 0.25 = 0.875.
  check(near(cutscene::pose_at_parameter(five, 3.5).pos[1], 0.875), "one-sided end tangent");
  check(near(cutscene::pose_at_parameter(five, 2.5).pos[1], -0.125), "interior Catmull-Rom tangent overshoot");
}

void test_post_and_fades() {
  cutscene::Library lib;
  std::string err;
  cutscene::parse_cutscenes(kDoc, lib, &err);
  const auto* cs = lib.find("Test_Scene");
  if (!cs) return;
  const auto ps = cutscene::sample_post(cs->cams[0], 0.5);
  check(ps.dof && near(ps.focal_distance, 3.0) && near(ps.exposure, 0.25), "post keys interpolate");
  check(near(cutscene::sample_post(cs->cams[0], 5.0).focal_max_blur, 3.0), "post keys hold past the end");
  check(near(cutscene::fade_curve(cutscene::CurveType::kS, 0.5), 0.5) &&
            cutscene::fade_curve(cutscene::CurveType::kS, 0.25) < 0.25,
        "S curve eases in");
  check(near(cutscene::cutscene_overlay(*cs, 0.2).alpha, 1.0), "fade-in holds the colour first");
  const float mid = cutscene::cutscene_overlay(*cs, 1.0).alpha;
  check(mid > 0.0f && mid < 1.0f, "fade-in in progress");
  check(near(cutscene::cutscene_overlay(*cs, 1.6).alpha, 0.0), "fade-in done");
  check(near(cutscene::cutscene_overlay(*cs, 4.0).alpha, 0.0), "no fade mid-cam");
  const auto out = cutscene::cutscene_overlay(*cs, 5.5);
  check(near(out.alpha, 0.5, 1e-5) && near(out.rgb[0], 1.0), "linear white fade-out over the cam's last second");
}

void test_player_and_world() {
  cutscene::Library lib;
  std::string err;
  cutscene::parse_cutscenes(kDoc, lib, &err);
  const auto* cs = lib.find("Test_Scene");
  if (!cs) return;
  cutscene::Player pl;
  pl.start(cs);
  auto fired = pl.step(0.0);
  check(fired.size() == 1 && fired[0]->type == cutscene::EventType::kCameraNode, "t=0 events fire on start");
  check(pl.nodes().size() == 1, "camera node recorded");
  // The first cam is relative to Node_A (heading 90: forward is +X).
  cutscene::WorldCamera w;
  const cutscene::Frame car{{0, 0, 0}, 0};
  check(pl.world_camera(car, w), "world camera available");
  check(near(w.eye[0], 100 - 10, 1e-6) && near(w.eye[1], 6) && near(w.eye[2], 200, 1e-6),
        "node frame: local -Z is world -X at heading 90");
  check(near(w.forward[0], 1, 1e-9) && near(w.up[1], 1, 1e-9), "yaw 0 looks along the node's heading");
  fired = pl.step(1.6);
  check(fired.size() == 2, "VO and the unknown trigger at 1.5 s fire once");
  fired = pl.step(1.6);
  check(fired.size() == 1 && fired[0]->type == cutscene::EventType::kPlacePlayerCar, "place-car at 3.0 s");
  check(pl.world_camera(car, w) && w.interior, "second cam (no target) is relative to the car, in the cockpit");
  // Hold: while the exit-on-finish voice-over (fired at 1.5 s) plays, the
  // cutscene loops from LoopFrom; it ends when the voice-over does.
  pl.set_hold(true);
  pl.set_voice_playing(true);
  check(pl.holding(), "holding while the voice-over plays");
  fired = pl.step(2.5);  // 3.2 + 2.5 = 5.7
  check(pl.active() && fired.size() == 1, "snapshot at 5.0 fires");
  fired = pl.step(0.5);  // past 6.0 -> wraps to loop_from
  check(pl.active() && pl.time() >= 4.0 && pl.time() < 6.0, "held cutscene loops from LoopFrom");
  check(pl.step(1.0).empty(), "events do not refire on loop");
  pl.set_voice_playing(false);
  pl.step(0.01);
  check(pl.finished() && !pl.active(), "the cutscene ends with its voice-over");

  // A voice-over that never starts: the hold lapses and the cutscene ends at
  // its duration instead of looping for ever.
  cutscene::Player quiet;
  quiet.start(cs);
  quiet.set_hold(true);
  quiet.step(0.0);
  quiet.step(1.6);  // VO fires
  check(quiet.holding(), "pending voice-over holds briefly");
  quiet.step(2.0);
  check(!quiet.holding(), "hold lapses when the voice-over never plays");
  quiet.step(3.0);
  check(quiet.finished(), "unvoiced cutscene finishes at its duration");

  // Pose conventions: yaw +90 looks along +X; pitch +10 looks down.
  cutscene::Pose pose;
  pose.yaw = 90;
  pose.pitch = 10;
  const auto c = cutscene::to_world(pose, cutscene::Frame{{0, 0, 0}, 0});
  check(c.forward[0] > 0.98 && c.forward[1] < -0.17 && c.forward[1] > -0.18, "yaw right, pitch down");
  cutscene::Pose rolled;
  rolled.roll = 90;
  const auto r = cutscene::to_world(rolled, cutscene::Frame{{0, 0, 0}, 0});
  check(near(r.up[0], -1.0, 1e-9), "positive roll leans the top left");
  const auto h = cutscene::to_world(cutscene::Pose{{1, 0, 0}, 0, 0, 0, 50}, cutscene::Frame{{10, 0, 10}, 180});
  check(near(h.eye[0], 9, 1e-9) && near(h.eye[2], 10, 1e-9) && near(h.forward[2], -1, 1e-9),
        "heading 180 mirrors the local axes");
}

void test_parts_and_nodes() {
  cutscene::Library lib;
  std::string err;
  cutscene::parse_cutscenes(kDoc, lib, &err);
  const auto* cs = lib.find("Parts");
  check(cs != nullptr, "parts cutscene parsed");
  if (!cs) return;
  cutscene::Player pl;
  pl.start(cs);
  pl.step(0.0);
  const cutscene::CarParts parts = {{"headlightL", {-0.7, 0.6, 2.0}}, {"wheelRR", {0.8, 0.3, -1.3}}};
  cutscene::WorldCamera w;
  // Car at the origin facing +X (heading 90): the left headlight is at
  // world (2, 0.6, 0.7); the cam 1 m ahead of it.
  const cutscene::Frame car{{0, 0, 0}, 90};
  check(pl.world_camera(car, w, &parts) && near(w.eye[0], 3.0, 1e-9) && near(w.eye[1], 0.6, 1e-9) &&
            near(w.eye[2], 0.7, 1e-9),
        "PartSpace cam follows its car part");
  check(pl.world_camera(car, w) && near(w.eye[0], 1.0, 1e-9), "no parts: the car's origin");
  // The second cut targets a node whose trigger fires later: resolved by
  // name, its part ignored.
  pl.step(2.5);
  check(pl.world_camera(car, w, &parts) && near(w.eye[0], 50.0, 1e-9) && near(w.eye[2], 0.0, 1e-9),
        "a node not fired yet is found by name; its part is ignored");
  // initialSpeed is mph.
  cutscene::Event e;
  e.attributes = {{"initialSpeed", "100"}};
  check(near(cutscene::place_speed_mps(e), 44.704, 1e-9), "initialSpeed 100 mph is 44.704 m/s");
}

// A node parented to the placed car (Skills_Unlock's CameraTargetNode_0 on
// PlacePlayerCar_0): its x/y/z and rotation are in the car's frame.
void test_parented_node() {
  static const char* kParented = R"(<CutsceneManager>
  <Cutscene name="Parented" PlaybackMode="Normal" Duration="4.0" LoopFrom="0.0">
    <Cams CutQueueType="Cutscene" NumCams="1">
      <Cam Version="3" Type="Animateable" StartCut="0.0" TargetName="Node_0" duration="4.0">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" NumKeys="1">
          <Key Time="0.0" PosX="0" PosY="1" PosZ="-5" Yaw="0" Pitch="0" Roll="0" FOV="40"/>
        </Anim>
      </Cam>
    </Cams>
    <Events>
      <Event time="0.0"><EventTrigger name="PlacePlayerCar_0" id="CCutscenePlaceCarTrigger" x="100" y="10" z="200" rotation="90" parent=""/></Event>
      <Event time="0.5"><EventTrigger name="Node_0" id="CCutsceneCameraNodeTrigger" x="0" y="0" z="2" rotation="10" parent="PlacePlayerCar_0"/></Event>
    </Events>
  </Cutscene>
</CutsceneManager>)";
  cutscene::Library lib;
  std::string err;
  check(cutscene::parse_cutscenes(kParented, lib, &err), "parented-node document parses");
  const auto* cs = lib.find("Parented");
  if (!cs) return;
  cutscene::Player pl;
  pl.start(cs);
  pl.step(0.0);
  // Before the node fires, found by name; after, the fired frame: the car
  // faces +X (90), so 2 m ahead is (102, 10, 200), heading 100.
  const cutscene::Frame far_car{{0, 0, 0}, 0};
  for (double dt : {0.1, 1.0}) {
    pl.step(dt);
    cutscene::WorldCamera w;
    const cutscene::Frame f = pl.frame_for(cs->cams[0], far_car);
    check(near(f.origin[0], 102.0, 1e-9) && near(f.origin[1], 10.0, 1e-9) && near(f.origin[2], 200.0, 1e-9) &&
              near(f.heading_deg, 100.0, 1e-9),
          "a node parented to the placed car is in the car's frame");
    check(pl.world_camera(far_car, w) && std::abs(w.eye[0] - 102.0) < 6.0 && std::abs(w.eye[2] - 200.0) < 6.0,
          "the camera sits by the placed car, not by the world origin");
  }
}

// The race's cutscenes: a Follow cut hands the view to the game's camera,
// a node the cutscene does not place comes from the scene (the route), and
// race_specific names resolve to the track's own copy first.
void test_race_cutscenes() {
  const char* doc = R"(<?xml version="1.0"?>
<CutsceneManager>
  <Cutscene name="prerace_countdown" PlaybackMode="Normal" Duration="8.0" MirrorForRightHandDrive="0" LoopFrom="0.0">
    <Cams CutQueueType="Cutscene" NumCams="2">
      <Cam Version="3" Group="0" Type="Animateable" StartCut="0.0" TargetName="start_location_00" duration="3.0">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" TimeAnimType="AffectsPath" IsInCockpit="0" NumKeys="1">
          <Key Time="0.0" PosX="0" PosY="1" PosZ="-5" Yaw="0" Pitch="0" Roll="0" FOV="40" TimeC0="0"/>
        </Anim>
      </Cam>
      <Cam Version="3" Group="0" Type="Follow" StartCut="3.0" TargetName="" duration="5.0"/>
    </Cams>
  </Cutscene>
  <Cutscene name="prerace_countdown_TRACK_A" PlaybackMode="Normal" Duration="10.0" MirrorForRightHandDrive="0" LoopFrom="0.0"/>
</CutsceneManager>)";
  cutscene::Library lib;
  std::string err;
  check(cutscene::parse_cutscenes(doc, lib, &err) && lib.cutscenes.size() == 2, "race doc parses");
  const auto* cs = cutscene::find_race_cutscene(lib, "prerace_countdown", "");
  check(cs && cs->name == "prerace_countdown" && cs->cams.size() == 2 && !cs->cams[0].follow && cs->cams[1].follow,
        "Type=\"Follow\" is kept");
  check(cutscene::find_race_cutscene(lib, "prerace_countdown", "TRACK_A") &&
            cutscene::find_race_cutscene(lib, "prerace_countdown", "TRACK_A")->duration == 10.0,
        "race_specific: the track's <name>_<DevName> first");
  check(cutscene::find_race_cutscene(lib, "prerace_countdown", "TRACK_B") == cs, "race_specific: else the default");
  check(cutscene::track_cutscenes_member("FESTIVAL_FEST_001") ==
            "Colorado/Cutscenes/Tracks/cutscenes_FESTIVAL_FEST_001.xml",
        "the track's member");

  cutscene::Player pl;
  pl.set_scene_nodes({{"start_location_00", {{100, 0, 200}, 0}}});
  pl.start(cs);
  pl.step(0.0);
  const cutscene::Frame car{{0, 0, 0}, 0};
  cutscene::WorldCamera w;
  check(pl.world_camera(car, w) && near(w.eye[0], 100.0, 1e-9) && near(w.eye[2], 195.0, 1e-9),
        "a node the cutscene does not place: the scene's");
  pl.step(3.5);
  check(pl.active() && !pl.world_camera(car, w), "the Follow cut: the game's own camera");
  pl.start(cs);
  pl.step(0.0);
  check(pl.world_camera(car, w) && near(w.eye[0], 100.0, 1e-9), "the scene's nodes stay across start()");
}

// Ground-snapped camera nodes (`snapToGround="1"`), CarSpace body-origin lift,
// exterior eye ground clearance, car-frame override, and +-180 deg angle unwrapping.
void test_ground_snap_lift_and_angle_unwrap() {
  static const char* kDoc2 = R"(<?xml version="1.0"?>
<CutsceneManager>
  <Cutscene name="Outro_Snap" PlaybackMode="Normal" Duration="4.0" LoopFrom="0.0">
    <Cams CutQueueType="Cutscene" NumCams="2">
      <Cam Version="3" Type="Animateable" StartCut="0.0" TargetName="CameraTargetNode_0" duration="2.0">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" NumKeys="3">
          <Key Time="0.0" PosX="0" PosY="1.15" PosZ="0" Yaw="361.9" Pitch="0" Roll="-5" FOV="45"/>
          <Key Time="0.5" PosX="0.5" PosY="1.20" PosZ="0" Yaw="-0.2" Pitch="-5" Roll="-5" FOV="45"/>
          <Key Time="2.0" PosX="1.0" PosY="1.18" PosZ="0" Yaw="-2.5" Pitch="-11" Roll="-5" FOV="45"/>
        </Anim>
      </Cam>
      <Cam Version="3" Type="Animateable" StartCut="2.0" TargetName="" duration="2.0">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" NumKeys="2">
          <Key Time="0.0" PosX="-1.36" PosY="-0.01" PosZ="0.08" Yaw="14" Pitch="-8" Roll="12" FOV="39"/>
          <Key Time="2.0" PosX="-1.36" PosY="-0.50" PosZ="0.08" Yaw="14" Pitch="-8" Roll="12" FOV="39"/>
        </Anim>
      </Cam>
    </Cams>
    <Events>
      <Event time="0.0">
        <EventTrigger name="CameraTargetNode_0" id="CCutsceneCameraNodeTrigger" x="-731.25" y="28.77" z="-1227.18" rotation="133.8" parent="" snapToGround="1"/>
      </Event>
    </Events>
  </Cutscene>
</CutsceneManager>)";

  cutscene::Library lib;
  std::string err;
  check(cutscene::parse_cutscenes(kDoc2, lib, &err), "ground-snap doc parses");
  const auto* cs = lib.find("Outro_Snap");
  if (!cs) return;

  // 1) Yaw unwrapping: 361.9 -> -0.2 pans smoothly through ~360.85 deg (+Z), never ~180 deg (-Z).
  const auto mid = cutscene::sample_pose(cs->cams[0], 0.25);
  const double mid_rad = mid.yaw * 3.14159265358979323846 / 180.0;
  check(std::cos(mid_rad) > 0.99, "yaw unwraps across 360 -> 0 boundary without spinning");

  // 2) snapToGround="1" lifts CameraTargetNode_0 from authored y=28.77 to road y=34.5,
  //    even if the ground query becomes available after start() / t=0.
  cutscene::Player pl;
  pl.start(cs);
  pl.step(0.0);
  cutscene::WorldCamera w;
  const cutscene::Frame car{{100.0, 110.0, -200.0}, 0.0};
  check(pl.world_camera(car, w) && near(w.eye[1], 28.77 + 1.15, 1e-4), "before ground query, uses authored y");
  pl.set_ground_query([](double, double, double, double& out_y) {
    out_y = 34.5;
    return true;
  });
  check(pl.world_camera(car, w) && near(w.eye[1], 34.5 + 1.15, 1e-4),
        "snapToGround=\"1\" lifts camera node onto road surface");

  // 3) CarSpace body offset (`car.lift`): PosY = -0.01 is lifted by +0.48 m so it stays
  //    well above the road instead of dipping under the tyre.
  pl.step(2.0);
  pl.set_car_body_offset_y(0.48);
  check(pl.world_camera(car, w) && near(w.eye[1], 110.0 + 0.48 - 0.01, 1e-4),
        "CarSpace cam adds car body lift offset");

  // 4) Exterior eye clearance safeguard: if PosY drops to -0.50 (110.0 + 0.48 - 0.50 = 109.98),
  //    the eye clearance query clamps it to road + 0.20 m = 110.20 m.
  pl.set_eye_clearance_query([](double, double, double, double& out_y) {
    out_y = 110.0;
    return true;
  });
  pl.step(2.0);  // t = 4.0 -> PosY = -0.50
  check(pl.world_camera(car, w) && near(w.eye[1], 110.0 + cutscene::Player::kMinEyeGroundClearance, 1e-4),
        "exterior cutscene camera never sinks below ground clearance");

  // 5) Car-frame override redirects TargetName="" cams (e.g. New_Diner_Cutscene_2 onto Corrado).
  pl.set_car_frame_override(cutscene::Frame{{1553.88, 117.97, -1934.26}, -120.86});
  pl.set_eye_clearance_query({});
  check(pl.world_camera(car, w) && std::abs(w.eye[0] - 1553.88) < 3.0 && std::abs(w.eye[2] + 1934.26) < 3.0,
        "car frame override anchors TargetName=\"\" cams to the scripted hero car");
}

void test_wp6_target_part_parents_and_clearance() {
  static const char* kDoc = R"(<?xml version="1.0"?>
<CutsceneManager>
  <Cutscene name="WP6_Cutscene" PlaybackMode="Normal" Duration="6.0" LoopFrom="0.0">
    <Cams CutQueueType="Cutscene" NumCams="4">
      <Cam Version="3" Type="Animateable" StartCut="0.0" TargetName="" duration="1.5">
        <Anim PosAnimType="CarSpace" RotAnimType="TargetPart" RotAnimPartToTarget="steering" IsInCockpit="1" NumKeys="2">
          <Key Time="0.0" PosX="-0.3" PosY="0.6" PosZ="-0.3" Yaw="0" Pitch="0" Roll="0" FOV="55"/>
          <Key Time="1.5" PosX="-0.3" PosY="0.6" PosZ="-0.3" Yaw="30" Pitch="0" Roll="0" FOV="50"/>
        </Anim>
      </Cam>
      <Cam Version="3" Type="Animateable" StartCut="1.5" TargetName="" duration="1.5">
        <Anim PosAnimType="TargetPart" RotAnimType="CarSpace" RotAnimPartToTarget="wheelLF" NumKeys="2">
          <Key Time="0.0" PosX="0" PosY="0.5" PosZ="1.0" Yaw="0" Pitch="0" Roll="0" FOV="45"/>
          <Key Time="1.5" PosX="0" PosY="0.5" PosZ="1.0" Yaw="0" Pitch="0" Roll="0" FOV="45"/>
        </Anim>
      </Cam>
      <Cam Version="3" Type="Animateable" StartCut="3.0" TargetName="ChildSnapNode" duration="1.5">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" NumKeys="2">
          <Key Time="0.0" PosX="0" PosY="1.0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="45"/>
          <Key Time="1.5" PosX="0" PosY="1.0" PosZ="0" Yaw="0" Pitch="0" Roll="0" FOV="45"/>
        </Anim>
      </Cam>
      <Cam Version="3" Type="Animateable" StartCut="4.5" TargetName="" duration="1.5">
        <Anim PosAnimType="CarSpace" RotAnimType="CarSpace" NumKeys="2">
          <Key Time="0.0" PosX="0" PosY="-0.8" PosZ="-5.0" Yaw="0" Pitch="0" Roll="10" FOV="45"/>
          <Key Time="1.5" PosX="0" PosY="-0.8" PosZ="-5.0" Yaw="0" Pitch="0" Roll="10" FOV="45"/>
        </Anim>
      </Cam>
    </Cams>
    <Events>
      <Event time="0.0">
        <EventTrigger name="RootCar" id="CCutsceneCarTrigger" x="10.0" y="2.0" z="20.0" rotation="90.0" parent=""/>
      </Event>
      <Event time="0.0">
        <EventTrigger name="AnimParent" id="CCutsceneAnimationTrigger" x="0.0" y="0.0" z="4.0" rotation="0.0" parent="RootCar"/>
      </Event>
      <Event time="0.0">
        <EventTrigger name="AnimChild" id="CCutsceneAnimationTrigger" x="2.0" y="0.0" z="0.0" rotation="45.0" parent="AnimParent"/>
      </Event>
      <Event time="0.0">
        <EventTrigger name="ChildSnapNode" id="CCutsceneCameraNodeTrigger" x="0.0" y="0.0" z="3.0" rotation="0.0" parent="RootCar" snapToGround="1"/>
      </Event>
    </Events>
  </Cutscene>
</CutsceneManager>)";

  cutscene::Library lib;
  std::string err;
  check(cutscene::parse_cutscenes(kDoc, lib, &err), "WP6 cutscene doc parses");
  const auto* cs = lib.find("WP6_Cutscene");
  if (!cs) return;

  cutscene::Player pl;
  pl.start(cs);
  pl.step(0.0);

  // 1) kCar and kAnimation parents in event_frame():
  // RootCar is at (10, 2, 20), heading +90 deg (+X).
  // AnimParent local (0, 0, 4) -> world (14, 2, 20), heading +90 deg.
  // AnimChild local (2, 0, 0) rotated by +90 deg -> (0, 0, -2) -> world (14, 2, 18), heading +135 deg.
  const cutscene::Frame ap = pl.event_frame(cs->events[1]);
  const cutscene::Frame ac = pl.event_frame(cs->events[2]);
  check(near(ap.origin[0], 14.0, 1e-6) && near(ap.origin[2], 20.0, 1e-6) && near(ap.heading_deg, 90.0, 1e-6),
        "event_frame resolves kCar parent");
  check(near(ac.origin[0], 14.0, 1e-6) && near(ac.origin[2], 18.0, 1e-6) && near(ac.heading_deg, 135.0, 1e-6),
        "event_frame resolves kAnimation parent chain");

  // 2) RotAnimType="TargetPart" with RotAnimPartToTarget="steering":
  const cutscene::Frame car{{100.0, 10.0, 200.0}, 0.0};
  const cutscene::CarParts parts{
      {"steering", {-0.3, 0.9, 0.7}},
      {"wheelLF", {-0.8, 0.35, 1.3}},
  };
  cutscene::WorldCamera w;
  check(pl.world_camera(car, w, &parts), "TargetPart rotation cam evaluates");
  // Eye is at (99.7, 10.6, 199.7), steering at (99.7, 10.9, 200.7) -> dir = (0, 0.3, 1.0).
  const double inv = 1.0 / std::hypot(0.3, 1.0);
  check(near(w.forward[0], 0.0, 1e-5) && near(w.forward[1], 0.3 * inv, 1e-5) && near(w.forward[2], 1.0 * inv, 1e-5),
        "RotAnimType=TargetPart aims directly at named car part when yaw/pitch offset is 0");
  // At t=1.5, Yaw=+30 deg rotates right from the steering target.
  pl.step(1.5);
  check(pl.world_camera(car, w, &parts) && near(w.eye[0], 99.2, 1e-5) && near(w.eye[1], 10.85, 1e-5) &&
            near(w.eye[2], 202.3, 1e-5),
        "PosAnimType=TargetPart anchors camera position to named car part");

  // 3) Parented snapToGround="1" node lazily re-snaps once ground query arrives:
  pl.step(1.5);  // t = 3.0 -> ChildSnapNode
  check(pl.world_camera(car, w, &parts) && near(w.eye[0], 13.0, 1e-5) && near(w.eye[1], 3.0, 1e-5) &&
            near(w.eye[2], 20.0, 1e-5),
        "parented snapToGround node uses parent height before terrain streams in");
  pl.set_ground_query([](double, double, double, double& out_y) {
    out_y = 7.25;
    return true;
  });
  check(pl.world_camera(car, w, &parts) && near(w.eye[1], 7.25 + 1.0, 1e-5),
        "parented snapToGround node lazily re-snaps when terrain streams in");

  // 4) Ground-clearance lift preserves target framing smoothly (pitches forward down toward aim point):
  pl.step(1.5);  // t = 4.5 -> eye at (100, 9.2, 195), forward (0, 0, 1) before clearance
  cutscene::WorldCamera unlifted;
  check(pl.world_camera(car, unlifted, &parts) && near(unlifted.forward[1], 0.0, 1e-5),
        "baseline low exterior cam forward is horizontal before clearance clamp");
  pl.set_eye_clearance_query([](double, double, double, double& out_y) {
    out_y = 10.0;
    return true;
  });
  cutscene::WorldCamera lifted;
  check(pl.world_camera(car, lifted, &parts) && near(lifted.eye[1], 10.2, 1e-5),
        "ground clearance lifts exterior eye to min_y");
  const double dot_fu =
      lifted.forward[0] * lifted.up[0] + lifted.forward[1] * lifted.up[1] + lifted.forward[2] * lifted.up[2];
  check(lifted.forward[1] < -0.15 && std::abs(dot_fu) < 1e-6 && lifted.up[1] > 0.8,
        "ground clearance lift re-aims forward down toward focus point and keeps up orthogonal");
}

}  // namespace

int main() {
  test_race_cutscenes();
  test_parse();
  test_interpolation();
  test_post_and_fades();
  test_player_and_world();
  test_parts_and_nodes();
  test_parented_node();
  test_ground_snap_lift_and_angle_unwrap();
  test_wp6_target_part_parents_and_clearance();
  if (g_failures) {
    std::fprintf(stderr, "cutscene_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("cutscene_tests: ok\n");
  return 0;
}
