#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "lob/event.hpp"
#include "lob/order.hpp"

namespace lob {

static constexpr std::size_t kOrderGroupLegCount = 3U;

struct alignas(64) OrderRequest {
    std::uint64_t quantity {};
    std::uint64_t timestamp {};
    std::int64_t price {};
    std::int64_t expected_price {};
    double slippage_tolerance {};
    AssetID asset_id {};
    Side side {Side::Buy};
};
static_assert(sizeof(OrderRequest) == 64);

struct alignas(64) OrderExecutionReport {
    std::int64_t expected_price {};
    std::int64_t vwap_price {};
    std::uint64_t requested_quantity {};
    std::uint64_t filled_quantity {};
    AssetID asset_id {};
    Side side {Side::Buy};
    bool fully_filled {};
    bool slippage_breached {};
};
static_assert(sizeof(OrderExecutionReport) == 64);

struct OrderGroup {
    std::uint64_t group_id {};
    std::uint64_t timestamp {};
    std::uint64_t intra_leg_latency_ns {75U};
    std::uint64_t intra_leg_jitter_ns {25U};
    double slippage_tolerance {0.0002};
    std::array<OrderRequest, kOrderGroupLegCount> legs {};
};

struct OrderGroupResult {
    std::uint64_t group_id {};
    std::array<OrderExecutionReport, kOrderGroupLegCount> reports {};
    bool completed {};
    bool panic_triggered {};
};

}  // namespace lob
