# YABE Central Limit Order Book & Matching Engine

YABE's matching engine is implemented inside the `lob::sim::MatchingBook` template class ([matching_book.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/sim/matching_book.hpp#L82)), which processes limit orders, market fills, and cancellations under strict $O(1)$ time constraints. 

---

## 1. Free List Slot Allocator

To prevent dynamic heap allocation on order insertions or cancellations, `MatchingBook` manages a flat memory model utilizing pre-allocated arrays and free list indexing.

### The Memory Layout
The book stores orders in a flat array of slots:
```cpp
std::array<OrderSlot, OrderCapacity> orders_;
```
Each slot is managed through static index pools:
* **`free_slots_`**: An array of size `OrderCapacity` storing the indices of inactive order slots.
* **`free_slots_ptr_`**: A stack pointer (index offset) representing the current free slot index.
* **Price Level Allocator**: Similarly, price levels are stored in a flat array `levels_` of size `PriceLevelCapacity`, with free levels managed via `free_level_indices_` and `free_level_indices_ptr_`.

### Allocation Mechanics
* **Insertion ($O(1)$)**: When a new order rests, YABE pops the next slot index from `free_slots_[free_slots_ptr_++]` and writes the order directly into `orders_[slot_index]`.
* **Removal ($O(1)$)**: When an order is filled or canceled, its slot index is pushed back onto the stack: `free_slots_[--free_slots_ptr_] = slot_index`.

---

## 2. Robin Hood Hash Index

To look up a resting order by its `OrderID` during cancellations or modifications in $O(1)$ time, `MatchingBook` implements a custom flat-memory Robin Hood hash map.

### The Hashing Structure
Instead of standard bucket-chaining (which incurs cache misses through pointer chasing), YABE uses open addressing inside a flat array:
```cpp
struct IndexEntry {
    OrderID order_id {};
    std::uint16_t slot_index {};
};
std::array<IndexEntry, OrderIndexCapacity> index_;
```

### Robin Hood Hash Invariants
* **Probe Sequence Length (PSL)**: Tracks how far an entry is from its ideal hash bucket position.
* **Robin Hood Rule**: On insertion collision, if the incoming entry has a larger PSL than the currently occupying entry, the occupying entry is evicted ("robbed") and the incoming entry is written. The evicted entry is then re-inserted further down the array. This dramatically narrows the variance of lookup times.
* **Backward-Shift Deletion**: When an order is removed from the index, YABE shifts subsequent entries backward until an entry with a PSL of 0 is encountered. This preserves the contiguous probe sequence, eliminates the need for "tombstone" placeholders, and maintains optimal steady-state $O(1)$ lookups.

---

## 3. Dual Search Policy for Price Levels

YABE maintains sorted price levels without dynamic node allocations. Depending on the size of `PriceLevelCapacity`, the engine adaptively switches its search policy:

```
IF PriceLevelCapacity <= 32 (Shallow Book):
    Utilize linear search and scan array (Cache-friendly, leverages SSE/prefetching)
ELSE (Deep Book):
    Maintain sorted index arrays (sorted_level_indices_) and utilize binary search
```

### Shallow Books (Size $\le 32$)
* Shallow books avoid the overhead of index sorting.
* Finding the best crossing price or locating a level is done via cache-friendly linear scans over the active levels array. Because the array is contiguous and small, CPU hardware prefetchers load the entire array into L1 Cache, outperforming binary searches.

### Deep Books (Size $> 32$)
* Deep books maintain sorted order arrays: `sorted_bid_level_indices_` and `sorted_ask_level_indices_`.
* Level lookups use binary searches (`std::lower_bound` / `std::upper_bound`).
* Insertions are sorted into the indices list, shifting subsequent level pointers.

---

## 4. FIFO Execution & Queue Matching

Doubly linked lists of slots inside the flat order array are used to represent queues of orders at each price level, maintaining FIFO priority:

```cpp
struct OrderSlot {
    SimOrder order {};
    std::uint64_t remaining_quantity_lots {};
    std::uint16_t next_slot {static_cast<std::uint16_t>(npos)};
    std::uint16_t prev_slot {static_cast<std::uint16_t>(npos)};
    std::uint16_t level_index {static_cast<std::uint16_t>(npos)};
};
```

Each price level contains entry points to the head and tail slots of its queue:
```cpp
struct PriceLevel {
    std::int64_t price_ticks {};
    std::uint64_t total_quantity_lots {};
    std::uint16_t head_slot {static_cast<std::uint16_t>(npos)};
    std::uint16_t tail_slot {static_cast<std::uint16_t>(npos)};
    bool active {false};
};
```

### Queue Matching Workflow

1. **Traversal**: An incoming order matches against the best crossing `PriceLevel`'s `head_slot`.
2. **Matching**: Matches are filled against the head order. If the head order is completely filled:
   * It is popped from the queue.
   * `head_slot` is updated to `orders_[head_slot].next_slot`.
   * The completed slot is returned to the free list.
3. **Queue Append**: If the incoming order cannot be completely filled, and is `TimeInForce::GTC`, it is appended to the tail of its target price level:
   * A new slot `slot_index` is allocated.
   * `orders_[tail_slot].next_slot = slot_index`.
   * `orders_[slot_index].prev_slot = tail_slot`.
   * `tail_slot = slot_index`.
