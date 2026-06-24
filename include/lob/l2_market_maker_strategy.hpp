#pragma once

#include <cstdint>

#include "lob/l2_order_book.hpp"
#include "lob/l2_strategy.hpp"
#include "lob/order.hpp"

namespace lob {

class L2MarketMakerStrategy final : public L2Strategy {
public:
    struct Config {
        double quote_offset {5.0};
        std::uint64_t quote_quantity {1'000'000U};
        std::uint64_t refresh_interval_ns {1'000'000'000ULL};
    };

    L2MarketMakerStrategy() : L2MarketMakerStrategy(Config {}) {}
    explicit L2MarketMakerStrategy(Config config) : config_(config) {}

    void on_tick(AssetID asset_id, const L2OrderBook& book, OrderGateway& gateway) override {
        const double best_bid = static_cast<double>(book.effective_best_bid()) / 100'000'000.0;
        const double best_ask = static_cast<double>(book.effective_best_ask()) / 100'000'000.0;
        const std::uint64_t timestamp = gateway.current_timestamp();
        if (best_bid <= 0.0 || best_ask <= 0.0 || timestamp < next_refresh_timestamp_) {
            return;
        }

        cancel_active_quotes(asset_id, gateway);

        const double mid_price = (best_bid + best_ask) * 0.5;
        bid_order_id_ = gateway.submit_order(
            asset_id,
            Side::Buy,
            mid_price - config_.quote_offset,
            config_.quote_quantity,
            timestamp
        );
        bid_remaining_quantity_ = config_.quote_quantity;

        ask_order_id_ = gateway.submit_order(
            asset_id,
            Side::Sell,
            mid_price + config_.quote_offset,
            config_.quote_quantity,
            timestamp
        );
        ask_remaining_quantity_ = config_.quote_quantity;

        next_refresh_timestamp_ = timestamp + config_.refresh_interval_ns;
    }

    void on_fill(const StrategyFill& fill, OrderGateway& gateway) override {
        (void)gateway;

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
    void cancel_active_quotes(AssetID asset_id, OrderGateway& gateway) {
        if (bid_order_id_ != 0U && gateway.cancel_order(asset_id, bid_order_id_)) {
            bid_order_id_ = 0U;
            bid_remaining_quantity_ = 0U;
        }

        if (ask_order_id_ != 0U && gateway.cancel_order(asset_id, ask_order_id_)) {
            ask_order_id_ = 0U;
            ask_remaining_quantity_ = 0U;
        }
    }

    Config config_ {};
    std::uint64_t bid_order_id_ {};
    std::uint64_t ask_order_id_ {};
    std::uint64_t bid_remaining_quantity_ {};
    std::uint64_t ask_remaining_quantity_ {};
    std::uint64_t next_refresh_timestamp_ {};
};

}  // namespace lob
