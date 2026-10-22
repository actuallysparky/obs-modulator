// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "lfo.hpp"
#include <algorithm>
#include <limits>

namespace modulator {
double normalized(Wave wave, double cycles)
{
  if (!std::isfinite(cycles)) return 0.0;
  const double phase = cycles - std::floor(cycles);
  switch (wave) {
  case Wave::Sine: return (1.0 - std::cos(phase * 6.2831853071795864769)) * 0.5;
  case Wave::Triangle: return 1.0 - std::abs(phase * 2.0 - 1.0);
  case Wave::RisingSaw: return phase;
  case Wave::FallingSaw: return 1.0 - phase;
  case Wave::Square: return phase < 0.5 ? 0.0 : 1.0;
  }
  return 0.0;
}
double mapped(Wave wave, double elapsed, double period, double low, double high,
              bool integer, double property_min, double property_max)
{
  if (!std::isfinite(period) || period <= 0 || !std::isfinite(low) ||
      !std::isfinite(high) || low > high || property_min > property_max)
    return std::numeric_limits<double>::quiet_NaN();
  double value = low + (high - low) * normalized(wave, elapsed / period);
  if (integer) value = std::round(value);
  return std::clamp(value, property_min, property_max);
}
bool same_value(double a, double b, bool integer)
{
  if (!std::isfinite(a) || !std::isfinite(b)) return false;
  return integer ? a == b : std::abs(a-b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
}
}
