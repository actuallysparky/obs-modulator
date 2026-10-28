// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

namespace modulator {
enum class Wave { Sine, Triangle, RisingSaw, FallingSaw, Square, Glider,
  ExponentialRising, LogarithmicRising, SCurveRising,
  ExponentialFalling, LogarithmicFalling, SCurveFalling };
constexpr int wave_count = 12;
const char *wave_name(int wave);
double random_value(uint64_t seed, uint64_t index);
// Glider joins seeded random knots with zero velocity/acceleration at each knot.
double normalized(Wave wave, double cycles, uint64_t seed = 0);
double mapped(Wave wave, double elapsed, double period, double low, double high,
              bool integer, double property_min, double property_max,
              uint64_t seed = 0, bool ease_in = false, double starting_value = 0);
double eased_return(double elapsed, double period, double from, double to,
                    bool integer, double property_min, double property_max);
// Finite phase includes the right endpoint; Glider always starts at readback.
double shot_mapped(Wave wave, double elapsed, double period, double low, double high,
                   bool integer, double property_min, double property_max,
                   uint64_t seed, bool ease_in, double starting_value, uint64_t shot_index);
bool same_value(double a, double b, bool integer);
}
