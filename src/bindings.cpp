#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "lob/graph_arbitrage_engine.hpp"
#include "lob/l2_backtest_engine.hpp"
#include "lob/l2_market_maker_strategy.hpp"
#include "lob/multi_asset_backtest_engine.hpp"
#include "lob/triangular_arbitrage_strategy.hpp"
#include "lob/wallet.hpp"

namespace py = pybind11;

namespace {

class PyWallet {
public:
    PyWallet() = default;

    PyWallet(lob::Wallet wallet, double, double)
        : wallet_(wallet) {}

    [[nodiscard]] double usdt() const noexcept {
        return lob::Wallet::to_double(wallet_.balance(0U));
    }

    [[nodiscard]] double btc() const noexcept {
        return lob::Wallet::to_double(wallet_.balance(1U));
    }

    [[nodiscard]] double eth() const noexcept {
        return lob::Wallet::to_double(wallet_.balance(2U));
    }

    [[nodiscard]] double get_nav() const noexcept {
        return lob::Wallet::to_double(wallet_.mark_to_market_nav());
    }

    [[nodiscard]] double get_total_inventory_risk() const noexcept {
        return lob::Wallet::to_double(wallet_.get_total_inventory_risk());
    }

    [[nodiscard]] double balance(lob::AssetID asset_id) const {
        if (!wallet_.contains(asset_id)) {
            throw std::out_of_range("wallet asset_id is out of range");
        }
        return lob::Wallet::to_double(wallet_.balance(asset_id));
    }

private:
    lob::Wallet wallet_ {};
};

class PyBacktestResult {
public:
    PyBacktestResult(const lob::MultiAssetBacktestEngine<3U>::Result& result)
        : wallet_(result.final_portfolio, result.btc_usdt_mid, result.eth_usdt_mid),
          nav_(result.final_mtm_nav_usdt),
          inventory_risk_(result.inventory_risk_usdt),
          events_processed_(result.events_processed),
          taker_notional_(result.execution.taker_notional),
          dropped_orders_(result.dropped_pending_orders) {}

    const PyWallet& wallet() const { return wallet_; }
    double nav() const { return nav_; }
    double inventory_risk() const { return inventory_risk_; }
    std::uint64_t events_processed() const { return events_processed_; }
    double taker_notional() const { return taker_notional_; }
    std::uint64_t dropped_orders() const { return dropped_orders_; }

private:
    PyWallet wallet_;
    double nav_;
    double inventory_risk_;
    std::uint64_t events_processed_;
    double taker_notional_;
    std::uint64_t dropped_orders_;
};

class PyGraphResult {
public:
    template <typename Result>
    explicit PyGraphResult(Result result) {
        copy_from(std::move(result));
    }

    std::uint64_t events_processed() const noexcept { return result_.events_processed; }
    std::uint64_t cycles_detected() const noexcept { return result_.cycles_detected; }
    std::uint64_t attempted_cycles() const noexcept { return result_.attempted_cycles; }
    std::uint64_t completed_cycles() const noexcept { return result_.completed_cycles; }
    std::uint64_t panic_closes() const noexcept { return result_.panic_closes; }
    std::uint64_t market_batches_processed() const noexcept { return result_.market_batches_processed; }
    std::uint64_t cycle_searches() const noexcept { return result_.cycle_searches; }
    double final_usdt() const noexcept { return result_.final_usdt; }
    double final_nav() const noexcept { return result_.final_nav; }
    double inventory_risk() const noexcept { return result_.inventory_risk; }
    const std::vector<double>& balances() const noexcept { return result_.balances; }
    const std::vector<lob::AssetID>& last_cycle() const noexcept { return result_.last_cycle; }
    std::size_t cycle_snapshot_count() const noexcept { return result_.cycle_snapshots.size(); }
    std::uint64_t cycle_snapshots_overwritten() const noexcept { return result_.cycle_snapshots_overwritten; }

