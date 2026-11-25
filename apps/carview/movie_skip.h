// Which input skips a movie. The original's boot splashes skip on a B tap
// (orig-title.steps) and its story movies are confirmskip (Q155): a movie
// skips on a press, never on a held button or a key's auto-repeat.
#pragma once

#include <SDL3/SDL_events.h>

namespace carview {

// True for a press of A, B or Start, Enter, Space or Esc (not a click: one
// that focuses the window must not skip).
inline bool movie_skip_event(const SDL_Event& e) {
  if (e.type == SDL_EVENT_KEY_DOWN)
    return !e.key.repeat && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                             e.key.key == SDLK_ESCAPE);
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
    return e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
           e.gbutton.button == SDL_GAMEPAD_BUTTON_START;
  return false;
}

}  // namespace carview
