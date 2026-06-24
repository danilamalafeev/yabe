#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lob {

using PriceCents = std::uint8_t;

inline constexpr PriceCents kPolymarketMinPriceCents {1U};
inline constexpr PriceCents kPolymarketMaxPriceCents {99U};
inline constexpr PriceCents kPolymarketInvalidPriceCents {0U};
inline constexpr std::size_t kPolymarketPriceLevelCount {100U};

[[nodiscard]] constexpr bool is_valid_polymarket_price(PriceCents price_cents) noexcept {
    return price_cents >= kPolymarketMinPriceCents && price_cents <= kPolymarketMaxPriceCents;
}

struct TokenId {
    using value_type = std::uint32_t;

    static constexpr value_type invalid_value {std::numeric_limits<value_type>::max()};

    value_type value {invalid_value};

    constexpr TokenId() noexcept = default;
    explicit constexpr TokenId(value_type id) noexcept
        : value(id) {}

    [[nodiscard]] static constexpr TokenId invalid() noexcept {
        return TokenId {invalid_value};
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != invalid_value;
    }

    [[nodiscard]] explicit constexpr operator value_type() const noexcept {
        return value;
    }

    [[nodiscard]] friend constexpr bool operator==(TokenId lhs, TokenId rhs) noexcept = default;
    [[nodiscard]] friend constexpr bool operator<(TokenId lhs, TokenId rhs) noexcept {
        return lhs.value < rhs.value;
    }
};

struct MarketId {
    using value_type = std::uint32_t;

    static constexpr value_type invalid_value {std::numeric_limits<value_type>::max()};

    value_type value {invalid_value};

    constexpr MarketId() noexcept = default;
    explicit constexpr MarketId(value_type id) noexcept
        : value(id) {}

    [[nodiscard]] static constexpr MarketId invalid() noexcept {
        return MarketId {invalid_value};
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != invalid_value;
    }

    [[nodiscard]] explicit constexpr operator value_type() const noexcept {
        return value;
    }

    [[nodiscard]] friend constexpr bool operator==(MarketId lhs, MarketId rhs) noexcept = default;
    [[nodiscard]] friend constexpr bool operator<(MarketId lhs, MarketId rhs) noexcept {
        return lhs.value < rhs.value;
    }
};

enum class PolymarketOutcome : std::uint8_t {
    Yes,
    No,
};

struct PolymarketMarketTokens {
    MarketId market_id {};
    TokenId yes_token_id {};
    TokenId no_token_id {};
};

static_assert(std::is_same_v<PriceCents, std::uint8_t>);
static_assert(std::is_trivially_copyable_v<TokenId>);
static_assert(std::is_trivially_copyable_v<MarketId>);
static_assert(kPolymarketPriceLevelCount == 100U);

}  // namespace lob
