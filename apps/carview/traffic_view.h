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
#include <string>

#include "drive/contact.h"
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
  // current ground is (it changes as the world streams).
  bool open(const std::filesystem::path& game_root, const drive::Ground* const* ground, double hour, uint64_t seed,
            std::string& error);
  void set_hour(double hour) { population_->set_hour(hour); }
  double hour() const { return population_->hour(); }
  // One fixed step (drive::Vehicle::kStep), after the player's car's.
  void step(drive::Vehicle& player);
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

 private:
  struct Guest {
    std::unique_ptr<Showroom> showroom;  // uploaded
    std::future<std::unique_ptr<Showroom>> loading;
    bool failed = false;
  };
  void update_guests();

  Renderer& r_;
  Showroom& host_;
  std::filesystem::path bake_dir_;
  bool sync_;
  traffic::NavMap map_;
  traffic::Roads roads_;
  traffic::TrafficConfig config_;
  std::unique_ptr<traffic::Population> population_;
  std::unique_ptr<traffic::Bodies> bodies_;
  const drive::Ground* const* ground_ = nullptr;
  std::map<int64_t, std::string> media_;  // Data_Car.Id -> media name
  std::map<int64_t, Guest> guests_;
  std::map<int64_t, drive::CarParams> params_;
  size_t drawn_ = 0;
  double upload_ms_max_ = 0;
  int uploads_ = 0;
  double guests_at_ = -1;  // the population time guests were last refreshed from step()
  double blend_ = 1.0;
  std::map<uint32_t, TrafficPose> before_;
};

// Another car drawn with the host's camera: `guest`'s model where `body`
// stands, in the player's car model frame.
Showroom::Instance guest_instance(const cars::bake::CarInfo& player_info, const drive::Vehicle& player, Showroom& guest,
                                  const drive::Vehicle& body);

}  // namespace carview
