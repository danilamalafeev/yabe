#pragma once

#include <cstdint>

namespace lob {

// Trade captures the execution report emitted by a successful match.
struct Trade {
    std::uint64_t buyer_id {};
    std::uint64_t seller_id {};
    std::uint64_t taker_order_id {};
    std::int64_t price {};
    std::uint64_t quantity {};
    std::uint64_t timestamp {};
};

}  // namespace lob
