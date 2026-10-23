// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

namespace modulator {
enum class Wave { Sine, Triangle, RisingSaw, FallingSaw, Square, Glider };
// Glider joins seeded random knots with zero velocity/acceleration at each knot.
double normalized(Wave wave, double cycles, uint64_t seed = 0);
double mapped(Wave wave, double elapsed, double period, double low, double high,
              bool integer, double property_min, double property_max,
              uint64_t seed = 0, bool ease_in = false, double starting_value = 0);
double eased_return(double elapsed, double period, double from, double to,
                    bool integer, double property_min, double property_max);
bool same_value(double a, double b, bool integer);
}
