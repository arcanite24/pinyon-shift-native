// carview's windowed start-up: SDL comes up with the subsystems the drive
// uses (video, gamepad, audio) under the dummy drivers, so a build without
// one of them fails here and not at a player's first start.
#include <cstdio>
#include <cstdlib>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int main(int, char**) {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  int failures = 0;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "FAIL SDL_Init(VIDEO | GAMEPAD): %s\n", SDL_GetError());
    return 1;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    std::fprintf(stderr, "FAIL SDL_InitSubSystem(AUDIO): %s\n", SDL_GetError());
    ++failures;
  } else {
    // The drive's engine stream: 48 kHz float stereo on the default device.
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
      std::fprintf(stderr, "FAIL SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
      ++failures;
    } else {
      SDL_DestroyAudioStream(stream);
    }
  }
  SDL_Quit();
  std::printf("%s\n", failures ? "sdl_init_tests: FAILED" : "sdl_init_tests: video, gamepad and audio up");
  return failures ? 1 : 0;
}
