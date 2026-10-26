// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace modulator {
enum class MonitorState { Off, Waiting, Starting, EasingIn, Running, EasingOut, Paused, Error };
struct MonitorRow {
  std::string id, source, name, target, property, reason;
  MonitorState state = MonitorState::Off;
  bool enabled = false, available = false, integer = false;
  double value = 0, low = 0, high = 0;
  int waveform = 0;
};
// UI-thread, pull-only interface. No observer or sampling work is installed in
// the modulation pump. Destroy/clear the session when the dock is not exposed.
class MonitorFeed {
public:
  virtual ~MonitorFeed() = default;
  virtual std::vector<MonitorRow> sample() = 0;
  virtual bool set_enabled(const std::string &id, bool enabled) = 0;
  virtual void clear() = 0;
};
std::unique_ptr<MonitorFeed> make_monitor_feed();
const char *monitor_state_name(MonitorState state);
const char *monitor_wave_name(int wave);
}
