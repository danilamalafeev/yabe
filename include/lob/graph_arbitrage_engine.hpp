#pragma once

#include <cmath>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/event.hpp"
#include "lob/wallet.hpp"
#include "lob/l2_order_book.hpp"
#include "lob/l2_update_csv_parser.hpp"
#include "lob/lookup_policy.hpp"

namespace lob {

template <typename LookupPolicy>
class GraphArbitrageEngineT {
public:
    static constexpr std::size_t kMaxGraphLegs = 16U;

    struct Config {
        double initial_usdt {100'000'000.0};
        std::string quote_asset {"USDT"};
        std::uint64_t latency_ns {0U};
        std::uint64_t intra_leg_latency_ns {75U};
        double taker_fee_bps {0.0};
        double max_cycle_notional_usdt {1'000.0};
        double max_adverse_obi {1.0};
        double max_spread_bps {1'000.0};
        double min_depth_usdt {0.0};
        double min_cycle_edge_bps {0.0};
        std::size_t cycle_snapshot_reserve {100'000U};
        std::size_t max_book_levels_per_side {100U};
    };

    struct PairConfig {
        std::string base_asset {};
        std::string quote_asset {};
        std::filesystem::path csv_file_path {};
    };

    struct PairState {
        AssetID base_id {};
        AssetID quote_id {};
        L2OrderBook book {};
        double obi {};
        double micro_price {};
        double spread_bps {};
    };

    struct CycleSnapshot {
        std::uint64_t timestamp_ns {};
        std::uint8_t leg_count {};
        std::array<AssetID, kMaxGraphLegs + 1U> cycle_path {};
        std::array<double, kMaxGraphLegs> leg_obis {};
        std::array<double, kMaxGraphLegs> leg_spreads_bps {};
        bool executed {};
    };

    struct Result {
        std::uint64_t events_processed {};
        std::uint64_t cycles_detected {};
        std::uint64_t attempted_cycles {};
        std::uint64_t completed_cycles {};
        std::uint64_t panic_closes {};
        std::uint64_t market_batches_processed {};
        std::uint64_t cycle_searches {};
        double initial_usdt {100'000'000.0};
        double final_usdt {100'000'000.0};
        double final_nav {100'000'000.0};
        double inventory_risk {};
        std::vector<double> balances {};
        std::vector<AssetID> last_cycle {};
        std::vector<CycleSnapshot> cycle_snapshots {};
        std::uint64_t cycle_snapshots_overwritten {};
    };

    GraphArbitrageEngineT()
        : config_(Config {}) {}

    explicit GraphArbitrageEngineT(Config config)
        : config_(config) {}

    GraphArbitrageEngineT(
        double initial_usdt,
        std::uint64_t latency_ns,
        std::uint64_t intra_leg_latency_ns,
        double taker_fee_bps,
        double max_cycle_notional_usdt
    )
        : config_(Config {
              .initial_usdt = initial_usdt,
              .latency_ns = latency_ns,
              .intra_leg_latency_ns = intra_leg_latency_ns,
              .taker_fee_bps = taker_fee_bps,
              .max_cycle_notional_usdt = max_cycle_notional_usdt,
          }) {}

    AssetID add_pair(std::string base_asset, std::string quote_asset, std::filesystem::path csv_file_path) {
        if (running_) {
            throw std::runtime_error("Cannot add pairs while GraphArbitrageEngine is running");
        }

        const AssetID base_id = asset_id_for(std::move(base_asset));
        const AssetID quote_id = asset_id_for(std::move(quote_asset));
        const AssetID pair_id = static_cast<AssetID>(pairs_.size());
        pairs_.push_back(PairConfig {
            .base_asset = asset_names_[base_id],
            .quote_asset = asset_names_[quote_id],
            .csv_file_path = std::move(csv_file_path),
        });
        pair_states_.push_back(PairState {
            .base_id = base_id,
            .quote_id = quote_id,
            .book = L2OrderBook {config_.max_book_levels_per_side},
        });
        return pair_id;
    }

    [[nodiscard]] Result run() {
        prepare_runtime();
        Result result {};
        result.initial_usdt = config_.initial_usdt;

        while (has_next_event() || has_pending_cycle()) {
            if (should_process_pending_cycle()) {
                process_next_pending_cycle(result);
                continue;
            }

            process_next_market_event(result);
        }

        refresh_all_edges();
        refresh_wallet_factors();
        result.final_usdt = wallet_balance_units(quote_asset_id_);
        result.final_nav = Wallet::to_double(wallet_.mark_to_market_nav());
        result.inventory_risk = Wallet::to_double(wallet_.get_total_inventory_risk());
        result.balances.reserve(wallet_.active_asset_count());
        for (std::size_t index = 0U; index < wallet_.active_asset_count(); ++index) {
            result.balances.push_back(Wallet::to_double(wallet_.balance(static_cast<AssetID>(index))));
        }
        result.last_cycle = cycle_;
        result.cycle_snapshots = cycle_snapshots_.to_vector();
        result.cycle_snapshots_overwritten = cycle_snapshots_.overwritten_count();
        running_ = false;
        return result;
    }

    [[nodiscard]] const std::vector<std::string>& assets() const noexcept {
        return asset_names_;
    }

private:
    static constexpr std::size_t kPendingCycleCapacity = 16'384U;
    static constexpr double kEpsilon = 1e-12;
    static constexpr double kWalletQuantum = 1e-8;

    struct Edge {
        AssetID from {};
        AssetID to {};
        AssetID pair_id {};
        bool use_bid {};
        double rate {};
        double available_from_qty {};
        double effective_rate {};
    };

    struct PendingCycle {
        std::uint64_t release_time_ns {};
        std::uint8_t leg_count {};
        std::uint8_t current_leg {};
        double current_quantity {};
        AssetID current_asset {};
        double initial_quantity {};
        std::array<AssetID, kMaxGraphLegs + 1U> assets {};
        std::array<double, kMaxGraphLegs> expected_rates {};
        std::array<double, kMaxGraphLegs> leg_outputs {};
    };

    struct ParserEvent {
        std::uint64_t release_time_ns {};
        AssetID pair_id {};
    };

    struct ParserEventGreater {
        [[nodiscard]] bool operator()(const ParserEvent& lhs, const ParserEvent& rhs) const noexcept {
            if (lhs.release_time_ns != rhs.release_time_ns) {
                return lhs.release_time_ns > rhs.release_time_ns;
            }
            return lhs.pair_id > rhs.pair_id;
        }
    };

    class CycleSnapshotRing {
    public:
        void reset(std::size_t capacity) {
            storage_.assign(capacity, CycleSnapshot {});
            next_index_ = 0U;
            count_ = 0U;
            overwritten_count_ = 0U;
        }

        void push(const CycleSnapshot& snapshot) noexcept {
            if (storage_.empty()) {
                ++overwritten_count_;
                return;
            }

            storage_[next_index_] = snapshot;
            next_index_ = (next_index_ + 1U) % storage_.size();
            if (count_ < storage_.size()) {
                ++count_;
            } else {
                ++overwritten_count_;
            }
        }

        [[nodiscard]] std::uint64_t overwritten_count() const noexcept {
            return overwritten_count_;
        }

        [[nodiscard]] std::vector<CycleSnapshot> to_vector() const {
            std::vector<CycleSnapshot> snapshots {};
            snapshots.reserve(count_);
            if (count_ == 0U) {
                return snapshots;
            }

            const std::size_t first_index = count_ == storage_.size() ? next_index_ : 0U;
            for (std::size_t offset = 0U; offset < count_; ++offset) {
                snapshots.push_back(storage_[(first_index + offset) % storage_.size()]);
            }
            return snapshots;
        }

    private:
        std::vector<CycleSnapshot> storage_ {};
        std::size_t next_index_ {};
        std::size_t count_ {};
        std::uint64_t overwritten_count_ {};
    };

    template <typename T, std::size_t Capacity>
    class PendingMinHeap {
    public:
        [[nodiscard]] bool empty() const noexcept {
            return size_ == 0U;
        }

        [[nodiscard]] const T& top() const noexcept {
            return heap_[0];
        }

        void clear() noexcept {
            size_ = 0U;
        }

        bool push(const T& item) noexcept {
            if (size_ == Capacity) {
                return false;
            }

            std::size_t index = size_++;
            heap_[index] = item;
            while (index > 0U) {
                const std::size_t parent = (index - 1U) / 2U;
                if (heap_[parent].release_time_ns <= heap_[index].release_time_ns) {
                    break;
                }
                std::swap(heap_[parent], heap_[index]);
                index = parent;
            }
            return true;
        }

        void pop() noexcept {
            heap_[0] = heap_[--size_];
            std::size_t index = 0U;
            while (true) {
                const std::size_t left = (index * 2U) + 1U;
                const std::size_t right = left + 1U;
                if (left >= size_) {
                    break;
                }

                std::size_t smallest = left;
                if (right < size_ && heap_[right].release_time_ns < heap_[left].release_time_ns) {
                    smallest = right;
                }
                if (heap_[index].release_time_ns <= heap_[smallest].release_time_ns) {
                    break;
                }
                std::swap(heap_[index], heap_[smallest]);
                index = smallest;
            }
        }

    private:
        std::array<T, Capacity> heap_ {};
        std::size_t size_ {};
    };

    [[nodiscard]] AssetID asset_id_for(std::string asset) {
        const auto found = asset_index_.find(asset);
        if (found != asset_index_.end()) {
            return found->second;
        }

        const AssetID id = static_cast<AssetID>(asset_names_.size());
        asset_names_.push_back(asset);
        asset_index_.emplace(std::move(asset), id);
        return id;
    }

    void prepare_runtime() {
        if (config_.max_book_levels_per_side == 0U) {
            throw std::invalid_argument("GraphArbitrageEngine max_book_levels_per_side must be greater than zero");
        }

        parsers_.clear();
        parsers_.reserve(pairs_.size());
        for (const PairConfig& pair : pairs_) {
            parsers_.emplace_back(pair.csv_file_path);
        }

        quote_asset_id_ = asset_id_for(config_.quote_asset);
        const std::size_t asset_count = asset_names_.size();
        rates_.assign(asset_count, 1.0);
        predecessors_.assign(asset_count, 0U);
        predecessor_edges_.assign(asset_count, 0U);
        adjacency_edges_.assign(asset_count, {});
        lookup_.init(asset_count, pairs_.size());
        std::vector<std::size_t> outgoing_counts(asset_count, 0U);
        for (AssetID pair_id = 0U; pair_id < pair_states_.size(); ++pair_id) {
            const PairState& pair = pair_states_[pair_id];
            if (pair.base_id < outgoing_counts.size()) {
                ++outgoing_counts[pair.base_id];
            }
            if (pair.quote_id < outgoing_counts.size()) {
                ++outgoing_counts[pair.quote_id];
            }
            set_route(pair.base_id, pair.quote_id, pair_id, true);
            set_route(pair.quote_id, pair.base_id, pair_id, false);
        }
        lookup_.finalize_routes();
        for (AssetID vertex = 0U; vertex < asset_count; ++vertex) {
            adjacency_edges_[vertex].reserve(outgoing_counts[vertex]);
        }
        cycle_.clear();
        cycle_.reserve(asset_count + 1U);
        wallet_.reset(asset_count, quote_asset_id_, Wallet::from_double_or_throw(config_.initial_usdt));
        edges_.clear();
        edges_.reserve(pairs_.size() * 2U);
        build_static_edges();
        refresh_all_edges();
        pending_cycles_.clear();
        cycle_snapshots_.reset(config_.cycle_snapshot_reserve);
        snapshot_clear_times_.assign(pair_states_.size(), 0U);
        snapshot_clear_valid_.assign(pair_states_.size(), 0U);
        parser_events_.clear();
        parser_events_.reserve(parsers_.size());
        for (std::size_t index = 0U; index < parsers_.size(); ++index) {
            push_parser_event_if_available(static_cast<AssetID>(index));
        }
        current_time_ns_ = 0U;
        running_ = true;
    }

    [[nodiscard]] static std::uint64_t normalize_timestamp_ns(std::uint64_t timestamp) noexcept {
        return timestamp < 10'000'000'000'000ULL ? timestamp * 1'000'000ULL : timestamp;
    }

    [[nodiscard]] bool has_next_event() const noexcept {
        return !parser_events_.empty();
    }

    [[nodiscard]] bool has_pending_cycle() const noexcept {
        return !pending_cycles_.empty();
    }

    [[nodiscard]] bool should_process_pending_cycle() const noexcept {
        if (!has_pending_cycle()) {
            return false;
        }
        return !has_next_event() || pending_cycles_.top().release_time_ns <= parser_events_.front().release_time_ns;
    }

    [[nodiscard]] AssetID next_pair_index() const noexcept {
        if (parser_events_.empty()) {
            return std::numeric_limits<AssetID>::max();
        }
        return parser_events_.front().pair_id;
    }

    [[nodiscard]] std::uint64_t next_event_time() const noexcept {
        if (parser_events_.empty()) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return parser_events_.front().release_time_ns;
    }

    void push_parser_event_if_available(AssetID pair_id) {
        L2UpdateCsvParser& parser = parsers_[pair_id];
        if (!parser.has_next()) {
            return;
        }
        parser_events_.push_back(ParserEvent {
            .release_time_ns = normalize_timestamp_ns(parser.peek_time()),
            .pair_id = pair_id,
        });
        std::push_heap(parser_events_.begin(), parser_events_.end(), ParserEventGreater {});
    }

    [[nodiscard]] ParserEvent pop_parser_event() noexcept {
        if (parser_events_.empty()) {
            return ParserEvent {};
        }
        std::pop_heap(parser_events_.begin(), parser_events_.end(), ParserEventGreater {});
        ParserEvent event = parser_events_.back();
        parser_events_.pop_back();
        return event;
    }

    void process_next_market_event(Result& result) noexcept {
        const std::uint64_t batch_time_ns = next_event_time();
        if (batch_time_ns == std::numeric_limits<std::uint64_t>::max()) {
            return;
        }
        current_time_ns_ = batch_time_ns;

        do {
            const ParserEvent parser_event = pop_parser_event();
            L2UpdateCsvParser& parser = parsers_[parser_event.pair_id];
            const L2UpdateEvent& event = parser.peek();

            apply_event(parser_event.pair_id, event);
            ++result.events_processed;
            parser.advance();
            push_parser_event_if_available(parser_event.pair_id);
        } while (has_next_event() && parser_events_.front().release_time_ns == batch_time_ns);

        ++result.market_batches_processed;
        ++result.cycle_searches;
        if (find_negative_cycle()) {
            ++result.cycles_detected;
            enqueue_cycle(result);
        }
    }

    void apply_event(AssetID pair_id, const L2UpdateEvent& event) noexcept {
        PairState& state = pair_states_[pair_id];

        if (
            event.is_snapshot &&
            (snapshot_clear_valid_[pair_id] == 0U || snapshot_clear_times_[pair_id] != current_time_ns_)
        ) {
            state.book.clear();
            snapshot_clear_times_[pair_id] = current_time_ns_;
            snapshot_clear_valid_[pair_id] = 1U;
        }
        state.book.update_level(event.is_bid, event.price, event.qty);
        update_pair_edges(pair_id);

        const double kQtyScale = 100'000'000.0;
        const double kNotionalScale = 10'000'000'000'000'000.0;  // 1e16 (ticks * lots)
        const double bid_depth = static_cast<double>(state.book.bid_total_qty()) / kQtyScale;
        const double ask_depth = static_cast<double>(state.book.ask_total_qty()) / kQtyScale;
        const double bid_notional = static_cast<double>(state.book.bid_total_notional()) / kNotionalScale;
        const double ask_notional = static_cast<double>(state.book.ask_total_notional()) / kNotionalScale;
        const double depth_sum = bid_depth + ask_depth;
        const double best_bid = static_cast<double>(state.book.best_bid()) / kQtyScale;
        const double best_ask = static_cast<double>(state.book.best_ask()) / kQtyScale;

        if (depth_sum > kEpsilon && best_bid > 0.0 && best_ask > 0.0) {
            state.obi = (bid_depth - ask_depth) / depth_sum;
            const double bid_vwap = bid_depth > kEpsilon ? bid_notional / bid_depth : best_bid;
            const double ask_vwap = ask_depth > kEpsilon ? ask_notional / ask_depth : best_ask;
            state.micro_price = ((bid_vwap * ask_depth) + (ask_vwap * bid_depth)) / depth_sum;
            state.spread_bps = ((best_ask - best_bid) / best_bid) * 10'000.0;
        } else {
            state.obi = 0.0;
            state.micro_price = 0.0;
            state.spread_bps = 0.0;
        }
    }

    void build_static_edges() {
        lookup_.clear_edges();
        for (AssetID pair_id = 0U; pair_id < pair_states_.size(); ++pair_id) {
            const PairState& pair = pair_states_[pair_id];
            add_static_edge(Edge {
                .from = pair.base_id,
                .to = pair.quote_id,
                .pair_id = pair_id,
                .use_bid = true,
            });
            add_static_edge(Edge {
                .from = pair.quote_id,
                .to = pair.base_id,
                .pair_id = pair_id,
                .use_bid = false,
            });
        }
    }

    void add_static_edge(const Edge& edge) {
        const std::size_t edge_index = edges_.size();
        edges_.push_back(edge);
        if (edge.from < adjacency_edges_.size()) {
            adjacency_edges_[edge.from].push_back(edge_index);
        }
        lookup_.set_edge(edge.from, edge.to, edge_index);
    }

    void refresh_all_edges() noexcept {
        for (AssetID pair_id = 0U; pair_id < pair_states_.size(); ++pair_id) {
            update_pair_edges(pair_id);
        }
        refresh_wallet_factors();
    }

    void update_pair_edges(AssetID pair_id) noexcept {
        if (pair_id >= pair_states_.size()) {
            return;
        }

        const PairState& pair = pair_states_[pair_id];
        if (Edge* bid_edge = mutable_edge_for(pair.base_id, pair.quote_id)) {
            const double best_bid = static_cast<double>(pair.book.best_bid()) / 100'000'000.0;
            const double bid_capacity = side_input_capacity(pair_id, true);
            bid_edge->rate = best_bid > 0.0 && bid_capacity > 0.0 ? best_bid : 0.0;
            bid_edge->available_from_qty = bid_capacity;
            bid_edge->effective_rate = bid_edge->rate * taker_fee_multiplier();
        }

        if (Edge* ask_edge = mutable_edge_for(pair.quote_id, pair.base_id)) {
            const double best_ask = static_cast<double>(pair.book.best_ask()) / 100'000'000.0;
            const double ask_capacity = side_input_capacity(pair_id, false);
            ask_edge->rate = best_ask > 0.0 && ask_capacity > 0.0 ? 1.0 / best_ask : 0.0;
            ask_edge->available_from_qty = ask_capacity;
            ask_edge->effective_rate = ask_edge->rate * taker_fee_multiplier();
        }
    }

    void refresh_wallet_factors() noexcept {
        (void)wallet_.refresh_liquidation_factors([this](AssetID asset) noexcept {
            double factor = 0.0;
            if (const Edge* direct = edge_for(asset, quote_asset_id_);
                direct != nullptr && direct->effective_rate > 0.0) {
                factor = direct->effective_rate;
            } else {
                for (const Edge& first : edges_) {
                    if (first.from != asset || first.effective_rate <= 0.0) {
                        continue;
                    }
                    const Edge* second = edge_for(first.to, quote_asset_id_);
                    if (second != nullptr && second->effective_rate > 0.0) {
                        factor = first.effective_rate * second->effective_rate;
                        break;
                    }
                }
            }
            std::int64_t fixed {};
            return Wallet::from_double(factor, fixed) ? fixed : 0;
        });
    }

    [[nodiscard]] bool find_negative_cycle() noexcept {
        const std::size_t asset_count = asset_names_.size();
        if (asset_count == 0U || edges_.empty()) {
            return false;
        }

        std::fill(rates_.begin(), rates_.end(), 1.0);
        std::fill(predecessors_.begin(), predecessors_.end(), std::numeric_limits<AssetID>::max());
        std::fill(predecessor_edges_.begin(), predecessor_edges_.end(), invalid_edge_index());

        AssetID relaxed_vertex = std::numeric_limits<AssetID>::max();
        for (std::size_t pass = 0U; pass < asset_count; ++pass) {
            relaxed_vertex = std::numeric_limits<AssetID>::max();
            for (std::size_t edge_index = 0U; edge_index < edges_.size(); ++edge_index) {
                const Edge& edge = edges_[edge_index];
                if (edge.effective_rate <= 0.0 || edge.from >= rates_.size() || edge.to >= rates_.size()) {
                    continue;
                }
                const double candidate = rates_[edge.from] * edge.effective_rate;
                if (candidate <= rates_[edge.to] * (1.0 + kEpsilon)) {
                    continue;
                }

                rates_[edge.to] = candidate;
                predecessors_[edge.to] = edge.from;
                predecessor_edges_[edge.to] = edge_index;
                relaxed_vertex = edge.to;
            }
            if (relaxed_vertex == std::numeric_limits<AssetID>::max()) {
                return false;
            }
        }

        return reconstruct_negative_cycle(relaxed_vertex, asset_count);
    }

    [[nodiscard]] bool reconstruct_negative_cycle(AssetID cycle_node, std::size_t asset_count) noexcept {
        cycle_.clear();
        if (cycle_node >= predecessors_.size()) {
            return false;
        }

        AssetID current = cycle_node;
        for (std::size_t step = 0U; step < asset_count; ++step) {
            current = predecessors_[current];
            if (current >= predecessors_.size()) {
                return false;
            }
        }

        const AssetID cycle_start = current;
        do {
            cycle_.push_back(current);
            current = predecessors_[current];
            if (current >= predecessors_.size() || cycle_.size() > asset_count) {
                cycle_.clear();
                return false;
            }
        } while (current != cycle_start);
        cycle_.push_back(cycle_start);
        std::reverse(cycle_.begin(), cycle_.end());
        return cycle_.size() > 2U && rotate_cycle_to_quote();
    }

    [[nodiscard]] bool rotate_cycle_to_quote() {
        if (cycle_.size() < 3U) {
            return false;
        }

        auto quote_it = std::find(cycle_.begin(), cycle_.end() - 1, quote_asset_id_);
        if (quote_it == cycle_.end() - 1) {
            return false;
        }

        std::rotate(cycle_.begin(), quote_it, cycle_.end() - 1);
        cycle_.back() = cycle_.front();
        return true;
    }

    void enqueue_cycle(Result& result) noexcept {
        ++result.attempted_cycles;
        if (cycle_.size() < 3U || cycle_.size() > kMaxGraphLegs + 1U || cycle_.front() != quote_asset_id_) {
            return;
        }

        const double start_quantity = calculate_fee_aware_bottleneck();
        const bool executable = start_quantity > 0.0 && cycle_passes_execution_filters() && cycle_edge_bps() >= config_.min_cycle_edge_bps;
        record_cycle_snapshot(executable);
        if (!executable) {
            return;
        }
        if (!wallet_.reserve_balance(quote_asset_id_, Wallet::from_double_or_throw(start_quantity))) {
            return;
        }

        PendingCycle pending {};
        pending.release_time_ns = current_time_ns_ + config_.latency_ns;
        pending.leg_count = static_cast<std::uint8_t>(cycle_.size() - 1U);
        pending.current_leg = 0U;
        pending.current_quantity = start_quantity;
        pending.current_asset = cycle_.front();
        pending.initial_quantity = start_quantity;
        for (std::size_t index = 0U; index < cycle_.size(); ++index) {
            pending.assets[index] = cycle_[index];
        }

        double current_qty = start_quantity;
        for (std::size_t index = 0U; index < pending.leg_count; ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge != nullptr) {
                pending.expected_rates[index] = edge->rate;
                const double gross_output = quote_visible_depth(*edge, current_qty);
                pending.leg_outputs[index] = gross_output;
                current_qty = gross_output * taker_fee_multiplier();
            }
        }

        if (!pending_cycles_.push(pending)) {
            (void)wallet_.release_reserved(quote_asset_id_, Wallet::from_double_or_throw(start_quantity));
            ++result.panic_closes;
        }
    }

    [[nodiscard]] double calculate_fee_aware_bottleneck() const noexcept {
        double cumulative_input = 1.0;
        const double available_quote = Wallet::to_double(wallet_.free_balance(quote_asset_id_));
        double max_start = config_.max_cycle_notional_usdt > 0.0 ? config_.max_cycle_notional_usdt : available_quote;
        if (available_quote < max_start) {
            max_start = available_quote;
        }

        const double fee_multiplier = taker_fee_multiplier();
        for (std::size_t index = 0U; index + 1U < cycle_.size(); ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr || edge->rate <= 0.0 || edge->available_from_qty <= 0.0 || cumulative_input <= 0.0) {
                return 0.0;
            }

            const double constrained_start = edge->available_from_qty / cumulative_input;
            if (constrained_start < max_start) {
                max_start = constrained_start;
            }
            cumulative_input *= edge->rate * fee_multiplier;
        }

        return max_start > 0.0 ? max_start : 0.0;
    }

