#pragma once

#include <array>
#include <bit>
#include <cstdint>

#include "lob/PolymarketTypes.hpp"
#include "lob/order.hpp"

namespace lob {

class alignas(64) PolymarketOrderBook {
public:
    struct Level {
        PriceCents price_cents {kPolymarketInvalidPriceCents};
        double size {};
        bool valid {};
    };

    struct BBO {
        Level bid {};
        Level ask {};

        [[nodiscard]] bool has_bid() const noexcept {
            return bid.valid;
        }

        [[nodiscard]] bool has_ask() const noexcept {
            return ask.valid;
        }

        [[nodiscard]] bool crossed() const noexcept {
            return has_bid() && has_ask() && bid.price_cents >= ask.price_cents;
        }
    };

    PolymarketOrderBook() = default;

    void clear() noexcept {
        bids_.fill(0.0);
        asks_.fill(0.0);
        bid_mask_low_ = 0U;
        bid_mask_high_ = 0U;
        ask_mask_low_ = 0U;
        ask_mask_high_ = 0U;
    }

    [[nodiscard]] bool apply_delta(Side side, PriceCents price_cents, double new_size) noexcept {
        if (!is_valid_polymarket_price(price_cents)) {
            return false;
        }

        const auto index = static_cast<std::size_t>(price_cents);
        auto& levels = side == Side::Buy ? bids_ : asks_;
        auto& mask_low = side == Side::Buy ? bid_mask_low_ : ask_mask_low_;
        auto& mask_high = side == Side::Buy ? bid_mask_high_ : ask_mask_high_;

        if (new_size > 0.0) {
            levels[index] = new_size;
            set_price_bit(mask_low, mask_high, price_cents);
        } else {
            levels[index] = 0.0;
            clear_price_bit(mask_low, mask_high, price_cents);
        }

        return true;
    }

    [[nodiscard]] BBO get_bbo() const noexcept {
        BBO bbo {};

        const PriceCents best_bid = best_bid_price_cents();
        if (best_bid != kPolymarketInvalidPriceCents) {
            bbo.bid = Level {
                .price_cents = best_bid,
                .size = bids_[static_cast<std::size_t>(best_bid)],
                .valid = true,
            };
        }

        const PriceCents best_ask = best_ask_price_cents();
        if (best_ask != kPolymarketInvalidPriceCents) {
            bbo.ask = Level {
                .price_cents = best_ask,
                .size = asks_[static_cast<std::size_t>(best_ask)],
                .valid = true,
            };
        }

        return bbo;
    }

    [[nodiscard]] PriceCents best_bid_price_cents() const noexcept {
        return highest_set_price(bid_mask_low_, bid_mask_high_);
    }

    [[nodiscard]] PriceCents best_ask_price_cents() const noexcept {
        return lowest_set_price(ask_mask_low_, ask_mask_high_);
    }

    [[nodiscard]] double best_bid_size() const noexcept {
        const PriceCents price = best_bid_price_cents();
        return price == kPolymarketInvalidPriceCents ? 0.0 : bids_[static_cast<std::size_t>(price)];
    }

    [[nodiscard]] double best_ask_size() const noexcept {
        const PriceCents price = best_ask_price_cents();
        return price == kPolymarketInvalidPriceCents ? 0.0 : asks_[static_cast<std::size_t>(price)];
    }

    [[nodiscard]] double depth(Side side, PriceCents price_cents) const noexcept {
        if (!is_valid_polymarket_price(price_cents)) {
            return 0.0;
        }

        const auto index = static_cast<std::size_t>(price_cents);
        return side == Side::Buy ? bids_[index] : asks_[index];
    }

    [[nodiscard]] bool empty(Side side) const noexcept {
        return side == Side::Buy
            ? (bid_mask_low_ == 0U && bid_mask_high_ == 0U)
            : (ask_mask_low_ == 0U && ask_mask_high_ == 0U);
    }

    [[nodiscard]] const std::array<double, kPolymarketPriceLevelCount>& bids() const noexcept {
        return bids_;
    }

    [[nodiscard]] const std::array<double, kPolymarketPriceLevelCount>& asks() const noexcept {
        return asks_;
    }

private:
    static void set_price_bit(std::uint64_t& low, std::uint64_t& high, PriceCents price_cents) noexcept {
        if (price_cents < 64U) {
            low |= (std::uint64_t {1U} << price_cents);
            return;
        }
        high |= (std::uint64_t {1U} << (price_cents - 64U));
    }

    static void clear_price_bit(std::uint64_t& low, std::uint64_t& high, PriceCents price_cents) noexcept {
        if (price_cents < 64U) {
            low &= ~(std::uint64_t {1U} << price_cents);
            return;
        }
        high &= ~(std::uint64_t {1U} << (price_cents - 64U));
    }

    [[nodiscard]] static PriceCents highest_set_price(std::uint64_t low, std::uint64_t high) noexcept {
        if (high != 0U) {
            return static_cast<PriceCents>(64U + std::bit_width(high) - 1U);
        }
        if (low != 0U) {
            return static_cast<PriceCents>(std::bit_width(low) - 1U);
        }
        return kPolymarketInvalidPriceCents;
    }

    [[nodiscard]] static PriceCents lowest_set_price(std::uint64_t low, std::uint64_t high) noexcept {
        if (low != 0U) {
            return static_cast<PriceCents>(std::countr_zero(low));
        }
        if (high != 0U) {
            return static_cast<PriceCents>(64U + std::countr_zero(high));
        }
        return kPolymarketInvalidPriceCents;
    }

    std::array<double, kPolymarketPriceLevelCount> bids_ {};
    std::array<double, kPolymarketPriceLevelCount> asks_ {};
    std::uint64_t bid_mask_low_ {};
    std::uint64_t bid_mask_high_ {};
    std::uint64_t ask_mask_low_ {};
    std::uint64_t ask_mask_high_ {};
};

}  // namespace lob

