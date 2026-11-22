// SDL events to the overlay pages' navigation (mods_page.h, save_sync_ui.h).
// Keyboard: arrows / WASD move, Enter / Space accept, Esc back, Q / E (and
// PageUp / PageDown) the bumpers, X and Y the face buttons (also F5 reloads
// textures on the Mods page). Gamepad: D-pad, A, B, LB, RB, X, Y.
#pragma once

#include <SDL3/SDL.h>

#include "mods_page.h"

namespace carview {

inline Nav nav_from_sdl(const SDL_Event& e) {
  if (e.type == SDL_EVENT_KEY_DOWN) {
    switch (e.key.key) {
      case SDLK_UP:
      case SDLK_W: return Nav::kUp;
      case SDLK_DOWN:
      case SDLK_S: return Nav::kDown;
      case SDLK_LEFT:
      case SDLK_A: return Nav::kLeft;
      case SDLK_RIGHT:
      case SDLK_D: return Nav::kRight;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_SPACE: return e.key.repeat ? Nav::kNone : Nav::kAccept;
      case SDLK_ESCAPE: return e.key.repeat ? Nav::kNone : Nav::kBack;
      case SDLK_Q:
      case SDLK_PAGEUP: return Nav::kTabPrev;
      case SDLK_E:
      case SDLK_PAGEDOWN: return Nav::kTabNext;
      case SDLK_X:
      case SDLK_F5: return e.key.repeat ? Nav::kNone : Nav::kX;
      case SDLK_Y: return e.key.repeat ? Nav::kNone : Nav::kY;
      default: return Nav::kNone;
    }
  }
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
    switch (e.gbutton.button) {
      case SDL_GAMEPAD_BUTTON_DPAD_UP: return Nav::kUp;
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Nav::kDown;
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Nav::kLeft;
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Nav::kRight;
      case SDL_GAMEPAD_BUTTON_SOUTH: return Nav::kAccept;
      case SDL_GAMEPAD_BUTTON_EAST:
      case SDL_GAMEPAD_BUTTON_START: return Nav::kBack;
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Nav::kTabPrev;
      case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Nav::kTabNext;
      case SDL_GAMEPAD_BUTTON_WEST: return Nav::kX;
      case SDL_GAMEPAD_BUTTON_NORTH: return Nav::kY;
      default: return Nav::kNone;
    }
  }
  return Nav::kNone;
}

}  // namespace carview
