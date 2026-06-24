# YABE (Yet Another Backtest Engine) - LLM Context Guide

Welcome to the YABE codebase. This document is written specifically to onboard LLM agents and human developers quickly. It provides a technical blueprint of the architecture, algorithmic invariants, and memory constraints that define this project.

## 1. Core Philosophy

YABE is a production-grade, low-latency, multi-asset quantitative backtesting and simulation framework built in **C++20**. Its primary goal is to simulate high-frequency (HFT) triangular and N-way arbitrage with absolute mathematical determinism, and to support closed-loop agent-based limit order book simulations.

**Strict Invariants:**
1. **Zero-Allocation Hot Path:** Heap allocations (`new`, `std::vector::push_back`, `std::make_shared`) are strictly forbidden during the main event loops (both backtesting and agent-based simulation). All memory is pre-allocated via `reserve()` or fixed-size structures (e.g., Ring Buffers, template-bounded arrays) during initialization.
2. **Zero-Copy Ingestion:** Market data is ingested via POSIX `mmap` or highly-optimized stream parsers. Parsers return references to stack-allocated or pre-allocated events to avoid copy overhead.
3. **Deterministic State Machine:** Wall-clock time does not exist. The engines are event-driven simulators strictly governed by the chronological processing of nanosecond timestamps.

---

## 2. Platform Architecture & Simulation Engines

The codebase is organized around three primary simulation architectures:

### A. `GraphArbitrageEngine` (State-of-the-Art Multi-Asset Arbitrage)
Located in `include/lob/graph_arbitrage_engine.hpp`.
- **Dynamic L2 Book Ingestion:** Replays continuous `L2UpdateEvent` sequences into a capped dynamic `L2OrderBook` rather than relying on fixed L2 depth snapshots.
- **Lookup Policies:** Designed as a templated class `GraphArbitrageEngineT<LookupPolicy>`. Supports dense (default `GraphEngine` for Binance-style triangular arbitrage) and sparse (`GraphEngineLarge` for large product graphs with sparse connectivity) topologies.
- **Stable Edge Topology:** Directed graph edges are constructed once at startup; pricing, rate, and capacity relaxations are updated in-place to bypass costly graph reconstruction in the hot loop.

### B. `L2BacktestEngine` (Single-Asset High-Frequency Backtester)
Located in `include/lob/l2_backtest_engine.hpp`.
- **Target:** Testing latency-delayed high-frequency strategies (e.g., `L2MarketMakerStrategy`).
- **Feature Generation:** Exports high-quality real-time OBI (Order Book Imbalance), spread, and NAV snapshot tables directly to Python arrays.
- **Latency Delay:** Implements a binary min-heap (`PendingOrderMinHeap`) to schedule limit and cancel actions, executing orders against local depth at their exact release time.

### C. `SimulationEngine` (Agent-Based L2 Simulation / Matching Engine)
Located in `include/lob/sim/simulation_engine.hpp`.
- **Closed-Loop Matching:** Integrates multiple competing virtual agents (`MarketAgent`) interacting with a high-performance central limit order book (`MatchingBook`).
- **Phase-Driven Scheduler:** Events (`ScheduledEvent`) are partitioned into four distinct chronological phases per time step:
  - `PreMarket`: Market-data feeds and external venue snapshots.
  - `Agent`: Wakeups and routing actions.
  - `Market`: Internal order matching and cancellation.
  - `PostMarket`: Public market data distribution.

---

## 3. Algorithmic Deep Dive

### Graph Arbitrage & Cycle Detection (SPFA)
- **Multiplicative Rate Accumulator:** We do NOT use log-scale additive weights due to precision degradation. Instead, we propagate rates multiplicatively starting from $1.0$. Relaxation maximizes: `rates_[edge.from] * edge.effective_rate`.
- **Cycle Condition:** A negative cycle is detected if the product of rates relaxes back to a value greater than $1.0$ at the starting asset, bypassing floating-point errors.
- **Depletion Ledger:** Tracks simulated liquidity exhaustion on L2 books between actual market updates, preventing the engine from virtual double-filling.

### Sim Order Book Matching (`MatchingBook`)
Located in `include/lob/sim/matching_book.hpp`.
- **Structure:** Maintains pre-allocated arrays of `OrderSlot` and `PriceLevel` structs. Indexing utilizes a fast, open-addressed hash map mapping `OrderID -> slot_index` to completely avoid dynamic pointer structures.
- **Order Execution Invariants:** Full compliance with Time-In-Force semantics:
  - `GTC` (Good 'Til Canceled): Rests unfilled volume in the book.
  - `IOC` (Immediate Or Cancel): Fills as much as possible immediately and cancels residual.
  - `FOK` (Fill Or Kill): Performs a pre-flight execution check. If the book cannot fill the order in its entirety immediately, or if the pre-allocated execution report capacity is insufficient, the order is rejected without modifying the book.

### Balance Ledger (`AccountStore`) & Risk
Located in `include/lob/sim/account_store.hpp`.
- Enforces strict capital constraints. Before dispatching or resting limit orders, the engine calls `reserve()` to lock the margin/collateral.
- Upon matching, `consume_reserved()` converts reserved collateral into asset position.
- If cancelled or rejected, `release()` unlocks the reserved balance.
- Total asset quantities and balances must satisfy the `accounting_invariants_hold()` check, halting the simulation immediately if any double-spending or leak is detected.

---

## 4. Latency Modeling

Located in `include/lob/sim/latency_model.hpp`.
- High-frequency order routing and public data feed delivery are governed by a deterministic `LatencyModel`.
- Supports four probability distributions: `Fixed`, `Uniform`, `Exponential`, and `LogNormal`.
- Leverages a seed-based pseudo-random number generator (PRNG) to ensure that despite stochastic latency paths, the entire multi-agent simulation remains bit-perfect and reproducible across runs.

---

## 5. Python Interop (`pybind11`)

The `src/bindings.cpp` file provides the `yabe` Python module:
- `PyGraphEngine`, `L2BacktestEngine`, and custom data structures are exposed to Python.
- Multi-threaded C++ runs release the Python **GIL** (`py::gil_scoped_release`) to avoid GC hiccups during the simulation loops.
- Core features and backtest summaries are converted into column-oriented dictionaries mapping directly to NumPy arrays for zero-copy ingestion into Pandas dataframes.

---

## 6. Next Steps & Ongoing Roadmap

For developers and agent systems, the active work is divided into:

### A. Core Engine Enhancements
- **Venue-Normalized Replay Log:** Create an immutable replay file format around current primitives (venue/product IDs, sequence validations, ticks/lots).
- **Integer Tick/Lot Book Path:** Migrate all price/quantity calculations in the book and graph engines to scaled integer ticks and lot arithmetic to align with Hyperliquid and Polymarket design.
- **Typed Edge Cost Models:** Replace simple global fee multipliers with complex per-edge models covering fixed gas, slippage, and multi-currency collateral fees.
- **Typed Executable Edges:** Add taker order book edges, settlement transforms, and outcome bundles.

### B. Research & Integration
- **Hyperliquid Research:** Construct a high-performance converter translating Hyperliquid L2 update streams into the normalized replay format.
- **Polymarket CLOB Arbitrage:** Model settlement bounds, outcome token pairs (YES/NO), complement bundle splits, and fee dynamics.
- **ML/RL Feature Export:** Expand feature tables with microprice, order book imbalance bands, cycle edge bps, and latency outcome labels.