    void process_next_pending_cycle(Result& result) noexcept {
        PendingCycle pending = pending_cycles_.top();
        pending_cycles_.pop();
        current_time_ns_ = std::max(current_time_ns_, pending.release_time_ns);
        execute_pending_leg(pending, result);
    }

    void execute_pending_leg(PendingCycle& pending, Result& result) noexcept {
        refresh_wallet_factors();
        if (pending.current_leg >= pending.leg_count) {
            if (pending.current_asset == quote_asset_id_) {
                ++result.completed_cycles;
                return;
            }
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
            return;
        }

        const AssetID next_asset = pending.assets[pending.current_leg + 1U];
        Edge current_edge {};
        const bool has_edge = current_edge_for(pending.current_asset, next_asset, current_edge);
        const bool spending_reserved_quote = pending.current_leg == 0U && pending.current_asset == quote_asset_id_;
        const double spendable_balance = spending_reserved_quote
            ? Wallet::to_double(wallet_.reserved(pending.current_asset))
            : wallet_balance_units(pending.current_asset);
            
        if (
            !has_edge ||
            current_edge.rate < pending.expected_rates[pending.current_leg] - kEpsilon ||
            current_edge.available_from_qty + kWalletQuantum < pending.current_quantity ||
            quote_visible_depth(current_edge, pending.current_quantity) + kWalletQuantum <
                pending.leg_outputs[pending.current_leg] ||
            spendable_balance + kWalletQuantum < pending.current_quantity
        ) {
            if (spending_reserved_quote) {
                (void)wallet_.release_reserved(
                    pending.current_asset,
                    Wallet::from_double_or_throw(pending.initial_quantity)
                );
            }
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
            return;
        }

        if (spending_reserved_quote) {
            if (!wallet_.consume_reserved(
                    pending.current_asset,
                    Wallet::from_double_or_throw(pending.current_quantity))) {
                (void)wallet_.release_reserved(
                    pending.current_asset,
                    Wallet::from_double_or_throw(pending.initial_quantity)
                );
                panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
                return;
            }
        } else {
            (void)wallet_.sub_balance(
                pending.current_asset,
                Wallet::from_double_or_throw(pending.current_quantity)
            );
        }

        const double gross_output = sweep_visible_depth(current_edge, pending.current_quantity);
        update_pair_edges(current_edge.pair_id);
        std::int64_t net_output {};
        if (!wallet_.apply_fill(
                next_asset,
                Wallet::from_double_or_throw(gross_output),
                config_.taker_fee_bps,
                net_output)) {
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
            return;
        }
        pending.current_quantity = Wallet::to_double(net_output);
        pending.current_asset = next_asset;
        ++pending.current_leg;

        if (pending.current_leg >= pending.leg_count) {
            if (pending.current_asset == quote_asset_id_) {
                ++result.completed_cycles;
                return;
            }
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
            return;
        }

        pending.release_time_ns += config_.intra_leg_latency_ns;
        if (!pending_cycles_.push(pending)) {
            panic_close_to_quote(pending.current_asset, pending.current_quantity, pending.initial_quantity, result);
        }
    }

