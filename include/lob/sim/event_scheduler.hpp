#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "lob/sim/types.hpp"

namespace lob::sim {

template <std::size_t Capacity>
class EventScheduler {
public:
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0U;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Capacity;
    }

    [[nodiscard]] constexpr std::size_t available_capacity() const noexcept {
        return Capacity - size_;
    }

    [[nodiscard]] bool has_event_for_market(MarketID market_id) const noexcept {
        for (std::size_t i = 0U; i < size_; ++i) {
            if (heap_[i].market_id == market_id) {
                return true;
            }
        }
        return false;
    }

    void clear() noexcept {
        size_ = 0U;
        next_sequence_number_ = 0U;
    }

    [[nodiscard]] const ScheduledEvent& top() const noexcept {
        assert(!empty());
        return heap_[0U];
    }

    [[nodiscard]] bool push(ScheduledEvent event) noexcept {
        if (size_ == Capacity) {
            return false;
        }

        event.sequence_number = next_sequence_number_++;
        heap_[size_] = event;
        sift_up(size_);
        ++size_;
        return true;
    }

    [[nodiscard]] bool pop(ScheduledEvent& event) noexcept {
        if (empty()) {
            return false;
        }

        event = heap_[0U];
        --size_;
        if (size_ > 0U) {
            heap_[0U] = heap_[size_];
            sift_down(0U);
        }
        return true;
    }

    [[nodiscard]] bool pop() noexcept {
        ScheduledEvent ignored {};
        return pop(ignored);
    }

private:
    [[nodiscard]] static constexpr bool comes_before(const ScheduledEvent& lhs,
                                                     const ScheduledEvent& rhs) noexcept {
        if (lhs.timestamp_ns != rhs.timestamp_ns) {
            return lhs.timestamp_ns < rhs.timestamp_ns;
        }

        const auto lhs_phase = static_cast<std::uint8_t>(lhs.phase);
        const auto rhs_phase = static_cast<std::uint8_t>(rhs.phase);
        if (lhs_phase != rhs_phase) {
            return lhs_phase < rhs_phase;
        }

        return lhs.sequence_number < rhs.sequence_number;
    }

    void sift_up(std::size_t index) noexcept {
        while (index > 0U) {
            const std::size_t parent = (index - 1U) / 2U;
            if (!comes_before(heap_[index], heap_[parent])) {
                break;
            }

            std::swap(heap_[index], heap_[parent]);
            index = parent;
        }
    }

    void sift_down(std::size_t index) noexcept {
        while (true) {
            const std::size_t left = (index * 2U) + 1U;
            const std::size_t right = left + 1U;
            std::size_t smallest = index;

            if (left < size_ && comes_before(heap_[left], heap_[smallest])) {
                smallest = left;
            }
            if (right < size_ && comes_before(heap_[right], heap_[smallest])) {
                smallest = right;
            }
            if (smallest == index) {
                break;
            }

            std::swap(heap_[index], heap_[smallest]);
            index = smallest;
        }
    }

    std::array<ScheduledEvent, Capacity> heap_ {};
    std::size_t size_ {};
    EventSequence next_sequence_number_ {};
};

}  // namespace lob::sim
