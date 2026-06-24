#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>

#include "lob/fixed_matching_book.hpp"
#include "lob/strategy.hpp"

namespace lob {

template <typename Gateway>
class TriangularArbitrageStrategy final : public Strategy<Gateway> {
public:
    struct Config {
        double threshold {1.0015};
        double minimum_notional_usdt {10.0};
        double max_cycle_notional_usdt {1'000.0};
        double slippage_tolerance {0.0002};
        double taker_fee_bps {};
        std::uint64_t cooldown_ns {50'000'000ULL};
        std::uint64_t intra_leg_latency_ns {75U};
        std::uint64_t intra_leg_jitter_ns {25U};
        bool verbose {};
    };

    TriangularArbitrageStrategy() = default;
    explicit TriangularArbitrageStrategy(Config config) : config_(config) {}

    void on_tick(AssetID asset_id, const FixedMatchingBook& book, Gateway& gateway) override {
        if (asset_id >= kAssetCount) {
            return;
        }

        best_bid_[asset_id] = book.get_best_bid();
        best_ask_[asset_id] = book.get_best_ask();
        books_[asset_id] = &book;

        for (std::size_t index = 0U; index < kAssetCount; ++index) {
            if (books_[index] == nullptr || best_bid_[index] <= 0.0 || best_ask_[index] <= 0.0) {
                return;
            }
        }

        const std::uint64_t timestamp = gateway.current_timestamp();
        if (last_arb_time_ns_ != 0U && timestamp < last_arb_time_ns_ + config_.cooldown_ns) {
            return;
        }

        const double ask_btc = best_ask_[kBtcUsdt];
        const double bid_btc = best_bid_[kBtcUsdt];
        const double ask_eth = best_ask_[kEthUsdt];
        const double bid_eth = best_bid_[kEthUsdt];
        const double ask_ethbtc = best_ask_[kEthBtc];
        const double bid_ethbtc = best_bid_[kEthBtc];

        const double rate1 = (1.0 / ask_btc) * (1.0 / ask_ethbtc) * bid_eth;
        const double rate2 = (1.0 / ask_eth) * bid_ethbtc * bid_btc;
        const double fee_multiplier = taker_fee_multiplier();
        if (fee_multiplier <= 0.0) {
            return;
        }
        const double three_leg_fee_multiplier = fee_multiplier * fee_multiplier * fee_multiplier;
        const double rate1_after_fees = rate1 * three_leg_fee_multiplier;
        const double rate2_after_fees = rate2 * three_leg_fee_multiplier;

        if (rate1_after_fees > config_.threshold) {
            const double bottleneck_usdt = apply_notional_cap(
                calculate_path_1_bottleneck_usdt(ask_btc, bid_eth, ask_ethbtc)
            );
            if (bottleneck_usdt <= config_.minimum_notional_usdt) {
                return;
            }

            execute_path_1(rate1_after_fees, bottleneck_usdt, timestamp, gateway);
            last_arb_time_ns_ = timestamp;
        } else if (rate2_after_fees > config_.threshold) {
            const double bottleneck_usdt = apply_notional_cap(
                calculate_path_2_bottleneck_usdt(ask_eth, bid_btc, bid_ethbtc)
            );
            if (bottleneck_usdt <= config_.minimum_notional_usdt) {
                return;
            }

            execute_path_2(rate2_after_fees, bottleneck_usdt, timestamp, gateway);
            last_arb_time_ns_ = timestamp;
        }
    }

private:
    static constexpr AssetID kBtcUsdt = 0U;
    static constexpr AssetID kEthUsdt = 1U;
    static constexpr AssetID kEthBtc = 2U;
    static constexpr std::size_t kAssetCount = 3U;
    static constexpr double kQuantityScale = 100'000'000.0;

    [[nodiscard]] static std::uint64_t ToScaledQuantity(double quantity) noexcept {
        return quantity > 0.0 ? static_cast<std::uint64_t>(quantity * kQuantityScale) : 0U;
    }

    [[nodiscard]] static double ToUnits(std::uint64_t scaled_quantity) noexcept {
        return static_cast<double>(scaled_quantity) / kQuantityScale;
    }