    [[nodiscard]] py::dict get_cycle_snapshots_dataframe() const {
        py::ssize_t row_count = 0;
        for (const lob::GraphArbitrageEngine::CycleSnapshot& snapshot : result_.cycle_snapshots) {
            row_count += static_cast<py::ssize_t>(snapshot.leg_count);
        }

        py::array_t<std::uint64_t> snapshot_ids {row_count};
        py::array_t<std::uint64_t> timestamps {row_count};
        py::array_t<std::uint32_t> leg_indices {row_count};
        py::array_t<std::uint32_t> from_assets {row_count};
        py::array_t<std::uint32_t> to_assets {row_count};
        py::array_t<double> obis {row_count};
        py::array_t<double> spreads_bps {row_count};
        py::array_t<std::uint8_t> executed {row_count};

        auto* snapshot_id_ptr = snapshot_ids.mutable_data();
        auto* timestamp_ptr = timestamps.mutable_data();
        auto* leg_index_ptr = leg_indices.mutable_data();
        auto* from_asset_ptr = from_assets.mutable_data();
        auto* to_asset_ptr = to_assets.mutable_data();
        auto* obi_ptr = obis.mutable_data();
        auto* spread_bps_ptr = spreads_bps.mutable_data();
        auto* executed_ptr = executed.mutable_data();

        py::ssize_t row = 0;
        for (std::size_t snapshot_index = 0U; snapshot_index < result_.cycle_snapshots.size(); ++snapshot_index) {
            const lob::GraphArbitrageEngine::CycleSnapshot& snapshot = result_.cycle_snapshots[snapshot_index];
            for (std::size_t leg = 0U; leg < snapshot.leg_count; ++leg) {
                snapshot_id_ptr[row] = static_cast<std::uint64_t>(snapshot_index);
                timestamp_ptr[row] = snapshot.timestamp_ns;
                leg_index_ptr[row] = static_cast<std::uint32_t>(leg);
                from_asset_ptr[row] = static_cast<std::uint32_t>(snapshot.cycle_path[leg]);
                to_asset_ptr[row] = static_cast<std::uint32_t>(snapshot.cycle_path[leg + 1U]);
                obi_ptr[row] = snapshot.leg_obis[leg];
                spread_bps_ptr[row] = snapshot.leg_spreads_bps[leg];
                executed_ptr[row] = snapshot.executed ? 1U : 0U;
                ++row;
            }
        }

        py::dict frame {};
        frame["snapshot_id"] = std::move(snapshot_ids);
        frame["timestamp_ns"] = std::move(timestamps);
        frame["leg_index"] = std::move(leg_indices);
        frame["from_asset"] = std::move(from_assets);
        frame["to_asset"] = std::move(to_assets);
        frame["obi"] = std::move(obis);
        frame["spread_bps"] = std::move(spreads_bps);
        frame["executed"] = std::move(executed);
        return frame;
    }

private:
    template <typename Result>
    void copy_from(Result result) {
        result_.events_processed = result.events_processed;
        result_.cycles_detected = result.cycles_detected;
        result_.attempted_cycles = result.attempted_cycles;
        result_.completed_cycles = result.completed_cycles;
        result_.panic_closes = result.panic_closes;
        result_.market_batches_processed = result.market_batches_processed;
        result_.cycle_searches = result.cycle_searches;
        result_.initial_usdt = result.initial_usdt;
        result_.final_usdt = result.final_usdt;
        result_.final_nav = result.final_nav;
        result_.inventory_risk = result.inventory_risk;
        result_.balances = std::move(result.balances);
        result_.last_cycle = std::move(result.last_cycle);
        result_.cycle_snapshots.clear();
        result_.cycle_snapshots.reserve(result.cycle_snapshots.size());
        for (const auto& snapshot : result.cycle_snapshots) {
            lob::GraphArbitrageEngine::CycleSnapshot copied {};
            copied.timestamp_ns = snapshot.timestamp_ns;
            copied.leg_count = snapshot.leg_count;
            copied.cycle_path = snapshot.cycle_path;
            copied.leg_obis = snapshot.leg_obis;
            copied.leg_spreads_bps = snapshot.leg_spreads_bps;
            copied.executed = snapshot.executed;
            result_.cycle_snapshots.push_back(copied);
        }
        result_.cycle_snapshots_overwritten = result.cycle_snapshots_overwritten;
    }

