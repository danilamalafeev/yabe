#pragma once

#include <cstddef>
#include <cstdint>

#include "lob/event.hpp"
#include "lob/sim/latency_model.hpp"
#include "lob/sim/matching_book.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim {

struct ProductSpec {
    AssetID base_asset_id {};
    AssetID quote_asset_id {1U};
    std::int64_t maker_fee_bps {};
    std::int64_t taker_fee_bps {};
};

template <std::size_t OrderCapacity = 256U,
          std::size_t PriceLevelCapacity = 64U,
          std::size_t MaxExecutionReports = 64U>
struct MarketState {
    using MatchingBookType = MatchingBook<OrderCapacity, PriceLevelCapacity, MaxExecutionReports>;

    MarketID market_id {invalid_market_id};
    MarketKey market_key {};
    ProductSpec product_spec {};
    LatencyModel order_entry_latency_model {};
    LatencyModel cancel_latency_model {};
    LatencyModel market_data_latency_model {};
    MatchingBookType matching_book {};
};

}  // namespace lob::sim
