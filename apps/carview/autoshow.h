// carview --drive: the Autoshow (SPEC-AUTOSHOW-PAINT), opened from the
// pause menu. Up and down pick a car (by class, then index), left and
// right jump a class, Enter buys it at BaseCost (no sign discount) in its
// first colour, Esc leaves. Ours: one list instead of the make carousel,
// only the cars the bake has (the others cannot be driven yet).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "garage/garage.h"

namespace carview {

class Autoshow {
 public:
  // `baked`: the bake's cars, lower-case media names.
  bool open(const cars::PartsDb& db, const std::set<std::string>& baked, std::string& why);
  void close() { open_ = false; }
  bool is_open() const { return open_; }
  void move(int by);
  void jump_class(int by);
  // Buys the shown car into `g` (selected); false with `message` when
  // refused. `media` gets the bought car's media name.
  bool buy(const cars::PartsDb& db, garage::Garage& g, int64_t& credits, std::string& media, std::string& message);
  void fill(const std::vector<garage::ClassBand>& bands, int64_t credits, std::string& title,
            std::vector<std::string>& lines, int& selected) const;

 private:
  std::vector<cars::DealerCar> cars_;
  size_t selected_ = 0;
  bool open_ = false;
};

}  // namespace carview
