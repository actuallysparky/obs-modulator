// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
namespace modulator {
// Timestamped, fixed-capacity history: UI stalls leave gaps, not catch-up
// samples. Extra immediate readbacks cannot fill it faster than 15 Hz.
struct SampleHistory {
  static constexpr int Capacity=150;
  struct Sample { double value=0, time=0; };
  std::array<Sample,Capacity> samples{};
  int start=0, count=0;
  void clear() { start=0;count=0; }
  const Sample &at(int i) const { return samples[static_cast<size_t>((start+i)%Capacity)]; }
  bool append(double value,double now) {
    if(!std::isfinite(value) || !std::isfinite(now))return false;
    if(count && now<at(count-1).time)clear();
    while(count && now-at(0).time>10){start=(start+1)%Capacity;--count;}
    if(count && std::floor(at(count-1).time*15)==std::floor(now*15))return false;
    samples[static_cast<size_t>((start+count)%Capacity)]={value,now};
    if(count<Capacity)++count;else start=(start+1)%Capacity;
    return true;
  }
};
}
