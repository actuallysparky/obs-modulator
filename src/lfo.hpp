// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>

namespace modulator {
enum class Wave { Sine, Triangle, RisingSaw, FallingSaw, Square };
double normalized(Wave wave, double cycles);
double mapped(Wave wave, double elapsed, double period, double low, double high,
              bool integer, double property_min, double property_max);
bool same_value(double a, double b, bool integer);
}
