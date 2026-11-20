#include "process_time.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>

double process_cpu_seconds() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  auto seconds = [](const FILETIME& f) {
    return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
  };
  return seconds(kernel) + seconds(user);
}
#else
#include <sys/resource.h>

double process_cpu_seconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}
#endif
