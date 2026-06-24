#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace lob {

template <typename T, std::size_t Capacity>
class IndexedPendingMinHeap {
    static_assert(Capacity > 0U);
    static_assert(Capacity <= static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U);

public:
    IndexedPendingMinHeap() noexcept {
        reset_free_slots();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0U;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] const T& top() const noexcept {
        return storage_[heap_indices_[0]];
    }

    void clear() noexcept {
        size_ = 0U;
        reset_free_slots();
    }

    bool push(const T& item) noexcept {
        if (free_size_ == 0U) {
            return false;
        }

        const std::uint16_t storage_index = free_indices_[--free_size_];
        storage_[storage_index] = item;

        std::size_t heap_index = size_++;
        heap_indices_[heap_index] = storage_index;
        while (heap_index > 0U) {
            const std::size_t parent = (heap_index - 1U) / 2U;
            if (!less(heap_indices_[heap_index], heap_indices_[parent])) {
                break;
            }
            std::swap(heap_indices_[parent], heap_indices_[heap_index]);
            heap_index = parent;
        }
        return true;
    }

    void pop() noexcept {
        if (size_ == 0U) {
            return;
        }

        const std::uint16_t released_storage_index = heap_indices_[0];
        --size_;
        if (size_ != 0U) {
            heap_indices_[0] = heap_indices_[size_];
            sift_down(0U);
        }
        free_indices_[free_size_++] = released_storage_index;
    }

private:
    [[nodiscard]] bool less(std::uint16_t lhs, std::uint16_t rhs) const noexcept {
        return storage_[lhs].release_time_ns < storage_[rhs].release_time_ns;
    }

    void sift_down(std::size_t heap_index) noexcept {
        while (true) {
            const std::size_t left = heap_index * 2U + 1U;
            if (left >= size_) {
                return;
            }

            const std::size_t right = left + 1U;
            std::size_t smallest = left;
            if (right < size_ && less(heap_indices_[right], heap_indices_[left])) {
                smallest = right;
            }
            if (!less(heap_indices_[smallest], heap_indices_[heap_index])) {
                return;
            }

            std::swap(heap_indices_[heap_index], heap_indices_[smallest]);
            heap_index = smallest;
        }
    }

    void reset_free_slots() noexcept {
        free_size_ = Capacity;
        for (std::size_t index = 0U; index < Capacity; ++index) {
            free_indices_[index] = static_cast<std::uint16_t>(Capacity - 1U - index);
        }
    }

    std::array<T, Capacity> storage_ {};
    std::array<std::uint16_t, Capacity> heap_indices_ {};
    std::array<std::uint16_t, Capacity> free_indices_ {};
    std::size_t size_ {};
    std::size_t free_size_ {};
};

}  // namespace lob