    void panic_close_to_quote(AssetID asset_id, double quantity, double initial_quote, Result& result) noexcept {
        refresh_wallet_factors();
        ++result.panic_closes;
        (void)initial_quote;
        if (asset_id == quote_asset_id_ || quantity <= 0.0) {
            return;
        }

        const double balance = wallet_balance_units(asset_id);
        const double close_quantity = balance < quantity ? balance : quantity;
        if (close_quantity <= 0.0) {
            return;
        }

        const double recovered_quote = convert_to_quote(asset_id, close_quantity);
        if (recovered_quote <= 0.0) {
            return;
        }
        (void)wallet_.sub_balance(asset_id, Wallet::from_double_or_throw(close_quantity));
        std::int64_t net_quote {};
        (void)wallet_.apply_fill(
            quote_asset_id_,
            Wallet::from_double_or_throw(recovered_quote),
            config_.taker_fee_bps,
            net_quote
        );
    }

    [[nodiscard]] double convert_to_quote(AssetID asset_id, double quantity) noexcept {
        if (asset_id == quote_asset_id_) {
            return quantity;
        }

        refresh_all_edges();
        if (const Edge* direct = edge_for(asset_id, quote_asset_id_)) {
            return execute_pessimistic_liquidation(*direct, quantity);
        }

        for (const Edge& first : edges_) {
            if (first.from != asset_id) {
                continue;
            }
            if (const Edge* second = edge_for(first.to, quote_asset_id_)) {
                (void)second;
                const double intermediate = execute_pessimistic_liquidation(first, quantity);
                update_pair_edges(first.pair_id);
                const Edge* refreshed_second = edge_for(first.to, quote_asset_id_);
                if (refreshed_second != nullptr) {
                    const double output = execute_pessimistic_liquidation(*refreshed_second, intermediate * taker_fee_multiplier());
                    update_pair_edges(refreshed_second->pair_id);
                    return output;
                }
                return 0.0;
            }
        }

        return 0.0;
    }

