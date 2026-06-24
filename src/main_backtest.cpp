#include <chrono>
#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/analytics.hpp"
#include "lob/async_logger.hpp"
#include "lob/backtest_engine.hpp"
#include "lob/inventory_skew_strategy.hpp"
#include "lob/multi_asset_backtest_engine.hpp"
#include "lob/triangular_arbitrage_strategy.hpp"

namespace {

[[nodiscard]] std::string FormatVirtualElapsed(std::uint64_t elapsed_milliseconds) {
    const std::uint64_t total_seconds = elapsed_milliseconds / 1'000U;
    const std::uint64_t milliseconds = elapsed_milliseconds % 1'000U;
    const std::uint64_t seconds = total_seconds % 60U;
    const std::uint64_t total_minutes = total_seconds / 60U;
    const std::uint64_t minutes = total_minutes % 60U;
    const std::uint64_t total_hours = total_minutes / 60U;
    const std::uint64_t hours = total_hours % 24U;
    const std::uint64_t days = total_hours / 24U;

    std::ostringstream stream {};
    if (days > 0U) {
        stream << days << "d ";
    }

    stream << std::setfill('0')
           << std::setw(2) << hours << ':'
           << std::setw(2) << minutes << ':'
           << std::setw(2) << seconds << '.'
           << std::setw(3) << milliseconds;
    return stream.str();
}

[[nodiscard]] double ParseDoubleArg(const char* value, const char* name) {
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string {"Invalid "} + name + ": " + value);
    }
}

[[nodiscard]] std::uint64_t ParseUint64Arg(const char* value, const char* name) {
    try {
        return static_cast<std::uint64_t>(std::stoull(value));
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string {"Invalid "} + name + ": " + value);
    }
}

[[nodiscard]] lob::BacktestEngine::LatencyDistribution ParseLatencyDistribution(const char* value) {
    const std::string distribution {value};
    if (distribution == "none") {
        return lob::BacktestEngine::LatencyDistribution::None;
    }

    if (distribution == "exponential") {
        return lob::BacktestEngine::LatencyDistribution::Exponential;
    }

    if (distribution == "lognormal") {
        return lob::BacktestEngine::LatencyDistribution::LogNormal;
    }

    throw std::invalid_argument("Invalid latency distribution: " + distribution);
}

struct RuntimeOptions {
    double maker_fee_bps {};
    double taker_fee_bps {};
    lob::BacktestEngine::LatencyConfig latency {};
    std::string async_log_path {};
    bool trace_enabled {};
    bool verbose_arb {};
};

[[nodiscard]] lob::MultiAssetBacktestEngine<3U>::LatencyDistribution ConvertMultiAssetLatencyDistribution(
    lob::BacktestEngine::LatencyDistribution distribution
) {
    using SingleDistribution = lob::BacktestEngine::LatencyDistribution;
    using MultiDistribution = lob::MultiAssetBacktestEngine<3U>::LatencyDistribution;

    switch (distribution) {
        case SingleDistribution::None:
            return MultiDistribution::None;
        case SingleDistribution::Exponential:
            return MultiDistribution::Exponential;
        case SingleDistribution::LogNormal:
            return MultiDistribution::LogNormal;
    }

    return MultiDistribution::None;
}

