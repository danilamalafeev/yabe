#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

#include "lob/l2_order_book.hpp"
#include "lob/l2_strategy.hpp"
#include "lob/l2_update_csv_parser.hpp"
#include "lob/indexed_pending_min_heap.hpp"
#include "lob/fixed_matching_book.hpp"
#include "lob/order_gateway.hpp"
#include "lob/strategy.hpp"

namespace lob {

class L2BacktestEngine final : public OrderGateway<L2BacktestEngine> {
public:
    struct Config {
        double initial_cash {100'000'000.0};
        double maker_fee_bps {};
        double taker_fee_bps {};
        std::uint64_t latency_ns {};
        std::uint64_t equity_sample_interval_ns {1'000'000'000ULL};
        std::size_t equity_curve_reserve {100'000U};
        std::size_t max_book_levels_per_side {100U};
        double quantity_scale {100'000'000.0};
        bool record_features {false};
        std::uint64_t feature_sample_interval_ns {};
        std::size_t feature_reserve {100'000U};
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
        std::uint64_t events_processed {};
        std::uint64_t market_batches_processed {};
        std::uint64_t strategy_ticks {};
        std::uint64_t orders_submitted {};
        std::uint64_t orders_canceled {};
        std::uint64_t dropped_pending_orders {};
        std::uint64_t active_orders {};
        std::uint64_t last_fill_timestamp {};
        double initial_cash {};
        double final_cash {};
        std::int64_t final_position {};
        double final_mid_price {};
        double final_best_bid {};
        double final_best_ask {};
        double final_nav {};
        ExecutionDiagnostics execution {};
        std::vector<double> equity_curve {};
        struct FeatureRow {
            std::uint64_t timestamp {};
            double best_bid {};
            double best_ask {};
            double mid_price {};
            double spread_bps {};
            double bid_qty_1 {};
            double ask_qty_1 {};
            double imbalance_1 {};
            double bid_qty_visible {};
            double ask_qty_visible {};
            std::int64_t position {};
            double nav {};
        };
        std::vector<FeatureRow> features {};
    };

    explicit L2BacktestEngine(Strategy<L2BacktestEngine>& strategy);
    L2BacktestEngine(Strategy<L2BacktestEngine>& strategy, Config config);
    explicit L2BacktestEngine(L2Strategy<L2BacktestEngine>& strategy);
    L2BacktestEngine(L2Strategy<L2BacktestEngine>& strategy, Config config);

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
        Cancel,
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

    struct LiveOrder {
        AssetID asset_id {};
        std::uint64_t order_id {};
        Side side {Side::Buy};
        double price {};
        std::uint64_t remaining_quantity {};
        bool active {};
        bool canceled {};
    };

    static constexpr std::size_t kPendingOrderCapacity = 16'384U;
    static constexpr std::size_t kLiveOrderCapacity = 16'384U;
    static constexpr std::size_t kInvalidOrderIndex = std::numeric_limits<std::size_t>::max();

    void reset_runtime();
    void apply_event_batch(Result& result, std::uint64_t batch_time_ns);
    void apply_event(const L2UpdateEvent& event) noexcept;
    void release_pending_orders(std::uint64_t now_ns);
    void execute_pending_order(const PendingOrder& pending);
    void process_active_crosses(std::uint64_t timestamp);
    void dispatch_strategy_tick(Result& result);
    void rebuild_strategy_book();
    void route_strategy_fill(const StrategyFill& fill);
    void sample_equity(std::uint64_t timestamp);
    void sample_features(std::uint64_t timestamp);

    [[nodiscard]] double current_mid_price() const noexcept;
    [[nodiscard]] double current_nav() const noexcept;
    [[nodiscard]] bool has_visible_book() const noexcept;
    [[nodiscard]] bool is_aggressive(const LiveOrder& live_order) const noexcept;
    [[nodiscard]] std::uint64_t sweep_visible_depth(LiveOrder& live_order, LiquidityRole role, std::uint64_t timestamp);
    [[nodiscard]] std::uint64_t level_qty_to_order_qty(std::uint64_t qty_lots) const noexcept;
    [[nodiscard]] double order_qty_to_units(std::uint64_t quantity) const noexcept;
    [[nodiscard]] std::uint64_t order_qty_to_lots(std::uint64_t quantity) const noexcept;
    [[nodiscard]] bool add_live_order(const LiveOrder& live_order) noexcept;
    [[nodiscard]] std::size_t find_live_order_index(std::uint64_t order_id) const noexcept;
    void erase_live_order(std::size_t index) noexcept;

    Strategy<L2BacktestEngine>* strategy_ {};
    L2Strategy<L2BacktestEngine>* l2_strategy_ {};
    Config config_ {};
    L2UpdateCsvParser parser_ {};
    L2OrderBook book_ {};
    FixedMatchingBook strategy_book_ {};
    IndexedPendingMinHeap<PendingOrder, kPendingOrderCapacity> pending_orders_ {};
    std::array<LiveOrder, kLiveOrderCapacity> live_orders_ {};
    std::size_t live_order_count_ {};
    std::vector<double> equity_curve_ {};
    std::vector<Result::FeatureRow> feature_rows_ {};
    std::uint64_t next_strategy_order_id_ {1ULL << 63U};
    std::uint64_t next_equity_sample_timestamp_ {};
    std::uint64_t next_feature_sample_timestamp_ {};
    std::uint64_t current_time_ns_ {};
    std::uint64_t synthetic_order_id_ {1U};
    std::uint64_t dropped_pending_orders_ {};
    std::uint64_t orders_submitted_ {};
    std::uint64_t orders_canceled_ {};
    std::uint64_t last_fill_timestamp_ {};
    ExecutionDiagnostics execution_ {};
    double cash_ {};
    std::int64_t position_ {};
};

}  // namespace lob