    lob::GraphArbitrageEngine::Result result_ {};
};

class PyL2BacktestResult {
public:
    explicit PyL2BacktestResult(lob::L2BacktestEngine::Result result)
        : result_(std::move(result)) {}

    std::uint64_t events_processed() const noexcept { return result_.events_processed; }
    std::uint64_t market_batches_processed() const noexcept { return result_.market_batches_processed; }
    std::uint64_t strategy_ticks() const noexcept { return result_.strategy_ticks; }
    std::uint64_t orders_submitted() const noexcept { return result_.orders_submitted; }
    std::uint64_t orders_canceled() const noexcept { return result_.orders_canceled; }
    std::uint64_t dropped_pending_orders() const noexcept { return result_.dropped_pending_orders; }
    std::uint64_t active_orders() const noexcept { return result_.active_orders; }
    std::uint64_t last_fill_timestamp() const noexcept { return result_.last_fill_timestamp; }
    double initial_cash() const noexcept { return result_.initial_cash; }
    double final_cash() const noexcept { return result_.final_cash; }
    std::int64_t final_position() const noexcept { return result_.final_position; }
    double final_mid_price() const noexcept { return result_.final_mid_price; }
    double final_best_bid() const noexcept { return result_.final_best_bid; }
    double final_best_ask() const noexcept { return result_.final_best_ask; }
    double final_nav() const noexcept { return result_.final_nav; }
    double pnl() const noexcept { return result_.final_nav - result_.initial_cash; }
    std::uint64_t maker_fills_count() const noexcept { return result_.execution.maker_fills_count; }
    std::uint64_t taker_fills_count() const noexcept { return result_.execution.taker_fills_count; }
    double maker_volume() const noexcept { return result_.execution.maker_volume; }
    double taker_volume() const noexcept { return result_.execution.taker_volume; }
    double maker_notional() const noexcept { return result_.execution.maker_notional; }
    double taker_notional() const noexcept { return result_.execution.taker_notional; }
    const std::vector<double>& equity_curve() const noexcept { return result_.equity_curve; }
    std::size_t feature_count() const noexcept { return result_.features.size(); }

