// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "midi-clock.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(0)

namespace {
using modulator::midi::Clock;
using modulator::midi::ClockMessage;

constexpr double tempo_tick = 1.0 / 48.0;
bool near(double actual, double expected)
{
  return std::abs(actual - expected) < 1e-9;
}

void ticks(Clock &clock, double from, int count, double spacing = tempo_tick)
{
  for (int index = 0; index < count; ++index)
    clock.receive(ClockMessage::Tick, from + spacing * index);
}
}

int main()
{
  using namespace modulator::midi;
  CHECK(near(division_quarters(Division::Bar), 4));
  CHECK(near(division_quarters(Division::Bar, 3, 4), 3));
  CHECK(near(division_quarters(Division::Bar, 7, 8), 3.5));
  CHECK(near(division_quarters(Division::Whole, 3, 4), 4));
  CHECK(near(division_quarters(Division::Half), 2));
  CHECK(near(division_quarters(Division::Quarter), 1));
  CHECK(near(division_quarters(Division::Eighth), 0.5));
  CHECK(near(division_quarters(Division::Sixteenth), 0.25));
  CHECK(near(next_downbeat(2.9, 3, 4), 3));
  CHECK(near(next_downbeat(3, 3, 4), 6));
  CHECK(near(next_downbeat(3.9, 7, 8), 7));

  Clock clock;
  CHECK(!clock.sample(0).valid);
  ticks(clock, 1, 25);
  auto state = clock.sample(1.5);
  CHECK(state.valid && !state.aligned && !state.stopped && !state.lost);
  CHECK(near(state.position, 1));
  CHECK(near(state.confirmed_position, 1));
  CHECK(near(state.seconds_per_quarter, 0.5));
  CHECK(near(clock.sample(1.5 + tempo_tick / 2).position, 1 + 0.5 / 24.0));

  clock.receive(ClockMessage::Start, 1.51);
  CHECK(!clock.sample(1.51).aligned);
  ticks(clock, 1.5 + tempo_tick, 72);
  const double three_four_downbeat = 1.5 + tempo_tick * 73;
  state = clock.sample(three_four_downbeat);
  CHECK(state.valid && state.aligned);
  CHECK(near(state.confirmed_position, 71.0 / 24.0));
  CHECK(state.position < 3);
  CHECK(near(next_downbeat(state.position, 3, 4), 3));
  clock.receive(ClockMessage::Tick, three_four_downbeat);
  CHECK(near(clock.sample(three_four_downbeat).confirmed_position, 3));
  CHECK(near(next_downbeat(3, 3, 4), 6));

  const double stop = three_four_downbeat + tempo_tick * 0.5;
  const double stopped_position = clock.sample(stop).position;
  const auto generation = clock.sample(stop).generation;
  clock.receive(ClockMessage::Stop, stop);
  CHECK(clock.sample(stop).stopped && clock.sample(stop).stop_generation == 1);
  ticks(clock, three_four_downbeat + tempo_tick, 100);
  state = clock.sample(5.2);
  CHECK(state.valid && state.aligned && state.stopped);
  CHECK(near(state.position, stopped_position));
  CHECK(near(state.confirmed_position, 3));
  clock.receive(ClockMessage::Continue, 5.2);
  CHECK(!clock.sample(5.2).stopped && near(clock.sample(5.2).position, stopped_position));
  clock.receive(ClockMessage::Tick, 5.21);
  CHECK(near(clock.sample(5.21).confirmed_position, 73.0 / 24.0));
  CHECK(clock.sample(5.21).generation == generation);

  // A deliberate transport pause may remain silent for a long time.
  clock.receive(ClockMessage::Stop, 5.22);
  clock.update(15.0);
  state = clock.sample(15.0);
  CHECK(!state.valid && state.aligned && state.stopped && !state.lost);
  CHECK(state.generation == generation);
  clock.receive(ClockMessage::Continue, 15.01);
  ticks(clock, 15.03, 2);
  state = clock.sample(15.03 + tempo_tick);
  CHECK(state.valid && state.aligned && !state.stopped && !state.lost);
  CHECK(near(state.confirmed_position, 75.0 / 24.0));

  // Unannounced running loss clears the bar reference, unlike Stop.
  clock.update(16.0);
  state = clock.sample(16.0);
  CHECK(!state.valid && !state.aligned && state.lost);
  CHECK(state.generation == generation + 1 && state.loss_generation == 1);
  ticks(clock, 16.01, 3);
  state = clock.sample(16.06);
  CHECK(state.valid && !state.aligned && !state.lost);
  CHECK(state.loss_generation == 1);
  clock.receive(ClockMessage::Start, 16.07);
  ticks(clock, 16.01 + 3 * tempo_tick, 1);
  state = clock.sample(16.01 + 3 * tempo_tick);
  CHECK(state.aligned && near(state.confirmed_position, 0));
  clock.disconnect();
  CHECK(clock.sample(16.08).lost && clock.sample(16.08).loss_generation == 2);

  // Faster clock changes future tempo without making phase run backwards.
  Clock accelerating;
  ticks(accelerating, 20.0, 25);
  double previous = accelerating.sample(20.5).position;
  for (int index = 1; index <= 96; ++index) {
    const double time = 20.5 + index * tempo_tick / 2;
    accelerating.receive(ClockMessage::Tick, time);
    const auto snapshot = accelerating.sample(time + tempo_tick / 4);
    CHECK(snapshot.position >= previous);
    previous = snapshot.position;
  }
  CHECK(near(accelerating.sample(21.5).seconds_per_quarter, 0.25));

  // A silent pause may straddle the running timeout right as Continue arrives.
  Clock silent_pause;
  silent_pause.receive(ClockMessage::Start, 30.0);
  ticks(silent_pause, 30.01, 3);
  silent_pause.receive(ClockMessage::Stop, 30.06);
  const auto pause = silent_pause.sample(30.06);
  silent_pause.receive(ClockMessage::Continue, 30.30);
  ticks(silent_pause, 30.32, 2);
  const auto resumed = silent_pause.sample(30.32 + tempo_tick);
  CHECK(resumed.valid && resumed.aligned && !resumed.lost);
  CHECK(resumed.generation == pause.generation);
  CHECK(near(resumed.seconds_per_quarter, 0.5));
  CHECK(near(resumed.confirmed_position, pause.confirmed_position + 2.0 / 24.0));

  // A transport Start without following ticks never leaves the clock armed.
  Clock missing_start;
  missing_start.receive(ClockMessage::Start, 40.0);
  missing_start.update(40.6);
  CHECK(missing_start.sample(40.6).lost && !missing_start.sample(40.6).aligned);

  // No render observation is necessary to detect a gap in queued clock ticks.
  accelerating.receive(ClockMessage::Tick, 22.1);
  ticks(accelerating, 22.11, 3, 0.01);
  CHECK(accelerating.sample(22.14).valid && accelerating.sample(22.14).loss_generation == 1);
  std::printf("%d MIDI clock checks passed\n", checks);
}
