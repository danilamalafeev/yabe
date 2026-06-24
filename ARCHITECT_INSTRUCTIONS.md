# Architect Blueprint & Implementation Instructions

This document contains precise, step-by-step technical requirements, structural designs, and algorithms for the YABE (Yet Another Backtest Engine) low-latency C++20 refactoring. 

Use these blueprints to implement each phase. Do not add heap allocations, runtime polymorphism, or dynamic array expansions in hot paths.

---

## Global Scaling & Arithmetic Standards
All prices and quantities must be converted from `double` to fixed-point integers:
- **PriceTicks (`std::int64_t`)**: Scaled by $10^8$ (i.e. `price_double * 100'000'000`).
- **QtyLots (`std::uint64_t`)**: Scaled by $10^8$ (i.e. `qty_double * 100'000'000`).
- Always use `static_cast<std::int64_t>(std::llround(double_val * 100'000'000.0))` when converting from doubles.
- Conversion back to doubles: `double_val = static_cast<double>(int_val) / 100'000'000.0`.

---

## Phase 1: Struct Redesigns & Memory Alignment
Rearrange fields in the following structures to order by size (8 -> 4 -> 2 -> 1 bytes) to eliminate compiler padding, and apply `alignas` for cache performance.

### 1. `L2UpdateEvent` in [event_l2_update.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/event_l2_update.hpp)
- **Target Size**: Exactly 32 bytes (half a cache line).
- **Definition**:
```cpp
#pragma once
#include <cstdint>

namespace lob {
struct alignas(32) L2UpdateEvent {
    std::uint64_t timestamp_ns {};
    std::int64_t price {};       // PriceTicks
    std::uint64_t qty {};        // QtyLots
    bool is_snapshot {};
    bool is_bid {};
};
static_assert(sizeof(L2UpdateEvent) == 32);
} // namespace lob
```

### 2. `PolymarketL2Update` in [PolymarketFeedAdapter.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/PolymarketFeedAdapter.hpp)
- **Target Size**: Exactly 32 bytes.
- **Definition**:
```cpp
struct alignas(32) PolymarketL2Update {
    std::uint64_t timestamp_ns {};
    std::uint64_t new_size {};   // QtyLots
    MarketId market_id {};       // 4 bytes
    TokenId token_id {};         // 4 bytes
    Side side {Side::Buy};       // 1 byte
    PriceCents price_cents {};   // 1 byte
    bool is_snapshot {};         // 1 byte
};
static_assert(sizeof(PolymarketL2Update) == 32);
```

### 3. `FillEvent` in [order_gateway.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/order_gateway.hpp)
- **Target Size**: Exactly 40 bytes.
- **Definition**:
```cpp
struct alignas(8) FillEvent {
    std::uint64_t order_id {};
    std::int64_t price {};       // PriceTicks
    std::uint64_t quantity {};    // QtyLots
    std::uint64_t timestamp {};
    AssetID asset_id {};         // 2 bytes
    Side side {Side::Buy};       // 1 byte
    LiquidityRole liquidity_role {LiquidityRole::Maker}; // 1 byte
};
static_assert(sizeof(FillEvent) == 40);
```

### 4. `OrderRequest` and `OrderExecutionReport` in [oms.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/oms.hpp)
- **Target Size**: Exactly 64 bytes (fits one cache line).
- **Definitions**:
```cpp
struct alignas(64) OrderRequest {
    std::uint64_t quantity {};    // QtyLots
    std::uint64_t timestamp {};
    std::int64_t price {};       // PriceTicks
    std::int64_t expected_price {}; // PriceTicks
    double slippage_tolerance {}; // 8 bytes (percentage ratio, remains double)
    AssetID asset_id {};         // 2 bytes
    Side side {Side::Buy};       // 1 byte
};
static_assert(sizeof(OrderRequest) == 64);

struct alignas(64) OrderExecutionReport {
    std::int64_t expected_price {};  // PriceTicks
    std::int64_t vwap_price {};      // PriceTicks
    std::uint64_t requested_quantity {}; // QtyLots
    std::uint64_t filled_quantity {};    // QtyLots
    AssetID asset_id {};             // 2 bytes
    Side side {Side::Buy};           // 1 byte
    bool fully_filled {};            // 1 byte
    bool slippage_breached {};       // 1 byte
};
static_assert(sizeof(OrderExecutionReport) == 64);
```

*Note: Update parsing logic in `src/l2_update_csv_parser.cpp` to multiply the double fields from files by `100'000'000` to convert to ticks/lots.*

---

## Phase 2: UnifiedMatchingBook (L3 CLOB)
Rewrite `lob::sim::MatchingBook` in [matching_book.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/sim/matching_book.hpp):

