#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#include "lob/csv_parser.hpp"
#include "lob/event.hpp"

namespace lob {

template <typename Parser>
concept StreamingParser = requires(Parser parser, const Parser& const_parser) {
    { const_parser.has_next() } noexcept -> std::same_as<bool>;
    { const_parser.peek_time() } noexcept -> std::same_as<std::uint64_t>;
    { parser.pop() } -> std::same_as<Event>;
};

template <std::size_t N, typename Parser = CsvParser>
requires StreamingParser<Parser>
class EventMerger {
    static_assert(N > 0U, "EventMerger requires at least one stream");
    static_assert(N <= static_cast<std::size_t>(std::numeric_limits<AssetID>::max()) + 1U,
                  "EventMerger asset_id does not fit in AssetID");

public:
    using ParserArray = std::array<Parser, N>;

    EventMerger() : EventMerger(ParserArray {}) {}

    explicit EventMerger(ParserArray parsers)
        : parsers_(std::move(parsers)) {
        if constexpr (N > 4U) {
            InitializeHeap();
        }
    }

    [[nodiscard]] inline bool has_next() const noexcept {
        if constexpr (N <= 4U) {
            for (const Parser& parser : parsers_) {
                if (parser.has_next()) {
                    return true;
                }
            }
            return false;
        } else {
            return heap_size_ != 0U;
        }
    }

    [[nodiscard]] inline Event get_next() {
        if constexpr (N <= 4U) {
            return GetNextFastPath();
        } else {
            return next_dynamic();
        }
    }

private:
    struct HeapNode {
        std::uint64_t next_timestamp {};
        AssetID asset_id {};
    };

    struct HeapCompare {
        [[nodiscard]] inline bool operator()(const HeapNode& lhs, const HeapNode& rhs) const noexcept {
            if (lhs.next_timestamp != rhs.next_timestamp) {
                return lhs.next_timestamp > rhs.next_timestamp;
            }
            return lhs.asset_id > rhs.asset_id;
        }
    };

    inline void PushHeap(HeapNode node) noexcept {
        heap_[heap_size_++] = node;
        std::push_heap(heap_.begin(), heap_.begin() + static_cast<std::ptrdiff_t>(heap_size_), HeapCompare {});
    }

    inline void InitializeHeap() {
        for (std::size_t index = 0U; index < N; ++index) {
            if (parsers_[index].has_next()) [[likely]] {
                PushHeap(HeapNode {
                    .next_timestamp = parsers_[index].peek_time(),
                    .asset_id = static_cast<AssetID>(index),
                });
            }
        }
    }

    [[nodiscard]] inline Event GetNextFastPath() {
        std::uint64_t best_timestamp = std::numeric_limits<std::uint64_t>::max();
        std::size_t best_index = N;

        for (std::size_t index = 0U; index < N; ++index) {
            Parser& parser = parsers_[index];
            if (!parser.has_next()) [[unlikely]] {
                continue;
            }

            const std::uint64_t timestamp = parser.peek_time();
            if (timestamp < best_timestamp) {
                best_timestamp = timestamp;
                best_index = index;
            }
        }

        if (best_index == N) {
            throw std::out_of_range("EventMerger::get_next called with no remaining events");
        }

        Event event = parsers_[best_index].pop();
        event.asset_id = static_cast<AssetID>(best_index);
        return event;
    }

    [[nodiscard]] inline Event next_dynamic() {
        if (heap_size_ == 0U) [[unlikely]] {
            throw std::out_of_range("EventMerger::get_next called with no remaining events");
        }

        std::pop_heap(heap_.begin(), heap_.begin() + static_cast<std::ptrdiff_t>(heap_size_), HeapCompare {});
        const HeapNode node = heap_[--heap_size_];

        Parser& parser = parsers_[node.asset_id];
        Event event = parser.pop();
        event.asset_id = node.asset_id;

        if (parser.has_next()) [[likely]] {
            PushHeap(HeapNode {
                .next_timestamp = parser.peek_time(),
                .asset_id = node.asset_id,
            });
        }

        return event;
    }

    ParserArray parsers_ {};
    std::array<HeapNode, N> heap_ {};
    std::size_t heap_size_ {};
};

}  // namespace lob
