#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "lob/event_l2_update.hpp"

namespace lob {

template <typename T, std::size_t Capacity>
class StaticVector {
public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    StaticVector() = default;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] bool full() const noexcept { return size_ >= Capacity; }

    [[nodiscard]] T* begin() noexcept { return data_.data(); }
    [[nodiscard]] const T* begin() const noexcept { return data_.data(); }
    [[nodiscard]] T* end() noexcept { return data_.data() + size_; }
    [[nodiscard]] const T* end() const noexcept { return data_.data() + size_; }

    [[nodiscard]] T& front() noexcept { return data_[0]; }
    [[nodiscard]] const T& front() const noexcept { return data_[0]; }
    [[nodiscard]] T& back() noexcept { return data_[size_ - 1]; }
    [[nodiscard]] const T& back() const noexcept { return data_[size_ - 1]; }
    [[nodiscard]] T& operator[](std::size_t idx) noexcept { return data_[idx]; }
    [[nodiscard]] const T& operator[](std::size_t idx) const noexcept { return data_[idx]; }

    void clear() noexcept { size_ = 0U; }

    void push_back(const T& val) noexcept {
        if (size_ < Capacity) { data_[size_++] = val; }
    }

    void pop_back() noexcept {
        if (size_ > 0U) { --size_; }
    }

    iterator insert(iterator pos, const T& val) noexcept {
        if (size_ >= Capacity) return pos;
        std::size_t idx = static_cast<std::size_t>(pos - data_.data());
        if (idx > size_) idx = size_;
        for (std::size_t i = size_; i > idx; --i) {
            data_[i] = data_[i - 1];
        }
        data_[idx] = val;
        ++size_;
        return data_.data() + idx;
    }

    iterator erase(iterator pos) noexcept {
        std::size_t idx = static_cast<std::size_t>(pos - data_.data());
        if (idx >= size_) return end();
        for (std::size_t i = idx; i + 1 < size_; ++i) {
            data_[i] = data_[i + 1];
        }
        --size_;
        return data_.data() + idx;
    }

    void reserve(std::size_t) noexcept { /* no-op for static */ }

private:
    std::array<T, Capacity> data_ {};
    std::size_t size_ {0U};
};

class L2OrderBook {
public:
    static constexpr std::size_t kCapacity = 128U;

    struct Level {
        std::int64_t price {};         // PriceTicks
        std::uint64_t qty {};          // QtyLots
        std::uint64_t depleted_qty {}; // QtyLots

        [[nodiscard]] std::uint64_t effective_qty() const noexcept {
            return qty > depleted_qty ? qty - depleted_qty : 0U;
        }
    };

    using LevelContainer = StaticVector<Level, kCapacity>;
    using LevelIterator = LevelContainer::iterator;
    using ConstLevelIterator = LevelContainer::const_iterator;

    explicit L2OrderBook(std::size_t max_levels_per_side = kCapacity)
        : max_levels_per_side_(clamp_capacity(max_levels_per_side)) {}

