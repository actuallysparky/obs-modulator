// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace modulator::midi {

enum class Kind { NoteOn, NoteOff, ControlChange, Clock, Start, Continue, Stop, Disconnected };

struct Event {
  std::string port;
  Kind kind;
  int channel = 0; // 1–16 for channel messages, zero for system events.
  int number = 0;
  int value = 0;
  double time = 0;
};

struct Port {
  std::string id, name;
};

class Input {
public:
  Input();
  ~Input();
  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;

  // Call ports()/select() on the control thread, periodically while in use.
  // drain() reads pending events without enumerating ports or opening devices.
  std::vector<Port> ports();
  void select(const std::set<std::string> &ports);
  bool connected(const std::string &port) const;
  std::vector<Event> drain();
  static double now();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace modulator::midi