    [[nodiscard]] double execute_pessimistic_liquidation(const Edge& edge, double input_quantity) noexcept {
        static constexpr double kPanicSlippagePenalty = 0.005;
        const double visible_quantity = input_quantity < edge.available_from_qty ? input_quantity : edge.available_from_qty;
        const double excess_quantity = input_quantity - visible_quantity;
        const double visible_output = sweep_visible_depth(edge, visible_quantity);
        const double penalized_output = excess_quantity > 0.0
            ? excess_quantity * edge.rate * (1.0 - kPanicSlippagePenalty)
            : 0.0;
        deplete_unobserved_tail(edge, excess_quantity);
        update_pair_edges(edge.pair_id);
        return visible_output + penalized_output;
    }

    [[nodiscard]] double side_input_capacity(AssetID pair_id, bool use_bid) const noexcept {
        const PairState& pair = pair_states_[pair_id];
        if (use_bid) {
            return static_cast<double>(pair.book.bid_effective_qty()) / 100'000'000.0;
        }
        return static_cast<double>(pair.book.ask_effective_notional()) / 10'000'000'000'000'000.0;
    }

    [[nodiscard]] double quote_visible_depth(const Edge& edge, double input_quantity) const noexcept {
        const PairState& pair = pair_states_[edge.pair_id];
        double remaining = input_quantity;
        double output = 0.0;

        if (edge.use_bid) {
            for (const auto& level : pair.book.bids()) {
                if (remaining <= kEpsilon) {
                    break;
                }
                const double effective_qty = static_cast<double>(level.effective_qty()) / 100'000'000.0;
                if (effective_qty <= 0.0) {
                    continue;
                }
                const double level_price = static_cast<double>(level.price) / 100'000'000.0;
                const double consumed = remaining < effective_qty ? remaining : effective_qty;
                remaining -= consumed;
                output += consumed * level_price;
            }
        } else {
            for (const auto& level : pair.book.asks()) {
                if (remaining <= kEpsilon) {
                    break;
                }
                const double effective_qty = static_cast<double>(level.effective_qty()) / 100'000'000.0;
                if (effective_qty <= 0.0) {
                    continue;
                }
                const double level_price = static_cast<double>(level.price) / 100'000'000.0;
                const double level_notional = level_price * effective_qty;
                const double consumed_quote = remaining < level_notional ? remaining : level_notional;
                remaining -= consumed_quote;
                output += consumed_quote / level_price;
            }
        }
        return output;
    }

