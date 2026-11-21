// The pause menu's settings page (ours, M7): the settings file's options
// as rows the player steps through with left and right, saved back to the
// file (settings.h). Changes apply on the next start; the file's other
// options (size, msaa, exposure, hour, play, mods) are kept as they are.
#pragma once

#include <string>
#include <vector>

namespace carview {

class SettingsPage {
 public:
  // `tokens`: the file's options as command-line tokens (read_settings).
  void load(const std::vector<std::string>& tokens);
  bool is_open() const { return open_; }
  void open() { open_ = true, row_ = 0; }
  void close() { open_ = false; }
  void move(int by);   // up and down, wrapping over the rows and the save line
  void step(int by);   // left and right on the selected row
  bool on_save_line() const { return row_ == int(rows_.size()); }
  // The file's tokens with the page's rows applied (write_settings).
  std::vector<std::string> tokens() const;
  void fill(std::string& title, std::vector<std::string>& lines, int& selected) const;
  // A row's shown choice, by its name ("difficulty"), for tests.
  std::string value(const std::string& name) const;

 private:
  struct Choice {
    const char* label;
    std::vector<std::string> tokens;  // what the choice writes (none: the default)
  };
  struct Row {
    const char* name;
    std::vector<Choice> choices;
    size_t pick = 0;
  };
  std::vector<Row> rows_ = build();
  std::vector<std::string> other_;  // the file's tokens no row owns
  bool open_ = false;
  int row_ = 0;
  static std::vector<Row> build();
};

}  // namespace carview