    void reserve(std::size_t max_levels_per_side) noexcept {
        max_levels_per_side_ = clamp_capacity(max_levels_per_side);
        trim_to_capacity(true, bids_);
        trim_to_capacity(false, asks_);
    }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        bid_total_qty_ = 0U;
        ask_total_qty_ = 0U;
        bid_total_notional_ = 0;
        ask_total_notional_ = 0;
        bid_depleted_qty_ = 0U;
        ask_depleted_qty_ = 0U;
        bid_depleted_notional_ = 0;
        ask_depleted_notional_ = 0;
    }

    void apply_update(const L2UpdateEvent& event) {
        if (event.is_snapshot) { clear(); }
        update_level(event.is_bid, event.price, event.qty);
    }

    void update_level(bool is_bid, std::int64_t price, std::uint64_t qty) {
        if (price <= 0) {
            return;
        }

        LevelContainer& side = levels_for(is_bid);
        auto level_it = find_level(side, is_bid, price);
        const bool found = level_it != side.end() && level_it->price == price;

        if (qty == 0U) {
            if (found) {
                acc_subtract(is_bid, *level_it);
                side.erase(level_it);
            }
            return;
        }

        if (found) {
            acc_subtract(is_bid, *level_it);
            level_it->qty = qty;
            if (level_it->depleted_qty > qty) {
                level_it->depleted_qty = qty;
            }
            acc_add(is_bid, *level_it);
            return;
        }

        if (side.size() >= max_levels_per_side_) {
            if (side.empty() || is_deeper_or_equal(is_bid, price, side.back().price)) {
                return;
            }
            acc_subtract(is_bid, side.back());
            side.pop_back();
            level_it = find_level(side, is_bid, price);
        }

        Level new_level {
            .price = price,
            .qty = qty,
            .depleted_qty = 0U,
        };
        acc_add(is_bid, new_level);
        side.insert(level_it, new_level);
    }

    void deplete_level(bool is_bid, std::int64_t price, std::uint64_t consumed_qty) noexcept {
        if (price <= 0 || consumed_qty == 0U) {
            return;
        }

        LevelContainer& side = levels_for(is_bid);
        auto level_it = find_level(side, is_bid, price);
        if (level_it == side.end() || level_it->price != price) {
            return;
        }

        level_it->depleted_qty += consumed_qty;
        if (is_bid) {
            bid_depleted_qty_ += consumed_qty;
            bid_depleted_notional_ += price * static_cast<std::int64_t>(consumed_qty);
        } else {
            ask_depleted_qty_ += consumed_qty;
            ask_depleted_notional_ += price * static_cast<std::int64_t>(consumed_qty);
        }
    }

    template <typename PriceContainer>
    void remove_levels_not_in(bool is_bid, const PriceContainer& prices) {
        LevelContainer& side = levels_for(is_bid);
        auto level_it = side.begin();
        while (level_it != side.end()) {
            bool found = false;
            for (const std::int64_t price : prices) {
                if (price > 0 && price == level_it->price) {
                    found = true;
                    break;
                }
            }

            if (found) {
                ++level_it;
            } else {
                acc_subtract(is_bid, *level_it);
                level_it = side.erase(level_it);
            }
        }
    }

    [[nodiscard]] std::uint64_t effective_qty(bool is_bid, std::int64_t price) const noexcept {
        if (price <= 0) {
            return 0U;
        }

        const LevelContainer& side = levels_for(is_bid);
        auto level_it = find_level(side, is_bid, price);
        if (level_it == side.end() || level_it->price != price) {
            return 0U;
        }
        return level_it->effective_qty();
    }

    [[nodiscard]] const LevelContainer& bids() const noexcept {
        return bids_;
    }

    [[nodiscard]] const LevelContainer& asks() const noexcept {
        return asks_;
    }

    [[nodiscard]] std::size_t max_levels_per_side() const noexcept {
        return max_levels_per_side_;
    }

    [[nodiscard]] std::int64_t best_bid() const noexcept {
        return bids_.empty() ? 0 : bids_.front().price;
    }

    [[nodiscard]] std::int64_t best_ask() const noexcept {
        return asks_.empty() ? 0 : asks_.front().price;
    }

    [[nodiscard]] std::int64_t effective_best_bid() const noexcept {
        for (const Level& level : bids_) {
            if (level.effective_qty() > 0U) {
                return level.price;
            }
        }
        return 0;
    }

    [[nodiscard]] std::int64_t effective_best_ask() const noexcept {
        for (const Level& level : asks_) {
            if (level.effective_qty() > 0U) {
                return level.price;
            }
        }
        return 0;
    }

    // --- O(1) accumulator accessors ---

    [[nodiscard]] std::uint64_t bid_total_qty() const noexcept { return bid_total_qty_; }
    [[nodiscard]] std::uint64_t ask_total_qty() const noexcept { return ask_total_qty_; }
    [[nodiscard]] std::int64_t bid_total_notional() const noexcept { return bid_total_notional_; }
    [[nodiscard]] std::int64_t ask_total_notional() const noexcept { return ask_total_notional_; }

    [[nodiscard]] std::uint64_t bid_depleted_qty() const noexcept { return bid_depleted_qty_; }
    [[nodiscard]] std::uint64_t ask_depleted_qty() const noexcept { return ask_depleted_qty_; }
    [[nodiscard]] std::int64_t bid_depleted_notional() const noexcept { return bid_depleted_notional_; }
    [[nodiscard]] std::int64_t ask_depleted_notional() const noexcept { return ask_depleted_notional_; }

    [[nodiscard]] std::uint64_t bid_effective_qty() const noexcept {
        const std::uint64_t v = bid_total_qty_ - bid_depleted_qty_;
        return v;
    }
    [[nodiscard]] std::uint64_t ask_effective_qty() const noexcept {
        const std::uint64_t v = ask_total_qty_ - ask_depleted_qty_;
        return v;
    }
    [[nodiscard]] std::int64_t bid_effective_notional() const noexcept {
        return bid_total_notional_ - bid_depleted_notional_;
    }
    [[nodiscard]] std::int64_t ask_effective_notional() const noexcept {
        return ask_total_notional_ - ask_depleted_notional_;
    }

