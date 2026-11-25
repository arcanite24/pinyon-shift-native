// Which input skips a movie. The original's boot splashes skip on a B tap
// (orig-title.steps) and its story movies are confirmskip (Q155): a movie
// skips on a press, never on a held button or a key's auto-repeat.
#pragma once

#include <SDL3/SDL_events.h>

namespace carview {

// True for a press of A, B or Start, Enter, Space or Esc, or a left or right
// click.
inline bool movie_skip_event(const SDL_Event& e) {
  if (e.type == SDL_EVENT_KEY_DOWN)
    return !e.key.repeat && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE ||
                             e.key.key == SDLK_ESCAPE);
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
    return e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
           e.gbutton.button == SDL_GAMEPAD_BUTTON_START;
  if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
    return e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT;
  return false;
}

}  // namespace carview