    [[nodiscard]] double calculate_path_1_bottleneck_usdt(
        double ask_btc,
        double bid_eth,
        double ask_ethbtc
    ) const noexcept {
        const double btcusdt_ask_btc = ToUnits(best_level_quantity(kBtcUsdt, Side::Sell, ask_btc));
        const double ethbtc_ask_eth = ToUnits(best_level_quantity(kEthBtc, Side::Sell, ask_ethbtc));
        const double ethusdt_bid_eth = ToUnits(best_level_quantity(kEthUsdt, Side::Buy, bid_eth));

        const double leg_1_capacity_usdt = btcusdt_ask_btc * ask_btc;
        const double leg_2_capacity_usdt = ethbtc_ask_eth * ask_ethbtc * ask_btc;
        const double leg_3_capacity_usdt = ethusdt_bid_eth * ask_ethbtc * ask_btc;
        return std::min({leg_1_capacity_usdt, leg_2_capacity_usdt, leg_3_capacity_usdt});
    }

    [[nodiscard]] double calculate_path_2_bottleneck_usdt(
        double ask_eth,
        double bid_btc,
        double bid_ethbtc
    ) const noexcept {
        (void)bid_btc;

        const double ethusdt_ask_eth = ToUnits(best_level_quantity(kEthUsdt, Side::Sell, ask_eth));
        const double ethbtc_bid_eth = ToUnits(best_level_quantity(kEthBtc, Side::Buy, bid_ethbtc));
        const double btcusdt_bid_btc = ToUnits(best_level_quantity(kBtcUsdt, Side::Buy, best_bid_[kBtcUsdt]));

        const double leg_1_capacity_usdt = ethusdt_ask_eth * ask_eth;
        const double leg_2_capacity_usdt = ethbtc_bid_eth * ask_eth;
        const double leg_3_capacity_usdt = bid_ethbtc > 0.0 ? btcusdt_bid_btc * ask_eth / bid_ethbtc : 0.0;
        return std::min({leg_1_capacity_usdt, leg_2_capacity_usdt, leg_3_capacity_usdt});
    }

    [[nodiscard]] std::uint64_t best_level_quantity(AssetID asset_id, Side side, double price) const noexcept {
        return books_[asset_id]->get_total_quantity_at_price(side, price);
    }

    [[nodiscard]] double apply_notional_cap(double bottleneck_usdt) const noexcept {
        if (config_.max_cycle_notional_usdt > 0.0 && bottleneck_usdt > config_.max_cycle_notional_usdt) {
            return config_.max_cycle_notional_usdt;
        }

        return bottleneck_usdt;
    }

    [[nodiscard]] double taker_fee_multiplier() const noexcept {
        return 1.0 - (config_.taker_fee_bps * 0.0001);
    }

    void execute_path_1(double rate, double bottleneck_usdt, std::uint64_t timestamp, Gateway& gateway) {
        const double btc_quantity = bottleneck_usdt / best_ask_[kBtcUsdt];
        const double btc_after_fee = btc_quantity * taker_fee_multiplier();
        const double eth_quantity = btc_after_fee / best_ask_[kEthBtc];
        const double eth_after_fee = eth_quantity * taker_fee_multiplier();

        const std::uint64_t btc_scaled = ToScaledQuantity(btc_quantity);
        const std::uint64_t eth_scaled = ToScaledQuantity(eth_quantity);
        const std::uint64_t eth_after_fee_scaled = ToScaledQuantity(eth_after_fee);
        if (btc_scaled == 0U || eth_scaled == 0U || eth_after_fee_scaled == 0U) {
            return;
        }

        if (config_.verbose) {
            std::cout << "ARB_DETECTED,path=USDT_BTC_ETH_USDT,ts_ns=" << timestamp
                      << ",rate=" << std::fixed << std::setprecision(8) << rate
                      << ",bottleneck_usdt=" << std::setprecision(2) << bottleneck_usdt << '\n';
        }

        OrderGroup group {
            .group_id = next_group_id_++,
            .timestamp = timestamp,
            .intra_leg_latency_ns = config_.intra_leg_latency_ns,
            .intra_leg_jitter_ns = config_.intra_leg_jitter_ns,
            .slippage_tolerance = config_.slippage_tolerance,
            .legs = {
                OrderRequest {
                    .quantity = btc_scaled,
                    .timestamp = timestamp,
                    .price = static_cast<std::int64_t>(std::llround(best_ask_[kBtcUsdt] * 100'000'000.0)),
                    .expected_price = static_cast<std::int64_t>(std::llround(best_ask_[kBtcUsdt] * 100'000'000.0)),
                    .slippage_tolerance = config_.slippage_tolerance,
                    .asset_id = kBtcUsdt,
                    .side = Side::Buy,
                },
                OrderRequest {
                    .quantity = eth_scaled,
                    .timestamp = timestamp,
                    .price = static_cast<std::int64_t>(std::llround(best_ask_[kEthBtc] * 100'000'000.0)),
                    .expected_price = static_cast<std::int64_t>(std::llround(best_ask_[kEthBtc] * 100'000'000.0)),
                    .slippage_tolerance = config_.slippage_tolerance,
                    .asset_id = kEthBtc,
                    .side = Side::Buy,
                },
                OrderRequest {
                    .quantity = eth_after_fee_scaled,
                    .timestamp = timestamp,
                    .price = static_cast<std::int64_t>(std::llround(best_bid_[kEthUsdt] * 100'000'000.0)),
                    .expected_price = static_cast<std::int64_t>(std::llround(best_bid_[kEthUsdt] * 100'000'000.0)),
                    .slippage_tolerance = config_.slippage_tolerance,
                    .asset_id = kEthUsdt,
                    .side = Side::Sell,
                },
            },
        };
        const OrderGroupResult result = gateway.execute_group(group);
        (void)result;
    }

