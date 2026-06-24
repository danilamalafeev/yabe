#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <algorithm>
#include <random>
#include <utility>
#include <vector>

#include "lob/async_logger.hpp"
#include "lob/event_merger.hpp"
#include "lob/order_book.hpp"
#include "lob/strategy.hpp"
#include "lob/wallet.hpp"

namespace lob {

template <std::size_t N, typename Parser = CsvParser>
class MultiAssetBacktestEngine final : public OrderGateway {
public:
    using ParserArray = std::array<Parser, N>;
    using PathArray = std::array<std::filesystem::path, N>;

    enum class LatencyDistribution : std::uint8_t {
        None,
        Exponential,
        LogNormal
    };

    struct LatencyConfig {
        std::uint64_t base_latency_ns {};
        LatencyDistribution distribution {LatencyDistribution::None};
        double jitter_mean_ns {};
        double lognormal_mu {};
        double lognormal_sigma {};
        std::uint64_t rng_seed {0x9E3779B97F4A7C15ULL};
    };

    struct Config {
        double initial_cash {100'000'000.0};
        double maker_fee_bps {};
        double taker_fee_bps {};
        LatencyConfig latency {};
        AsyncLogger<>* async_logger {};
        std::uint64_t intra_leg_latency_ns {75U};
        std::uint64_t intra_leg_jitter_ns {25U};
        std::size_t microstructure_reserve {};
    };

    struct ExecutionDiagnostics {
        std::uint64_t maker_fills_count {};
        std::uint64_t taker_fills_count {};
        double maker_volume {};
        double taker_volume {};
        double maker_notional {};
        double taker_notional {};
    };

    struct PerAssetStats {
        double volume_traded {};
        double fees_paid {};
        double slippage_realized {};
        double total_pnl {};
    };

    struct Result {
        double final_cash {};
        double initial_usdt {};
        Wallet final_portfolio {};
        double btc_usdt_mid {};
        double eth_usdt_mid {};
        double final_mtm_nav_usdt {};
        double inventory_risk_usdt {};
        std::array<std::int64_t, N> final_position {};
        std::array<PerAssetStats, N> per_asset_stats {};
        ExecutionDiagnostics execution {};
        std::uint64_t events_processed {};
        std::uint64_t dropped_pending_orders {};
    };

    MultiAssetBacktestEngine(Strategy& strategy, ParserArray parsers, Config config = {})
        : strategy_(strategy),
          config_(config),
          merger_(std::move(parsers)),
          rng_(config.latency.rng_seed),
          exponential_jitter_(config.latency.jitter_mean_ns > 0.0 ? 1.0 / config.latency.jitter_mean_ns : 1.0),
          lognormal_jitter_(config.latency.lognormal_mu, config.latency.lognormal_sigma),
          wallet_(Wallet {
              .usdt = config.initial_cash,
          }) {
        trade_buffer_.reserve(1'024U);
    }

    MultiAssetBacktestEngine(Strategy& strategy, const PathArray& file_paths, Config config = {})
        : MultiAssetBacktestEngine(strategy, MakeParsers(file_paths), config) {}

    [[nodiscard]] Result run() {
        Result result {};
        strategy_.on_start(*this);
        reset_microstructure_recording_state();

        while (merger_.has_next()) {
            Event event = merger_.get_next();
            current_time_ns_ = event.timestamp * kNanosecondsPerMillisecond;
            current_asset_id_ = event.asset_id;

            release_pending_orders(current_time_ns_);

            Order order {event.order};
            order.timestamp = current_time_ns_;
            books_[event.asset_id].process_order(order, trade_buffer_);
            update_last_mid(event.asset_id);
            maybe_record_microstructure(event.asset_id);

            strategy_.on_tick(event.asset_id, books_[event.asset_id], *this);
            release_pending_orders(current_time_ns_);
            ++result.events_processed;
        }

        result.initial_usdt = config_.initial_cash;
        result.final_cash = wallet_.usdt;
        result.final_portfolio = wallet_;
        result.btc_usdt_mid = last_mid_[0U];
        if constexpr (N > 1U) {
            result.eth_usdt_mid = last_mid_[1U];
        }
        result.final_mtm_nav_usdt = wallet_.mark_to_market_nav(result.btc_usdt_mid, result.eth_usdt_mid);
        result.inventory_risk_usdt = wallet_.get_total_inventory_risk(result.btc_usdt_mid, result.eth_usdt_mid);
        result.final_position = position_;
        result.per_asset_stats = per_asset_stats_;
        result.execution = execution_;
        result.dropped_pending_orders = dropped_pending_orders_;
        return result;
    }

    [[nodiscard]] std::uint64_t submit_order(
        AssetID asset_id,
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    ) override {
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
            .intended_taker = is_aggressive(asset_id, side, price),
            .active = false,
            .canceled = false,
        })) {
            ++dropped_pending_orders_;
            return order_id;
        }