    [[nodiscard]] py::dict get_features_dataframe() const {
        const py::ssize_t size = static_cast<py::ssize_t>(result_.features.size());
        py::array_t<std::uint64_t> timestamps {size};
        py::array_t<double> best_bids {size};
        py::array_t<double> best_asks {size};
        py::array_t<double> mids {size};
        py::array_t<double> spreads_bps {size};
        py::array_t<double> bid_qty_1 {size};
        py::array_t<double> ask_qty_1 {size};
        py::array_t<double> imbalance_1 {size};
        py::array_t<double> bid_qty_visible {size};
        py::array_t<double> ask_qty_visible {size};
        py::array_t<std::int64_t> positions {size};
        py::array_t<double> navs {size};

        auto* timestamp_ptr = timestamps.mutable_data();
        auto* best_bid_ptr = best_bids.mutable_data();
        auto* best_ask_ptr = best_asks.mutable_data();
        auto* mid_ptr = mids.mutable_data();
        auto* spread_bps_ptr = spreads_bps.mutable_data();
        auto* bid_qty_1_ptr = bid_qty_1.mutable_data();
        auto* ask_qty_1_ptr = ask_qty_1.mutable_data();
        auto* imbalance_1_ptr = imbalance_1.mutable_data();
        auto* bid_qty_visible_ptr = bid_qty_visible.mutable_data();
        auto* ask_qty_visible_ptr = ask_qty_visible.mutable_data();
        auto* position_ptr = positions.mutable_data();
        auto* nav_ptr = navs.mutable_data();

        for (py::ssize_t index = 0; index < size; ++index) {
            const lob::L2BacktestEngine::Result::FeatureRow& row =
                result_.features[static_cast<std::size_t>(index)];
            timestamp_ptr[index] = row.timestamp;
            best_bid_ptr[index] = row.best_bid;
            best_ask_ptr[index] = row.best_ask;
            mid_ptr[index] = row.mid_price;
            spread_bps_ptr[index] = row.spread_bps;
            bid_qty_1_ptr[index] = row.bid_qty_1;
            ask_qty_1_ptr[index] = row.ask_qty_1;
            imbalance_1_ptr[index] = row.imbalance_1;
            bid_qty_visible_ptr[index] = row.bid_qty_visible;
            ask_qty_visible_ptr[index] = row.ask_qty_visible;
            position_ptr[index] = row.position;
            nav_ptr[index] = row.nav;
        }

        py::dict frame {};
        frame["timestamp"] = std::move(timestamps);
        frame["best_bid"] = std::move(best_bids);
        frame["best_ask"] = std::move(best_asks);
        frame["mid_price"] = std::move(mids);
        frame["spread_bps"] = std::move(spreads_bps);
        frame["bid_qty_1"] = std::move(bid_qty_1);
        frame["ask_qty_1"] = std::move(ask_qty_1);
        frame["imbalance_1"] = std::move(imbalance_1);
        frame["bid_qty_visible"] = std::move(bid_qty_visible);
        frame["ask_qty_visible"] = std::move(ask_qty_visible);
        frame["position"] = std::move(positions);
        frame["nav"] = std::move(navs);
        return frame;
    }

private:
    lob::L2BacktestEngine::Result result_ {};
};

class PyL2MarketMakerBacktest {
public:
    using Strategy = lob::L2MarketMakerStrategy<lob::L2BacktestEngine>;

    PyL2MarketMakerBacktest(
        double initial_cash,
        double maker_fee_bps,
        double taker_fee_bps,
        std::uint64_t latency_ns,
        std::size_t max_book_levels_per_side,
        double quantity_scale,
        double quote_offset,
        std::uint64_t quote_quantity,
        std::uint64_t refresh_interval_ns,
        bool record_features,
        std::uint64_t feature_sample_interval_ns,
        std::size_t feature_reserve
    )
        : engine_config_(lob::L2BacktestEngine::Config {
              .initial_cash = initial_cash,
              .maker_fee_bps = maker_fee_bps,
              .taker_fee_bps = taker_fee_bps,
              .latency_ns = latency_ns,
              .max_book_levels_per_side = max_book_levels_per_side,
              .quantity_scale = quantity_scale,
              .record_features = record_features,
              .feature_sample_interval_ns = feature_sample_interval_ns,
              .feature_reserve = feature_reserve,
          }),
          strategy_config_(Strategy::Config {
              .quote_offset = quote_offset,
              .quote_quantity = quote_quantity,
              .refresh_interval_ns = refresh_interval_ns,
          }) {}

