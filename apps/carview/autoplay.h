// --autoplay: a dev harness that plays a timed script in the windowed loop
// (key presses, PNG captures of the presented frame, the player's car handed
// to the sat-nav autopilot) and writes a beat log, so a stretch of the game
// can be captured and timed the way the recomp's render-test routes are.
// Times are wall-clock seconds since the harness started.
//
//   # comment
//   <t> key <return|escape|space|up|down|left|right|backspace|tab|x|y|m>
//   <t> key mapclose              escape on an open map that is not the career's locked one
//   <t> key advance               return, but only in a race, a movie, a blocking career beat or the title
//   <t> capture <name>            the next presented frame -> <out>/<name>.png
//   <t> every <seconds> <prefix>  periodic captures <prefix>_<ttttt>.png from t
//   <t> drive <auto|off>          the player's input from the sat-nav autopilot
//   <t> mapto heats               an open map's cursor onto the career's Horizon Heats
//   <t> launch <event id>         the session restarts into that event, as a menu launch does (an unknown id
//                                 is the failed launch of Q280)
//   <t> stop
//
// The beat log (<out>/beats.csv: t,field,value) records each observed field
// when its value changes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace carview {

class Autoplay {
 public:
  struct Step {
    double t = 0.0;
    std::string cmd;
    std::string arg;
    double num = 0.0;
  };

  bool load(const std::filesystem::path& script, const std::filesystem::path& out_dir, std::string* error);
  bool active() const { return active_; }
  double now() const;
  // Due key presses (SDL keycodes as names) since the last call; sets the
  // stop and drive flags as their steps come due.
  std::vector<std::string> take_due_keys();
  // A capture name due now, or empty.
  std::string take_due_capture();
  bool drive_auto() const { return drive_auto_; }
  bool stop_requested() const { return stop_; }
  void observe(const char* field, const std::string& value);
  std::filesystem::path png_path(const std::string& name) const { return out_ / (name + ".png"); }
  void note(const std::string& line);

 private:
  void advance();
  bool active_ = false;
  std::vector<Step> steps_;
  size_t next_ = 0;
  std::vector<std::string> keys_;
  std::vector<std::string> captures_;
  double every_s_ = 0.0, next_every_ = 0.0;
  std::string every_prefix_;
  bool drive_auto_ = false, stop_ = false;
  int64_t t0_ns_ = 0;
  std::filesystem::path out_;
  std::map<std::string, std::string> last_;
};

}  // namespace carview
