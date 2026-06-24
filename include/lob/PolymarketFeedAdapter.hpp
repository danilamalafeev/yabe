#pragma once

#include <cstdint>
#include <span>
#include <type_traits>

#include "lob/PolymarketOrderBook.hpp"
#include "lob/PolymarketTypes.hpp"
#include "lob/order.hpp"

namespace lob {

struct alignas(32) PolymarketL2Update {
    std::uint64_t timestamp_ns {};
    std::uint64_t new_size {};
    MarketId market_id {};
    TokenId token_id {};
    Side side {Side::Buy};
    PriceCents price_cents {};
    bool is_snapshot {};
};
static_assert(sizeof(PolymarketL2Update) == 32);

class PolymarketFeedAdapter {
public:
    PolymarketFeedAdapter() = default;

    explicit PolymarketFeedAdapter(std::span<PolymarketOrderBook*> books_by_token_id) noexcept
        : books_by_token_id_(books_by_token_id) {}

    void reset_routes(std::span<PolymarketOrderBook*> books_by_token_id) noexcept {
        books_by_token_id_ = books_by_token_id;
    }

    [[nodiscard]] bool bind_book(TokenId token_id, PolymarketOrderBook& book) noexcept {
        PolymarketOrderBook** slot = route_slot(token_id);
        if (slot == nullptr) {
            return false;
        }
        *slot = &book;
        return true;
    }

    [[nodiscard]] bool unbind_book(TokenId token_id) noexcept {
        PolymarketOrderBook** slot = route_slot(token_id);
        if (slot == nullptr) {
            return false;
        }
        *slot = nullptr;
        return true;
    }

    [[nodiscard]] PolymarketOrderBook* book_for(TokenId token_id) const noexcept {
        if (!token_id.is_valid()) {
            return nullptr;
        }

        const auto index = static_cast<std::size_t>(token_id.value);
        if (index >= books_by_token_id_.size()) {
            return nullptr;
        }

        return books_by_token_id_[index];
    }

    [[nodiscard]] bool on_l2_update(const PolymarketL2Update& update) noexcept {
        PolymarketOrderBook* book = book_for(update.token_id);
        if (book == nullptr) {
            return false;
        }

        if (update.is_snapshot) {
            book->clear();
        }

        return book->apply_delta(update.side, update.price_cents, static_cast<double>(update.new_size) / 100'000'000.0);
    }

private:
    [[nodiscard]] PolymarketOrderBook** route_slot(TokenId token_id) noexcept {
        if (!token_id.is_valid()) {
            return nullptr;
        }

        const auto index = static_cast<std::size_t>(token_id.value);
        if (index >= books_by_token_id_.size()) {
            return nullptr;
        }

        return &books_by_token_id_[index];
    }

    std::span<PolymarketOrderBook*> books_by_token_id_ {};
};

static_assert(std::is_trivially_copyable_v<PolymarketL2Update>);

}  // namespace lob

