# YABE Backtesting Engines & Graph Arbitrage

YABE provides a series of high-performance replayers and arbitrage engines to test trading strategies against historical datasets.

---

## 1. Single-Asset L3 Backtester (`BacktestEngine`)

The L3 backtester ([backtest_engine.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/backtest_engine.hpp)) is designed to replay raw transaction/trade feeds.
* **Order Placement**: Simulates detailed strategy logic (e.g. `InventorySkewStrategy` which uses inventory-based bid/ask skewing to manage inventory risk) by placing resting limit orders and executing taker fills.
* **Integer Match**: Matches all executions utilizing safe integer arithmetic (`PriceTicks` and `QtyLots`).
* **OMS Emulation**: Realistically models latency delay loops, order cancel gates, and slippage rejections.

---

## 2. Single-Asset L2 Backtester (`L2BacktestEngine`)

The L2 backtester ([l2_backtest_engine.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/l2_backtest_engine.hpp)) replays incremental L2 order book updates.

### Queue Position & Fill Estimation
* **Queue Placement**: When a passive limit order is placed, `L2BacktestEngine` records its queue position by caching the cumulative visible volume ahead of it at that price level.
* **Queue Depletion**: The replayer tracks incoming L2 trade updates at that price. The volume of trades depletes the queue ahead of the strategy's order.
* **Taker Sweeps**: Once the volume ahead is reduced to zero, subsequent crossing L2 updates trigger execution fills.
* **Aggressive Fills**: Aggressive (taker) orders are filled immediately against the resting depth up to `max_book_levels_per_side`.

---

## 3. Multi-Asset Graph Arbitrage Engine

The `GraphArbitrageEngineT` ([graph_arbitrage_engine.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/graph_arbitrage_engine.hpp#L25)) merges multiple asset L2 updates by timestamp and represents the exchange as a directed graph:
* **Nodes**: Represent assets (e.g., USDT, BTC, ETH).
* **Edges**: Represent active order books (e.g., BTC/USDT, ETH/USDT, ETH/BTC). Edge weight is determined by the asset conversion exchange rate, and edge capacity is the bottleneck depth.

### Compile-Time Lookup Policies
YABE optimizes edge mapping using two policies defined in [lookup_policy.hpp](file:///Users/danilamalafeev/Documents/YABE/include/lob/lookup_policy.hpp):
1. **`DenseLookupPolicy` (`GraphEngine`)**:
   * Uses flat pre-allocated lookup matrices ($N \times N$) where asset IDs serve directly as row and column offsets.
   * Edge access is a cache-local $O(1)$ array lookup. Optimized for small dense networks (e.g. triangular arbitrage).
2. **`SparseLookupPolicy` (`GraphEngineLarge`)**:
   * Stores adjacencies in packed flat arrays.
   * Ideal for large sparse graphs (e.g. crossing hundreds of products).

---

## 4. Multiplicative Cycle Detection (Bellman-Ford / SPFA)

Arbitrage cycles are multiplicative: starting with 1 unit of USDT, the cycle is profitable if the product of rates exceeds 1.0:

$$\prod_{i=1}^k \text{Rate}_{\text{edge}_i} > 1.0$$

### Logarithmic Additive Transformation
To detect cycles using standard shortest-path algorithms, YABE transforms rates into additive edge costs:

$$\text{Cost} = -\ln(\text{Rate}_{\text{edge}})$$

Summing costs along a path corresponds to multiplying rates. Profitability translates to a negative sum:

$$\sum_{i=1}^k -\ln(\text{Rate}_{\text{edge}_i}) < 0$$

Searching for negative cycles is executed using an optimized **SPFA (Shortest Path Faster Algorithm)** or **Bellman-Ford** check. If a negative cycle is found, it represents an arbitrage loop starting and ending in the quote currency (`USDT`).

---

## 5. Smart Order Routing (SOR) & Sizing Filters

Before executing a detected cycle, YABE passes the route through a series of low-latency execution filters:
* **Adverse OBI (Order Book Imbalance) Filter**: Rejects cycles when the bid-ask volume imbalance is unfavorable (e.g., buying into strong selling pressure).
* **Spread Bps Filter**: Blocks execution on assets experiencing wide spreads.
* **Fee-Aware Sizing**: Taker fees are deducted from the received asset leg-by-leg. YABE sizes the orders to ensure each leg matches the post-fee received amount of the previous leg:
  
  $$\text{Qty}_{\text{leg}_{n+1}} = \text{Qty}_{\text{leg}_n} \times \text{Rate}_n \times (1.0 - \text{TakerFee})$$
  
  This eliminates dust accumulation on intermediate legs.
* **Pessimistic Panic Close**: If an order group leg fails to execute, the OMS triggers a panic reverse sweep to close outstanding exposure. The sizing accounts for a penalty factor if the required size exceeds top-of-book visible depth.

---

## 6. Hot-Path Optimization: Lazy Wallet Updates

Determining the net asset value (NAV) of a multi-asset portfolio requires translating all holdings back into the quote currency (e.g., USDT). In early designs, this was done on every book update, triggering multiple graph traversals.

YABE achieves massive throughput improvements by implementing **Lazy Wallet Updates** (Phase 6):
1. Tick updates modify L2 books but *never* traverse the graph to update asset values.
2. Asset values and liquidation factors are marked stale.
3. Liquidation factors are calculated lazily, traversing the graph *only* when:
   * A trade execution begins.
   * A panic close is triggered.
   * The final simulation report is generated.

This isolates the hot parsing/update path from graph search overhead, keeping throughput above **7.5M EPS**.
