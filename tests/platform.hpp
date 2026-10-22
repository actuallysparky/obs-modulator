#pragma once
#include <cstdint>
namespace test_platform {
void initialize();
void process_events();
uint64_t resident_bytes();
double cpu_seconds();
}
