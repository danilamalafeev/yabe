#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "lob/order.hpp"
#include "lob/sim/matching_book.hpp"
#include "lob/trade.hpp"

namespace lob {

struct FixedPriceLevelInfo {
    std::int64_t price_ticks {};
    std::uint64_t quantity_lots {};
};

class FixedMatchingBook {
public:
    static constexpr std::int64_t kScale = 100'000'000LL;
    using Core = sim::MatchingBook<8'192U, 1'024U, 512U, 16'385U>;
    using Result = Core::Result;

    FixedMatchingBook()
        : core_(std::make_unique<Core>()) {}

    FixedMatchingBook(const FixedMatchingBook&) = delete;
    FixedMatchingBook& operator=(const FixedMatchingBook&) = delete;
    FixedMatchingBook(FixedMatchingBook&&) noexcept = default;
    FixedMatchingBook& operator=(FixedMatchingBook&&) noexcept = default;

    [[nodiscard]] static std::int64_t price_to_ticks(double price) noexcept {
        if (!std::isfinite(price) || price <= 0.0) {
            return 0;
        }
        const long double scaled = static_cast<long double>(price) * kScale;
        if (scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
            return 0;
        }
        return static_cast<std::int64_t>(std::llround(scaled));
    }

    [[nodiscard]] static double ticks_to_price(std::int64_t ticks) noexcept {
        return static_cast<double>(ticks) / static_cast<double>(kScale);
    }

    [[nodiscard]] Result submit(
        std::uint64_t order_id,
        Side side,
        std::int64_t price_ticks,
        std::uint64_t quantity_lots,
        std::uint64_t timestamp_ns,
        sim::TimeInForce time_in_force = sim::TimeInForce::GTC
    ) noexcept {
        return core_->submit_limit(sim::SimOrder {
            .order_id = order_id,
            .agent_id = sim::invalid_agent_id,
            .side = side,
            .price_ticks = price_ticks,
            .quantity_lots = quantity_lots,
            .timestamp_ns = timestamp_ns,
            .time_in_force = time_in_force,
        });
    }

    [[nodiscard]] Result submit(const Order& order) noexcept {
        return submit(order.id, order.side, order.price, order.quantity, order.timestamp);
    }

    void process_order(const Order& order, std::vector<Trade>& trades) {
        trades.clear();
        const Result result = submit(order);
        trades.reserve(result.execution_count / 2U);
        for (std::size_t index = 0U; index + 1U < result.execution_count; index += 2U) {
            const sim::TradeExecution& taker = result.executions[index];
            const sim::TradeExecution& maker = result.executions[index + 1U];
            trades.push_back(Trade {
                .buyer_id = taker.side == Side::Buy ? taker.order_id : maker.order_id,
                .seller_id = taker.side == Side::Sell ? taker.order_id : maker.order_id,
                .taker_order_id = taker.order_id,
                .price = taker.price_ticks,
                .quantity = taker.quantity_lots,
                .timestamp = taker.timestamp_ns,
            });
        }
    }

    [[nodiscard]] sim::CancelResult cancel(std::uint64_t order_id) noexcept {
        return core_->cancel(order_id);
    }

    [[nodiscard]] std::int64_t best_bid_ticks() const noexcept {
        return core_->best_bid().value_or(0);
    }

    [[nodiscard]] std::int64_t best_ask_ticks() const noexcept {
        return core_->best_ask().value_or(0);
    }

    [[nodiscard]] double get_best_bid() const noexcept {
        return ticks_to_price(best_bid_ticks());
    }

    [[nodiscard]] double get_best_ask() const noexcept {
        return ticks_to_price(best_ask_ticks());
    }

    [[nodiscard]] std::uint64_t total_quantity_at_price(
        Side side,
        std::int64_t price_ticks
    ) const noexcept {
        return core_->total_quantity_at_price(side, price_ticks);
    }

    [[nodiscard]] std::uint64_t get_total_quantity_at_price(Side side, double price) const noexcept {
        return total_quantity_at_price(side, price_to_ticks(price));
    }

    template <typename Visitor>
    void for_each_level(Side side, Visitor&& visitor) const noexcept {
        core_->for_each_level(side, static_cast<Visitor&&>(visitor));
    }

    void get_l2_snapshot(
        std::vector<FixedPriceLevelInfo>& bids,
        std::vector<FixedPriceLevelInfo>& asks,
        std::size_t depth
    ) const {
        bids.clear();
        asks.clear();
        bids.reserve(depth);
        asks.reserve(depth);
        core_->for_each_level(Side::Buy, [&](std::int64_t price, std::uint64_t quantity) {
            if (bids.size() == depth) {
                return false;
            }
            bids.push_back({price, quantity});
            return true;
        });
        core_->for_each_level(Side::Sell, [&](std::int64_t price, std::uint64_t quantity) {
            if (asks.size() == depth) {
                return false;
            }
            asks.push_back({price, quantity});
            return true;
        });
    }

    [[nodiscard]] std::size_t resting_order_count() const noexcept {
        return core_->resting_order_count();
    }

    [[nodiscard]] std::size_t active_price_level_count() const noexcept {
        return core_->active_price_level_count();
    }

    void clear() noexcept {
        core_->clear();
    }

private:
    std::unique_ptr<Core> core_;
};

}  // namespace lob