        if (!pending_orders_.push(PendingOrder {
            .type = PendingCommandType::Submit,
            .asset_id = asset_id,
            .release_time_ns = current_time_ns_ + sample_latency_ns(),
            .order_id = order_id,
            .side = side,
            .price = price,
            .quantity = quantity,
        })) {
            erase_live_order(find_live_order_index(order_id));
            ++dropped_pending_orders_;
        }

        return order_id;
    }

    [[nodiscard]] bool cancel_order(AssetID asset_id, std::uint64_t order_id) override {
        if (!pending_orders_.push(PendingOrder {
            .type = PendingCommandType::Cancel,
            .asset_id = asset_id,
            .release_time_ns = current_time_ns_ + sample_latency_ns(),
            .order_id = order_id,
        })) {
            ++dropped_pending_orders_;
            return false;
        }

        return true;
    }

    [[nodiscard]] std::uint64_t current_timestamp() const noexcept override {
        return current_time_ns_;
    }

    void enable_microstructure_recording(bool enable, std::uint64_t sampling_interval_ns = 0U) {
        record_microstructure_ = enable;
        sampling_interval_ns_ = sampling_interval_ns;
        snapshots_.clear();
        if (config_.microstructure_reserve > 0U) {
            snapshots_.reserve(config_.microstructure_reserve);
        }
        last_snapshot_time_by_asset_.fill(0U);
    }

    [[nodiscard]] const std::vector<MarketSnapshot>& microstructure_snapshots() const noexcept {
        return snapshots_;
    }

    [[nodiscard]] std::vector<MarketSnapshot> take_microstructure_snapshots() noexcept {
        return std::move(snapshots_);
    }

    [[nodiscard]] OrderGroupResult execute_group(const OrderGroup& group) override {
        OrderGroupResult dummy {};
        dummy.group_id = group.group_id;
        
        if (!pending_groups_.push(PendingGroup {
            .release_time_ns = current_time_ns_ + sample_latency_ns(),
            .group = group,
        })) {
            ++dropped_pending_orders_;
        }
        
        return dummy;
    }

    void execute_pending_group(const OrderGroup& group, std::uint64_t release_time_ns) {
        OrderGroupResult result {};
        result.group_id = group.group_id;
        log_group_event(group, LogEventType::OrderGroup, release_time_ns, 0U, Side::Buy, 0.0, 0U);

        std::uint64_t leg_timestamp = release_time_ns;
        for (std::size_t leg_index = 0U; leg_index < kOrderGroupLegCount; ++leg_index) {
            leg_timestamp += sample_intra_leg_latency_ns(group);

            const OrderRequest& request = group.legs[leg_index];
            OrderExecutionReport& report = result.reports[leg_index];
            execute_fok_request(request, group, leg_timestamp, report);

            if (!report.fully_filled || report.slippage_breached) {
                result.panic_triggered = true;
                log_group_event(
                    group,
                    report.fully_filled ? LogEventType::Drop : LogEventType::PartialFill,
                    leg_timestamp,
                    request.asset_id,
                    request.side,
                    static_cast<double>(report.vwap_price) / 100'000'000.0,
                    report.filled_quantity
                );
                panic_close_group(result, leg_index, leg_timestamp);
                return;
            }
        }

        result.completed = true;
    }