    [[nodiscard]] PyL2BacktestResult run(const std::string& file_path) const {
        Strategy strategy {strategy_config_};
        lob::L2BacktestEngine engine {strategy, engine_config_};
        return PyL2BacktestResult {engine.run(file_path)};
    }

private:
    lob::L2BacktestEngine::Config engine_config_ {};
    Strategy::Config strategy_config_ {};
};

template <typename Engine>
class PyGraphEngine {
public:
    PyGraphEngine(
        double initial_usdt,
        std::uint64_t latency_ns,
        std::uint64_t intra_leg_latency_ns,
        double taker_fee_bps,
        double max_cycle_notional_usdt,
        double max_adverse_obi,
        double max_spread_bps,
        double min_depth_usdt,
        double min_cycle_edge_bps,
        std::size_t cycle_snapshot_reserve,
        std::string quote_asset,
        std::size_t max_book_levels_per_side
    )
        : engine_(typename Engine::Config {
              .initial_usdt = initial_usdt,
              .quote_asset = std::move(quote_asset),
              .latency_ns = latency_ns,
              .intra_leg_latency_ns = intra_leg_latency_ns,
              .taker_fee_bps = taker_fee_bps,
              .max_cycle_notional_usdt = max_cycle_notional_usdt,
              .max_adverse_obi = max_adverse_obi,
              .max_spread_bps = max_spread_bps,
              .min_depth_usdt = min_depth_usdt,
              .min_cycle_edge_bps = min_cycle_edge_bps,
              .cycle_snapshot_reserve = cycle_snapshot_reserve,
              .max_book_levels_per_side = max_book_levels_per_side,
          }) {}

    lob::AssetID add_pair(const std::string& base_asset, const std::string& quote_asset, const std::string& csv_file_path) {
        return engine_.add_pair(base_asset, quote_asset, csv_file_path);
    }

    [[nodiscard]] PyGraphResult run() {
        return PyGraphResult {engine_.run()};
    }

    [[nodiscard]] const std::vector<std::string>& assets() const noexcept {
        return engine_.assets();
    }

private:
    Engine engine_;
};

using PyGraphEngineDense = PyGraphEngine<lob::GraphArbitrageEngine>;
using PyGraphEngineLarge = PyGraphEngine<lob::GraphArbitrageEngineLarge>;

class PyTriangularEngine {
public:
    PyTriangularEngine(std::uint64_t latency_ns, double maker_fee_bps, double taker_fee_bps, bool verbose)
        : latency_ns_(latency_ns), maker_fee_bps_(maker_fee_bps), taker_fee_bps_(taker_fee_bps), verbose_(verbose) {}

    [[nodiscard]] PyBacktestResult run(const std::vector<std::string>& file_paths) {
        if (file_paths.size() != 3U) {
            throw std::invalid_argument("TriangularEngine.run expects exactly 3 CSV file paths");
        }

        using Engine = lob::MultiAssetBacktestEngine<3U>;

        using Strategy = lob::TriangularArbitrageStrategy<Engine>;
        Strategy strategy {Strategy::Config {
            .taker_fee_bps = taker_fee_bps_,
            .verbose = verbose_,
        }};

        Engine engine {strategy, Engine::PathArray {
            file_paths[0],
            file_paths[1],
            file_paths[2],
        }, Engine::Config {
            .maker_fee_bps = maker_fee_bps_,
            .taker_fee_bps = taker_fee_bps_,
            .latency = Engine::LatencyConfig {
                .base_latency_ns = latency_ns_,
            },
        }};

        if (record_microstructure_) {
            engine.enable_microstructure_recording(true, sampling_interval_ns_);
        }

        PyBacktestResult result {engine.run()};
        if (record_microstructure_) {
            snapshots_ = engine.take_microstructure_snapshots();
        } else {
            snapshots_.clear();
        }
        return result;
    }

    void enable_microstructure_recording(bool enable, std::uint64_t sampling_interval_ns = 0U) {
        record_microstructure_ = enable;
        sampling_interval_ns_ = sampling_interval_ns;
        snapshots_.clear();
    }

