// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform.hpp"
#include <obs-nix-platform.h>
#include <cstdio>
#include <sys/resource.h>
#include <unistd.h>
namespace test_platform {
void initialize() { obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL); }
void process_events() {}
uint64_t resident_bytes()
{
  unsigned long total = 0, resident = 0;
  FILE *f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  int fields = std::fscanf(f, "%lu %lu", &total, &resident); std::fclose(f);
  return fields == 2 ? static_cast<uint64_t>(resident) * sysconf(_SC_PAGESIZE) : 0;
}
double cpu_seconds()
{
  rusage r = {}; getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}
}
