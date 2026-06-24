#pragma once

#include <cstdint>

namespace lob {

struct alignas(32) L2UpdateEvent {
    std::uint64_t timestamp_ns {};
    std::int64_t price {};
    std::uint64_t qty {};
    bool is_snapshot {};
    bool is_bid {};
};
static_assert(sizeof(L2UpdateEvent) == 32);

}  // namespace lob