    void execute_path_2(double rate, double bottleneck_usdt, std::uint64_t timestamp, Gateway& gateway) {
        const double eth_quantity = bottleneck_usdt / best_ask_[kEthUsdt];
        const double eth_after_fee = eth_quantity * taker_fee_multiplier();
        const double btc_quantity = eth_after_fee * best_bid_[kEthBtc] * taker_fee_multiplier();

        const std::uint64_t eth_scaled = ToScaledQuantity(eth_quantity);
        const std::uint64_t eth_after_fee_scaled = ToScaledQuantity(eth_after_fee);
        const std::uint64_t btc_scaled = ToScaledQuantity(btc_quantity);
        if (eth_scaled == 0U || eth_after_fee_scaled == 0U || btc_scaled == 0U) {
            return;
        }

        if (config_.verbose) {
            std::cout << "ARB_DETECTED,path=USDT_ETH_BTC_USDT,ts_ns=" << timestamp
                      << ",rate=" << std::fixed << std::setprecision(8) << rate
                      << ",bottleneck_usdt=" << std::setprecision(2) << bottleneck_usdt << '\n';
        }

        OrderGroup group {
            .group_id = next_group_id_++,
            .timestamp = timestamp,
            .intra_leg_latency_ns = config_.intra_leg_latency_ns,
            .intra_leg_jitter_ns = config_.intra_leg_jitter_ns,
            .slippage_tolerance = config_.slippage_tolerance,
            .legs = {
                OrderRequest {
                    .quantity = eth_scaled,
                    .timestamp = timestamp,
                    .price = static_cast<std::int64_t>(std::llround(best_ask_[kEthUsdt] * 100'000'000.0)),
                    .expected_price = static_cast<std::int64_t>(std::llround(best_ask_[kEthUsdt] * 100'000'000.0)),
                    .slippage_tolerance = config_.slippage_tolerance,
                    .asset_id = kEthUsdt,
                    .side = Side::Buy,
                },
                OrderRequest {
                    .quantity = eth_after_fee_scaled,
                    .timestamp = timestamp,
                    .price = static_cast<std::int64_t>(std::llround(best_bid_[kEthBtc] * 100'000'000.0)),
                    .expected_price = static_cast<std::int64_t>(std::llround(best_bid_[kEthBtc] * 100'000'000.0)),
                    .slippage_tolerance = config_.slippage_tolerance,
                    .asset_id = kEthBtc,
                    .side = Side::Sell,
                },
                OrderRequest {
                    .quantity = btc_scaled,
                    .timestamp = timestamp,
                    .price = static_cast<std::int64_t>(std::llround(best_bid_[kBtcUsdt] * 100'000'000.0)),
                    .expected_price = static_cast<std::int64_t>(std::llround(best_bid_[kBtcUsdt] * 100'000'000.0)),
                    .slippage_tolerance = config_.slippage_tolerance,
                    .asset_id = kBtcUsdt,
                    .side = Side::Sell,
                },
            },
        };
        const OrderGroupResult result = gateway.execute_group(group);
        (void)result;
    }

    Config config_ {};
    std::array<double, kAssetCount> best_bid_ {};
    std::array<double, kAssetCount> best_ask_ {};
    std::array<const FixedMatchingBook*, kAssetCount> books_ {};
    std::uint64_t last_arb_time_ns_ {};
    std::uint64_t next_group_id_ {1U};
};

}  // namespace lob