private:
    // --- Accumulator helpers ---

    void acc_add(bool is_bid, const Level& level) noexcept {
        if (is_bid) {
            bid_total_qty_ += level.qty;
            bid_total_notional_ += level.price * static_cast<std::int64_t>(level.qty);
            bid_depleted_qty_ += level.depleted_qty;
            bid_depleted_notional_ += level.price * static_cast<std::int64_t>(level.depleted_qty);
        } else {
            ask_total_qty_ += level.qty;
            ask_total_notional_ += level.price * static_cast<std::int64_t>(level.qty);
            ask_depleted_qty_ += level.depleted_qty;
            ask_depleted_notional_ += level.price * static_cast<std::int64_t>(level.depleted_qty);
        }
    }

    void acc_subtract(bool is_bid, const Level& level) noexcept {
        if (is_bid) {
            bid_total_qty_ -= level.qty;
            bid_total_notional_ -= level.price * static_cast<std::int64_t>(level.qty);
            bid_depleted_qty_ -= level.depleted_qty;
            bid_depleted_notional_ -= level.price * static_cast<std::int64_t>(level.depleted_qty);
        } else {
            ask_total_qty_ -= level.qty;
            ask_total_notional_ -= level.price * static_cast<std::int64_t>(level.qty);
            ask_depleted_qty_ -= level.depleted_qty;
            ask_depleted_notional_ -= level.price * static_cast<std::int64_t>(level.depleted_qty);
        }
    }

    [[nodiscard]] LevelContainer& levels_for(bool is_bid) noexcept {
        return is_bid ? bids_ : asks_;
    }

    [[nodiscard]] const LevelContainer& levels_for(bool is_bid) const noexcept {
        return is_bid ? bids_ : asks_;
    }

    [[nodiscard]] static LevelIterator find_level(LevelContainer& side, bool is_bid, std::int64_t price) noexcept {
        if (side.size() <= 16U) {
            if (is_bid) {
                for (auto it = side.begin(); it != side.end(); ++it) {
                    if (it->price <= price) return it;
                }
                return side.end();
            }
            for (auto it = side.begin(); it != side.end(); ++it) {
                if (it->price >= price) return it;
            }
            return side.end();
        }

        if (is_bid) {
            return std::lower_bound(
                side.begin(),
                side.end(),
                price,
                [](const Level& level, std::int64_t target_price) noexcept {
                    return level.price > target_price;
                }
            );
        }

        return std::lower_bound(
            side.begin(),
            side.end(),
            price,
            [](const Level& level, std::int64_t target_price) noexcept {
                return level.price < target_price;
            }
        );
    }

    [[nodiscard]] static ConstLevelIterator find_level(
        const LevelContainer& side,
        bool is_bid,
        std::int64_t price
    ) noexcept {
        if (side.size() <= 16U) {
            if (is_bid) {
                for (auto it = side.begin(); it != side.end(); ++it) {
                    if (it->price <= price) return it;
                }
                return side.end();
            }
            for (auto it = side.begin(); it != side.end(); ++it) {
                if (it->price >= price) return it;
            }
            return side.end();
        }

        if (is_bid) {
            return std::lower_bound(
                side.begin(),
                side.end(),
                price,
                [](const Level& level, std::int64_t target_price) noexcept {
                    return level.price > target_price;
                }
            );
        }

        return std::lower_bound(
            side.begin(),
            side.end(),
            price,
            [](const Level& level, std::int64_t target_price) noexcept {
                return level.price < target_price;
            }
        );
    }

    [[nodiscard]] static bool is_deeper_or_equal(bool is_bid, std::int64_t price, std::int64_t tail_price) noexcept {
        return is_bid ? price <= tail_price : price >= tail_price;
    }

    void trim_to_capacity(bool is_bid, LevelContainer& side) noexcept {
        while (side.size() > max_levels_per_side_) {
            acc_subtract(is_bid, side.back());
            side.pop_back();
        }
    }

    [[nodiscard]] static constexpr std::size_t clamp_capacity(std::size_t value) noexcept {
        if (value == 0U) {
            return 1U;
        }
        return value > kCapacity ? kCapacity : value;
    }

    std::size_t max_levels_per_side_ {kCapacity};
    LevelContainer bids_ {};
    LevelContainer asks_ {};

    // Incremental accumulators — maintained in O(1) per update (integer arithmetic)
    std::uint64_t bid_total_qty_ {};
    std::uint64_t ask_total_qty_ {};
    std::int64_t bid_total_notional_ {};
    std::int64_t ask_total_notional_ {};
    std::uint64_t bid_depleted_qty_ {};
    std::uint64_t ask_depleted_qty_ {};
    std::int64_t bid_depleted_notional_ {};
    std::int64_t ask_depleted_notional_ {};
};

}  // namespace lob
