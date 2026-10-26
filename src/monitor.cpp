// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "monitor.hpp"
namespace modulator {
const char *monitor_state_name(MonitorState s)
{
  switch (s) {
  case MonitorState::Off: return "Off / holding";
  case MonitorState::Waiting: return "Waiting for visibility";
  case MonitorState::Starting: return "Starting";
  case MonitorState::EasingIn: return "Easing in";
  case MonitorState::Running: return "Running";
  case MonitorState::EasingOut: return "Easing out";
  case MonitorState::Paused: return "Paused by external edit";
  case MonitorState::Error: return "Needs attention";
  }
  return "Unavailable";
}
const char *monitor_wave_name(int wave)
{
  const char *names[] = {"Sine", "Triangle", "Rising saw", "Falling saw", "Square", "Glider"};
  return wave >= 0 && wave < 6 ? names[wave] : "Unknown waveform";
}
}
