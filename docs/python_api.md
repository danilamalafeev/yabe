# YABE Python API & Bindings Reference

YABE compiles into a high-performance Python module (`yabe`) using `pybind11`. This allows researchers to configure engines in Python, run simulations at native C++ speeds, and export raw data directly into NumPy arrays and Pandas DataFrames.

---

## 1. Compilation & Import

To build the Python bindings, configure the project in Release mode and run the build target:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target yabe
```

This compiles a shared library:
* **Linux / macOS**: `build-release/yabe.so`
* **Windows**: `build-release/yabe.pyd`

To load the module, add the build directory to your python search path:

```python
import sys
from pathlib import Path

# Adjust path to your build output
sys.path.insert(0, str(Path("./build-release").resolve()))
import yabe
```

---

## 2. API Reference: Backtesting & Graph Engines

### 2.1 `yabe.L2MarketMakerBacktest`

Runs a single-asset L2 order book replay strategy.

* **Constructor**:
  ```python
  backtest = yabe.L2MarketMakerBacktest(
      initial_cash=100_000_000.0,
      maker_fee_bps=0.0,
      taker_fee_bps=7.5,
      latency_ns=500_000,
      max_book_levels_per_side=20,
      quantity_scale=100_000_000.0,
      quote_offset=0.5,
      quote_quantity=1_000_000,
      refresh_interval_ns=1_000_000_000,
      record_features=False,
      feature_sample_interval_ns=0,
      feature_reserve=100_000
  )
  ```
* **Method**:
  * `run(file_path: str) -> L2BacktestResult`: Executes the backtest on the specified L2 update CSV file.

---

### 2.2 `yabe.GraphEngine` & `yabe.GraphEngineLarge`

Runs multi-asset arbitrage simulations. `GraphEngine` uses a dense lookup policy ($O(1)$ edge access), while `GraphEngineLarge` is optimized for sparse graphs.

* **Constructor**:
  ```python
  engine = yabe.GraphEngine(  # Or yabe.GraphEngineLarge
      initial_usdt=100_000_000.0,
      latency_ns=500_000,
      intra_leg_latency_ns=75,
      taker_fee_bps=7.5,
      max_cycle_notional_usdt=1_000.0,
      max_adverse_obi=1.0,
      max_spread_bps=1_000.0,
      min_depth_usdt=0.0,
      min_cycle_edge_bps=0.0,
      cycle_snapshot_reserve=100_000,
      quote_asset="USDT",
      max_book_levels_per_side=100
  )
  ```
* **Methods**:
  * `add_pair(base: str, quote: str, csv_path: str) -> None`: Registers an asset pair and feeds its historical update CSV path.
  * `run() -> GraphResult`: Runs the simulation loop.

---

### 2.3 `yabe.TriangularEngine`

Runs the legacy multi-asset triangular trade-replay engine.

* **Constructor**:
  ```python
  engine = yabe.TriangularEngine(
      latency_ns=500_000,
      maker_fee_bps=-1.0,
      taker_fee_bps=7.5,
      verbose=False
  )
  ```
* **Methods**:
  * `run(csv_paths: list[str]) -> BacktestResult`: Runs the triangular backtest.
  * `enable_microstructure_recording(enable: bool, sampling_interval_ns: int) -> None`
  * `get_microstructure_dataframe() -> dict`

---

## 3. API Reference: Result Objects

### 3.1 `L2BacktestResult`

Exposes stats from `L2MarketMakerBacktest.run()`.

| Property | Return Type | Description |
| :--- | :--- | :--- |
| `events_processed` | `int` | Total L2 update rows processed. |
| `market_batches_processed` | `int` | Batched event updates processed. |
| `orders_submitted` | `int` | Count of orders placed by the strategy. |
| `orders_canceled` | `int` | Count of canceled orders. |
| `final_cash` | `float` | Cash balance at completion. |
| `final_position` | `int` | Asset holdings (scaled by $10^8$). |
| `final_nav` | `float` | Net Asset Value in cash currency. |
| `pnl` | `float` | Net realized PnL. |
| `maker_fills_count` | `int` | Count of maker fills. |
| `taker_fills_count` | `int` | Count of taker fills. |
| `equity_curve` | `list[float]` | Cumulative NAV values sampled over time. |

* **Method**:
  * `get_features_dataframe() -> dict[str, numpy.array]`: Returns a structured dictionary of columns suitable for machine learning.

---

### 3.2 `GraphResult`

Exposes stats from the graph arbitrage engine execution.

| Property | Return Type | Description |
| :--- | :--- | :--- |
| `events_processed` | `int` | Total updates replayed. |
| `cycles_detected` | `int` | Number of cycles found. |
| `attempted_cycles` | `int` | Number of cycles sent to execution queue. |
| `completed_cycles` | `int` | Successful arbitrage loops completed. |
| `panic_closes` | `int` | Cycles requiring emergency liquidations. |
| `final_usdt` | `float` | Final liquid USDT balance. |
| `final_nav` | `float` | Mark-to-market total portfolio value. |
| `inventory_risk` | `float` | Sum of non-quote exposure values in USDT. |
| `balances` | `list[float]` | Balances corresponding to assets. |
| `last_cycle` | `list[int]` | Sequence of asset IDs in the last cycle. |

---

## 4. Pandas & NumPy Integration

The methods `get_features_dataframe()` and `get_microstructure_dataframe()` return standard C++ containers converted into flat NumPy arrays via `pybind11` buffer protocol. This avoids copying overhead and enables instant loading into Pandas:

```python
import pandas as pd
import yabe

backtest = yabe.L2MarketMakerBacktest(record_features=True, feature_sample_interval_ns=1_000_000_000)
result = backtest.run("data/BTCUSDT-l2-2024-03-01.csv")

# Export features directly to Pandas
features_dict = result.get_features_dataframe()
df = pd.DataFrame(features_dict)

# Print metrics
print(df.head())
print(f"Final NAV: {result.final_nav}")
```

---

## 5. Threaded Parameter Sweeps

All C++ execution loops release the Python **Global Interpreter Lock (GIL)** (`py::gil_scoped_release`) before running. This allows you to launch multiple backtests in parallel using standard Python threading, utilizing all CPU cores:

```python
import concurrent.futures
import yabe

configs = [0.1, 0.2, 0.3, 0.5, 0.8]

def run_sweep(offset):
    backtest = yabe.L2MarketMakerBacktest(quote_offset=offset)
    res = backtest.run("data/BTCUSDT-l2-2024-03-01.csv")
    return offset, res.final_nav

# Run parallel simulations across multiple CPU cores
with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
    results = list(executor.map(run_sweep, configs))

for offset, nav in results:
    print(f"Offset: {offset} -> Final NAV: {nav}")
```
