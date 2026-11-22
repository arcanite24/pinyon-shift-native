/* A sample native mod for mods_tests: logs at init (with its option), on
 * the third frame and at shutdown, so the test sees the whole lifecycle. */
#include <stdio.h>
#include <stdlib.h>

#include "cars/shift_mod_api.h"

typedef struct State {
  const ShiftModHost* host;
  unsigned ticks;
} State;

SHIFT_MOD_EXPORT uint32_t shift_mod_abi_version(void) { return SHIFT_MOD_ABI_VERSION; }

SHIFT_MOD_EXPORT int shift_mod_init(const ShiftModHost* host, void** state) {
  State* s = (State*)calloc(1, sizeof(State));
  char buf[160];
  const char* greeting = host->option(host->host_data, "greeting");
  if (!s) return 1;
  s->host = host;
  snprintf(buf, sizeof buf, "init abi=%u greeting=%s", host->abi, greeting ? greeting : "(none)");
  host->log(host->host_data, buf);
  host->notify(host->host_data, "sample mod loaded", 2.0f);
  *state = s;
  return 0;
}

SHIFT_MOD_EXPORT void shift_mod_tick(void* state, const ShiftModFrame* frame) {
  State* s = (State*)state;
  ++s->ticks;
  if (frame->frame == 2) {
    char buf[96];
    snprintf(buf, sizeof buf, "tick %u speed %.1f gear %d", s->ticks, (double)frame->speed, frame->gear);
    s->host->log(s->host->host_data, buf);
  }
}

SHIFT_MOD_EXPORT void shift_mod_shutdown(void* state) {
  State* s = (State*)state;
  char buf[64];
  snprintf(buf, sizeof buf, "shutdown after %u ticks", s->ticks);
  s->host->log(s->host->host_data, buf);
  free(s);
}