    [[nodiscard]] double sweep_visible_depth(const Edge& edge, double input_quantity) noexcept {
        PairState& pair = pair_states_[edge.pair_id];
        double remaining = input_quantity;
        double output = 0.0;
        
        if (edge.use_bid) {
            for (const auto& level : pair.book.bids()) {
                if (remaining <= kEpsilon) {
                    break;
                }
                const double effective_qty = static_cast<double>(level.effective_qty()) / 100'000'000.0;
                if (effective_qty <= 0.0) {
                    continue;
                }
                const double consumed = remaining < effective_qty ? remaining : effective_qty;
                pair.book.deplete_level(true, level.price, static_cast<std::uint64_t>(std::llround(consumed * 100'000'000.0)));
                remaining -= consumed;
                output += consumed * static_cast<double>(level.price) / 100'000'000.0;
            }
        } else {
            for (const auto& level : pair.book.asks()) {
                if (remaining <= kEpsilon) {
                    break;
                }
                const double effective_qty = static_cast<double>(level.effective_qty()) / 100'000'000.0;
                if (effective_qty <= 0.0) {
                    continue;
                }
                const double level_price = static_cast<double>(level.price) / 100'000'000.0;
                const double level_notional = level_price * effective_qty;
                const double consumed_quote = remaining < level_notional ? remaining : level_notional;
                const double consumed_base = consumed_quote / level_price;
                pair.book.deplete_level(false, level.price, static_cast<std::uint64_t>(std::llround(consumed_base * 100'000'000.0)));
                remaining -= consumed_quote;
                output += consumed_base;
            }
        }
        return output;
    }

