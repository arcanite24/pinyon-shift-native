// Unit tests for drive::GamepadRumble (SPEC-CAMERAS, Gamepad rumble).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/rumble.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

// Synthetic XML covering all channels plus the v4 malformed two-dot float quirk.
const char* kSampleXml = R"xml(
<RumbleDef>
  <Controller>
    <EngineNormRPM>
      <LeftMotor x0="0.0" y0="0.0" x1="0.0" y1="0.0" x2="0.0" y2="0.0"/>
      <RightMotor x0="0.6" y0="0.0" x1="0.8" y1="0.1" x2="1.0" y2="0.2"/>
    </EngineNormRPM>
    <CarToWorldCollision>
      <LeftMotor x0="20.0" y0="0.0" x1="70.0" y1="0.4" x2="350.0" y2="0.8"/>
      <RightMotor x0="0.0" y0="0.0" x1="10.0" y1="0.3" x2="75.0" y2="0.5"/>
    </CarToWorldCollision>
    <CarToWorldCollisionTime>
      <LeftMotor x0="0.0" y0="0.25" x1="25.0" y1="0.20" x2="500.0" y2="0.30"/>
      <RightMotor x0="0.0" y0="0.25" x1="25.0" y1="0.20" x2="500.0" y2="0.30"/>
    </CarToWorldCollisionTime>
    <TireTractionFront>
      <LeftMotor x0="1.0" y0="0.0" x1="2.0" y1="0.05" x2="3.0" y2="0.10"/>
      <RightMotor x0="0.7" y0="0.0" x1="1.1" y1="0.08" x2="3.0" y2="0.16"/>
    </TireTractionFront>
    <TireTractionRear>
      <LeftMotor x0="0.9" y0="0.0" x1="1.5" y1="0.04" x2="3.0" y2="0.08"/>
      <RightMotor x0="1.0" y0="0.0" x1="2.0" y1="0.0.083335" x2="3.0" y2="0.12"/>
    </TireTractionRear>
    <SurfaceBumpiness>
      <LeftMotor x0="0.0" y0="0.0" x1="0.1" y1="0.3" x2="1.0" y2="0.6"/>
      <RightMotor x0="0.0" y0="0.0" x1="0.1" y1="0.2" x2="1.0" y2="0.4"/>
    </SurfaceBumpiness>
    <OnShift>
      <LeftMotor rumble="0.25"/>
      <RightMotor rumble="0.0"/>
    </OnShift>
    <OnEngineSputter>
      <LeftMotor rumble="0.30"/>
      <RightMotor rumble="0.30"/>
    </OnEngineSputter>
    <OnBackfire>
      <LeftMotor rumble="0.05"/>
      <RightMotor rumble="0.10"/>
    </OnBackfire>
  </Controller>
</RumbleDef>
)xml";

}  // namespace

int main() {
  drive::RumbleConfig cfg;
  std::string err;
  CHECK(drive::parse_controller_rumble(kSampleXml, cfg, &err));
  CHECK(cfg.loaded);

  // Retail v4 malformed float ("0.0.083335") parses up to the second point as 0.0.
  CHECK(near(cfg.tire_traction_rear.right.y[1], 0.0));
  CHECK(near(cfg.tire_traction_rear.right.y[2], 0.12));

  // Curve evaluation: below x0, mid-segment, above x2.
  CHECK(near(cfg.engine_norm_rpm.right.evaluate(0.5), 0.0));
  CHECK(near(cfg.engine_norm_rpm.right.evaluate(0.7), 0.05));
  CHECK(near(cfg.engine_norm_rpm.right.evaluate(0.9), 0.15));
  CHECK(near(cfg.engine_norm_rpm.right.evaluate(1.5), 0.20));
  CHECK(near(cfg.engine_norm_rpm.left.evaluate(1.0), 0.0));

  // Dual-motor split: RPM drives RightMotor only; OnShift drives LeftMotor only.
  drive::GamepadRumble rumble;
  rumble.set_config(cfg);
  {
    drive::RumbleInput in;
    in.engine_norm_rpm = 1.0;
    auto out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.0));
    CHECK(near(out.right, 0.20));
    CHECK(out.left_u16() == 0);
    CHECK(out.right_u16() == static_cast<uint16_t>(std::lround(0.20 * 65535.0)));
  }
  {
    drive::RumbleInput in;
    in.on_shift = true;
    auto out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.25));
    CHECK(near(out.right, 0.0));
  }
  // Event decays after kEventHold.
  {
    drive::RumbleInput in;
    auto out = rumble.update(0.10, in);
    CHECK(near(out.left, 0.25));
    out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.0));
  }

  // Collision pulse triggers and decays over CollisionTime.
  {
    rumble.reset();
    drive::RumbleInput in;
    in.car_to_world_impulse = 70.0;  // left amp 0.4, right amp ~0.4846
    auto out0 = rumble.update(0.016, in);
    CHECK(out0.left > 0.35 && out0.right > 0.45);
    in.car_to_world_impulse = 0.0;
    auto out1 = rumble.update(0.10, in);
    CHECK(out1.left > 0.0 && out1.left < out0.left);
    rumble.update(0.50, in);
    auto out2 = rumble.update(0.016, in);
    CHECK(near(out2.left, 0.0) && near(out2.right, 0.0));
  }

  // Pause mute gate clears active pulses and outputs 0.
  {
    drive::RumbleInput in;
    in.car_to_world_impulse = 200.0;
    in.engine_norm_rpm = 1.0;
    in.paused = true;
    auto out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.0) && near(out.right, 0.0));
    in.paused = false;
    in.car_to_world_impulse = 0.0;
    in.engine_norm_rpm = 0.0;
    out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.0) && near(out.right, 0.0));
  }

  // Enabled=false and scale clamping.
  {
    drive::RumbleInput in;
    in.surface_bumpiness = 1.0;
    in.car_to_world_impulse = 350.0;
    in.on_engine_sputter = true;
    in.enabled = false;
    auto out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.0) && near(out.right, 0.0));

    in.enabled = true;
    in.scale = 1.0;
    out = rumble.update(0.016, in);
    CHECK(near(out.left, 1.0) && near(out.right, 1.0));  // clamped at 1.0
    CHECK(out.left_u16() == 65535 && out.right_u16() == 65535);

    in.scale = 0.5;
    in.car_to_world_impulse = 0.0;
    in.on_engine_sputter = false;
    rumble.reset();
    in.surface_bumpiness = 1.0;  // left 0.6, right 0.4
    out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.30) && near(out.right, 0.20));
  }

  // Real ControllerRumble.xml from game files when SHIFT_GAME_ROOT is set.
  if (const char* root = std::getenv("SHIFT_GAME_ROOT")) {
    cars::Install install;
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    if (cars::discover_install(root, state ? state : "", "", install, &err)) {
      std::string xml;
      CHECK(cars::load_rumble_xml(install, xml, &err));
      drive::RumbleConfig live_cfg;
      CHECK(drive::parse_controller_rumble(xml, live_cfg, &err));
      CHECK(live_cfg.loaded);
      CHECK(live_cfg.engine_norm_rpm.right.evaluate(1.0) > 0.05);
      CHECK(live_cfg.on_shift.left > 0.1);
      CHECK(live_cfg.surface_bumpiness.right.evaluate(0.5) > 0.1);
    }
  }

  std::printf("rumble_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
