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

struct alignas(8) FillEvent {
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

template <typename Derived>
class OrderGateway {
public:
    [[nodiscard]] std::uint64_t submit_order(
        AssetID asset_id,
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    ) {
        return derived().submit_order_impl(asset_id, side, price, quantity, timestamp);
    }

    [[nodiscard]] std::uint64_t submit_order(
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    ) {
        return submit_order(0U, side, price, quantity, timestamp);
    }

    [[nodiscard]] bool cancel_order(AssetID asset_id, std::uint64_t order_id) {
        return derived().cancel_order_impl(asset_id, order_id);
    }

    [[nodiscard]] bool cancel_order(std::uint64_t order_id) {
        return cancel_order(0U, order_id);
    }

    [[nodiscard]] OrderGroupResult execute_group(const OrderGroup& group) {
        if constexpr (requires(Derived& gateway) { gateway.execute_group_impl(group); }) {
            return derived().execute_group_impl(group);
        } else {
            OrderGroupResult result {};
            result.group_id = group.group_id;
            return result;
        }
    }

    [[nodiscard]] std::uint64_t current_timestamp() const noexcept {
        return derived().current_timestamp_impl();
    }

protected:
    ~OrderGateway() = default;

private:
    [[nodiscard]] Derived& derived() noexcept {
        return static_cast<Derived&>(*this);
    }

    [[nodiscard]] const Derived& derived() const noexcept {
        return static_cast<const Derived&>(*this);
    }
};

}  // namespace lob
