// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "lfo.hpp"
#include <algorithm>
#include <limits>

namespace modulator {
namespace {
// Quintic smoothstep: zero first and second derivatives at both ends.
double gentle(double t)
{
  t = std::clamp(t, 0.0, 1.0);
  return std::clamp(t * t * t * (t * (t * 6.0 - 15.0) + 10.0), 0.0, 1.0);
}
double knot(uint64_t seed, uint64_t index)
{
  uint64_t x = seed + index * UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return static_cast<double>(x >> 11) * 0x1p-53;
}
}
const char *wave_name(int wave)
{
  const char *names[] = {"Sine", "Triangle", "Rising saw", "Falling saw", "Square", "Glider",
    "Exponential rising", "Logarithmic rising", "S curve rising",
    "Exponential falling", "Logarithmic falling", "S curve falling"};
  return wave >= 0 && wave < wave_count ? names[wave] : "Unknown waveform";
}
double random_value(uint64_t seed, uint64_t index) { return knot(seed,index); }
double normalized(Wave wave, double cycles, uint64_t seed)
{
  if (!std::isfinite(cycles)) return 0.0;
  const double phase = cycles - std::floor(cycles);
  switch (wave) {
  case Wave::Sine: return (1.0 - std::cos(phase * 6.2831853071795864769)) * 0.5;
  case Wave::Triangle: return 1.0 - std::abs(phase * 2.0 - 1.0);
  case Wave::RisingSaw: return phase;
  case Wave::FallingSaw: return 1.0 - phase;
  case Wave::Square: return phase < 0.5 ? 0.0 : 1.0;
  case Wave::ExponentialRising: return std::expm1(4.0*phase)/std::expm1(4.0);
  case Wave::LogarithmicRising: return std::log1p(std::expm1(4.0)*phase)/4.0;
  case Wave::SCurveRising: return gentle(phase);
  case Wave::ExponentialFalling: return std::expm1(4.0*(1.0-phase))/std::expm1(4.0);
  case Wave::LogarithmicFalling: return std::log1p(std::expm1(4.0)*(1.0-phase))/4.0;
  case Wave::SCurveFalling: return gentle(1.0-phase);
  case Wave::Glider: {
    // Guard conversion even for callers outside the engine's bounded time range.
    if (cycles < -0x1p63 || cycles >= 0x1p63) return 0.0;
    const auto index = static_cast<uint64_t>(static_cast<int64_t>(std::floor(cycles)));
    const double a = knot(seed, index), b = knot(seed, index + 1);
    return a + (b - a) * gentle(phase);
  }
  }
  return 0.0;
}
double mapped(Wave wave, double elapsed, double period, double low, double high,
              bool integer, double property_min, double property_max,
              uint64_t seed, bool ease_in, double starting_value)
{
  if (!std::isfinite(period) || period <= 0 || !std::isfinite(low) ||
      !std::isfinite(high) || low > high || property_min > property_max)
    return std::numeric_limits<double>::quiet_NaN();
  double value = low + (high - low) * normalized(wave, elapsed / period, seed);
  if (ease_in) {
    if (!std::isfinite(starting_value) || !std::isfinite(elapsed))
      return std::numeric_limits<double>::quiet_NaN();
    // The starting value may lie outside the LFO endpoints. Blend first, then
    // apply the property's actual bounds and round integer controls only once.
    value = starting_value + (value - starting_value) * gentle(elapsed / period);
  }
  if (integer) value = std::round(value);
  return std::clamp(value, property_min, property_max);
}
double shot_mapped(Wave wave, double elapsed, double period, double low, double high,
                   bool integer, double property_min, double property_max,
                   uint64_t seed, bool ease_in, double starting_value, uint64_t shot_index)
{
  if (!std::isfinite(elapsed) || !std::isfinite(period) || period<=0 ||
      !std::isfinite(low) || !std::isfinite(high) || low>high ||
      !std::isfinite(property_min) || !std::isfinite(property_max) || property_min>property_max ||
      !std::isfinite(starting_value) || static_cast<int>(wave)<0 || static_cast<int>(wave)>=wave_count)
    return std::numeric_limits<double>::quiet_NaN();
  const double u=std::clamp(elapsed/period,0.0,1.0);
  double value;
  if(wave==Wave::Glider) {
    const double destination=low+(high-low)*knot(seed,shot_index);
    value=u>=1 ? destination : starting_value+(destination-starting_value)*gentle(u);
  } else {
    const bool high_end=wave==Wave::RisingSaw || wave==Wave::Square ||
      wave==Wave::ExponentialRising || wave==Wave::LogarithmicRising || wave==Wave::SCurveRising;
    const double v=u>=1 ? (high_end ? 1.0:0.0) : normalized(wave,u,seed);
    value=low+(high-low)*v;
    if(ease_in && u<1)value=starting_value+(value-starting_value)*gentle(u);
  }
  if(integer)value=std::round(value);
  return std::clamp(value,property_min,property_max);
}
double eased_return(double elapsed, double period, double from, double to,
                    bool integer, double property_min, double property_max)
{
  if (!std::isfinite(elapsed) || !std::isfinite(period) || period <= 0 ||
      !std::isfinite(from) || !std::isfinite(to) ||
      !std::isfinite(property_min) || !std::isfinite(property_max) || property_min > property_max)
    return std::numeric_limits<double>::quiet_NaN();
  const double t=gentle(elapsed/period);
  double value=elapsed>=period ? to : from+(to-from)*t;
  if(integer)value=std::round(value);
  return std::clamp(value,property_min,property_max);
}
bool same_value(double a, double b, bool integer)
{
  if (!std::isfinite(a) || !std::isfinite(b)) return false;
  return integer ? a == b : std::abs(a-b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
}
}