    void deplete_unobserved_tail(const Edge& edge, double input_quantity) noexcept {
        if (input_quantity <= 0.0) {
            return;
        }
        PairState& pair = pair_states_[edge.pair_id];
        if (edge.use_bid) {
            if (!pair.book.bids().empty()) {
                pair.book.deplete_level(true, pair.book.bids().back().price, static_cast<std::uint64_t>(std::llround(input_quantity * 100'000'000.0)));
            }
        } else {
            if (!pair.book.asks().empty()) {
                const std::int64_t tail_price = pair.book.asks().back().price;
                const double tail_price_double = static_cast<double>(tail_price) / 100'000'000.0;
                pair.book.deplete_level(false, tail_price, static_cast<std::uint64_t>(std::llround((input_quantity / tail_price_double) * 100'000'000.0)));
            }
        }
    }

    [[nodiscard]] double taker_fee_multiplier() const noexcept {
        return 1.0 - (config_.taker_fee_bps * 0.0001);
    }

    [[nodiscard]] const Edge* edge_for(AssetID from, AssetID to) const noexcept {
        const std::size_t edge_index = lookup_.find_edge(from, to, adjacency_edges_, edges_);
        return edge_index < edges_.size() ? &edges_[edge_index] : nullptr;
    }

    [[nodiscard]] Edge* mutable_edge_for(AssetID from, AssetID to) noexcept {
        const std::size_t edge_index = lookup_.find_edge(from, to, adjacency_edges_, edges_);
        return edge_index < edges_.size() ? &edges_[edge_index] : nullptr;
    }