    [[nodiscard]] py::dict get_microstructure_dataframe() const {
        const py::ssize_t size = static_cast<py::ssize_t>(snapshots_.size());
        py::array_t<std::uint64_t> timestamps {size};
        py::array_t<std::uint32_t> asset_ids {size};
        py::array_t<double> bid_prices {size};
        py::array_t<double> bid_quantities {size};
        py::array_t<double> ask_prices {size};
        py::array_t<double> ask_quantities {size};

        auto* timestamp_ptr = timestamps.mutable_data();
        auto* asset_id_ptr = asset_ids.mutable_data();
        auto* bid_price_ptr = bid_prices.mutable_data();
        auto* bid_quantity_ptr = bid_quantities.mutable_data();
        auto* ask_price_ptr = ask_prices.mutable_data();
        auto* ask_quantity_ptr = ask_quantities.mutable_data();

        for (py::ssize_t index = 0; index < size; ++index) {
            const lob::MarketSnapshot& snapshot = snapshots_[static_cast<std::size_t>(index)];
            timestamp_ptr[index] = snapshot.ts;
            asset_id_ptr[index] = snapshot.asset;
            bid_price_ptr[index] = snapshot.bid_p;
            bid_quantity_ptr[index] = snapshot.bid_q;
            ask_price_ptr[index] = snapshot.ask_p;
            ask_quantity_ptr[index] = snapshot.ask_q;
        }

        py::dict frame {};
        frame["timestamp"] = std::move(timestamps);
        frame["asset_id"] = std::move(asset_ids);
        frame["bid_price"] = std::move(bid_prices);
        frame["bid_qty"] = std::move(bid_quantities);
        frame["ask_price"] = std::move(ask_prices);
        frame["ask_qty"] = std::move(ask_quantities);
        return frame;
    }

private:
    std::uint64_t latency_ns_ {};
    double maker_fee_bps_ {};
    double taker_fee_bps_ {};
    bool verbose_ {};
    bool record_microstructure_ {};
    std::uint64_t sampling_interval_ns_ {};
    std::vector<lob::MarketSnapshot> snapshots_ {};
};

PyBacktestResult run_triangular(
    const std::vector<std::string>& file_paths,
    std::uint64_t latency_ns,
    double maker_fee_bps,
    double taker_fee_bps,
    bool verbose
) {
    PyTriangularEngine engine(latency_ns, maker_fee_bps, taker_fee_bps, verbose);
    return engine.run(file_paths);
}

}  // namespace

