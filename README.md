# YABE: Yet Another Backtest Engine

YABE is a high-performance, low-latency, zero-allocation C++20 backtesting engine for limit-order-book (LOB) replay, high-frequency trading (HFT) strategy simulation, and graph-based multi-asset arbitrage research.

---

## Technical Highlights & Low-Latency Invariants

1. **Zero-Allocation Hot Path**: Memory is fully pre-allocated at startup. All runtime operations (event merging, order routing, cycle matching, and depth updates) utilize static buffers, object pools, or flat array indexes.
2. **Fixed-Point Scaling ($10^8$ ticks/lots)**: All price and quantity representations are normalized from double-precision floats to fixed-point integers:
   - `PriceTicks` (`std::int64_t`): Price scaled by $10^8$ (e.g., `price * 100_000_000`).
   - `QtyLots` (`std::uint64_t`): Quantity scaled by $10^8$ (e.g., `qty * 100_000_000`).
3. **Flat-Memory L3 Matching Book**: The Central Limit Order Book (`MatchingBook`) utilizes Robin Hood hashing with backward-shift deletion and an index-based free list to execute limit and cancel operations in strict $O(1)$ time without dynamic pointer chasing.
4. **Static L2 Depth**: Pre-allocated structures like `StaticVector` handle L2 order books without heap allocation, utilizing adaptive linear searches for size-16 lists and binary lookups for larger depths.
5. **Lazy Wallet Factor Updates**: Asset liquidation factors (translating inventory values into quote currency) are computed lazily, bypassing graph traversals on the L2 tick update path and maintaining throughputs above 7.5M EPS.

---

## Directory Structure

```text
include/lob/
  ├── sim/
  │   ├── matching_book.hpp        # High-performance L3 Robin Hood matching book
  │   ├── account_store.hpp        # Margin, locking, and balance ledgers
  │   ├── latency_model.hpp        # Latency delay generators (Fixed, Exponential, etc.)
  │   └── simulation_engine.hpp    # Multi-agent simulation loop and scheduler
  ├── graph_arbitrage_engine.hpp   # N-asset graph arb engine (dense and sparse)
  ├── l2_backtest_engine.hpp       # Single-asset L2 incremental update replayer
  ├── backtest_engine.hpp          # Single-asset trade and order replayer
  ├── fixed_matching_book.hpp      # Scale-checked wrapper for L3 simulation books
  ├── wallet.hpp                   # Integer-based wallet tracking asset exposure
  ├── l2_order_book.hpp            # StaticVector-based L2 depth tracker
  └── venue_replay.hpp             # Sequence checking and direct-address validation
src/
  ├── backtest_engine.cpp          # Execution engines and strategy loops
  ├── csv_parser.cpp               # Parsers for CSV format conversions
  ├── bindings.cpp                 # pybind11 Python bindings
  └── main_backtest.cpp            # Standalone CLI binary for L3 backtesting
tests/                             # GoogleTest suites (LOB and simulation coverage)
benchmarks/                        # Google Benchmark microbenchmarks
data/                              # Replay data directory (CSV files)
scripts/                           # Python scripts for data conversion and optimization
```

---

## Simulation Engines

### 1. GraphArbitrageEngine (Multi-Asset Arbitrage)
Replays continuous `L2UpdateEvent` feeds, updating a dynamic `L2OrderBook` for each asset pair. It maintains a stable directed graph topology and searches for profitable execution cycles using an optimized Bellman-Ford search.
- **`GraphEngine`**: Dense lookup policy optimized for triangular arbitrage (e.g. BTC-ETH-USDT).
- **`GraphEngineLarge`**: Sparse lookup policy designed for complex product graphs.

### 2. L2BacktestEngine (Single-Asset L2 Replayer)
Simulates passive (maker) and aggressive (taker) strategy fills against historical L2 depth updates. Features include:
- Queue position approximations and latency-delayed execution.
- Direct export of features (OBI, spread, NAV) into Python arrays.

### 3. BacktestEngine (Single-Asset L3 Engine)
Replays transaction events into a matching order book, routing trades to strategy instances (e.g., `InventorySkewStrategy`) and executing fills with integer arithmetic.

### 4. SimulationEngine (Agent-Based CLOB Simulator)
Enables closed-loop simulations where competing virtual agents (`MarketAgent`) place, modify, and cancel orders in a central matching engine (`MatchingBook`) under margin restrictions.

---

## Build Instructions

```bash
# Configure and build the Release build
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

# Run unit tests
ctest --test-dir build-release --output-on-failure

# Run microbenchmarks
./build-release/lob_benchmarks
```

### Python Bindings Build
To compile the Python module:
```bash
cmake --build build-release --target yabe
```
This produces `build-release/yabe.so` (or `yabe.pyd` on Windows) which can be loaded into Python scripts.

---

## Data Configuration

All historical csv logs must be placed inside the `data/` directory.

### Ingestion Format (L2 updates)
```csv
timestamp,is_snapshot,is_bid,price,qty
1709677440000000000,1,1,67200.5,1.2
1709677440000000000,1,0,67201.0,0.8
1709677440001000000,0,1,67200.6,0.5
```
*   `timestamp`: Nanoseconds.
*   `is_snapshot`: `1` for initial snapshots, `0` for incremental updates.
*   `qty`: Quantities of `0` denote price level deletion.

Conversions from Tardis formatted archives are supported via `scripts/tardis_to_l2update.py`.
