#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "lob/event.hpp"
#include "lob/order.hpp"

namespace lob {

class Wallet {
public:
    static constexpr std::size_t kMaxAssets = 256U;
    static constexpr std::int64_t kScale = 100'000'000LL;

    Wallet() = default;

    void reset(std::size_t asset_count, AssetID quote_currency_id, std::int64_t quote_balance) {
        if (asset_count == 0U || asset_count > kMaxAssets || quote_currency_id >= asset_count) {
            throw std::out_of_range("Wallet configuration is out of range");
        }
        balances_.fill(0);
        reserved_.fill(0);
        liquidation_factors_.fill(0);
        active_asset_count_ = asset_count;
        quote_currency_id_ = quote_currency_id;
        balances_[quote_currency_id_] = quote_balance;
        liquidation_factors_[quote_currency_id_] = kScale;
        (void)recompute_cached_totals();
    }

    [[nodiscard]] static bool from_double(double value, std::int64_t& result) noexcept {
        if (!std::isfinite(value)) {
            return false;
        }
        const long double scaled = static_cast<long double>(value) * static_cast<long double>(kScale);
        if (scaled < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
            scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        result = static_cast<std::int64_t>(std::llround(scaled));
        return true;
    }

    [[nodiscard]] static std::int64_t from_double_or_throw(double value) {
        std::int64_t result {};
        if (!from_double(value, result)) {
            throw std::overflow_error("Wallet fixed-point conversion overflow");
        }
        return result;
    }

    [[nodiscard]] static double to_double(std::int64_t value) noexcept {
        return static_cast<double>(value) / static_cast<double>(kScale);
    }

    [[nodiscard]] std::size_t active_asset_count() const noexcept {
        return active_asset_count_;
    }

    [[nodiscard]] AssetID quote_currency_id() const noexcept {
        return quote_currency_id_;
    }

    [[nodiscard]] bool contains(AssetID asset) const noexcept {
        return static_cast<std::size_t>(asset) < active_asset_count_;
    }

    [[nodiscard]] std::int64_t balance(AssetID asset) const noexcept {
        return contains(asset) ? balances_[asset] : 0;
    }

    [[nodiscard]] std::int64_t reserved(AssetID asset) const noexcept {
        return contains(asset) ? reserved_[asset] : 0;
    }

    [[nodiscard]] std::int64_t free_balance(AssetID asset) const noexcept {
        if (!contains(asset)) {
            return 0;
        }
        const __int128 value = static_cast<__int128>(balances_[asset]) - reserved_[asset];
        return checked_narrow(value).value;
    }

    [[nodiscard]] std::int64_t liquidation_factor(AssetID asset) const noexcept {
        return contains(asset) ? liquidation_factors_[asset] : 0;
    }

    [[nodiscard]] const std::array<std::int64_t, kMaxAssets>& balances() const noexcept {
        return balances_;
    }

    [[nodiscard]] const std::array<std::int64_t, kMaxAssets>& reserved_balances() const noexcept {
        return reserved_;
    }

    [[nodiscard]] bool add_balance(AssetID asset, std::int64_t quantity) noexcept {
        if (!contains(asset)) {
            return false;
        }
        const __int128 next = static_cast<__int128>(balances_[asset]) + quantity;
        const NarrowResult narrowed = checked_narrow(next);
        return narrowed.ok && set_balance(asset, narrowed.value);
    }

    [[nodiscard]] bool sub_balance(AssetID asset, std::int64_t quantity) noexcept {
        if (quantity == std::numeric_limits<std::int64_t>::min()) {
            return false;
        }
        return add_balance(asset, -quantity);
    }

    [[nodiscard]] bool reserve_balance(AssetID asset, std::int64_t quantity) noexcept {
        if (!contains(asset) || quantity < 0 || free_balance(asset) < quantity) {
            return false;
        }
        const NarrowResult next = checked_narrow(static_cast<__int128>(reserved_[asset]) + quantity);
        if (!next.ok) {
            return false;
        }
        reserved_[asset] = next.value;
        return true;
    }

    [[nodiscard]] bool release_reserved(AssetID asset, std::int64_t quantity) noexcept {
        if (!contains(asset) || quantity < 0 || reserved_[asset] < quantity) {
            return false;
        }
        reserved_[asset] -= quantity;
        return true;
    }

    [[nodiscard]] bool consume_reserved(AssetID asset, std::int64_t quantity) noexcept {
        if (!contains(asset) || quantity < 0 || reserved_[asset] < quantity || balances_[asset] < quantity) {
            return false;
        }
        const std::int64_t old_reserved = reserved_[asset];
        reserved_[asset] -= quantity;
        if (!sub_balance(asset, quantity)) {
            reserved_[asset] = old_reserved;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool set_liquidation_factor(AssetID asset, std::int64_t factor) noexcept {
        if (!contains(asset) || factor < 0) {
            return false;
        }
        const std::int64_t old_factor = liquidation_factors_[asset];
        liquidation_factors_[asset] = factor;
        if (recompute_cached_totals()) {
            return true;
        }
        liquidation_factors_[asset] = old_factor;
        (void)recompute_cached_totals();
        return false;
    }

    template <typename FactorProvider>
    [[nodiscard]] bool refresh_liquidation_factors(FactorProvider&& provider) noexcept {
        const auto old_factors = liquidation_factors_;
        for (std::size_t index = 0U; index < active_asset_count_; ++index) {
            const AssetID asset = static_cast<AssetID>(index);
            const std::int64_t factor = asset == quote_currency_id_ ? kScale : provider(asset);
            if (factor < 0) {
                liquidation_factors_ = old_factors;
                return false;
            }
            liquidation_factors_[index] = factor;
        }
        if (recompute_cached_totals()) {
            return true;
        }
        liquidation_factors_ = old_factors;
        (void)recompute_cached_totals();
        return false;
    }

    [[nodiscard]] std::int64_t mark_to_market_nav() const noexcept {
        return cached_nav_;
    }

    [[nodiscard]] std::int64_t get_total_inventory_risk() const noexcept {
        return cached_inventory_risk_;
    }

    [[nodiscard]] bool apply_spot_fill(
        AssetID base_asset,
        AssetID quote_asset,
        Side side,
        std::uint64_t base_quantity,
        std::int64_t price,
        double fee_bps
    ) noexcept {
        if (!contains(base_asset) || !contains(quote_asset) || price < 0) {
            return false;
        }
        const NarrowResult base = checked_narrow(static_cast<__int128>(base_quantity));
        const NarrowResult quote = mul_scaled(base.value, price);
        std::int64_t fee_multiplier {};
        if (!from_double(1.0 - fee_bps * 0.0001, fee_multiplier) || fee_multiplier < 0) {
            return false;
        }
        const NarrowResult net_base = mul_scaled(base.value, fee_multiplier);
        const NarrowResult net_quote = mul_scaled(quote.value, fee_multiplier);
        if (!base.ok || !quote.ok || !net_base.ok || !net_quote.ok) {
            return false;
        }

        const auto old_balances = balances_;
        const std::int64_t old_nav = cached_nav_;
        const std::int64_t old_risk = cached_inventory_risk_;
        const bool ok = side == Side::Buy
            ? add_balance(base_asset, net_base.value) && sub_balance(quote_asset, quote.value)
            : sub_balance(base_asset, base.value) && add_balance(quote_asset, net_quote.value);
        if (!ok) {
            balances_ = old_balances;
            cached_nav_ = old_nav;
            cached_inventory_risk_ = old_risk;
        }
        return ok;
    }

    [[nodiscard]] bool apply_fill(AssetID asset, std::int64_t gross_quantity, double taker_fee_bps,
                                  std::int64_t& net_quantity) noexcept {
        std::int64_t multiplier {};
        if (gross_quantity < 0 ||
            !from_double(1.0 - taker_fee_bps * 0.0001, multiplier) ||
            multiplier < 0) {
            return false;
        }
        const NarrowResult net = mul_scaled(gross_quantity, multiplier);
        if (!net.ok || !add_balance(asset, net.value)) {
            return false;
        }
        net_quantity = net.value;
        return true;
    }

private:
    struct NarrowResult {
        std::int64_t value {};
        bool ok {};
    };

    [[nodiscard]] static NarrowResult checked_narrow(__int128 value) noexcept {
        if (value < std::numeric_limits<std::int64_t>::min() ||
            value > std::numeric_limits<std::int64_t>::max()) {
            return {};
        }
        return {static_cast<std::int64_t>(value), true};
    }

    [[nodiscard]] static NarrowResult mul_scaled(std::int64_t lhs, std::int64_t rhs) noexcept {
        return checked_narrow((static_cast<__int128>(lhs) * rhs) / kScale);
    }

    [[nodiscard]] bool set_balance(AssetID asset, std::int64_t value) noexcept {
        const std::int64_t old_balance = balances_[asset];
        const NarrowResult old_nav = mul_scaled(old_balance, liquidation_factors_[asset]);
        const NarrowResult new_nav = mul_scaled(value, liquidation_factors_[asset]);
        const NarrowResult old_risk = asset == quote_currency_id_
            ? NarrowResult {0, true}
            : mul_scaled(abs_saturated(old_balance), liquidation_factors_[asset]);
        const NarrowResult new_risk = asset == quote_currency_id_
            ? NarrowResult {0, true}
            : mul_scaled(abs_saturated(value), liquidation_factors_[asset]);
        if (!old_nav.ok || !new_nav.ok || !old_risk.ok || !new_risk.ok) {
            return false;
        }
        const NarrowResult next_nav = checked_narrow(
            static_cast<__int128>(cached_nav_) - old_nav.value + new_nav.value);
        const NarrowResult next_risk = checked_narrow(
            static_cast<__int128>(cached_inventory_risk_) - old_risk.value + new_risk.value);
        if (!next_nav.ok || !next_risk.ok) {
            return false;
        }
        balances_[asset] = value;
        cached_nav_ = next_nav.value;
        cached_inventory_risk_ = next_risk.value;
        return true;
    }

    [[nodiscard]] static std::int64_t abs_saturated(std::int64_t value) noexcept {
        if (value == std::numeric_limits<std::int64_t>::min()) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return value < 0 ? -value : value;
    }

    [[nodiscard]] bool recompute_cached_totals() noexcept {
        __int128 nav = 0;
        __int128 risk = 0;
        for (std::size_t index = 0U; index < active_asset_count_; ++index) {
            nav += (static_cast<__int128>(balances_[index]) * liquidation_factors_[index]) / kScale;
            if (index != quote_currency_id_) {
                risk += (static_cast<__int128>(abs_saturated(balances_[index])) *
                         liquidation_factors_[index]) / kScale;
            }
        }
        const NarrowResult nav_result = checked_narrow(nav);
        const NarrowResult risk_result = checked_narrow(risk);
        if (!nav_result.ok || !risk_result.ok) {
            return false;
        }
        cached_nav_ = nav_result.value;
        cached_inventory_risk_ = risk_result.value;
        return true;
    }

    std::array<std::int64_t, kMaxAssets> balances_ {};
    std::array<std::int64_t, kMaxAssets> reserved_ {};
    std::array<std::int64_t, kMaxAssets> liquidation_factors_ {};
    std::size_t active_asset_count_ {};
    AssetID quote_currency_id_ {};
    std::int64_t cached_nav_ {};
    std::int64_t cached_inventory_risk_ {};
};

}  // namespace lob
