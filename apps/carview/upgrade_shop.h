// carview --drive: the upgrade shop (SPEC-GARAGE-UPGRADES), opened from the
// pause menu on the selected garage car. Up and down pick a slot, left and
// right a part of that slot, Enter fits it (bought at the discounted price,
// or refitted for free), Esc leaves. Ours: the game's own shop screens are
// not specified; this is a list in the pause menu's panel.
#pragma once

#include <string>
#include <vector>

#include "cars/physics.h"
#include "garage/garage.h"

namespace carview {

class UpgradeShop {
 public:
  // False with `why` when the car has no parts to offer.
  bool open(const cars::PartsDb& db, garage::Garage& g, int64_t garage_id, std::string& why);
  void close() { open_ = false; }
  bool is_open() const { return open_; }
  void move_slot(int by);
  void move_part(int by);
  // Fits the shown part; `message` says what happened.
  bool fit(int discount_percent, int64_t& credits, std::string& message);
  // A part was fitted since the shop opened (the car must be rebuilt).
  bool changed() const { return changed_; }
  // The title and the lines around the selected slot, for the menu panel.
  void fill(const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits, std::string& title,
            std::vector<std::string>& lines, int& selected) const;

 private:
  struct Slot {
    const cars::UpgradeSlot* slot = nullptr;
    std::vector<cars::PartRow> parts;  // by level
    size_t shown = 0;                  // the part on show
  };
  garage::Garage* garage_ = nullptr;
  int64_t garage_id_ = 0;
  std::vector<Slot> slots_;
  size_t selected_ = 0;
  bool open_ = false, changed_ = false;
  size_t fitted_index(const Slot& s) const;
};

}  // namespace carview
