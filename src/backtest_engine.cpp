#include "lob/backtest_engine.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace lob {
namespace {

constexpr double kQuantityScale = 100'000'000.0;
constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000ULL;

[[nodiscard]] double QuantityToUnits(std::uint64_t quantity) noexcept {
    return static_cast<double>(quantity) / kQuantityScale;
}

[[nodiscard]] double PositionToUnits(std::int64_t position) noexcept {
    return static_cast<double>(position) / kQuantityScale;
}

[[nodiscard]] double SnapshotMidPrice(const MarketSnapshot& snapshot) noexcept {
    if (snapshot.bid_p > 0.0 && snapshot.ask_p > 0.0) {
        return (snapshot.bid_p + snapshot.ask_p) * 0.5;
    }
    if (snapshot.bid_p > 0.0) {
        return snapshot.bid_p;
    }
    return snapshot.ask_p;
}

}  // namespace

BacktestEngine::BacktestEngine(Strategy<BacktestEngine>& strategy)
    : BacktestEngine(strategy, Config {}) {}

BacktestEngine::BacktestEngine(Strategy<BacktestEngine>& strategy, Config config)
    : strategy_(strategy),
      config_(config),
      rng_(config.latency.rng_seed),
      exponential_jitter_(config.latency.jitter_mean_ns > 0.0 ? 1.0 / config.latency.jitter_mean_ns : 1.0),
      lognormal_jitter_(config.latency.lognormal_mu, config.latency.lognormal_sigma),
      cash_(config.initial_cash) {
    trade_buffer_.reserve(1'024U);
    pending_fills_.reserve(256U);
}

BacktestEngine::Result BacktestEngine::run(const std::filesystem::path& file_path) {
    Result result {};
    result.initial_cash = config_.initial_cash;
    order_book_.clear();
    pending_orders_.clear();
    live_order_count_ = 0U;
    snapshot_ = MarketSnapshot {};
    next_strategy_order_id_ = 1ULL << 63U;
    next_equity_sample_timestamp_ = 0U;
    current_time_ns_ = 0U;
    dropped_pending_orders_ = 0U;
    execution_ = ExecutionDiagnostics {};
    cash_ = config_.initial_cash;
    position_ = 0;
    equity_curve_.clear();
    equity_curve_.reserve(config_.equity_curve_reserve);
    open_trace_log();

    strategy_.on_start(*this);

    result.replay_stats.orders_processed = parser_.parse_file(file_path, [this, &result](const Order& order) {
        if (result.replay_stats.first_timestamp == 0U) {
            result.replay_stats.first_timestamp = order.timestamp;
            next_equity_sample_timestamp_ = order.timestamp * kNanosecondsPerMillisecond;
        }

        result.replay_stats.last_timestamp = order.timestamp;
        current_time_ns_ = order.timestamp * kNanosecondsPerMillisecond;
        update_snapshot(current_time_ns_, order.price);
        result.replay_stats.generated_trades += release_pending_orders(current_time_ns_);

        Order market_order {order};
        market_order.timestamp = current_time_ns_;
        process_market_order(market_order);
        result.replay_stats.generated_trades += static_cast<std::uint64_t>(trade_buffer_.size());

        update_snapshot(current_time_ns_, order.price);
        route_trades(trade_buffer_);
        strategy_.on_tick(0U, order_book_, *this);
        result.replay_stats.generated_trades += release_pending_orders(current_time_ns_);
        sample_equity(current_time_ns_);
    });

    if (SnapshotMidPrice(snapshot_) > 0.0) {
        equity_curve_.push_back(current_equity());
        write_trace_row(current_time_ns_, 0, "None");
    }

    result.dropped_pending_orders = dropped_pending_orders_;
    result.execution = execution_;
    result.final_cash = cash_;
    result.final_position = position_;
    result.final_mid_price = SnapshotMidPrice(snapshot_);
    result.equity_curve = std::move(equity_curve_);
    close_trace_log();
    return result;
}

std::uint64_t BacktestEngine::submit_order_impl(
    AssetID asset_id,
    Side side,
    double price,
    std::uint64_t quantity,
    std::uint64_t timestamp
) {
    const std::uint64_t order_id = next_strategy_order_id_++;
    if (quantity == 0U) {
        return order_id;
    }

    const std::int64_t price_ticks = FixedMatchingBook::price_to_ticks(price);
    const std::uint64_t release_time_ns = current_time_ns_ + sample_latency_ns();
    if (!add_live_order(LiveOrder {
        .asset_id = asset_id,
        .order_id = order_id,
        .side = side,
        .price = price_ticks,
        .remaining_quantity = quantity,
        .volume_ahead = 0U,
        .active = false,
        .canceled = false,
    })) {
        ++dropped_pending_orders_;
        return order_id;
    }

    if (!pending_orders_.push(PendingOrder {
        .type = PendingCommandType::Submit,
        .asset_id = asset_id,
        .release_time_ns = release_time_ns,
        .order_id = order_id,
        .side = side,
        .price = price_ticks,
        .quantity = quantity,
    })) {
        erase_live_order(find_live_order_index(order_id));
        ++dropped_pending_orders_;
    }

    (void)timestamp;
    return order_id;
}

bool BacktestEngine::cancel_order_impl(AssetID asset_id, std::uint64_t order_id) {
    const std::uint64_t release_time_ns = current_time_ns_ + sample_latency_ns();
    if (!pending_orders_.push(PendingOrder {
        .type = PendingCommandType::Cancel,
        .asset_id = asset_id,
        .release_time_ns = release_time_ns,
        .order_id = order_id,
    })) {
        ++dropped_pending_orders_;
        return false;
    }

    return true;
}

std::uint64_t BacktestEngine::current_timestamp_impl() const noexcept {
    return snapshot_.ts;
}

void BacktestEngine::process_market_order(const Order& order) {
    order_book_.process_order(order, trade_buffer_);
    update_passive_queue_on_market_trade(order);
    apply_pessimistic_volume_update();
}

void BacktestEngine::route_trades(const std::vector<Trade>& trades) {
    pending_fills_.clear();

    for (const Trade& trade : trades) {
        const bool own_buy = is_strategy_order(trade.buyer_id);
        const bool own_sell = is_strategy_order(trade.seller_id);

        if (!own_buy && !own_sell) {
            write_trace_row(trade.timestamp, 0, "None");
        }

        if (own_buy) {
            pending_fills_.push_back(StrategyFill {
                .order_id = trade.buyer_id,
                .price = trade.price,
                .quantity = trade.quantity,
                .timestamp = trade.timestamp,
                .asset_id = 0U,
                .side = Side::Buy,
                .liquidity_role = trade.buyer_id == trade.taker_order_id ? LiquidityRole::Taker : LiquidityRole::Maker,
            });
        }

        if (own_sell) {
            pending_fills_.push_back(StrategyFill {
                .order_id = trade.seller_id,
                .price = trade.price,
                .quantity = trade.quantity,
                .timestamp = trade.timestamp,
                .asset_id = 0U,
                .side = Side::Sell,
                .liquidity_role = trade.seller_id == trade.taker_order_id ? LiquidityRole::Taker : LiquidityRole::Maker,
            });
        }
    }

    for (const StrategyFill& fill : pending_fills_) {
        route_strategy_fill(fill);
    }
}

void BacktestEngine::route_strategy_fill(const StrategyFill& fill) {
    const double quantity_units = QuantityToUnits(fill.quantity);
    const double notional = (static_cast<double>(fill.price) / 100'000'000.0) * quantity_units;
    const double fee_bps = fill.liquidity_role == LiquidityRole::Maker
        ? config_.maker_fee_bps
        : config_.taker_fee_bps;
    const double fee = notional * fee_bps * 0.0001;

    if (fill.liquidity_role == LiquidityRole::Maker) {
        ++execution_.maker_fills_count;
        execution_.maker_volume += quantity_units;
        execution_.maker_notional += notional;
    } else {
        ++execution_.taker_fills_count;
        execution_.taker_volume += quantity_units;
        execution_.taker_notional += notional;
    }

    const std::size_t live_order_index = find_live_order_index(fill.order_id);
    if (live_order_index != kInvalidOrderIndex) {
        LiveOrder& live_order = live_orders_[live_order_index];
        live_order.remaining_quantity =
            live_order.remaining_quantity > fill.quantity
                ? live_order.remaining_quantity - fill.quantity
                : 0U;

        if (live_order.remaining_quantity == 0U) {
            erase_live_order(live_order_index);
        }
    }

    if (fill.side == Side::Buy) {
        cash_ -= notional;
        cash_ -= fee;
        position_ += static_cast<std::int64_t>(fill.quantity);
    } else {
        cash_ += notional;
        cash_ -= fee;
        position_ -= static_cast<std::int64_t>(fill.quantity);
    }

    strategy_.on_fill(fill, *this);
    write_trace_row(fill.timestamp, 1, fill.side == Side::Buy ? "Buy" : "Sell");
}

std::uint64_t BacktestEngine::release_pending_orders(std::uint64_t now_ns) {
    std::uint64_t generated_trades = 0U;

    while (!pending_orders_.empty()) {
        const PendingOrder pending = pending_orders_.top();
        if (pending.release_time_ns > now_ns) {
            break;
        }

        pending_orders_.pop();
        execute_pending_order(pending);
        generated_trades += static_cast<std::uint64_t>(trade_buffer_.size());
    }

    return generated_trades;
}

void BacktestEngine::execute_pending_order(const PendingOrder& pending) {
    trade_buffer_.clear();

    const std::size_t live_order_index = find_live_order_index(pending.order_id);
    if (pending.type == PendingCommandType::Cancel) {
        if (live_order_index == kInvalidOrderIndex) {
            return;
        }

        LiveOrder& live_order = live_orders_[live_order_index];
        if (!live_order.active) {
            live_order.canceled = true;
            return;
        }

        erase_live_order(live_order_index);
        return;
    }

    if (live_order_index == kInvalidOrderIndex) {
        return;
    }

    LiveOrder& live_order = live_orders_[live_order_index];
    if (live_order.canceled) {
        erase_live_order(live_order_index);
        return;
    }

    if (is_aggressive(live_order)) {
        sweep_book(live_order, pending.release_time_ns);
    }

    const std::size_t post_sweep_index = find_live_order_index(pending.order_id);
    if (post_sweep_index == kInvalidOrderIndex) {
        return;
    }

    LiveOrder& resting_order = live_orders_[post_sweep_index];
    resting_order.active = true;
    resting_order.volume_ahead = order_book_.get_total_quantity_at_price(resting_order.side, resting_order.price);
}

void BacktestEngine::sweep_book(LiveOrder& live_order, std::uint64_t timestamp) {
    if (live_order.remaining_quantity == 0U) {
        return;
    }

    const std::uint64_t order_id = live_order.order_id;
    const AssetID asset_id = live_order.asset_id;
    const std::int64_t limit_ticks = live_order.price;
    if (live_order.side == Side::Buy) {
        order_book_.for_each_level(Side::Sell, [&](std::int64_t price_ticks, std::uint64_t level_quantity) {
            const std::size_t index = find_live_order_index(order_id);
            if (index == kInvalidOrderIndex || price_ticks > limit_ticks) {
                return false;
            }
            const std::uint64_t fill_quantity = std::min(level_quantity, live_orders_[index].remaining_quantity);
            route_strategy_fill(StrategyFill {
                .order_id = order_id,
                .price = price_ticks,
                .quantity = fill_quantity,
                .timestamp = timestamp,
                .asset_id = asset_id,
                .side = Side::Buy,
                .liquidity_role = LiquidityRole::Taker,
            });
            return find_live_order_index(order_id) != kInvalidOrderIndex;
        });
        return;
    }

    order_book_.for_each_level(Side::Buy, [&](std::int64_t price_ticks, std::uint64_t level_quantity) {
        const std::size_t index = find_live_order_index(order_id);
        if (index == kInvalidOrderIndex || price_ticks < limit_ticks) {
            return false;
        }
        const std::uint64_t fill_quantity = std::min(level_quantity, live_orders_[index].remaining_quantity);
        route_strategy_fill(StrategyFill {
            .order_id = order_id,
            .price = price_ticks,
            .quantity = fill_quantity,
            .timestamp = timestamp,
            .asset_id = asset_id,
            .side = Side::Sell,
            .liquidity_role = LiquidityRole::Taker,
        });
        return find_live_order_index(order_id) != kInvalidOrderIndex;
    });
}

void BacktestEngine::update_passive_queue_on_market_trade(const Order& market_order) {
    std::uint64_t trade_quantity_remaining = market_order.quantity;
    std::size_t index = 0U;

    while (index < live_order_count_ && trade_quantity_remaining > 0U) {
        LiveOrder& live_order = live_orders_[index];
        const bool can_fill =
            live_order.active
            && !live_order.canceled
            && live_order.side != market_order.side
            && live_order.price == market_order.price;

        if (!can_fill) {
            ++index;
            continue;
        }

        if (trade_quantity_remaining <= live_order.volume_ahead) {
            live_order.volume_ahead -= trade_quantity_remaining;
            break;
        }

        trade_quantity_remaining -= live_order.volume_ahead;
        live_order.volume_ahead = 0U;

        const std::uint64_t fill_quantity = live_order.remaining_quantity < trade_quantity_remaining
            ? live_order.remaining_quantity
            : trade_quantity_remaining;
        if (fill_quantity == 0U) {
            ++index;
            continue;
        }

        const std::uint64_t order_id = live_order.order_id;
        route_strategy_fill(StrategyFill {
            .order_id = order_id,
            .price = live_order.price,
            .quantity = fill_quantity,
            .timestamp = market_order.timestamp,
            .asset_id = live_order.asset_id,
            .side = live_order.side,
            .liquidity_role = LiquidityRole::Maker,
        });
        trade_quantity_remaining -= fill_quantity;

        index = find_live_order_index(order_id) == kInvalidOrderIndex ? 0U : index + 1U;
    }
}

void BacktestEngine::apply_pessimistic_volume_update() {
    for (std::size_t index = 0U; index < live_order_count_; ++index) {
        LiveOrder& live_order = live_orders_[index];
        if (!live_order.active || live_order.canceled) {
            continue;
        }

        const std::uint64_t observed_total = order_book_.get_total_quantity_at_price(
            live_order.side,
            live_order.price
        );
        const std::uint64_t modeled_total = observed_total + live_order.remaining_quantity;
        const std::uint64_t max_volume_ahead = modeled_total > live_order.remaining_quantity
            ? modeled_total - live_order.remaining_quantity
            : 0U;
        if (live_order.volume_ahead > max_volume_ahead) {
            live_order.volume_ahead = max_volume_ahead;
        }
    }
}

std::uint64_t BacktestEngine::sample_latency_ns() {
    std::uint64_t jitter_ns = 0U;

    switch (config_.latency.distribution) {
        case LatencyDistribution::None:
            break;
        case LatencyDistribution::Exponential:
            if (config_.latency.jitter_mean_ns > 0.0) {
                jitter_ns = static_cast<std::uint64_t>(exponential_jitter_(rng_));
            }
            break;
        case LatencyDistribution::LogNormal:
            if (config_.latency.lognormal_sigma > 0.0) {
                jitter_ns = static_cast<std::uint64_t>(lognormal_jitter_(rng_));
            }
            break;
    }

    return config_.latency.base_latency_ns + jitter_ns;
}

void BacktestEngine::sample_equity(std::uint64_t timestamp) {
    if (SnapshotMidPrice(snapshot_) <= 0.0 || config_.equity_sample_interval_ns == 0U) {
        return;
    }

    while (timestamp >= next_equity_sample_timestamp_) {
        equity_curve_.push_back(current_equity());
        write_trace_row(timestamp, 0, "None");
        next_equity_sample_timestamp_ += config_.equity_sample_interval_ns;
    }
}

void BacktestEngine::update_snapshot(std::uint64_t timestamp, std::int64_t fallback_price_ticks) {
    snapshot_.ts = timestamp;
    snapshot_.asset = 0U;

    const std::int64_t best_bid = order_book_.best_bid_ticks();
    const std::int64_t best_ask = order_book_.best_ask_ticks();

    snapshot_.bid_p = best_bid > 0 ? FixedMatchingBook::ticks_to_price(best_bid) : 0.0;
    snapshot_.ask_p = best_ask > 0 ? FixedMatchingBook::ticks_to_price(best_ask) : 0.0;
    snapshot_.bid_q = best_bid > 0 ? QuantityToUnits(order_book_.total_quantity_at_price(Side::Buy, best_bid)) : 0.0;
    snapshot_.ask_q = best_ask > 0 ? QuantityToUnits(order_book_.total_quantity_at_price(Side::Sell, best_ask)) : 0.0;

    if (SnapshotMidPrice(snapshot_) <= 0.0) {
        const double fallback = FixedMatchingBook::ticks_to_price(fallback_price_ticks);
        snapshot_.bid_p = fallback;
        snapshot_.ask_p = fallback;
    }
}

void BacktestEngine::open_trace_log() {
    if (!config_.trace_enabled) {
        return;
    }

    trace_log_.open(config_.trace_path, std::ios::out | std::ios::trunc);
    if (!trace_log_.is_open()) {
        throw std::runtime_error("Unable to open trace log: " + config_.trace_path.string());
    }

    trace_log_ << "timestamp,mid_price,equity,is_bot_trade,trade_side\n";
    trace_log_ << std::fixed << std::setprecision(8);
}

void BacktestEngine::close_trace_log() {
    if (trace_log_.is_open()) {
        trace_log_.close();
    }
}

void BacktestEngine::write_trace_row(std::uint64_t timestamp, int is_bot_trade, const char* trade_side) {
    const double mid_price = SnapshotMidPrice(snapshot_);
    if (!trace_log_.is_open() || mid_price <= 0.0) {
        return;
    }

    trace_log_ << timestamp << ','
               << mid_price << ','
               << current_equity() << ','
               << is_bot_trade << ','
               << trade_side << '\n';
}

double BacktestEngine::current_equity() const noexcept {
    return cash_ + (PositionToUnits(position_) * SnapshotMidPrice(snapshot_));
}

bool BacktestEngine::is_strategy_order(std::uint64_t order_id) const noexcept {
    return find_live_order_index(order_id) != kInvalidOrderIndex;
}

std::size_t BacktestEngine::find_live_order_index(std::uint64_t order_id) const noexcept {
    for (std::size_t index = 0U; index < live_order_count_; ++index) {
        if (live_orders_[index].order_id == order_id) {
            return index;
        }
    }

    return kInvalidOrderIndex;
}

bool BacktestEngine::is_aggressive(const LiveOrder& live_order) const noexcept {
    if (live_order.side == Side::Buy) {
        const std::int64_t best_ask = order_book_.best_ask_ticks();
        return best_ask > 0 && live_order.price >= best_ask;
    }

    const std::int64_t best_bid = order_book_.best_bid_ticks();
    return best_bid > 0 && live_order.price <= best_bid;
}

bool BacktestEngine::add_live_order(const LiveOrder& live_order) noexcept {
    if (live_order_count_ == kLiveOrderCapacity) {
        return false;
    }

    live_orders_[live_order_count_++] = live_order;
    return true;
}

void BacktestEngine::erase_live_order(std::size_t index) noexcept {
    if (index >= live_order_count_) {
        return;
    }

    --live_order_count_;
    if (index != live_order_count_) {
        live_orders_[index] = live_orders_[live_order_count_];
    }
}

}  // namespace lob