private:
    enum class PendingCommandType : std::uint8_t {
        Submit,
        Cancel
    };

    struct PendingOrder {
        PendingCommandType type {PendingCommandType::Submit};
        AssetID asset_id {};
        std::uint64_t release_time_ns {};
        std::uint64_t order_id {};
        Side side {Side::Buy};
        double price {};
        std::uint64_t quantity {};
    };

    struct PendingGroup {
        std::uint64_t release_time_ns {};
        OrderGroup group {};
    };

    template <typename T, std::size_t Capacity>
    class PendingMinHeap {
    public:
        [[nodiscard]] bool empty() const noexcept {
            return size_ == 0U;
        }

        [[nodiscard]] const T& top() const noexcept {
            return heap_[0];
        }

        bool push(const T& item) noexcept {
            if (size_ == Capacity) {
                return false;
            }

            std::size_t index = size_++;
            heap_[index] = item;
            while (index > 0U) {
                const std::size_t parent = (index - 1U) / 2U;
                if (heap_[parent].release_time_ns <= heap_[index].release_time_ns) {
                    break;
                }

                std::swap(heap_[parent], heap_[index]);
                index = parent;
            }
            return true;
        }

        void pop() noexcept {
            heap_[0] = heap_[--size_];
            std::size_t index = 0U;
            while (true) {
                const std::size_t left = (index * 2U) + 1U;
                const std::size_t right = left + 1U;
                if (left >= size_) {
                    break;
                }

                std::size_t smallest = left;
                if (right < size_ && heap_[right].release_time_ns < heap_[left].release_time_ns) {
                    smallest = right;
                }
                if (heap_[index].release_time_ns <= heap_[smallest].release_time_ns) {
                    break;
                }

                std::swap(heap_[index], heap_[smallest]);
                index = smallest;
            }
        }

    private:
        std::array<T, Capacity> heap_ {};
        std::size_t size_ {};
    };

    struct LiveOrder {
        AssetID asset_id {};
        std::uint64_t order_id {};
        Side side {Side::Buy};
        double price {};
        std::uint64_t remaining_quantity {};
        std::uint64_t volume_ahead {};
        bool intended_taker {};
        bool active {};
        bool canceled {};
    };

    static constexpr std::size_t kPendingOrderCapacity = 16'384U;
    static constexpr std::size_t kLiveOrderCapacity = 16'384U;
    static constexpr std::size_t kInvalidOrderIndex = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000ULL;
    static constexpr double kQuantityScale = 100'000'000.0;

    [[nodiscard]] static ParserArray MakeParsers(const PathArray& file_paths) {
        ParserArray parsers {};
        for (std::size_t index = 0U; index < N; ++index) {
            parsers[index] = Parser {file_paths[index]};
        }
        return parsers;
    }

    void reset_microstructure_recording_state() {
        if (!record_microstructure_) {
            return;
        }

        snapshots_.clear();
        if (config_.microstructure_reserve > 0U && snapshots_.capacity() < config_.microstructure_reserve) {
            snapshots_.reserve(config_.microstructure_reserve);
        }
        last_snapshot_time_by_asset_.fill(0U);
    }

    void maybe_record_microstructure(AssetID asset_id) {
        if (!record_microstructure_ || asset_id >= N) {
            return;
        }

        std::uint64_t& last_snapshot_time = last_snapshot_time_by_asset_[asset_id];
        if (sampling_interval_ns_ != 0U &&
            last_snapshot_time != 0U &&
            current_time_ns_ < last_snapshot_time + sampling_interval_ns_) {
            return;
        }

        const OrderBook& book = books_[asset_id];
        const double best_bid = book.get_best_bid();
        const double best_ask = book.get_best_ask();
        snapshots_.push_back(MarketSnapshot {
            .ts = current_time_ns_,
            .asset = static_cast<std::uint32_t>(asset_id),
            .bid_p = best_bid,
            .bid_q = best_bid > 0.0
                ? static_cast<double>(book.get_total_quantity_at_price(Side::Buy, best_bid)) / kQuantityScale
                : 0.0,
            .ask_p = best_ask,
            .ask_q = best_ask > 0.0
                ? static_cast<double>(book.get_total_quantity_at_price(Side::Sell, best_ask)) / kQuantityScale
                : 0.0,
        });
        last_snapshot_time = current_time_ns_;
    }

    void release_pending_orders(std::uint64_t now_ns) {
        while (!pending_orders_.empty() || !pending_groups_.empty()) {
            std::uint64_t next_order_time = pending_orders_.empty() ? std::numeric_limits<std::uint64_t>::max() : pending_orders_.top().release_time_ns;
            std::uint64_t next_group_time = pending_groups_.empty() ? std::numeric_limits<std::uint64_t>::max() : pending_groups_.top().release_time_ns;

            if (next_order_time > now_ns && next_group_time > now_ns) {
                break;
            }

            if (next_order_time <= next_group_time) {
                const PendingOrder pending = pending_orders_.top();
                pending_orders_.pop();
                execute_pending_order(pending);
            } else {
                const PendingGroup pending = pending_groups_.top();
                pending_groups_.pop();
                execute_pending_group(pending.group, pending.release_time_ns);
            }
        }
    }

    void execute_pending_order(const PendingOrder& pending) {
        const std::size_t index = find_live_order_index(pending.order_id);
        if (index == kInvalidOrderIndex) {
            return;
        }

        LiveOrder& live_order = live_orders_[index];
        if (pending.type == PendingCommandType::Cancel) {
            erase_live_order(index);
            return;
        }

        if (live_order.canceled) {
            erase_live_order(index);
            return;
        }

        if (live_order.intended_taker) {
            if (is_aggressive(live_order) && available_cross_quantity(live_order) >= live_order.remaining_quantity) {
                sweep_book(live_order, pending.release_time_ns);
            }

            const std::size_t remaining_index = find_live_order_index(pending.order_id);
            if (remaining_index != kInvalidOrderIndex) {
                erase_live_order(remaining_index);
            }
            return;
        }

        if (is_aggressive(live_order)) {
            sweep_book(live_order, pending.release_time_ns);
        }

        const std::size_t remaining_index = find_live_order_index(pending.order_id);
        if (remaining_index == kInvalidOrderIndex) {
            return;
        }

        LiveOrder& resting_order = live_orders_[remaining_index];
        resting_order.active = true;
        resting_order.volume_ahead =
            books_[resting_order.asset_id].get_total_quantity_at_price(resting_order.side, resting_order.price);
    }

    void sweep_book(LiveOrder& live_order, std::uint64_t timestamp) {
        OrderBook& book = books_[live_order.asset_id];
        const AssetID asset_id = live_order.asset_id;
        const std::uint64_t order_id = live_order.order_id;
        const Side side = live_order.side;
        const double limit_price = live_order.price;
        std::uint64_t remaining_quantity = live_order.remaining_quantity;

        if (side == Side::Buy) {
            for (auto ask_it = book.asks().begin();
                 ask_it != book.asks().end() && remaining_quantity > 0U && ask_it->first <= limit_price;
                 ++ask_it) {
                std::uint64_t level_quantity = 0U;
                for (const Order& order : ask_it->second.orders) {
                    level_quantity += order.quantity;
                }
                const std::uint64_t fill_quantity =
                    level_quantity < remaining_quantity ? level_quantity : remaining_quantity;
                if (fill_quantity > 0U) {
                    route_fill(FillEvent {
                        .order_id = order_id,
                        .price = static_cast<std::int64_t>(std::llround(ask_it->first * 100'000'000.0)),
                        .quantity = fill_quantity,
                        .timestamp = timestamp,
                        .asset_id = asset_id,
                        .side = Side::Buy,
                        .liquidity_role = LiquidityRole::Taker,
                    });
                    remaining_quantity -= fill_quantity;
                }
            }
            return;
        }

        for (auto bid_it = book.bids().begin();
             bid_it != book.bids().end() && remaining_quantity > 0U && bid_it->first >= limit_price;
             ++bid_it) {
            std::uint64_t level_quantity = 0U;
            for (const Order& order : bid_it->second.orders) {
                level_quantity += order.quantity;
            }
            const std::uint64_t fill_quantity =
                level_quantity < remaining_quantity ? level_quantity : remaining_quantity;
            if (fill_quantity > 0U) {
                route_fill(FillEvent {
                    .order_id = order_id,
                    .price = static_cast<std::int64_t>(std::llround(bid_it->first * 100'000'000.0)),
                    .quantity = fill_quantity,
                    .timestamp = timestamp,
                    .asset_id = asset_id,
                    .side = Side::Sell,
                    .liquidity_role = LiquidityRole::Taker,
                });
                remaining_quantity -= fill_quantity;
            }
        }
    }

    void route_fill(
        const FillEvent& fill,
        std::uint64_t group_id = 0U,
        LogEventType log_event_type = LogEventType::Fill
    ) {
        const double quantity_units = static_cast<double>(fill.quantity) / kQuantityScale;
        const double fill_price_double = static_cast<double>(fill.price) / 100'000'000.0;
        const double notional = fill_price_double * quantity_units;
        const double fee_bps =
            fill.liquidity_role == LiquidityRole::Maker ? config_.maker_fee_bps : config_.taker_fee_bps;
        const double fee_usdt = fee_to_usdt(fill.asset_id, fill.side, quantity_units, notional, fee_bps);
        const double slippage_usdt = slippage_to_usdt(fill.asset_id, fill.side, fill_price_double, quantity_units);
        const double pnl_impact = -fee_usdt - slippage_usdt;
        apply_portfolio_fill(fill.asset_id, fill.side, quantity_units, notional, fee_bps);

        position_[fill.asset_id] += fill.side == Side::Buy
            ? static_cast<std::int64_t>(fill.quantity)
            : -static_cast<std::int64_t>(fill.quantity);

        const double notional_usdt = quote_notional_to_usdt(fill.asset_id, notional);
        if (fill.asset_id < N) {
            PerAssetStats& asset_stats = per_asset_stats_[fill.asset_id];
            asset_stats.volume_traded += quantity_units;
            asset_stats.fees_paid += fee_usdt;
            asset_stats.slippage_realized += slippage_usdt;
            asset_stats.total_pnl += pnl_impact;
        }

        if (fill.liquidity_role == LiquidityRole::Maker) {
            ++execution_.maker_fills_count;
            execution_.maker_volume += quantity_units;
            execution_.maker_notional += notional_usdt;
        } else {
            ++execution_.taker_fills_count;
            execution_.taker_volume += quantity_units;
            execution_.taker_notional += notional_usdt;
        }

        const std::size_t index = find_live_order_index(fill.order_id);
        if (index != kInvalidOrderIndex) {
            LiveOrder& live_order = live_orders_[index];
            live_order.remaining_quantity =
                live_order.remaining_quantity > fill.quantity ? live_order.remaining_quantity - fill.quantity : 0U;
            if (live_order.remaining_quantity == 0U) {
                erase_live_order(index);
            }
        }

        if (config_.async_logger != nullptr) {
            (void)config_.async_logger->push(LogRecord {
                .nanoseconds = fill.timestamp,
                .asset_id = fill.asset_id,
                .event_type = log_event_type,
                .group_id = group_id,
                .side = fill.side,
                .price = fill_price_double,
                .qty = quantity_units,
                .pnl_impact = pnl_impact,
                .current_nav = mark_to_market_nav(),
            });
        }

        strategy_.on_fill(fill, *this);
    }

    void execute_fok_request(
        const OrderRequest& request,
        const OrderGroup& group,
        std::uint64_t timestamp,
        OrderExecutionReport& report
    ) {
        report = OrderExecutionReport {
            .expected_price = request.expected_price > 0 ? request.expected_price : request.price,
            .requested_quantity = request.quantity,
            .asset_id = request.asset_id,
            .side = request.side,
        };

        if (request.asset_id >= N || request.quantity == 0U) {
            return;
        }

        const double request_price_double = static_cast<double>(request.price) / 100'000'000.0;
        const std::uint64_t available_quantity =
            available_cross_quantity(request.asset_id, request.side, request_price_double, request.quantity);
        if (available_quantity < request.quantity) {
            return;
        }

        const std::uint64_t order_id = next_strategy_order_id_++;
        double vwap_price_double = 0.0;
        report.filled_quantity = sweep_request(
            request.asset_id,
            request.side,
            request_price_double,
            request.quantity,
            timestamp,
            order_id,
            group.group_id,
            LogEventType::Fill,
            &vwap_price_double
        );
        report.vwap_price = static_cast<std::int64_t>(std::llround(vwap_price_double * 100'000'000.0));
        report.fully_filled = report.filled_quantity == request.quantity;
        report.slippage_breached = report.fully_filled && is_slippage_breached(report, group);
    }

    [[nodiscard]] std::uint64_t sweep_request(
        AssetID asset_id,
        Side side,
        double limit_price,
        std::uint64_t quantity,
        std::uint64_t timestamp,
        std::uint64_t order_id,
        std::uint64_t group_id,
        LogEventType log_event_type,
        double* vwap_price
    ) {
        OrderBook& book = books_[asset_id];
        std::uint64_t remaining_quantity = quantity;
        std::uint64_t filled_quantity = 0U;
        double notional = 0.0;

        if (side == Side::Buy) {
            for (auto ask_it = book.asks().begin();
                 ask_it != book.asks().end() && remaining_quantity > 0U && ask_it->first <= limit_price;
                 ++ask_it) {
                std::uint64_t level_quantity = 0U;
                for (const Order& order : ask_it->second.orders) {
                    level_quantity += order.quantity;
                }
                const std::uint64_t fill_quantity =
                    level_quantity < remaining_quantity ? level_quantity : remaining_quantity;
                if (fill_quantity > 0U) {
                    route_fill(FillEvent {
                        .order_id = order_id,
                        .price = static_cast<std::int64_t>(std::llround(ask_it->first * 100'000'000.0)),
                        .quantity = fill_quantity,
                        .timestamp = timestamp,
                        .asset_id = asset_id,
                        .side = Side::Buy,
                        .liquidity_role = LiquidityRole::Taker,
                    }, group_id, log_event_type);
                    remaining_quantity -= fill_quantity;
                    filled_quantity += fill_quantity;
                    notional += ask_it->first * (static_cast<double>(fill_quantity) / kQuantityScale);
                }
            }
        } else {
            for (auto bid_it = book.bids().begin();
                 bid_it != book.bids().end() && remaining_quantity > 0U && bid_it->first >= limit_price;
                 ++bid_it) {
                std::uint64_t level_quantity = 0U;
                for (const Order& order : bid_it->second.orders) {
                    level_quantity += order.quantity;
                }
                const std::uint64_t fill_quantity =
                    level_quantity < remaining_quantity ? level_quantity : remaining_quantity;
                if (fill_quantity > 0U) {
                    route_fill(FillEvent {
                        .order_id = order_id,
                        .price = static_cast<std::int64_t>(std::llround(bid_it->first * 100'000'000.0)),
                        .quantity = fill_quantity,
                        .timestamp = timestamp,
                        .asset_id = asset_id,
                        .side = Side::Sell,
                        .liquidity_role = LiquidityRole::Taker,
                    }, group_id, log_event_type);
                    remaining_quantity -= fill_quantity;
                    filled_quantity += fill_quantity;
                    notional += bid_it->first * (static_cast<double>(fill_quantity) / kQuantityScale);
                }
            }
        }

        if (vwap_price != nullptr && filled_quantity > 0U) {
            *vwap_price = notional / (static_cast<double>(filled_quantity) / kQuantityScale);
        }
        return filled_quantity;
    }

    [[nodiscard]] bool is_slippage_breached(const OrderExecutionReport& report, const OrderGroup& group) const noexcept {
        const double tolerance = group.slippage_tolerance > 0.0 ? group.slippage_tolerance : 0.0;
        if (report.expected_price <= 0 || tolerance <= 0.0) {
            return false;
        }

        if (report.side == Side::Buy) {
            return report.vwap_price > report.expected_price * (1.0 + tolerance);
        }
        return report.vwap_price < report.expected_price * (1.0 - tolerance);
    }

    void panic_close_group(
        const OrderGroupResult& result,
        std::size_t failed_leg_index,
        std::uint64_t timestamp
    ) {
        for (std::size_t index = 0U; index <= failed_leg_index && index < kOrderGroupLegCount; ++index) {
            const OrderExecutionReport& report = result.reports[index];
            if (report.filled_quantity == 0U || report.asset_id >= N) {
                continue;
            }

            timestamp += sample_intra_leg_latency_ns(config_.intra_leg_latency_ns, config_.intra_leg_jitter_ns);
            const Side close_side = report.side == Side::Buy ? Side::Sell : Side::Buy;
            const double market_limit = close_side == Side::Buy ? std::numeric_limits<double>::max() : 0.0;
            (void)sweep_request(
                report.asset_id,
                close_side,
                market_limit,
                report.filled_quantity,
                timestamp,
                next_strategy_order_id_++,
                result.group_id,
                LogEventType::PanicClose,
                nullptr
            );
        }
    }

    void log_group_event(
        const OrderGroup& group,
        LogEventType type,
        std::uint64_t timestamp,
        AssetID asset_id,
        Side side,
        double price,
        std::uint64_t quantity
    ) noexcept {
        if (config_.async_logger == nullptr) {
            return;
        }

        (void)config_.async_logger->push(LogRecord {
            .nanoseconds = timestamp,
            .asset_id = asset_id,
            .event_type = type,
            .group_id = group.group_id,
            .side = side,
            .price = price,
            .qty = static_cast<double>(quantity) / kQuantityScale,
            .pnl_impact = 0.0,
            .current_nav = mark_to_market_nav(),
        });
    }

    void apply_portfolio_fill(
        AssetID asset_id,
        Side side,
        double base_quantity,
        double quote_notional,
        double fee_bps
    ) noexcept {
        if (base_quantity <= 0.0) {
            return;
        }

        const double fill_price = quote_notional / base_quantity;
        wallet_.apply_spot_fill(asset_id, side, base_quantity, fill_price, fee_bps);
    }

    void update_last_mid(AssetID asset_id) noexcept {
        if (asset_id >= N) {
            return;
        }

        const double best_bid = books_[asset_id].get_best_bid();
        const double best_ask = books_[asset_id].get_best_ask();
        if (best_bid > 0.0 && best_ask > 0.0) {
            last_mid_[asset_id] = (best_bid + best_ask) * 0.5;
        }
    }

    [[nodiscard]] double quote_notional_to_usdt(AssetID asset_id, double quote_notional) const noexcept {
        if (asset_id == 2U) {
            return quote_notional * last_mid_[0U];
        }

        return quote_notional;
    }

    [[nodiscard]] double base_quantity_to_usdt(AssetID asset_id, double base_quantity) const noexcept {
        switch (asset_id) {
            case 0U:
                return base_quantity * last_mid_[0U];
            case 1U:
            case 2U:
                return N > 1U ? base_quantity * last_mid_[1U] : 0.0;
            default:
                return 0.0;
        }
    }

    [[nodiscard]] double fee_to_usdt(
        AssetID asset_id,
        Side side,
        double base_quantity,
        double quote_notional,
        double fee_bps
    ) const noexcept {
        const double fee_rate = fee_bps * 0.0001;
        if (side == Side::Buy) {
            return base_quantity_to_usdt(asset_id, base_quantity * fee_rate);
        }

        return quote_notional_to_usdt(asset_id, quote_notional * fee_rate);
    }

    [[nodiscard]] double slippage_to_usdt(
        AssetID asset_id,
        Side side,
        double fill_price,
        double base_quantity
    ) const noexcept {
        if (asset_id >= N) {
            return 0.0;
        }

        const double mid_price = last_mid_[asset_id];
        if (mid_price <= 0.0) {
            return 0.0;
        }

        const double slippage_in_quote = side == Side::Buy
            ? (fill_price - mid_price) * base_quantity
            : (mid_price - fill_price) * base_quantity;
        return quote_notional_to_usdt(asset_id, slippage_in_quote);
    }

    [[nodiscard]] double mark_to_market_nav() const noexcept {
        return wallet_.mark_to_market_nav(last_mid_[0U], N > 1U ? last_mid_[1U] : 0.0);
    }

    [[nodiscard]] std::uint64_t available_cross_quantity(const LiveOrder& live_order) const noexcept {
        return available_cross_quantity(
            live_order.asset_id,
            live_order.side,
            live_order.price,
            live_order.remaining_quantity
        );
    }

    [[nodiscard]] std::uint64_t available_cross_quantity(
        AssetID asset_id,
        Side side,
        double limit_price,
        std::uint64_t target_quantity
    ) const noexcept {
        if (asset_id >= N) {
            return 0U;
        }

        const OrderBook& book = books_[asset_id];
        std::uint64_t available_quantity = 0U;

        if (side == Side::Buy) {
            for (auto ask_it = book.asks().begin();
                 ask_it != book.asks().end() && ask_it->first <= limit_price;
                 ++ask_it) {
                for (const Order& order : ask_it->second.orders) {
                    available_quantity += order.quantity;
                    if (available_quantity >= target_quantity) {
                        return available_quantity;
                    }
                }
            }
            return available_quantity;
        }

        for (auto bid_it = book.bids().begin();
             bid_it != book.bids().end() && bid_it->first >= limit_price;
             ++bid_it) {
            for (const Order& order : bid_it->second.orders) {
                available_quantity += order.quantity;
                if (available_quantity >= target_quantity) {
                    return available_quantity;
                }
            }
        }
        return available_quantity;
    }

    [[nodiscard]] std::uint64_t sample_intra_leg_latency_ns(const OrderGroup& group) {
        const std::uint64_t base_latency =
            group.intra_leg_latency_ns != 0U ? group.intra_leg_latency_ns : config_.intra_leg_latency_ns;
        const std::uint64_t jitter_bound =
            group.intra_leg_jitter_ns != 0U ? group.intra_leg_jitter_ns : config_.intra_leg_jitter_ns;
        return sample_intra_leg_latency_ns(base_latency, jitter_bound);
    }

    [[nodiscard]] std::uint64_t sample_intra_leg_latency_ns(
        std::uint64_t base_latency,
        std::uint64_t jitter_bound
    ) {
        const std::uint64_t jitter = jitter_bound > 0U ? rng_() % (jitter_bound + 1U) : 0U;
        return base_latency + jitter;
    }

    [[nodiscard]] std::uint64_t sample_latency_ns() {
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

    [[nodiscard]] std::size_t find_live_order_index(std::uint64_t order_id) const noexcept {
        for (std::size_t index = 0U; index < live_order_count_; ++index) {
            if (live_orders_[index].order_id == order_id) {
                return index;
            }
        }
        return kInvalidOrderIndex;
    }

    [[nodiscard]] bool is_aggressive(const LiveOrder& live_order) const noexcept {
        return is_aggressive(live_order.asset_id, live_order.side, live_order.price);
    }

    [[nodiscard]] bool is_aggressive(AssetID asset_id, Side side, double price) const noexcept {
        if (asset_id >= N) {
            return false;
        }

        const OrderBook& book = books_[asset_id];
        if (side == Side::Buy) {
            const double best_ask = book.get_best_ask();
            return best_ask > 0.0 && price >= best_ask;
        }
        const double best_bid = book.get_best_bid();
        return best_bid > 0.0 && price <= best_bid;
    }

    [[nodiscard]] bool add_live_order(const LiveOrder& live_order) noexcept {
        if (live_order_count_ == kLiveOrderCapacity) {
            return false;
        }
        live_orders_[live_order_count_++] = live_order;
        return true;
    }

    void erase_live_order(std::size_t index) noexcept {
        --live_order_count_;
        if (index != live_order_count_) {
            live_orders_[index] = live_orders_[live_order_count_];
        }
    }

    Strategy& strategy_;
    Config config_ {};
    EventMerger<N, Parser> merger_;
    std::array<OrderBook, N> books_ {};
    PendingMinHeap<PendingOrder, kPendingOrderCapacity> pending_orders_ {};
    PendingMinHeap<PendingGroup, kPendingOrderCapacity> pending_groups_ {};
    std::array<LiveOrder, kLiveOrderCapacity> live_orders_ {};
    std::size_t live_order_count_ {};
    std::vector<Trade> trade_buffer_ {};
    std::mt19937_64 rng_ {};
    std::exponential_distribution<double> exponential_jitter_ {};
    std::lognormal_distribution<double> lognormal_jitter_ {};
    std::array<std::int64_t, N> position_ {};
    std::array<double, N> last_mid_ {};
    std::array<std::uint64_t, N> last_snapshot_time_by_asset_ {};
    std::array<PerAssetStats, N> per_asset_stats_ {};
    std::vector<MarketSnapshot> snapshots_ {};
    Wallet wallet_ {};
    ExecutionDiagnostics execution_ {};
    AssetID current_asset_id_ {};
    std::uint64_t current_time_ns_ {};
    std::uint64_t next_strategy_order_id_ {1ULL << 63U};
    std::uint64_t dropped_pending_orders_ {};
    bool record_microstructure_ {};
    std::uint64_t sampling_interval_ns_ {};
};

}  // namespace lob
