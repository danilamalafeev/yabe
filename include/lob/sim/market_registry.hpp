#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/sim/types.hpp"

namespace lob::sim {

struct MarketKeyHash {
    [[nodiscard]] std::size_t operator()(MarketKey key) const noexcept {
        const auto packed = (static_cast<std::uint64_t>(key.venue_id) << 32U)
                            | static_cast<std::uint64_t>(key.product_id);
        return static_cast<std::size_t>(packed ^ (packed >> 32U));
    }
};

class MarketRegistry {
public:
    [[nodiscard]] MarketID register_market(MarketKey key) {
        const auto existing = market_ids_by_key_.find(key);
        if (existing != market_ids_by_key_.end()) {
            return existing->second;
        }

        const auto id = static_cast<MarketID>(market_keys_.size());
        market_keys_.push_back(key);
        market_ids_by_key_.emplace(key, id);
        return id;
    }

    [[nodiscard]] std::optional<MarketID> find_market(MarketKey key) const {
        const auto it = market_ids_by_key_.find(key);
        if (it == market_ids_by_key_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] const MarketKey* market_key(MarketID market_id) const noexcept {
        if (static_cast<std::size_t>(market_id) >= market_keys_.size()) {
            return nullptr;
        }
        return &market_keys_[market_id];
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return market_keys_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return market_keys_.empty();
    }

    void clear() {
        market_keys_.clear();
        market_ids_by_key_.clear();
    }

private:
    std::vector<MarketKey> market_keys_ {};
    std::unordered_map<MarketKey, MarketID, MarketKeyHash> market_ids_by_key_ {};
};

}  // namespace lob::sim