int RunBacktest(
    const std::filesystem::path& csv_path,
    double base_spread,
    double gamma,
    double imbalance_threshold,
    const RuntimeOptions& runtime_options
) {
    constexpr std::uint64_t kEquitySampleIntervalMs = 1'000U;
    constexpr std::uint64_t kEquitySampleIntervalNs = kEquitySampleIntervalMs * 1'000'000ULL;
    constexpr double kQuantityScale = 100'000'000.0;

    using Strategy = lob::InventorySkewStrategy<lob::BacktestEngine>;
    Strategy strategy {Strategy::Config {
        .target_position = 0.0,
        .max_position = 10.0,
        .base_spread = base_spread,
        .risk_aversion_gamma = gamma,
        .imbalance_threshold = imbalance_threshold,
        .quote_quantity = 1'000'000U,
        .refresh_interval_ns = 1'000'000'000ULL,
    }};
    lob::BacktestEngine engine {strategy, lob::BacktestEngine::Config {
        .initial_cash = 100'000'000.0,
        .equity_sample_interval_ns = kEquitySampleIntervalNs,
        .equity_curve_reserve = 100'000U,
        .maker_fee_bps = runtime_options.maker_fee_bps,
        .taker_fee_bps = runtime_options.taker_fee_bps,
        .latency = runtime_options.latency,
        .trace_enabled = runtime_options.trace_enabled,
        .trace_path = "trace_log.csv",
    }};

    const auto wall_start = std::chrono::steady_clock::now();
    const std::clock_t cpu_start = std::clock();
    lob::BacktestEngine::Result result = engine.run(csv_path);

    const std::clock_t cpu_end = std::clock();
    const auto wall_end = std::chrono::steady_clock::now();

    const double cpu_seconds = static_cast<double>(cpu_end - cpu_start) / static_cast<double>(CLOCKS_PER_SEC);
    const std::chrono::duration<double> wall_seconds = wall_end - wall_start;
    const double events_per_second = cpu_seconds > 0.0
        ? static_cast<double>(result.replay_stats.orders_processed) / cpu_seconds
        : 0.0;
    const std::uint64_t virtual_elapsed = result.replay_stats.last_timestamp >= result.replay_stats.first_timestamp
        ? result.replay_stats.last_timestamp - result.replay_stats.first_timestamp
        : 0U;
    const lob::BacktestAnalytics analytics =
        lob::BacktestAnalytics::analyze(
            result.equity_curve,
            kEquitySampleIntervalMs,
            result.execution,
            result.initial_cash
        );

    std::cout << "Total Orders Processed: " << result.replay_stats.orders_processed << '\n';
    std::cout << "Generated Trades: " << result.replay_stats.generated_trades << '\n';
    std::cout << "Virtual Market Time Elapsed: " << FormatVirtualElapsed(virtual_elapsed) << '\n';
    std::cout << "Actual CPU Time Taken: " << std::fixed << std::setprecision(6) << cpu_seconds << " seconds\n";
    std::cout << "Wall Clock Time Taken: " << std::fixed << std::setprecision(6) << wall_seconds.count() << " seconds\n";
    std::cout << "EPS: " << std::fixed << std::setprecision(2) << events_per_second << '\n';
    std::cout << '\n';
    std::cout << "=== Quantitative Tear Sheet ===\n";
    std::cout << "Initial Equity: " << std::fixed << std::setprecision(2) << result.initial_cash << '\n';
    std::cout << "Final Cash: " << std::fixed << std::setprecision(2) << result.final_cash << '\n';
    std::cout << "Final Position: " << std::fixed << std::setprecision(8)
              << (static_cast<double>(result.final_position) / kQuantityScale) << '\n';
    std::cout << "Final Mid Price: " << std::fixed << std::setprecision(2) << result.final_mid_price << '\n';
    std::cout << "Equity Samples: " << result.equity_curve.size() << '\n';
    std::cout << "Total Realized PnL: " << std::fixed << std::setprecision(2) << analytics.total_realized_pnl << '\n';
    std::cout << "Sharpe Ratio: " << std::fixed << std::setprecision(4) << analytics.sharpe_ratio << '\n';
    std::cout << "Max Drawdown: " << std::fixed << std::setprecision(2) << analytics.max_drawdown
              << " (" << std::setprecision(2) << (analytics.max_drawdown_pct * 100.0) << "%)\n";
    std::cout << "Maker Fills: " << analytics.maker_fills_count << '\n';
    std::cout << "Taker Fills: " << analytics.taker_fills_count << '\n';
    std::cout << "Maker Volume: " << std::fixed << std::setprecision(8) << analytics.maker_volume << '\n';
    std::cout << "Taker Volume: " << std::fixed << std::setprecision(8) << analytics.taker_volume << '\n';
    std::cout << "Maker Notional: " << std::fixed << std::setprecision(2) << analytics.maker_notional << '\n';
    std::cout << "Taker Notional: " << std::fixed << std::setprecision(2) << analytics.taker_notional << '\n';
    std::cout << "Turnover: " << std::fixed << std::setprecision(8) << analytics.turnover << '\n';
    if (runtime_options.trace_enabled) {
        std::cout << "Trace Log: trace_log.csv\n";
    }
    std::cout << "RESULT_CSV,"
              << std::fixed << std::setprecision(6)
              << base_spread << ','
              << gamma << ','
              << imbalance_threshold << ','
              << analytics.total_realized_pnl << ','
              << analytics.sharpe_ratio << ','
              << analytics.max_drawdown << ','
              << analytics.maker_fills_count << ','
              << analytics.taker_fills_count << ','
              << analytics.maker_notional << ','
              << analytics.taker_notional << ','
              << analytics.turnover << '\n';

    return 0;
}

