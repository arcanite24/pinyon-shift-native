// carview --drive's free-roam traffic (SPEC-TRAFFIC): the population and
// its bodies (libs/traffic) beside the player's car, and each traffic model
// drawn as a guest Showroom (its M1 bake) wherever a car of it stands.
// The models load as the population's loaded set changes: on a worker in
// the window, at once headless (so headless runs repeat).
#pragma once
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "drive/contact.h"
#include "drive/lamps.h"
#include "drive/vehicle.h"
#include "drive_helpers.h"
#include "showroom.h"
#include "traffic/bodies.h"
#include "traffic/config.h"
#include "traffic/nav.h"
#include "traffic/population.h"
#include "traffic/roads.h"

namespace carview {

class TrafficView {
 public:
  TrafficView(Renderer& renderer, Showroom& host, std::filesystem::path bake_dir, bool sync_loads);
  ~TrafficView();
  TrafficView(const TrafficView&) = delete;
  TrafficView& operator=(const TrafficView&) = delete;

  // Reads the road map and settings from the disc (`game_root`), the
  // traffic models' bakes from the bake folder. `ground`: where the
  // current ground is (it changes as the world streams). When `race_mode` is
  // true, loads `<Settings name="race">` from AIOpenWorld.xml.
  bool open(const std::filesystem::path& game_root, const drive::Ground* const* ground, double hour, uint64_t seed,
            std::string& error, bool race_mode = false);
  void set_hour(double hour) { population_->set_hour(hour); }
  double hour() const { return population_->hour(); }
  // Registers Race AI opponents (e.g. from RaceView::after_step) so traffic
  // brakes/yields for them and promotes nearby traffic cars to physics bodies.
  void set_external_vehicles(std::vector<traffic::ExternalVehicle> vehicles);
  // One fixed step (drive::Vehicle::kStep), after the player's car's.
  void step(drive::Vehicle& player, std::span<const traffic::ExternalVehicle> obstacles = {});
  // Fraction of a step past the last one (1.0: as stepped).
  void set_blend(double alpha) { blend_ = alpha; }
  // The guests' loads and the host's instances for this frame (player's
  // car model frame).
  void place(const cars::bake::CarInfo& player_info, const drive::Vehicle& player);
  size_t active() const { return population_ ? population_->cars().size() : 0; }
  const std::vector<traffic::Car>* cars() const { return population_ ? &population_->cars() : nullptr; }
  size_t bodies() const { return bodies_ ? bodies_->count() : 0; }
  // The largest impulse traffic gave the player's car in the last step (N s).
  double player_impulse() const { return bodies_ ? bodies_->player_impulse() : 0; }
  size_t drawn() const { return drawn_; }
  std::string summary() const;
  // The traffic bodies for a contact pass, in `group` (traffic with
  // traffic and with the player is resolved in step()).
  void contact_bodies(std::vector<drive::ContactBody>& out, int group);

  // Full traffic rewind snapshot & restore (population + physics bodies).
  struct RewindSnapshot {
    traffic::Population::Snapshot population;
    traffic::Bodies::Snapshot bodies;
    std::map<uint32_t, TrafficPose> before;
  };
  RewindSnapshot capture_rewind() const;
  void restore_rewind(const RewindSnapshot& s);

 private:
  // What a loading worker returns: the model, and (in the window) its GPU
  // upload too, done on the worker through the renderer's second queue so
  // the render thread never waits for it.
  struct Loaded {
    std::unique_ptr<Showroom> showroom;
    bool uploaded = false;
    std::string error;
    double upload_ms = 0;
  };
  static constexpr size_t kMaxWheelClonesPerModel = 3;
  struct Guest {
    std::vector<std::unique_ptr<Showroom>> showrooms;  // [0] primary + on-demand per-car wheel clones
    std::future<Loaded> loading;
    bool failed = false;
  };
  void update_guests();

  Renderer& r_;
  Showroom& host_;
  std::filesystem::path bake_dir_;
  std::filesystem::path game_root_;
  bool sync_;
  traffic::NavMap map_;
  traffic::Roads roads_;
  traffic::TrafficConfig config_;
  std::unique_ptr<traffic::Population> population_;
  std::unique_ptr<traffic::Bodies> bodies_;
  const drive::Ground* const* ground_ = nullptr;
  std::map<int64_t, std::string> media_;  // Data_Car.Id -> media name
  std::map<int64_t, Guest> guests_;
  std::map<int64_t, size_t> desired_slots_;
  std::map<int64_t, drive::CarParams> params_;
  size_t drawn_ = 0;
  double upload_ms_max_ = 0;
  int uploads_ = 0;
  double guests_at_ = -1;  // the population time guests were last refreshed from step()
  double lamps_at_ = -1;   // population time when lamps were last updated
  double blend_ = 1.0;
  std::map<uint32_t, TrafficPose> before_;
  std::map<uint32_t, drive::CarLampState> lamps_;
  std::map<uint32_t, float> wheel_spin_;
  std::vector<traffic::ExternalVehicle> external_vehicles_;
  int external_ttl_ = 0;
};

// Another car drawn with the host's camera: `guest`'s model where `body`
// stands, in the player's car model frame.
Showroom::Instance guest_instance(const cars::bake::CarInfo& player_info, const drive::Vehicle& player, Showroom& guest,
                                  const drive::Vehicle& body);
// Same as guest_instance for an arbitrary world pose (e.g. translucent Rivals ghost car).
Showroom::Instance guest_instance_pose(const cars::bake::CarInfo& player_info, const drive::Vehicle& player,
                                       Showroom& guest, const drive::Vec3& world_pos, const drive::Quat& world_rot);

}  // namespace carview
