// Thin entry point for `carview --drive`.
// CLI parsing lives in drive_cli.{h,cpp}; standalone `--bake` showroom mode
// lives in showroom_mode.cpp; app/world/session lifecycles live in
// drive_app.{h,cpp}, drive_world_host.{h,cpp}, and drive_session.{h,cpp}.
#include "drive_app.h"
#include "drive_cli.h"
#include "drive_world_host.h"

namespace carview {

int run_drive(int argc, char** argv) {
  DriveApp app;
  int exit_code = 0;
  if (!app.parse(argc, argv, exit_code)) return exit_code;
  if (!app.menu_png.empty()) return run_headless_menu_dumps(app, app.font_cache);
  if (!app.init(exit_code)) return exit_code;

  for (app.again = true; app.again;) {
    app.again = false;
    if (app.skill_tracker) app.skill_tracker->reset_session_clock();
    {
      DriveWorldHost host(app);
      host.run();
    }
    if (app.handle_rally_world_switch()) app.again = true;
  }

  app.shutdown();
  return app.status;
}

}  // namespace carview