int RunTriangularBacktest(
    const std::array<std::filesystem::path, 3U>& csv_paths,
    const RuntimeOptions& runtime_options
) {
    using Engine = lob::MultiAssetBacktestEngine<3U>;

    using Strategy = lob::TriangularArbitrageStrategy<Engine>;
    Strategy strategy {Strategy::Config {
        .taker_fee_bps = runtime_options.taker_fee_bps,
        .verbose = runtime_options.verbose_arb,
    }};
    std::unique_ptr<lob::AsyncLogger<>> async_logger {};
    if (!runtime_options.async_log_path.empty()) {
        async_logger = std::make_unique<lob::AsyncLogger<>>(runtime_options.async_log_path);
    }

    Engine engine {strategy, Engine::PathArray {
        csv_paths[0],
        csv_paths[1],
        csv_paths[2],
    }, Engine::Config {
        .initial_cash = 100'000'000.0,
        .maker_fee_bps = runtime_options.maker_fee_bps,
        .taker_fee_bps = runtime_options.taker_fee_bps,
        .latency = Engine::LatencyConfig {
            .base_latency_ns = runtime_options.latency.base_latency_ns,
            .distribution = ConvertMultiAssetLatencyDistribution(runtime_options.latency.distribution),
            .jitter_mean_ns = runtime_options.latency.jitter_mean_ns,
            .lognormal_mu = runtime_options.latency.lognormal_mu,
            .lognormal_sigma = runtime_options.latency.lognormal_sigma,
            .rng_seed = runtime_options.latency.rng_seed,
        },
        .async_logger = async_logger.get(),
    }};

    const auto wall_start = std::chrono::steady_clock::now();
    const std::clock_t cpu_start = std::clock();
    const Engine::Result result = engine.run();
    const std::clock_t cpu_end = std::clock();
    const auto wall_end = std::chrono::steady_clock::now();

    const double cpu_seconds = static_cast<double>(cpu_end - cpu_start) / static_cast<double>(CLOCKS_PER_SEC);
    const std::chrono::duration<double> wall_seconds = wall_end - wall_start;
    const double events_per_second = cpu_seconds > 0.0
        ? static_cast<double>(result.events_processed) / cpu_seconds
        : 0.0;
    const double traded_notional = result.execution.maker_notional + result.execution.taker_notional;
    const double turnover = result.initial_usdt > 0.0 ? traded_notional / result.initial_usdt : 0.0;

    std::cout << "Mode: Triangular Arbitrage\n";
    std::cout << "Assets: 0=BTCUSDT, 1=ETHUSDT, 2=ETHBTC\n";
    std::cout << "Total Events Processed: " << result.events_processed << '\n';
    std::cout << "Actual CPU Time Taken: " << std::fixed << std::setprecision(6) << cpu_seconds << " seconds\n";
    std::cout << "Wall Clock Time Taken: " << std::fixed << std::setprecision(6) << wall_seconds.count() << " seconds\n";
    std::cout << "EPS: " << std::fixed << std::setprecision(2) << events_per_second << '\n';
    std::cout << '\n';
    std::cout << "=== Multi-Asset Execution Analytics ===\n";
    std::cout << "Initial USDT: " << std::fixed << std::setprecision(2) << result.initial_usdt << '\n';
    std::cout << "Final USDT Liquid: " << std::fixed << std::setprecision(2)
              << lob::Wallet::to_double(result.final_portfolio.balance(0U)) << '\n';
    std::cout << "Final BTC Balance: " << std::fixed << std::setprecision(8)
              << lob::Wallet::to_double(result.final_portfolio.balance(1U)) << '\n';
    std::cout << "Final ETH Balance: " << std::fixed << std::setprecision(8)
              << lob::Wallet::to_double(result.final_portfolio.balance(2U)) << '\n';
    std::cout << "Final BTCUSDT Mid: " << std::fixed << std::setprecision(2) << result.btc_usdt_mid << '\n';
    std::cout << "Final ETHUSDT Mid: " << std::fixed << std::setprecision(2) << result.eth_usdt_mid << '\n';
    std::cout << "Total MTM NAV USDT: " << std::fixed << std::setprecision(2) << result.final_mtm_nav_usdt << '\n';
    std::cout << "MTM PnL USDT: " << std::fixed << std::setprecision(2)
              << (result.final_mtm_nav_usdt - result.initial_usdt) << '\n';
    std::cout << "Inventory Risk USDT: " << std::fixed << std::setprecision(2)
              << result.inventory_risk_usdt << '\n';
    std::cout << "Maker Fills: " << result.execution.maker_fills_count << '\n';
    std::cout << "Taker Fills: " << result.execution.taker_fills_count << '\n';
    std::cout << "Maker Notional USDT: " << std::fixed << std::setprecision(2)
              << result.execution.maker_notional << '\n';
    std::cout << "Taker Notional USDT: " << std::fixed << std::setprecision(2)
              << result.execution.taker_notional << '\n';
    std::cout << "Turnover: " << std::fixed << std::setprecision(8) << turnover << '\n';
    std::cout << "Dropped Pending Orders: " << result.dropped_pending_orders << '\n';
    if (async_logger != nullptr) {
        std::cout << "Async Log: " << runtime_options.async_log_path
                  << " (dropped_records=" << async_logger->dropped() << ")\n";
    }
    for (std::size_t asset_id = 0U; asset_id < result.per_asset_stats.size(); ++asset_id) {
        const auto& stats = result.per_asset_stats[asset_id];
        const double avg_slippage = stats.volume_traded > 0.0
            ? stats.slippage_realized / stats.volume_traded
            : 0.0;
        std::cout << "RESULT_ASSETS_CSV,"
                  << asset_id << ','
                  << std::fixed << std::setprecision(6)
                  << stats.volume_traded << ','
                  << stats.total_pnl << ','
                  << avg_slippage << '\n';
    }
    std::cout << "RESULT_TRI_CSV,"
              << std::fixed << std::setprecision(6)
              << result.events_processed << ','
              << lob::Wallet::to_double(result.final_portfolio.balance(0U)) << ','
              << lob::Wallet::to_double(result.final_portfolio.balance(1U)) << ','
              << lob::Wallet::to_double(result.final_portfolio.balance(2U)) << ','
              << result.final_mtm_nav_usdt << ','
              << (result.final_mtm_nav_usdt - result.initial_usdt) << ','
              << result.inventory_risk_usdt << ','
              << result.execution.maker_fills_count << ','
              << result.execution.taker_fills_count << ','
              << result.execution.maker_notional << ','
              << result.execution.taker_notional << ','
              << turnover << ','
              << result.dropped_pending_orders << '\n';

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage:\n"
                      << "  lob_backtest <binance_trades.csv> [base_spread gamma imbalance_threshold] [--trace]"
                      << " [--maker-fee-bps x] [--taker-fee-bps x] [--base-latency-ns n]"
                      << " [--latency-dist none|exponential|lognormal] [--jitter-mean-ns n]"
                      << " [--lognormal-mu x] [--lognormal-sigma x] [--latency-seed n]\n"
                      << "  lob_backtest <BTCUSDT.csv> <ETHUSDT.csv> <ETHBTC.csv>"
                      << " [--maker-fee-bps x] [--taker-fee-bps x] [--base-latency-ns n]"
                      << " [--latency-dist none|exponential|lognormal] [--jitter-mean-ns n]"
                      << " [--lognormal-mu x] [--lognormal-sigma x] [--latency-seed n]"
                      << " [--async-log path] [--verbose-arb]" << '\n';
            return 1;
        }

        RuntimeOptions runtime_options {};
        std::vector<const char*> positional_args {};
        positional_args.reserve(4U);

        for (int index = 1; index < argc; ++index) {
            const std::string argument {argv[index]};
            if (argument == "--trace") {
                runtime_options.trace_enabled = true;
            } else if (argument == "--verbose-arb") {
                runtime_options.verbose_arb = true;
            } else if (argument == "--maker-fee-bps" && index + 1 < argc) {
                runtime_options.maker_fee_bps = ParseDoubleArg(argv[++index], "maker_fee_bps");
            } else if (argument == "--taker-fee-bps" && index + 1 < argc) {
                runtime_options.taker_fee_bps = ParseDoubleArg(argv[++index], "taker_fee_bps");
            } else if (argument == "--base-latency-ns" && index + 1 < argc) {
                runtime_options.latency.base_latency_ns = ParseUint64Arg(argv[++index], "base_latency_ns");
            } else if (argument == "--latency-dist" && index + 1 < argc) {
                runtime_options.latency.distribution = ParseLatencyDistribution(argv[++index]);
            } else if (argument == "--jitter-mean-ns" && index + 1 < argc) {
                runtime_options.latency.jitter_mean_ns = ParseDoubleArg(argv[++index], "jitter_mean_ns");
            } else if (argument == "--lognormal-mu" && index + 1 < argc) {
                runtime_options.latency.lognormal_mu = ParseDoubleArg(argv[++index], "lognormal_mu");
            } else if (argument == "--lognormal-sigma" && index + 1 < argc) {
                runtime_options.latency.lognormal_sigma = ParseDoubleArg(argv[++index], "lognormal_sigma");
            } else if (argument == "--latency-seed" && index + 1 < argc) {
                runtime_options.latency.rng_seed = ParseUint64Arg(argv[++index], "latency_seed");
            } else if (argument == "--async-log" && index + 1 < argc) {
                runtime_options.async_log_path = argv[++index];
            } else {
                positional_args.push_back(argv[index]);
            }
        }

        if (positional_args.size() != 1U && positional_args.size() != 3U && positional_args.size() != 4U) {
            std::cerr << "Usage:\n"
                      << "  lob_backtest <binance_trades.csv> [base_spread gamma imbalance_threshold] [--trace]"
                      << " [--maker-fee-bps x] [--taker-fee-bps x] [--base-latency-ns n]"
                      << " [--latency-dist none|exponential|lognormal] [--jitter-mean-ns n]"
                      << " [--lognormal-mu x] [--lognormal-sigma x] [--latency-seed n]\n"
                      << "  lob_backtest <BTCUSDT.csv> <ETHUSDT.csv> <ETHBTC.csv>"
                      << " [--maker-fee-bps x] [--taker-fee-bps x] [--base-latency-ns n]"
                      << " [--latency-dist none|exponential|lognormal] [--jitter-mean-ns n]"
                      << " [--lognormal-mu x] [--lognormal-sigma x] [--latency-seed n]"
                      << " [--async-log path] [--verbose-arb]" << '\n';
            return 1;
        }

        const double base_spread = positional_args.size() == 4U
            ? ParseDoubleArg(positional_args[1], "base_spread")
            : 10.0;
        const double gamma = positional_args.size() == 4U
            ? ParseDoubleArg(positional_args[2], "gamma")
            : 2.0;
        const double imbalance_threshold = positional_args.size() == 4U
            ? ParseDoubleArg(positional_args[3], "imbalance_threshold")
            : 3.0;

        if (runtime_options.latency.distribution == lob::BacktestEngine::LatencyDistribution::None) {
            if (runtime_options.latency.lognormal_sigma > 0.0) {
                runtime_options.latency.distribution = lob::BacktestEngine::LatencyDistribution::LogNormal;
            } else if (runtime_options.latency.jitter_mean_ns > 0.0) {
                runtime_options.latency.distribution = lob::BacktestEngine::LatencyDistribution::Exponential;
            }
        }

        if (positional_args.size() == 3U) {
            return RunTriangularBacktest(
                std::array<std::filesystem::path, 3U> {
                    positional_args[0],
                    positional_args[1],
                    positional_args[2],
                },
                runtime_options
            );
        }

        return RunBacktest(positional_args[0], base_spread, gamma, imbalance_threshold, runtime_options);
    } catch (const std::exception& exception) {
        std::cerr << "Backtest failed: " << exception.what() << '\n';
        return 1;
    }
}
