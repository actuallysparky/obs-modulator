// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace test_platform {
void initialize();
void process_events();
uint64_t resident_bytes();
double cpu_seconds();
}
