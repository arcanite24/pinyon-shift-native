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
</CutsceneManager>
)";

void test_parse() {
  cutscene::Library lib;
  std::string err;
  check(cutscene::parse_cutscenes(kDoc, lib, &err), "parses the synthetic document");
  check(lib.cutscenes.size() == 2, "two cutscenes");
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
  // Hold: a Normal cutscene with LoopFrom loops while held.
  pl.set_hold(true);
  fired = pl.step(2.5);  // 3.2 + 2.5 = 5.7
  check(pl.active() && fired.size() == 1, "snapshot at 5.0 fires");
  fired = pl.step(0.5);  // past 6.0 -> wraps to loop_from
  check(pl.active() && pl.time() >= 4.0 && pl.time() < 6.0, "held cutscene loops from LoopFrom");
  check(pl.step(1.0).empty(), "events do not refire on loop");
  pl.set_hold(false);
  pl.step(3.0);
  check(pl.finished() && !pl.active(), "released cutscene finishes at its duration");

  // Pose conventions: yaw +90 looks along +X; pitch +10 looks down.
  cutscene::Pose pose;
  pose.yaw = 90;
  pose.pitch = 10;
  const auto c = cutscene::to_world(pose, cutscene::Frame{{0, 0, 0}, 0});
  check(c.forward[0] > 0.98 && c.forward[1] < -0.17 && c.forward[1] > -0.18, "yaw right, pitch down");
  cutscene::Pose rolled;
  rolled.roll = 90;
  const auto r = cutscene::to_world(rolled, cutscene::Frame{{0, 0, 0}, 0});
  check(near(r.up[0], 1.0, 1e-9), "positive roll leans the top right");
  const auto h = cutscene::to_world(cutscene::Pose{{1, 0, 0}, 0, 0, 0, 50}, cutscene::Frame{{10, 0, 10}, 180});
  check(near(h.eye[0], 9, 1e-9) && near(h.eye[2], 10, 1e-9) && near(h.forward[2], -1, 1e-9),
        "heading 180 mirrors the local axes");
}

}  // namespace

int main() {
  test_parse();
  test_interpolation();
  test_post_and_fades();
  test_player_and_world();
  if (g_failures) {
    std::fprintf(stderr, "cutscene_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("cutscene_tests: ok\n");
  return 0;
}
