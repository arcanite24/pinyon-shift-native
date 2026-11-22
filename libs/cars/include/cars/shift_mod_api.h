/* shift-engine native mod API (ours, Slice 13), C ABI version 1.
 *
 * A native mod is a shared library (.dll / .so / .dylib) named by its
 * mod.toml `library`. It runs only when the player has switched on native
 * mods (mods.toml `native_plugins = true`, or --mods-native), because it
 * runs with the player's permissions.
 *
 * The library exports, with C linkage:
 *
 *   uint32_t shift_mod_abi_version(void);
 *       returns SHIFT_MOD_ABI_VERSION; a different major refuses the mod.
 *   int shift_mod_init(const ShiftModHost* host, void** state);
 *       0 on success; *state is passed back to the other calls.
 *   void shift_mod_tick(void* state, const ShiftModFrame* frame);
 *       once per rendered frame (optional export).
 *   void shift_mod_shutdown(void* state);
 *       at exit, or when the mod is unloaded (optional export).
 *
 * The structs only grow: new members are appended and `size` tells which
 * exist, so a mod built against an older header keeps working.
 */
#ifndef SHIFT_MOD_API_H
#define SHIFT_MOD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIFT_MOD_ABI_VERSION 1u

#if defined(_WIN32)
#define SHIFT_MOD_EXPORT __declspec(dllexport)
#else
#define SHIFT_MOD_EXPORT __attribute__((visibility("default")))
#endif

typedef struct ShiftModHost {
  uint32_t size; /* sizeof(ShiftModHost) as the host built it */
  uint32_t abi;  /* SHIFT_MOD_ABI_VERSION */
  const char* mod_name;
  const char* mod_dir; /* UTF-8 */
  void* host_data;     /* pass back to the callbacks */
  /* Writes a line to the game's log, prefixed with the mod's name. */
  void (*log)(void* host_data, const char* message);
  /* A mod option's chosen value (mod.toml [options.<key>]), or NULL. */
  const char* (*option)(void* host_data, const char* key);
  /* Shows a short notice on the HUD for `seconds`. */
  void (*notify)(void* host_data, const char* message, float seconds);
} ShiftModHost;

typedef struct ShiftModFrame {
  uint32_t size; /* sizeof(ShiftModFrame) as the host built it */
  uint32_t frame;
  double time;    /* s since start */
  double dt;      /* s since the previous frame */
  float position[3]; /* the player's car, m (world) */
  float heading;  /* rad */
  float speed;    /* m/s */
  float rpm;
  int32_t gear;
  int32_t paused; /* 1 in menus */
} ShiftModFrame;

typedef uint32_t (*ShiftModAbiVersionFn)(void);
typedef int (*ShiftModInitFn)(const ShiftModHost* host, void** state);
typedef void (*ShiftModTickFn)(void* state, const ShiftModFrame* frame);
typedef void (*ShiftModShutdownFn)(void* state);

#ifdef __cplusplus
}
#endif

#endif /* SHIFT_MOD_API_H */