    void set_route(AssetID from, AssetID to, AssetID pair_id, bool use_bid) {
        lookup_.set_route(from, to, pair_id, use_bid);
    }

    [[nodiscard]] bool current_edge_for(AssetID from, AssetID to, Edge& edge) const noexcept {
        AssetID pair_id {};
        bool use_bid {};
        if (!lookup_.find_route(from, to, pair_id, use_bid) || pair_id >= pair_states_.size()) {
            return false;
        }

        const PairState& pair = pair_states_[pair_id];
        const double price = static_cast<double>(use_bid ? pair.book.best_bid() : pair.book.best_ask()) / 100'000'000.0;
        const double capacity = side_input_capacity(pair_id, use_bid);
        if (price <= 0.0 || capacity <= 0.0) {
            return false;
        }

        const double rate = use_bid ? price : 1.0 / price;
        edge = Edge {
            .from = from,
            .to = to,
            .pair_id = pair_id,
            .use_bid = use_bid,
            .rate = rate,
            .available_from_qty = capacity,
            .effective_rate = rate * taker_fee_multiplier(),
        };
        return true;
    }

    [[nodiscard]] bool cycle_passes_execution_filters() const noexcept {
        for (std::size_t index = 0U; index + 1U < cycle_.size(); ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr) {
                return false;
            }

            const PairState& state = pair_states_[edge->pair_id];
            if (state.spread_bps > config_.max_spread_bps) {
                return false;
            }
            if (config_.min_depth_usdt > 0.0 && depth_to_quote(*edge) < config_.min_depth_usdt) {
                return false;
            }
            if (!obi_is_acceptable(*edge, state)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] double cycle_edge_bps() const noexcept {
        double gross_multiplier = 1.0;
        for (std::size_t index = 0U; index + 1U < cycle_.size(); ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr || edge->rate <= 0.0) {
                return -std::numeric_limits<double>::infinity();
            }
            gross_multiplier *= edge->effective_rate;
        }
        return (gross_multiplier - 1.0) * 10'000.0;
    }

