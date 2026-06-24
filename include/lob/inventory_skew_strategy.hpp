#pragma once

#include <cstdint>
#include <vector>

#include "lob/order.hpp"
#include "lob/strategy.hpp"

namespace lob {

template <typename Gateway>
class InventorySkewStrategy final : public Strategy<Gateway> {
public:
    struct Config {
        double target_position {};
        double max_position {10.0};
        double base_spread {10.0};
        double risk_aversion_gamma {2.0};
        double imbalance_threshold {3.0};
        std::uint64_t quote_quantity {1'000'000U};
        std::uint64_t refresh_interval_ns {1'000'000'000ULL};
    };

    InventorySkewStrategy() : InventorySkewStrategy(Config {}) {}
    explicit InventorySkewStrategy(Config config) : config_(config) {
        l2_bids_.reserve(kL2Depth);
        l2_asks_.reserve(kL2Depth);
    }

    void on_tick(AssetID asset_id, const FixedMatchingBook& book, Gateway& gateway) override {
        const std::uint64_t timestamp = gateway.current_timestamp();
        if (timestamp < next_refresh_timestamp_) {
            return;
        }

        const double best_bid = book.get_best_bid();
        const double best_ask = book.get_best_ask();
        if (best_bid <= 0.0 || best_ask <= 0.0) {
            return;
        }

        cancel_active_quotes(asset_id, gateway);

        book.get_l2_snapshot(l2_bids_, l2_asks_, kL2Depth);
        double bid_volume = 0.0;
        for (const FixedPriceLevelInfo& level : l2_bids_) {
            bid_volume += static_cast<double>(level.quantity_lots);
        }

        double ask_volume = 0.0;
        for (const FixedPriceLevelInfo& level : l2_asks_) {
            ask_volume += static_cast<double>(level.quantity_lots);
        }

        const bool massive_sell_pressure = ask_volume > bid_volume * config_.imbalance_threshold;
        const bool massive_buy_pressure = bid_volume > ask_volume * config_.imbalance_threshold;
        const double inventory_delta = position_ - config_.target_position;
        const double skew = inventory_delta * config_.risk_aversion_gamma;
        const double half_spread = config_.base_spread * 0.5;
        const double mid_price = (best_bid + best_ask) * 0.5;
        const bool can_increase_long = position_ < config_.max_position;
        const bool can_increase_short = position_ > -config_.max_position;

        if (can_increase_long && !massive_sell_pressure) {
            bid_order_id_ = gateway.submit_order(
                asset_id,
                Side::Buy,
                mid_price - half_spread - skew,
                config_.quote_quantity,
                timestamp
            );
            bid_remaining_quantity_ = config_.quote_quantity;
        }

        if (can_increase_short && !massive_buy_pressure) {
            ask_order_id_ = gateway.submit_order(
                asset_id,
                Side::Sell,
                mid_price + half_spread - skew,
                config_.quote_quantity,
                timestamp
            );
            ask_remaining_quantity_ = config_.quote_quantity;
        }

        next_refresh_timestamp_ = timestamp + config_.refresh_interval_ns;
    }

    void on_fill(const StrategyFill& fill, Gateway& gateway) override {
        (void)gateway;

        const double filled_units = static_cast<double>(fill.quantity) / kQuantityScale;
        if (fill.side == Side::Buy) {
            position_ += filled_units;
        } else {
            position_ -= filled_units;
        }

        if (fill.order_id == bid_order_id_) {
            bid_remaining_quantity_ = bid_remaining_quantity_ > fill.quantity
                ? bid_remaining_quantity_ - fill.quantity
                : 0U;
            if (bid_remaining_quantity_ == 0U) {
                bid_order_id_ = 0U;
            }
        } else if (fill.order_id == ask_order_id_) {
            ask_remaining_quantity_ = ask_remaining_quantity_ > fill.quantity
                ? ask_remaining_quantity_ - fill.quantity
                : 0U;
            if (ask_remaining_quantity_ == 0U) {
                ask_order_id_ = 0U;
            }
        }
    }

private:
    void cancel_active_quotes(AssetID asset_id, Gateway& gateway) {
        if (bid_order_id_ != 0U && gateway.cancel_order(asset_id, bid_order_id_)) {
            bid_order_id_ = 0U;
            bid_remaining_quantity_ = 0U;
        }

        if (ask_order_id_ != 0U && gateway.cancel_order(asset_id, ask_order_id_)) {
            ask_order_id_ = 0U;
            ask_remaining_quantity_ = 0U;
        }
    }

    static constexpr int kL2Depth = 5;
    static constexpr double kQuantityScale = 100'000'000.0;

    Config config_ {};
    double position_ {};
    std::vector<FixedPriceLevelInfo> l2_bids_ {};
    std::vector<FixedPriceLevelInfo> l2_asks_ {};
    std::uint64_t bid_order_id_ {};
    std::uint64_t ask_order_id_ {};
    std::uint64_t bid_remaining_quantity_ {};
    std::uint64_t ask_remaining_quantity_ {};
    std::uint64_t next_refresh_timestamp_ {};
};

}  // namespace lob
