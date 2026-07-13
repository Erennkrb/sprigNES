// No-op stub for the profiling helper from shuichitakano/pico_lib.
// sprigNES doesn't use the on-screen work meter.
#pragma once
#include <stdint.h>

namespace util {
inline void WorkMeterMark(uint16_t) {}
inline void WorkMeterReset() {}
}  // namespace util
