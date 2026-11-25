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

  // FEEL-27: OnEngineSputter comes from engine damage (none below
  // EngineDamageLevelStartSputter, at most EngineDamageMaxSputtersPerSec).
  {
    drive::EngineSputter sp;
    sp.start_damage = 0.3;
    sp.max_per_sec = 1.0;
    auto count = [&](double damage, double seconds, double dt) {
      int n = 0;
      for (double t = 0; t < seconds - 1e-9; t += dt) n += sp.step(dt, damage) ? 1 : 0;
      return n;
    };
    CHECK(count(0.0, 10.0, 1.0 / 64) == 0);
    CHECK(count(0.29, 10.0, 1.0 / 64) == 0);
    CHECK(near(sp.rate(0.3), 0.0) && near(sp.rate(0.65), 0.5) && near(sp.rate(1.0), 1.0) && near(sp.rate(2.0), 1.0));
    sp.reset();
    CHECK(count(1.0, 10.0, 1.0 / 64) == 10);  // the cap: one a second at full damage
    sp.reset();
    CHECK(count(0.65, 10.5, 1.0 / 64) == 5);
    sp.reset();
    CHECK(count(1.0, 10.0, 0.5) == 10);  // frame rate does not change the count
    {
      bool fired = false;
      for (int i = 0; i < 300; ++i) fired = fired || sp.step(0.0, 1.0);  // paused: no time, no sputter
      CHECK(!fired);
    }
    drive::EngineSputter off;
    off.max_per_sec = 0.0;
    int n = 0;
    for (int i = 0; i < 600; ++i) n += off.step(1.0 / 60, 1.0) ? 1 : 0;
    CHECK(n == 0);
  }

  // LAB-87: Stationary gating (zero vibration when stopped even with residual slip/bumpiness)
  // and smooth attack/release envelope decay.
  {
    rumble.reset();
    drive::RumbleInput in;
    in.speed_mps = 0.0;
    in.smooth = true;
    in.engine_norm_rpm = 0.0;
    in.surface_bumpiness = 0.8;
    in.tire_traction_front = 2.0;
    in.tire_traction_rear = 2.0;
    auto out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.0) && near(out.right, 0.0));

    // Impact triggers smoothly and decays cleanly back to 0 when stopped.
    in.car_to_world_impulse = 120.0;
    auto hit0 = rumble.update(0.016, in);
    CHECK(hit0.left > 0.0 && hit0.left < 0.8);  // smooth attack (no jarring 1-frame snap)
    in.car_to_world_impulse = 0.0;
    for (int i = 0; i < 80; ++i) out = rumble.update(0.016, in);
    CHECK(near(out.left, 0.0) && near(out.right, 0.0));
  }

  // CarToWorldCollision unit scaling aligned with LeftMotor x0 = 20.0 (not / 35.0),
  // OnShift suppression for Neutral (0) / Reverse (-1), and <Wheel> + SteerTorque scaling.
  {
    const double scaled_light = drive::world_collision_rumble_input(12000.0, 1.0 / 120.0, 1400.0);
    const double scaled_med = drive::world_collision_rumble_input(42000.0, 1.0 / 120.0, 1400.0);
    CHECK(near(scaled_light, 20.0, 1e-4));
    CHECK(near(scaled_med, 70.0, 1e-4));
    rumble.reset();
    drive::RumbleInput in;
    in.car_to_world_force_n = 42000.0;
    in.car_mass_kg = 1400.0;
    const auto out_w = rumble.update(0.016, in);
    CHECK(out_w.left >= 0.39 && out_w.right >= 0.45);

    // Suppress OnShift when shifting into Neutral (0) or Reverse (-1).
    CHECK(!drive::should_trigger_shift_rumble(1, 0));
    CHECK(!drive::should_trigger_shift_rumble(1, -1));
    CHECK(drive::should_trigger_shift_rumble(1, 2));
    rumble.reset();
    drive::RumbleInput sh_n;
    sh_n.on_shift = true;
    sh_n.gear = 0;
    CHECK(near(rumble.update(0.016, sh_n).left, 0.0));
    sh_n.gear = -1;
    CHECK(near(rumble.update(0.016, sh_n).left, 0.0));
    sh_n.gear = 2;
    CHECK(near(rumble.update(0.016, sh_n).left, 0.25));

    // <Wheel> rumble parsing & surface <SteerTorque> scaling.
    const char* kWheelXml = R"xml(
<RumbleDef>
  <Wheel>
    <SteerTorque value="0.8"/>
    <SurfaceBumpiness>
      <WheelMotor x0="0.0" y0="0.0" x1="0.5" y1="0.5" x2="1.0" y2="1.0"/>
    </SurfaceBumpiness>
    <OnShift>
      <Motor rumble="0.20"/>
    </OnShift>
  </Wheel>
</RumbleDef>
)xml";
    drive::RumbleConfig wcfg;
    CHECK(drive::parse_controller_rumble(kWheelXml, wcfg, &err, /*wheel=*/true));
    CHECK(wcfg.loaded && wcfg.is_wheel && near(wcfg.wheel_steer_torque_scale, 0.8));
    drive::GamepadRumble wrumble;
    wrumble.set_config(wcfg);
    drive::RumbleInput win;
    win.surface_bumpiness = 0.5;
    win.surface_steer_torque_scale = 0.5;  // 0.5 curve * 0.8 wheel * 0.5 surface = 0.20
    const auto wout = wrumble.update(0.016, win);
    CHECK(near(wout.left, 0.20, 1e-4) && near(wout.right, 0.20, 1e-4));
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
