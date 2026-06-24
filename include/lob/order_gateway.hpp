#pragma once

#include <cstdint>

#include "lob/oms.hpp"
#include "lob/trade.hpp"

namespace lob {

enum class LiquidityRole : std::uint8_t {
    Maker,
    Taker
};

struct MarketSnapshot {
    std::uint64_t ts {};
    std::uint32_t asset {};
    double bid_p {};
    double bid_q {};
    double ask_p {};
    double ask_q {};
};

struct FillEvent {
    std::uint64_t order_id {};
    std::int64_t price {};
    std::uint64_t quantity {};
    std::uint64_t timestamp {};
    AssetID asset_id {};
    Side side {Side::Buy};
    LiquidityRole liquidity_role {LiquidityRole::Maker};
};
static_assert(sizeof(FillEvent) == 40);

using StrategyFill = FillEvent;

class OrderGateway {
public:
    virtual ~OrderGateway() = default;

    [[nodiscard]] virtual std::uint64_t submit_order(
        AssetID asset_id,
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    ) = 0;

    [[nodiscard]] std::uint64_t submit_order(
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    ) {
        return submit_order(0U, side, price, quantity, timestamp);
    }

    [[nodiscard]] virtual bool cancel_order(AssetID asset_id, std::uint64_t order_id) = 0;

    [[nodiscard]] bool cancel_order(std::uint64_t order_id) {
        return cancel_order(0U, order_id);
    }

    [[nodiscard]] virtual OrderGroupResult execute_group(const OrderGroup& group) {
        OrderGroupResult result {};
        result.group_id = group.group_id;
        return result;
    }

    [[nodiscard]] virtual std::uint64_t current_timestamp() const noexcept = 0;
};

}  // namespace lob
