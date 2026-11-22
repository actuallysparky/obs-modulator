// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace modulator::midi {
enum class ClockMessage { Tick, Start, Continue, Stop };
enum class Division { Bar, Whole, Half, Quarter, Eighth, Sixteenth };

struct ClockSample {
  bool valid = false;
  bool aligned = false;
  bool stopped = false;
  bool lost = false;
  double position = 0;
  double confirmed_position = 0;
  double seconds_per_quarter = 0;
  std::uint64_t generation = 0;
  std::uint64_t stop_generation = 0;
  std::uint64_t loss_generation = 0;
};

// Positions use quarter notes. Event and sample times are monotonic seconds
// from the same timebase. Deliver events in timestamp order before update().
class Clock {
public:
  void receive(ClockMessage message, double now);
  void update(double now);
  void disconnect();
  ClockSample sample(double now) const;

private:
  double position(double now) const;
  double timeout() const;

  double last_event_ = -1;
  double last_clock_ = -1;
  double last_position_tick_ = -1;
  double transport_started_ = -1;
  double interval_ = 0;
  double held_position_ = 0;
  std::uint64_t ticks_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t stop_generation_ = 0;
  std::uint64_t loss_generation_ = 0;
  bool has_position_ = false;
  bool pending_start_ = false;
  bool awaiting_tick_ = false;
  bool aligned_ = false;
  bool stopped_ = false;
  bool lost_ = false;
};

double division_quarters(Division division, int numerator = 4, int denominator = 4);
double next_downbeat(double position, int numerator = 4, int denominator = 4);
}
