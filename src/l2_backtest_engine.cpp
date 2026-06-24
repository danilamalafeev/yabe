#include "lob/l2_backtest_engine.hpp"

#include <cmath>
#include <stdexcept>

namespace lob {
namespace {

[[nodiscard]] double SnapshotMidPrice(double bid, double ask) noexcept {
    if (bid > 0.0 && ask > 0.0) {
        return (bid + ask) * 0.5;
    }
    if (bid > 0.0) {
        return bid;
    }
    return ask;
}

}  // namespace

L2BacktestEngine::L2BacktestEngine(Strategy& strategy)
    : L2BacktestEngine(strategy, Config {}) {}

L2BacktestEngine::L2BacktestEngine(Strategy& strategy, Config config)
    : strategy_(&strategy),
      config_(config),
      book_(config.max_book_levels_per_side),
      cash_(config.initial_cash) {
    equity_curve_.reserve(config_.equity_curve_reserve);
    if (config_.record_features) {
        feature_rows_.reserve(config_.feature_reserve);
    }
}

L2BacktestEngine::L2BacktestEngine(L2Strategy& strategy)
    : L2BacktestEngine(strategy, Config {}) {}

L2BacktestEngine::L2BacktestEngine(L2Strategy& strategy, Config config)
    : l2_strategy_(&strategy),
      config_(config),
      book_(config.max_book_levels_per_side),
      cash_(config.initial_cash) {
    equity_curve_.reserve(config_.equity_curve_reserve);
    if (config_.record_features) {
        feature_rows_.reserve(config_.feature_reserve);
    }
}

L2BacktestEngine::Result L2BacktestEngine::run(const std::filesystem::path& file_path) {
    if (config_.quantity_scale <= 0.0) {
        throw std::invalid_argument("L2BacktestEngine quantity_scale must be greater than zero");
    }
    if (config_.max_book_levels_per_side == 0U) {
        throw std::invalid_argument("L2BacktestEngine max_book_levels_per_side must be greater than zero");
    }

    reset_runtime();
    parser_ = L2UpdateCsvParser {file_path};

    Result result {};
    result.initial_cash = config_.initial_cash;
    if (l2_strategy_ != nullptr) {
        l2_strategy_->on_start(*this);
    } else {
        strategy_->on_start(*this);
    }

    while (parser_.has_next() || !pending_orders_.empty()) {
        if (
            !pending_orders_.empty() &&
            (!parser_.has_next() || pending_orders_.top().release_time_ns <= parser_.peek_time())
        ) {
            current_time_ns_ = pending_orders_.top().release_time_ns;
            release_pending_orders(current_time_ns_);
            continue;
        }

        const std::uint64_t batch_time_ns = parser_.peek_time();
        current_time_ns_ = batch_time_ns;
        release_pending_orders(current_time_ns_);
        apply_event_batch(result, batch_time_ns);
        process_active_crosses(current_time_ns_);
        dispatch_strategy_tick(result);
        release_pending_orders(current_time_ns_);
        process_active_crosses(current_time_ns_);
        sample_equity(current_time_ns_);
        sample_features(current_time_ns_);
    }

    if (current_mid_price() > 0.0) {
        equity_curve_.push_back(current_nav());
    }

    result.dropped_pending_orders = dropped_pending_orders_;
    result.orders_submitted = orders_submitted_;
    result.orders_canceled = orders_canceled_;
    result.active_orders = live_order_count_;
    result.execution = execution_;
    result.final_cash = cash_;
    result.final_position = position_;
    result.final_best_bid = static_cast<double>(book_.effective_best_bid()) / 100'000'000.0;
    result.final_best_ask = static_cast<double>(book_.effective_best_ask()) / 100'000'000.0;
    result.final_mid_price = current_mid_price();
    result.final_nav = current_nav();
    result.last_fill_timestamp = last_fill_timestamp_;
    result.equity_curve = std::move(equity_curve_);
    result.features = std::move(feature_rows_);
    return result;
}

std::uint64_t L2BacktestEngine::submit_order(
    AssetID asset_id,
    Side side,
    double price,
    std::uint64_t quantity,
    std::uint64_t timestamp
) {
    (void)timestamp;
    const std::uint64_t order_id = next_strategy_order_id_++;
    if (quantity == 0U) {
        return order_id;
    }
    if (!add_live_order(LiveOrder {
        .asset_id = asset_id,
        .order_id = order_id,
        .side = side,
        .price = price,
        .remaining_quantity = quantity,
        .active = false,
        .canceled = false,
    })) {
        ++dropped_pending_orders_;
        return order_id;
    }
    if (!pending_orders_.push(PendingOrder {
        .type = PendingCommandType::Submit,
        .asset_id = asset_id,
        .release_time_ns = current_time_ns_ + config_.latency_ns,
        .order_id = order_id,
        .side = side,
        .price = price,
        .quantity = quantity,
    })) {
        erase_live_order(find_live_order_index(order_id));
        ++dropped_pending_orders_;
        return order_id;
    }
    ++orders_submitted_;
    return order_id;
}

bool L2BacktestEngine::cancel_order(AssetID asset_id, std::uint64_t order_id) {
    if (!pending_orders_.push(PendingOrder {
        .type = PendingCommandType::Cancel,
        .asset_id = asset_id,
        .release_time_ns = current_time_ns_ + config_.latency_ns,
        .order_id = order_id,
    })) {
        ++dropped_pending_orders_;
        return false;
    }
    ++orders_canceled_;
    return true;
}

std::uint64_t L2BacktestEngine::current_timestamp() const noexcept {
    return current_time_ns_;
}

void L2BacktestEngine::reset_runtime() {
    book_ = L2OrderBook {config_.max_book_levels_per_side};
    strategy_book_ = OrderBook {};
    pending_orders_.clear();
    live_order_count_ = 0U;
    equity_curve_.clear();
    equity_curve_.reserve(config_.equity_curve_reserve);
    feature_rows_.clear();
    if (config_.record_features) {
        feature_rows_.reserve(config_.feature_reserve);
    }
    next_strategy_order_id_ = 1ULL << 63U;
    next_equity_sample_timestamp_ = 0U;
    next_feature_sample_timestamp_ = 0U;
    current_time_ns_ = 0U;
    synthetic_order_id_ = 1U;
    dropped_pending_orders_ = 0U;
    orders_submitted_ = 0U;
    orders_canceled_ = 0U;
    last_fill_timestamp_ = 0U;
    execution_ = ExecutionDiagnostics {};
    cash_ = config_.initial_cash;
    position_ = 0;
}

void L2BacktestEngine::apply_event_batch(Result& result, std::uint64_t batch_time_ns) {
    bool snapshot_cleared = false;
    do {
        const L2UpdateEvent& event = parser_.peek();
        if (event.is_snapshot && !snapshot_cleared) {
            book_.clear();
            snapshot_cleared = true;
        }
        apply_event(event);
        ++result.events_processed;
        parser_.advance();
    } while (parser_.has_next() && parser_.peek_time() == batch_time_ns);
    ++result.market_batches_processed;
}

void L2BacktestEngine::apply_event(const L2UpdateEvent& event) noexcept {
    book_.update_level(event.is_bid, event.price, event.qty);
}

void L2BacktestEngine::release_pending_orders(std::uint64_t now_ns) {
    while (!pending_orders_.empty() && pending_orders_.top().release_time_ns <= now_ns) {
        const PendingOrder pending = pending_orders_.top();
        pending_orders_.pop();
        execute_pending_order(pending);
    }
}

void L2BacktestEngine::execute_pending_order(const PendingOrder& pending) {
    const std::size_t live_order_index = find_live_order_index(pending.order_id);
    if (pending.type == PendingCommandType::Cancel) {
        if (live_order_index != kInvalidOrderIndex) {
            erase_live_order(live_order_index);
        }
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
    if (has_visible_book() && is_aggressive(live_order)) {
        (void)sweep_visible_depth(live_order, LiquidityRole::Taker, pending.release_time_ns);
    }
    const std::size_t post_sweep_index = find_live_order_index(pending.order_id);
    if (post_sweep_index != kInvalidOrderIndex) {
        live_orders_[post_sweep_index].active = true;
    }
}

void L2BacktestEngine::process_active_crosses(std::uint64_t timestamp) {
    if (!has_visible_book()) {
        return;
    }
    std::size_t index = 0U;
    while (index < live_order_count_) {
        LiveOrder& live_order = live_orders_[index];
        if (live_order.active && !live_order.canceled && is_aggressive(live_order)) {
            const std::uint64_t order_id = live_order.order_id;
            (void)sweep_visible_depth(live_order, LiquidityRole::Maker, timestamp);
            index = find_live_order_index(order_id) == kInvalidOrderIndex ? 0U : index + 1U;
        } else {
            ++index;
        }
    }
}

void L2BacktestEngine::dispatch_strategy_tick(Result& result) {
    if (!has_visible_book()) {
        return;
    }

    if (l2_strategy_ != nullptr) {
        l2_strategy_->on_tick(0U, book_, *this);
    } else {
        rebuild_strategy_book();
        strategy_->on_tick(0U, strategy_book_, *this);
    }
    ++result.strategy_ticks;
}

void L2BacktestEngine::rebuild_strategy_book() {
    strategy_book_ = OrderBook {};
    for (const L2OrderBook::Level& level : book_.bids()) {
        const std::uint64_t quantity = level_qty_to_order_qty(level.effective_qty());
        if (quantity == 0U) {
            continue;
        }
        (void)strategy_book_.process_order(Order {
            .id = synthetic_order_id_++,
            .price = static_cast<double>(level.price) / 100'000'000.0,
            .quantity = quantity,
            .side = Side::Buy,
            .timestamp = current_time_ns_,
        });
    }
    for (const L2OrderBook::Level& level : book_.asks()) {
        const std::uint64_t quantity = level_qty_to_order_qty(level.effective_qty());
        if (quantity == 0U) {
            continue;
        }
        (void)strategy_book_.process_order(Order {
            .id = synthetic_order_id_++,
            .price = static_cast<double>(level.price) / 100'000'000.0,
            .quantity = quantity,
            .side = Side::Sell,
            .timestamp = current_time_ns_,
        });
    }
}

void L2BacktestEngine::route_strategy_fill(const StrategyFill& fill) {
    const double quantity_units = order_qty_to_units(fill.quantity);
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
        live_order.remaining_quantity = live_order.remaining_quantity > fill.quantity
            ? live_order.remaining_quantity - fill.quantity
            : 0U;
        if (live_order.remaining_quantity == 0U) {
            erase_live_order(live_order_index);
        }
    }

    if (fill.side == Side::Buy) {
        cash_ -= notional + fee;
        position_ += static_cast<std::int64_t>(fill.quantity);
    } else {
        cash_ += notional - fee;
        position_ -= static_cast<std::int64_t>(fill.quantity);
    }

    last_fill_timestamp_ = fill.timestamp;
    if (l2_strategy_ != nullptr) {
        l2_strategy_->on_fill(fill, *this);
    } else {
        strategy_->on_fill(fill, *this);
    }
}

void L2BacktestEngine::sample_equity(std::uint64_t timestamp) {
    if (current_mid_price() <= 0.0 || config_.equity_sample_interval_ns == 0U) {
        return;
    }
    if (next_equity_sample_timestamp_ == 0U) {
        next_equity_sample_timestamp_ = timestamp;
    }
    while (timestamp >= next_equity_sample_timestamp_) {
        equity_curve_.push_back(current_nav());
        next_equity_sample_timestamp_ += config_.equity_sample_interval_ns;
    }
}

void L2BacktestEngine::sample_features(std::uint64_t timestamp) {
    if (!config_.record_features || !has_visible_book()) {
        return;
    }
    if (config_.feature_sample_interval_ns != 0U) {
        if (next_feature_sample_timestamp_ == 0U) {
            next_feature_sample_timestamp_ = timestamp;
        }
        if (timestamp < next_feature_sample_timestamp_) {
            return;
        }
        while (next_feature_sample_timestamp_ <= timestamp) {
            next_feature_sample_timestamp_ += config_.feature_sample_interval_ns;
        }
    }

    const double best_bid = static_cast<double>(book_.effective_best_bid()) / 100'000'000.0;
    const double best_ask = static_cast<double>(book_.effective_best_ask()) / 100'000'000.0;
    const double mid = SnapshotMidPrice(best_bid, best_ask);
    const double spread_bps = mid > 0.0 ? (best_ask - best_bid) / mid * 10'000.0 : 0.0;
    double bid_qty_1 = 0.0;
    for (const L2OrderBook::Level& level : book_.bids()) {
        bid_qty_1 = static_cast<double>(level.effective_qty()) / 100'000'000.0;
        if (bid_qty_1 > 0.0) {
            break;
        }
    }
    double ask_qty_1 = 0.0;
    for (const L2OrderBook::Level& level : book_.asks()) {
        ask_qty_1 = static_cast<double>(level.effective_qty()) / 100'000'000.0;
        if (ask_qty_1 > 0.0) {
            break;
        }
    }
    const double top_qty_sum = bid_qty_1 + ask_qty_1;
    const double imbalance_1 = top_qty_sum > 0.0 ? (bid_qty_1 - ask_qty_1) / top_qty_sum : 0.0;

    feature_rows_.push_back(Result::FeatureRow {
        .timestamp = timestamp,
        .best_bid = best_bid,
        .best_ask = best_ask,
        .mid_price = mid,
        .spread_bps = spread_bps,
        .bid_qty_1 = bid_qty_1,
        .ask_qty_1 = ask_qty_1,
        .imbalance_1 = imbalance_1,
        .bid_qty_visible = static_cast<double>(book_.bid_effective_qty()) / 100'000'000.0,
        .ask_qty_visible = static_cast<double>(book_.ask_effective_qty()) / 100'000'000.0,
        .position = position_,
        .nav = current_nav(),
    });
}

double L2BacktestEngine::current_mid_price() const noexcept {
    return SnapshotMidPrice(
        static_cast<double>(book_.effective_best_bid()) / 100'000'000.0,
        static_cast<double>(book_.effective_best_ask()) / 100'000'000.0
    );
}

double L2BacktestEngine::current_nav() const noexcept {
    return cash_ + (static_cast<double>(position_) / config_.quantity_scale * current_mid_price());
}

bool L2BacktestEngine::has_visible_book() const noexcept {
    return book_.effective_best_bid() > 0 && book_.effective_best_ask() > 0;
}

bool L2BacktestEngine::is_aggressive(const LiveOrder& live_order) const noexcept {
    if (live_order.side == Side::Buy) {
        const double best_ask = static_cast<double>(book_.effective_best_ask()) / 100'000'000.0;
        return best_ask > 0.0 && live_order.price >= best_ask;
    }
    const double best_bid = static_cast<double>(book_.effective_best_bid()) / 100'000'000.0;
    return best_bid > 0.0 && live_order.price <= best_bid;
}

std::uint64_t L2BacktestEngine::sweep_visible_depth(
    LiveOrder& live_order,
    LiquidityRole role,
    std::uint64_t timestamp
) {
    if (live_order.remaining_quantity == 0U) {
        return 0U;
    }

    std::uint64_t total_filled = 0U;
    if (live_order.side == Side::Buy) {
        for (const L2OrderBook::Level& level : book_.asks()) {
            const double level_price = static_cast<double>(level.price) / 100'000'000.0;
            if (live_order.remaining_quantity == 0U || level_price > live_order.price) {
                break;
            }
            const std::uint64_t level_quantity = level_qty_to_order_qty(level.effective_qty());
            if (level_quantity == 0U) {
                continue;
            }
            const std::uint64_t fill_quantity = level_quantity < live_order.remaining_quantity
                ? level_quantity
                : live_order.remaining_quantity;
            book_.deplete_level(false, level.price, order_qty_to_lots(fill_quantity));
            route_strategy_fill(StrategyFill {
                .order_id = live_order.order_id,
                .price = level.price,
                .quantity = fill_quantity,
                .timestamp = timestamp,
                .asset_id = live_order.asset_id,
                .side = Side::Buy,
                .liquidity_role = role,
            });
            total_filled += fill_quantity;
            if (find_live_order_index(live_order.order_id) == kInvalidOrderIndex) {
                return total_filled;
            }
        }
        return total_filled;
    }

    for (const L2OrderBook::Level& level : book_.bids()) {
        const double level_price = static_cast<double>(level.price) / 100'000'000.0;
        if (live_order.remaining_quantity == 0U || level_price < live_order.price) {
            break;
        }
        const std::uint64_t level_quantity = level_qty_to_order_qty(level.effective_qty());
        if (level_quantity == 0U) {
            continue;
        }
        const std::uint64_t fill_quantity = level_quantity < live_order.remaining_quantity
            ? level_quantity
            : live_order.remaining_quantity;
        book_.deplete_level(true, level.price, order_qty_to_lots(fill_quantity));
        route_strategy_fill(StrategyFill {
            .order_id = live_order.order_id,
            .price = level.price,
            .quantity = fill_quantity,
            .timestamp = timestamp,
            .asset_id = live_order.asset_id,
            .side = Side::Sell,
            .liquidity_role = role,
        });
        total_filled += fill_quantity;
        if (find_live_order_index(live_order.order_id) == kInvalidOrderIndex) {
            return total_filled;
        }
    }
    return total_filled;
}

std::uint64_t L2BacktestEngine::level_qty_to_order_qty(std::uint64_t qty_lots) const noexcept {
    if (qty_lots == 0U) {
        return 0U;
    }
    return static_cast<std::uint64_t>(std::llround(static_cast<double>(qty_lots) / 100'000'000.0 * config_.quantity_scale));
}

double L2BacktestEngine::order_qty_to_units(std::uint64_t quantity) const noexcept {
    return static_cast<double>(quantity) / config_.quantity_scale;
}

std::uint64_t L2BacktestEngine::order_qty_to_lots(std::uint64_t quantity) const noexcept {
    return static_cast<std::uint64_t>(std::llround(order_qty_to_units(quantity) * 100'000'000.0));
}

bool L2BacktestEngine::add_live_order(const LiveOrder& live_order) noexcept {
    if (live_order_count_ == kLiveOrderCapacity) {
        return false;
    }
    live_orders_[live_order_count_++] = live_order;
    return true;
}

std::size_t L2BacktestEngine::find_live_order_index(std::uint64_t order_id) const noexcept {
    for (std::size_t index = 0U; index < live_order_count_; ++index) {
        if (live_orders_[index].order_id == order_id) {
            return index;
        }
    }
    return kInvalidOrderIndex;
}

void L2BacktestEngine::erase_live_order(std::size_t index) noexcept {
    if (index >= live_order_count_) {
        return;
    }
    --live_order_count_;
    if (index != live_order_count_) {
        live_orders_[index] = live_orders_[live_order_count_];
    }
}

}  // namespace lob
