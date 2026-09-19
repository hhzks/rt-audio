#pragma once
#include "ffi/rt_ffi.h"
#include "io/DeviceFactory.h"

#include <cstdint>

namespace rt {

inline std::uint32_t capsFlags(const BackendCaps& c) noexcept {
    return (c.ring ? RT_CAP_RING : 0u) | (c.oneDriver ? RT_CAP_ONE_DRIVER : 0u) |
           (c.driverPanel ? RT_CAP_DRIVER_PANEL : 0u) |
           (c.exclusiveMode ? RT_CAP_EXCLUSIVE_MODE : 0u) |
           (c.rateFromDevice ? RT_CAP_RATE_FROM_DEVICE : 0u) |
           (c.blockZeroPreferred ? RT_CAP_BLOCK_ZERO_PREFERRED : 0u) |
           (c.blockRounded ? RT_CAP_BLOCK_ROUNDED : 0u);
}

} // namespace rt
