// carview --world: draws the Colorado world-space render models whose
// header box overlaps an X/Z rectangle, headless, with a clip matrix and a
// viewport mapping read from a frame file, and compares the result with a
// capture of the same frame. Spec: WORLD-GEOMETRY.
#pragma once

namespace carview {

// argv as given to carview, starting at "--world". Returns the exit code.
int run_world(int argc, char** argv);

}  // namespace carview
