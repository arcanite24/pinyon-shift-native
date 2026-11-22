// The pause menu's settings page: rows read from the file's tokens, left
// and right step a row, and the tokens written keep the file's other
// options.
#include <cstdio>
#include <string>
#include <vector>

#include "settings_page.h"

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static bool has(const std::vector<std::string>& v, const std::vector<std::string>& seq) {
  for (size_t i = 0; i + seq.size() <= v.size(); ++i) {
    bool ok = true;
    for (size_t k = 0; k < seq.size(); ++k) ok = ok && v[i + k] == seq[k];
    if (ok) return true;
  }
  return false;
}

int main() {
  carview::SettingsPage page;
  page.load(
      {"--size", "1280x720", "--preset", "low", "--no-traffic", "--difficulty", "hard", "--hour", "18", "--language", "FR"});
  CHECK(page.value("preset") == "low");
  CHECK(page.value("traffic") == "off");
  CHECK(page.value("difficulty") == "hard");
  CHECK(page.value("radio") == "on");
  CHECK(page.value("camera") == "default");

  // Unchanged: the same options come back.
  auto t = page.tokens();
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));
  CHECK(has(t, {"--preset", "low"}) && has(t, {"--no-traffic"}) && has(t, {"--difficulty", "hard"}));

  // Rows: preset, shadows, bloom, difficulty, rubber-banding, traffic, ...
  page.open();
  page.step(1);  // preset low -> high
  CHECK(page.value("preset") == "high");
  page.move(5);  // traffic
  page.step(1);  // off -> on
  CHECK(page.value("traffic") == "on");
  page.move(-2);  // difficulty
  page.step(1);   // hard -> pro
  CHECK(page.value("difficulty") == "pro");
  t = page.tokens();
  CHECK(has(t, {"--preset", "high"}) && !has(t, {"--no-traffic"}) && has(t, {"--difficulty", "pro"}));
  CHECK(has(t, {"--size", "1280x720"}) && has(t, {"--hour", "18"}) && has(t, {"--language", "FR"}));

  // The save line follows the last row and wraps.
  page.open();
  page.move(-1);
  CHECK(page.on_save_line());
  page.move(1);
  CHECK(!page.on_save_line());
  std::string title;
  std::vector<std::string> lines;
  int selected = -1;
  page.fill(title, lines, selected);
  CHECK(lines.size() == 11 && selected == 0 && lines[0] == "preset  < high >");

  std::printf("settings_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
