#pragma once

#include <cstdint>

namespace lob {

// Side is stored as a compact strongly typed enum for branch-safe comparisons.
enum class Side : std::uint8_t {
    Buy,
    Sell
};

// Order is the canonical immutable payload stored inside each price-level queue.
struct Order {
    std::uint64_t id {};
    std::int64_t price {};
    std::uint64_t quantity {};
    Side side {Side::Buy};
    std::uint64_t timestamp {};
};

}  // namespace lob