    [[nodiscard]] bool obi_is_acceptable(const Edge& edge, const PairState& state) const noexcept {
        if (config_.max_adverse_obi >= 1.0) {
            return true;
        }
        if (!edge.use_bid && state.obi > config_.max_adverse_obi) {
            return false;
        }
        if (edge.use_bid && state.obi < -config_.max_adverse_obi) {
            return false;
        }
        return true;
    }

    [[nodiscard]] double depth_to_quote(const Edge& edge) const noexcept {
        if (edge.from == quote_asset_id_) {
            return edge.available_from_qty;
        }
        if (edge.to == quote_asset_id_) {
            return edge.available_from_qty * edge.rate;
        }
        if (const Edge* to_quote = edge_for(edge.to, quote_asset_id_)) {
            return edge.available_from_qty * edge.rate * to_quote->rate;
        }
        if (const Edge* from_quote = edge_for(edge.from, quote_asset_id_)) {
            return edge.available_from_qty * from_quote->rate;
        }
        return 0.0;
    }

    void record_cycle_snapshot(bool executed) {
        CycleSnapshot snapshot {};
        snapshot.timestamp_ns = current_time_ns_;
        snapshot.executed = executed;
        snapshot.leg_count = static_cast<std::uint8_t>(cycle_.size() > 0U ? cycle_.size() - 1U : 0U);
        for (std::size_t index = 0U; index < cycle_.size() && index < snapshot.cycle_path.size(); ++index) {
            snapshot.cycle_path[index] = cycle_[index];
        }
        for (std::size_t index = 0U; index < snapshot.leg_count; ++index) {
            const Edge* edge = edge_for(cycle_[index], cycle_[index + 1U]);
            if (edge == nullptr) {
                snapshot.leg_obis[index] = 0.0;
                snapshot.leg_spreads_bps[index] = 0.0;
                continue;
            }

            const PairState& state = pair_states_[edge->pair_id];
            snapshot.leg_obis[index] = state.obi;
            snapshot.leg_spreads_bps[index] = state.spread_bps;
        }
        cycle_snapshots_.push(snapshot);
    }

    Config config_ {};
    AssetID quote_asset_id_ {};
    std::uint64_t current_time_ns_ {};
    bool running_ {};
    std::vector<PairConfig> pairs_ {};
    std::vector<PairState> pair_states_ {};
    std::vector<L2UpdateCsvParser> parsers_ {};
    std::vector<std::string> asset_names_ {};
    std::unordered_map<std::string, AssetID> asset_index_ {};
    std::vector<Edge> edges_ {};
    std::vector<std::vector<std::size_t>> adjacency_edges_ {};
    LookupPolicy lookup_ {};
    std::vector<double> rates_ {};
    std::vector<AssetID> predecessors_ {};
    std::vector<std::size_t> predecessor_edges_ {};
    std::vector<AssetID> cycle_ {};
    std::vector<std::uint64_t> snapshot_clear_times_ {};
    std::vector<std::uint8_t> snapshot_clear_valid_ {};
    std::vector<ParserEvent> parser_events_ {};
    CycleSnapshotRing cycle_snapshots_ {};
    [[nodiscard]] double wallet_balance_units(AssetID asset) const noexcept {
        return Wallet::to_double(wallet_.balance(asset));
    }

    Wallet wallet_ {};
    PendingMinHeap<PendingCycle, kPendingCycleCapacity> pending_cycles_ {};

    [[nodiscard]] static constexpr std::size_t invalid_edge_index() noexcept {
        return std::numeric_limits<std::size_t>::max();
    }
};

using GraphArbitrageEngine = GraphArbitrageEngineT<DenseLookupPolicy>;
using GraphArbitrageEngineLarge = GraphArbitrageEngineT<SparseLookupPolicy>;

}  // namespace lob
