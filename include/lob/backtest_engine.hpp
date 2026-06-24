#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "lob/csv_parser.hpp"
#include "lob/indexed_pending_min_heap.hpp"
#include "lob/fixed_matching_book.hpp"
#include "lob/strategy.hpp"

namespace lob {

class BacktestEngine final : public OrderGateway<BacktestEngine> {
public:
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
        std::uint64_t equity_sample_interval_ns {1'000'000'000ULL};
        std::size_t equity_curve_reserve {100'000U};
        double maker_fee_bps {};
        double taker_fee_bps {};
        LatencyConfig latency {};
        bool trace_enabled {};
        std::filesystem::path trace_path {"trace_log.csv"};
    };

    struct ExecutionDiagnostics {
        std::uint64_t maker_fills_count {};
        std::uint64_t taker_fills_count {};
        double maker_volume {};
        double taker_volume {};
        double maker_notional {};
        double taker_notional {};
    };

    struct Result {
        CsvParser::ReplayStats replay_stats {};
        double initial_cash {};
        double final_cash {};
        std::int64_t final_position {};
        double final_mid_price {};
        std::uint64_t dropped_pending_orders {};
        ExecutionDiagnostics execution {};
        std::vector<double> equity_curve {};
    };

    explicit BacktestEngine(Strategy<BacktestEngine>& strategy);
    BacktestEngine(Strategy<BacktestEngine>& strategy, Config config);

    [[nodiscard]] Result run(const std::filesystem::path& file_path);

    [[nodiscard]] std::uint64_t submit_order_impl(
        AssetID asset_id,
        Side side,
        double price,
        std::uint64_t quantity,
        std::uint64_t timestamp
    );

    [[nodiscard]] bool cancel_order_impl(AssetID asset_id, std::uint64_t order_id);
    [[nodiscard]] std::uint64_t current_timestamp_impl() const noexcept;

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
        std::int64_t price {};
        std::uint64_t quantity {};
    };

    struct LiveOrder {
        AssetID asset_id {};
        std::uint64_t order_id {};
        Side side {Side::Buy};
        std::int64_t price {};
        std::uint64_t remaining_quantity {};
        std::uint64_t volume_ahead {};
        bool active {};
        bool canceled {};
    };

    static constexpr std::size_t kPendingOrderCapacity = 16'384U;
    static constexpr std::size_t kLiveOrderCapacity = 16'384U;
    static constexpr std::size_t kInvalidOrderIndex = std::numeric_limits<std::size_t>::max();

    void process_market_order(const Order& order);
    void route_trades(const std::vector<Trade>& trades);
    void route_strategy_fill(const StrategyFill& fill);
    [[nodiscard]] std::uint64_t release_pending_orders(std::uint64_t now_ns);
    void execute_pending_order(const PendingOrder& pending);
    void sweep_book(LiveOrder& live_order, std::uint64_t timestamp);
    void update_passive_queue_on_market_trade(const Order& market_order);
    void apply_pessimistic_volume_update();
    [[nodiscard]] std::uint64_t sample_latency_ns();
    void sample_equity(std::uint64_t timestamp);
    void update_snapshot(std::uint64_t timestamp, std::int64_t fallback_price_ticks);
    void open_trace_log();
    void close_trace_log();
    void write_trace_row(std::uint64_t timestamp, int is_bot_trade, const char* trade_side);

    [[nodiscard]] double current_equity() const noexcept;
    [[nodiscard]] bool is_strategy_order(std::uint64_t order_id) const noexcept;
    [[nodiscard]] std::size_t find_live_order_index(std::uint64_t order_id) const noexcept;
    [[nodiscard]] bool is_aggressive(const LiveOrder& live_order) const noexcept;
    [[nodiscard]] bool add_live_order(const LiveOrder& live_order) noexcept;
    void erase_live_order(std::size_t index) noexcept;

    Strategy<BacktestEngine>& strategy_;
    Config config_ {};
    CsvParser parser_ {};
    FixedMatchingBook order_book_ {};
    MarketSnapshot snapshot_ {};
    IndexedPendingMinHeap<PendingOrder, kPendingOrderCapacity> pending_orders_ {};
    std::vector<Trade> trade_buffer_ {};
    std::array<LiveOrder, kLiveOrderCapacity> live_orders_ {};
    std::size_t live_order_count_ {};
    std::vector<StrategyFill> pending_fills_ {};
    std::vector<double> equity_curve_ {};
    std::ofstream trace_log_ {};
    std::mt19937_64 rng_ {};
    std::exponential_distribution<double> exponential_jitter_ {};
    std::lognormal_distribution<double> lognormal_jitter_ {};
    std::uint64_t next_strategy_order_id_ {1ULL << 63U};
    std::uint64_t next_equity_sample_timestamp_ {};
    std::uint64_t current_time_ns_ {};
    std::uint64_t dropped_pending_orders_ {};
    ExecutionDiagnostics execution_ {};
    double cash_ {};
    std::int64_t position_ {};
};

}  // namespace lob
