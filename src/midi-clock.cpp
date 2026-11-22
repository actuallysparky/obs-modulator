// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "midi-clock.hpp"
#include <algorithm>
#include <cmath>

namespace modulator::midi {
namespace {
constexpr double ticks_per_quarter = 24;
constexpr double minimum_interval = 0.0001;
}

double Clock::timeout() const
{
  return interval_ > 0 ? std::clamp(8 * interval_, 0.25, 5.0) : 0.5;
}

double Clock::position(double now) const
{
  if (stopped_ || awaiting_tick_) return held_position_;
  if (!has_position_) return 0;
  const double lower = static_cast<double>(ticks_) / ticks_per_quarter;
  const double next = static_cast<double>(ticks_ + 1) / ticks_per_quarter;
  const double fraction = interval_ > 0
    ? std::max(0.0, (now - last_position_tick_) / interval_)
    : 0;
  return std::min(lower + fraction / ticks_per_quarter, std::nextafter(next, lower));
}

void Clock::disconnect()
{
  ++generation_;
  ++loss_generation_;
  lost_ = true;
  last_clock_ = last_position_tick_ = transport_started_ = -1;
  interval_ = held_position_ = 0;
  ticks_ = 0;
  has_position_ = pending_start_ = awaiting_tick_ = aligned_ = stopped_ = false;
}

void Clock::update(double now)
{
  const double anchor = awaiting_tick_ && !stopped_
    ? std::max(last_clock_, transport_started_) : last_clock_;
  if (anchor < 0 || now - anchor <= timeout()) return;
  if (stopped_) {
    // Quiet transport stops are valid. Drop the stale tempo estimate while
    // retaining the intended pause position for a later Continue.
    last_clock_ = -1;
    interval_ = 0;
    return;
  }
  disconnect();
}

void Clock::receive(ClockMessage message, double now)
{
  if (!std::isfinite(now) || now < 0 || now < last_event_) return;
  update(now);
  last_event_ = now;
  switch (message) {
  case ClockMessage::Start:
    ++generation_;
    ticks_ = 0;
    held_position_ = 0;
    has_position_ = aligned_ = stopped_ = false;
    pending_start_ = awaiting_tick_ = true;
    transport_started_ = now;
    return;
  case ClockMessage::Stop:
    ++stop_generation_;
    held_position_ = position(now);
    stopped_ = true;
    return;
  case ClockMessage::Continue:
    if (stopped_) {
      stopped_ = false;
      awaiting_tick_ = true;
      transport_started_ = now;
    }
    return;
  case ClockMessage::Tick:
    break;
  }

  if (last_clock_ >= 0) {
    const double difference = now - last_clock_;
    if (awaiting_tick_ && interval_ > 0 && difference > 2.5 * interval_) {
      // A gap in stopped transport is not part of the new tempo.
      interval_ = 0;
    } else if (difference >= minimum_interval) {
      interval_ = interval_ > 0 ? interval_ + 0.25 * (difference - interval_) : difference;
      lost_ = false;
    }
  }
  last_clock_ = now;
  if (stopped_) return;

  if (pending_start_) {
    ticks_ = 0;
    pending_start_ = false;
    aligned_ = true;
  } else if (has_position_) {
    ++ticks_;
  }
  has_position_ = true;
  awaiting_tick_ = false;
  last_position_tick_ = now;
}

ClockSample Clock::sample(double now) const
{
  const bool valid = interval_ > 0 && last_clock_ >= 0 && now - last_clock_ <= timeout();
  return {valid, aligned_, stopped_, lost_, position(now),
          static_cast<double>(ticks_) / ticks_per_quarter,
          interval_ * ticks_per_quarter, generation_, stop_generation_, loss_generation_};
}

double division_quarters(Division division, int numerator, int denominator)
{
  switch (division) {
  case Division::Bar:
    return 4.0 * std::max(1, numerator) / std::max(1, denominator);
  case Division::Whole: return 4;
  case Division::Half: return 2;
  case Division::Quarter: return 1;
  case Division::Eighth: return 0.5;
  case Division::Sixteenth: return 0.25;
  }
  return 1;
}

double next_downbeat(double position, int numerator, int denominator)
{
  const double bar = division_quarters(Division::Bar, numerator, denominator);
  return (std::floor(position / bar) + 1) * bar;
}
}
