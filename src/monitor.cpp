// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "monitor.hpp"
#include "lfo.hpp"
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
  case MonitorState::Completed: return "Completed";
  case MonitorState::Cancelled: return "Cancelled / holding";
  }
  return "Unavailable";
}
const char *monitor_wave_name(int wave)
{
  return wave_name(wave);
}
}