PYBIND11_MODULE(yabe, module) {
    module.doc() = "YABE: Yet Another Backtest Engine Python bindings";

    py::class_<PyWallet>(module, "Wallet")
        .def(py::init<>())
        .def_property_readonly("usdt", &PyWallet::usdt)
        .def_property_readonly("btc", &PyWallet::btc)
        .def_property_readonly("eth", &PyWallet::eth)
        .def("balance", &PyWallet::balance, py::arg("asset_id"))
        .def("get_nav", &PyWallet::get_nav)
        .def("get_total_inventory_risk", &PyWallet::get_total_inventory_risk);

    py::class_<PyBacktestResult>(module, "BacktestResult")
        .def_property_readonly("wallet", &PyBacktestResult::wallet)
        .def_property_readonly("nav", &PyBacktestResult::nav)
        .def_property_readonly("inventory_risk", &PyBacktestResult::inventory_risk)
        .def_property_readonly("events_processed", &PyBacktestResult::events_processed)
        .def_property_readonly("taker_notional", &PyBacktestResult::taker_notional)
        .def_property_readonly("dropped_orders", &PyBacktestResult::dropped_orders);

    py::class_<PyL2BacktestResult>(module, "L2BacktestResult")
        .def_property_readonly("events_processed", &PyL2BacktestResult::events_processed)
        .def_property_readonly("market_batches_processed", &PyL2BacktestResult::market_batches_processed)
        .def_property_readonly("strategy_ticks", &PyL2BacktestResult::strategy_ticks)
        .def_property_readonly("orders_submitted", &PyL2BacktestResult::orders_submitted)
        .def_property_readonly("orders_canceled", &PyL2BacktestResult::orders_canceled)
        .def_property_readonly("dropped_pending_orders", &PyL2BacktestResult::dropped_pending_orders)
        .def_property_readonly("active_orders", &PyL2BacktestResult::active_orders)
        .def_property_readonly("last_fill_timestamp", &PyL2BacktestResult::last_fill_timestamp)
        .def_property_readonly("initial_cash", &PyL2BacktestResult::initial_cash)
        .def_property_readonly("final_cash", &PyL2BacktestResult::final_cash)
        .def_property_readonly("final_position", &PyL2BacktestResult::final_position)
        .def_property_readonly("final_mid_price", &PyL2BacktestResult::final_mid_price)
        .def_property_readonly("final_best_bid", &PyL2BacktestResult::final_best_bid)
        .def_property_readonly("final_best_ask", &PyL2BacktestResult::final_best_ask)
        .def_property_readonly("final_nav", &PyL2BacktestResult::final_nav)
        .def_property_readonly("pnl", &PyL2BacktestResult::pnl)
        .def_property_readonly("maker_fills_count", &PyL2BacktestResult::maker_fills_count)
        .def_property_readonly("taker_fills_count", &PyL2BacktestResult::taker_fills_count)
        .def_property_readonly("maker_volume", &PyL2BacktestResult::maker_volume)
        .def_property_readonly("taker_volume", &PyL2BacktestResult::taker_volume)
        .def_property_readonly("maker_notional", &PyL2BacktestResult::maker_notional)
        .def_property_readonly("taker_notional", &PyL2BacktestResult::taker_notional)
        .def_property_readonly("equity_curve", &PyL2BacktestResult::equity_curve)
        .def_property_readonly("feature_count", &PyL2BacktestResult::feature_count)
        .def("get_features_dataframe", &PyL2BacktestResult::get_features_dataframe);

    py::class_<PyL2MarketMakerBacktest>(module, "L2MarketMakerBacktest")
        .def(
            py::init<double, double, double, std::uint64_t, std::size_t, double, double, std::uint64_t, std::uint64_t, bool, std::uint64_t, std::size_t>(),
            py::arg("initial_cash") = 100'000'000.0,
            py::arg("maker_fee_bps") = 0.0,
            py::arg("taker_fee_bps") = 7.5,
            py::arg("latency_ns") = 500'000U,
            py::arg("max_book_levels_per_side") = 20U,
            py::arg("quantity_scale") = 100'000'000.0,
            py::arg("quote_offset") = 0.5,
            py::arg("quote_quantity") = 1'000'000U,
            py::arg("refresh_interval_ns") = 1'000'000'000ULL,
            py::arg("record_features") = false,
            py::arg("feature_sample_interval_ns") = 0U,
            py::arg("feature_reserve") = 100'000U
        )
        .def(
            "run",
            &PyL2MarketMakerBacktest::run,
            py::arg("file_path"),
            py::call_guard<py::gil_scoped_release>()
        );

    py::class_<PyGraphResult>(module, "GraphResult")
        .def_property_readonly("events_processed", &PyGraphResult::events_processed)
        .def_property_readonly("cycles_detected", &PyGraphResult::cycles_detected)
        .def_property_readonly("attempted_cycles", &PyGraphResult::attempted_cycles)
        .def_property_readonly("completed_cycles", &PyGraphResult::completed_cycles)
        .def_property_readonly("panic_closes", &PyGraphResult::panic_closes)
        .def_property_readonly("market_batches_processed", &PyGraphResult::market_batches_processed)
        .def_property_readonly("cycle_searches", &PyGraphResult::cycle_searches)
        .def_property_readonly("final_usdt", &PyGraphResult::final_usdt)
        .def_property_readonly("final_nav", &PyGraphResult::final_nav)
        .def_property_readonly("inventory_risk", &PyGraphResult::inventory_risk)
        .def_property_readonly("balances", &PyGraphResult::balances)
        .def_property_readonly("last_cycle", &PyGraphResult::last_cycle)
        .def_property_readonly("cycle_snapshot_count", &PyGraphResult::cycle_snapshot_count)
        .def_property_readonly("cycle_snapshots_overwritten", &PyGraphResult::cycle_snapshots_overwritten)
        .def("get_cycle_snapshots_dataframe", &PyGraphResult::get_cycle_snapshots_dataframe);

    py::class_<PyGraphEngineDense>(module, "GraphEngine")
        .def(
            py::init<double, std::uint64_t, std::uint64_t, double, double, double, double, double, double, std::size_t, std::string, std::size_t>(),
            py::arg("initial_usdt") = 100'000'000.0,
            py::arg("latency_ns") = 0,
            py::arg("intra_leg_latency_ns") = 75,
            py::arg("taker_fee_bps") = 0.0,
            py::arg("max_cycle_notional_usdt") = 1'000.0,
            py::arg("max_adverse_obi") = 1.0,
            py::arg("max_spread_bps") = 1'000.0,
            py::arg("min_depth_usdt") = 0.0,
            py::arg("min_cycle_edge_bps") = 0.0,
            py::arg("cycle_snapshot_reserve") = 100'000U,
            py::arg("quote_asset") = "USDT",
            py::arg("max_book_levels_per_side") = 100U
        )
        .def("add_pair", &PyGraphEngineDense::add_pair, py::arg("base_asset"), py::arg("quote_asset"), py::arg("csv_file_path"))
        .def_property_readonly("assets", &PyGraphEngineDense::assets)
        .def("run", &PyGraphEngineDense::run, py::call_guard<py::gil_scoped_release>());

    py::class_<PyGraphEngineLarge>(module, "GraphEngineLarge")
        .def(
            py::init<double, std::uint64_t, std::uint64_t, double, double, double, double, double, double, std::size_t, std::string, std::size_t>(),
            py::arg("initial_usdt") = 100'000'000.0,
            py::arg("latency_ns") = 0,
            py::arg("intra_leg_latency_ns") = 75,
            py::arg("taker_fee_bps") = 0.0,
            py::arg("max_cycle_notional_usdt") = 1'000.0,
            py::arg("max_adverse_obi") = 1.0,
            py::arg("max_spread_bps") = 1'000.0,
            py::arg("min_depth_usdt") = 0.0,
            py::arg("min_cycle_edge_bps") = 0.0,
            py::arg("cycle_snapshot_reserve") = 100'000U,
            py::arg("quote_asset") = "USDT",
            py::arg("max_book_levels_per_side") = 100U
        )
        .def("add_pair", &PyGraphEngineLarge::add_pair, py::arg("base_asset"), py::arg("quote_asset"), py::arg("csv_file_path"))
        .def_property_readonly("assets", &PyGraphEngineLarge::assets)
        .def("run", &PyGraphEngineLarge::run, py::call_guard<py::gil_scoped_release>());

    py::class_<PyTriangularEngine>(module, "TriangularEngine")
        .def(py::init<std::uint64_t, double, double, bool>(),
             py::arg("latency_ns") = 0,
             py::arg("maker_fee_bps") = 0.0,
             py::arg("taker_fee_bps") = 0.0,
             py::arg("verbose") = false)
        .def(
            "run",
            &PyTriangularEngine::run,
            py::arg("file_paths"),
            py::call_guard<py::gil_scoped_release>()
        )
        .def(
            "enable_microstructure_recording",
            &PyTriangularEngine::enable_microstructure_recording,
            py::arg("enable"),
            py::arg("sampling_interval_ns") = 0
        )
        .def("get_microstructure_dataframe", &PyTriangularEngine::get_microstructure_dataframe);

    module.def("run_triangular", &run_triangular,
               py::arg("file_paths"),
               py::arg("latency_ns") = 0,
               py::arg("maker_fee_bps") = 0.0,
               py::arg("taker_fee_bps") = 0.0,
               py::arg("verbose") = false,
               py::call_guard<py::gil_scoped_release>(),
               "Run a triangular arbitrage backtest.");
}
