#pragma once

namespace silicon_probe::shared_types {

// Cache levels used by cache benchmarks.
enum class CacheLevel {
    l1d = 0,  // L1 data cache.
    l2  = 1,  // L2 cache.
    l3  = 2,  // L3 or last-level cache.
};

}  // namespace silicon_probe::shared_types