1. **Free List Indexing**:
   - Declare `std::array<std::uint16_t, OrderCapacity> free_slots_;` and `std::uint16_t free_slots_ptr_ {0};`.
   - In constructor / `clear()`, populate `free_slots_[i] = i` for $i \in [0, \text{OrderCapacity}-1]$.
   - Allocation: `std::size_t slot = free_slots_[free_slots_ptr_++];`
   - Release: `free_slots_[--free_slots_ptr_] = slot;`
   - This keeps order insertions and cancellations strictly $O(1)$ and avoids scanning the full array.

2. **Dual Search Policy**:
   - Shallow books (`PriceLevelCapacity <= 32`): Use sequential linear scans when locating or sorting levels to optimize L1 cache and vectorization.
   - Deep books: Store levels in `std::array<PriceLevel, PriceLevelCapacity>`. Maintain an active levels index: `std::array<std::uint16_t, PriceLevelCapacity> sorted_level_indices_` and use `std::lower_bound` or binary search on indices to maintain $O(\log M)$ sorting.

3. **Robin Hood Hashing with Backward Shift Deletion**:
   - In `index_` array: `std::array<IndexEntry, OrderIndexCapacity>`, use `IndexEntry` with `OrderID order_id` and `std::uint16_t slot_index`.
   - Use Robin Hood insertion tracking Probe Sequence Length (PSL). If incoming PSL > current bucket PSL, swap them.
   - For deletion, shift elements at subsequent positions `(index + 1) % Capacity` backward by one slot until meeting a bucket with PSL == 0 or an empty slot. Do not use tombstone states (`Deleted`).

---

## Phase 3: Static L2OrderBook
Rewrite `lob::L2OrderBook` in [l2_order_book.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/l2_order_book.hpp) to avoid heap allocation.

1. **StaticVector**:
   - Define a templated structure `StaticVector<T, Capacity>`:
   ```cpp
   template <typename T, std::size_t Capacity>
   class StaticVector {
   private:
       std::array<T, Capacity> data_ {};
       std::size_t size_ {0U};
   public:
       // Implement: size(), empty(), begin(), end(), push_back(), pop_back(), back(), front(), insert(), erase()
   };
   ```
   - Replace `std::vector<Level> bids_` and `asks_` with `StaticVector<Level, 128U>`.

2. **Adaptive Search**:
   - For `size() <= 16`: Use a direct linear scan (`for` loop) to find price levels instead of `std::lower_bound`.

---

## Phase 4: CRTP OrderGateway & Indirection Heap

1. **CRTP OrderGateway**:
   - Convert `OrderGateway` into a template base class in `include/lob/order_gateway.hpp`.
   - Update `Strategy` and `L2Strategy` to accept `Gateway&` templates:
   ```cpp
   template <typename Gateway>
   class Strategy {
   public:
       virtual void on_tick(AssetID asset_id, const OrderBook& book, Gateway& gateway) = 0;
   };
   ```
   - Inheriting backtest engines pass themselves as template types (e.g. `class L2BacktestEngine : public OrderGateway<L2BacktestEngine>`).

2. **Indirection heap for `PendingOrderMinHeap`**:
   - In `PendingOrderMinHeap`:
   ```cpp
   template <std::size_t Capacity>
   class PendingOrderMinHeap {
   private:
       std::array<PendingOrder, Capacity> orders_ {};
       std::array<std::uint16_t, Capacity> heap_indices_ {};
       std::size_t size_ {0};
   public:
       // Balance indices inside heap_indices_ during push/pop based on orders_[heap_indices_[i]].release_time_ns.
       // This prevents swapping 56-byte structures, replacing it with swapping 2-byte indices.
   };
   ```

3. **EventMerger Array Heap**:
   - In [event_merger.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/event_merger.hpp):
   - Replace `std::priority_queue` with `std::array<HeapNode, N>` and manage with `std::push_heap` / `std::pop_heap`.

4. **Direct Addressing Validator**:
   - In `ReplaySequenceValidator` (`include/lob/venue_replay.hpp`):
   - Replace `std::unordered_map` with `std::array<ProductState, 4096U> product_states_` using mapping: `index = venue_id * 256 + product_id`.

---

## Phase 5: Wallet & Legacy Deprecation

1. **Fixed-Point Wallet**:
   - Replace `std::vector<double>` in `DynamicWallet` with `std::array<std::int64_t, MaxAssets>` for balances.
   - Cache liquidation factors to quote currency: update factors upon edge/price changes and compute MTM NAV and inventory risk via single element lookups and multiplications ($O(1)$) instead of graph traversals.

2. **Legacy Deletions**:
   - Remove `lob::OrderBook` and `lob::Wallet`. Rename `DynamicWallet` to `Wallet`.
   - Update `bindings.cpp` and `main_backtest.cpp` to use the refactored, templated, fixed-point engine types.
