#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "lob/sim/account_store.hpp"
#include "lob/sim/agent_context.hpp"
#include "lob/sim/event_scheduler.hpp"
#include "lob/sim/market_agent.hpp"
#include "lob/sim/market_registry.hpp"
#include "lob/sim/market_state.hpp"
#include "lob/sim/order_request.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim {

enum class SimulationStatus : std::uint8_t {
    Running,
    HaltedAccountingFailure,
    HaltedSinkOverflow,
    HaltedMarketDataOverflow,
};

template <std::size_t SchedulerCapacity,
          std::size_t MarketCapacity = 16U,
          std::size_t BookOrderCapacity = 256U,
          std::size_t BookPriceLevelCapacity = 64U,
          std::size_t BookMaxExecutionReports = 64U,
          std::size_t ResultSinkCapacity = 64U,
          std::size_t AccountCapacity = 32U,
          std::size_t AssetCapacity = 16U,
          std::size_t AccountingSinkCapacity = 64U,
          std::size_t ReservationCapacity = (MarketCapacity * BookOrderCapacity) + SchedulerCapacity,
          std::size_t MarketDataSubscriberCapacity = AccountCapacity,
          std::size_t AgentCapacity = AccountCapacity>
class SimulationEngine {
    struct OrderReservation {
        OrderID order_id {invalid_order_id};
        AgentID agent_id {invalid_agent_id};
        MarketID market_id {invalid_market_id};
        Side side {Side::Buy};
        AssetID reserve_asset_id {};
        PriceTicks price_ticks {};
        BalanceLots reserved_amount_lots {};
        bool active {};
    };

public:
    using MarketStateType = MarketState<BookOrderCapacity, BookPriceLevelCapacity, BookMaxExecutionReports>;
    using MatchResultType = typename MarketStateType::MatchingBookType::Result;
    using AccountStoreType = AccountStore<AccountCapacity, AssetCapacity>;
    using AgentContextType = AgentContext<SimulationEngine>;
    using MarketAgentType = MarketAgent<SimulationEngine>;

    struct AccountingEvent {
        TimeNs timestamp_ns {};
        MarketID market_id {invalid_market_id};
        AgentID buyer_agent_id {invalid_agent_id};
        AgentID seller_agent_id {invalid_agent_id};
        PriceTicks price_ticks {};
        QtyLots quantity_lots {};
        BalanceLots buyer_fee_lots {};
        BalanceLots seller_fee_lots {};
    };

    static constexpr std::size_t VisibleDepthCapacity = MarketDataSubscriberCapacity * MarketCapacity;

    struct VisibleDepthSlot {
        AgentID agent_id {invalid_agent_id};
        MarketID market_id {invalid_market_id};
        MarketDepthSnapshot<1U> snapshot {};
        bool active {};
    };

    struct MarketDataSubscriber {
        AgentID agent_id {invalid_agent_id};
        LatencyModel feed_latency_model {};
        bool uses_market_latency {true};
    };

    struct AgentSlot {
        AgentID agent_id {invalid_agent_id};
        MarketAgentType* agent {};
        bool active {};
    };

    [[nodiscard]] TimeNs current_time_ns() const noexcept {
        return current_time_ns_;
    }

    [[nodiscard]] std::size_t processed_event_count() const noexcept {
        return processed_event_count_;
    }

    [[nodiscard]] SimulationStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool halted() const noexcept {
        return status_ != SimulationStatus::Running;
    }

    [[nodiscard]] const MarketRegistry& market_registry() const noexcept {
        return market_registry_;
    }

    [[nodiscard]] MarketID register_market(MarketKey key) {
        const auto existing = market_registry_.find_market(key);
        if (existing.has_value()) {
            return *existing;
        }
        if (market_state_count_ == MarketCapacity) {
            return invalid_market_id;
        }

        const MarketID market_id = market_registry_.register_market(key);
        market_states_[market_state_count_++] = MarketStateType {
            .market_id = market_id,
            .market_key = key,
            .order_entry_latency_model = {},
            .cancel_latency_model = {},
            .market_data_latency_model = {},
            .matching_book = {},
        };
        return market_id;
    }

    [[nodiscard]] std::size_t market_state_count() const noexcept {
        return market_state_count_;
    }

    [[nodiscard]] bool register_account(AgentID agent_id) noexcept {
        return accounts_.register_account(agent_id);
    }

    [[nodiscard]] bool set_balance(AgentID agent_id, AssetID asset_id, BalanceLots amount_lots) noexcept {
        return accounts_.set_balance(agent_id, asset_id, amount_lots);
    }

    [[nodiscard]] std::optional<AccountBalance> account_balance(AgentID agent_id, AssetID asset_id) const noexcept {
        return accounts_.balance(agent_id, asset_id);
    }

    [[nodiscard]] const AccountStoreType& account_store() const noexcept {
        return accounts_;
    }

    [[nodiscard]] bool register_agent(AgentID agent_id, MarketAgentType& agent) noexcept {
        if (halted() || agents_started_ || agent_id == invalid_agent_id || !accounts_.has_account(agent_id)
            || find_agent_slot(agent_id) != nullptr) {
            return false;
        }
        if (agent_count_ == AgentCapacity) {
            return false;
        }
        agent_slots_[agent_count_++] = AgentSlot {
            .agent_id = agent_id,
            .agent = &agent,
            .active = true,
        };
        return true;
    }

    [[nodiscard]] std::size_t agent_count() const noexcept {
        return agent_count_;
    }

    [[nodiscard]] bool start() noexcept {
        if (halted()) {
            return false;
        }
        if (agents_started_) {
            return true;
        }
        agents_started_ = true;
        for (std::size_t i = 0U; i < agent_count_; ++i) {
            AgentSlot& slot = agent_slots_[i];
            if (!slot.active || slot.agent == nullptr) {
                continue;
            }
            AgentContextType context {*this, slot.agent_id};
            slot.agent->on_start(context);
            if (halted()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool initialize_agents() noexcept {
        return start();
    }

    [[nodiscard]] bool schedule_agent_wakeup(AgentID agent_id, TimeNs wakeup_time_ns) noexcept {
        if (halted() || find_agent_slot(agent_id) == nullptr) {
            return false;
        }
        return scheduler_.push(ScheduledEvent {
            .timestamp_ns = wakeup_time_ns,
            .phase = EventPhase::Agent,
            .kind = EventKind::AgentWakeup,
            .agent_id = agent_id,
        });
    }

    [[nodiscard]] std::size_t unknown_agent_wakeup_count() const noexcept {
        return unknown_agent_wakeup_count_;
    }

#ifdef LOB_SIM_TESTING
    [[nodiscard]] const EventScheduler<SchedulerCapacity>& scheduler() const noexcept {
        return scheduler_;
    }

    [[nodiscard]] const MarketStateType* market_state(MarketID market_id) const noexcept {
        if (static_cast<std::size_t>(market_id) >= market_state_count_) {
            return nullptr;
        }
        return &market_states_[market_id];
    }

    [[nodiscard]] bool schedule_event(ScheduledEvent event) noexcept {
        return scheduler_.push(event);
    }
#endif

    [[nodiscard]] bool set_market_product_spec(MarketID market_id, ProductSpec product_spec) noexcept {
        MarketStateType* state = mutable_market_state(market_id);
        if (halted() || state == nullptr || !valid_product_spec(product_spec)
            || market_product_spec_locked(market_id, *state)) {
            return false;
        }
        state->product_spec = product_spec;
        return true;
    }

    [[nodiscard]] bool set_market_latency_model(MarketID market_id, LatencyModel latency_model) noexcept {
        MarketStateType* state = mutable_market_state(market_id);
        if (halted() || state == nullptr || !latency_model.valid_config()) {
            return false;
        }
        state->order_entry_latency_model = latency_model;
        state->cancel_latency_model = latency_model;
        state->market_data_latency_model = latency_model;
        return true;
    }

    [[nodiscard]] bool set_market_latency(MarketID market_id,
                                          TimeNs base_latency_ns,
                                          TimeNs jitter_bound_ns = 0U,
                                          std::uint64_t seed = 1U) noexcept {
        return set_market_latency_model(market_id, LatencyModel {base_latency_ns, jitter_bound_ns, seed});
    }

    [[nodiscard]] bool set_order_entry_latency_model(MarketID market_id, LatencyModel latency_model) noexcept {
        MarketStateType* state = mutable_market_state(market_id);
        if (halted() || state == nullptr || !latency_model.valid_config()) {
            return false;
        }
        state->order_entry_latency_model = latency_model;
        return true;
    }

    [[nodiscard]] bool set_cancel_latency_model(MarketID market_id, LatencyModel latency_model) noexcept {
        MarketStateType* state = mutable_market_state(market_id);
        if (halted() || state == nullptr || !latency_model.valid_config()) {
            return false;
        }
        state->cancel_latency_model = latency_model;
        return true;
    }

    [[nodiscard]] bool set_market_data_latency_model(MarketID market_id, LatencyModel latency_model) noexcept {
        MarketStateType* state = mutable_market_state(market_id);
        if (halted() || state == nullptr || !latency_model.valid_config()) {
            return false;
        }
        state->market_data_latency_model = latency_model;
        return true;
    }

    [[nodiscard]] bool set_order_entry_latency(MarketID market_id,
                                               TimeNs base_latency_ns,
                                               TimeNs jitter_bound_ns = 0U,
                                               std::uint64_t seed = 1U) noexcept {
        return set_order_entry_latency_model(market_id, LatencyModel {base_latency_ns, jitter_bound_ns, seed});
    }

    [[nodiscard]] bool set_cancel_latency(MarketID market_id,
                                          TimeNs base_latency_ns,
                                          TimeNs jitter_bound_ns = 0U,
                                          std::uint64_t seed = 1U) noexcept {
        return set_cancel_latency_model(market_id, LatencyModel {base_latency_ns, jitter_bound_ns, seed});
    }

    [[nodiscard]] bool set_market_data_latency(MarketID market_id,
                                               TimeNs base_latency_ns,
                                               TimeNs jitter_bound_ns = 0U,
                                               std::uint64_t seed = 1U) noexcept {
        return set_market_data_latency_model(market_id, LatencyModel {base_latency_ns, jitter_bound_ns, seed});
    }

    [[nodiscard]] bool register_market_data_subscriber(AgentID agent_id) noexcept {
        if (halted() || !accounts_.has_account(agent_id)) {
            return false;
        }
        for (std::size_t i = 0U; i < market_data_subscriber_count_; ++i) {
            if (market_data_subscribers_[i].agent_id == agent_id) {
                return true;
            }
        }
        if (market_data_subscriber_count_ == MarketDataSubscriberCapacity) {
            return false;
        }
        market_data_subscribers_[market_data_subscriber_count_++] = MarketDataSubscriber {
            .agent_id = agent_id,
            .feed_latency_model = {},
            .uses_market_latency = true,
        };
        return true;
    }

    [[nodiscard]] bool register_market_data_subscriber(AgentID agent_id, LatencyModel feed_latency_model) noexcept {
        if (halted() || !accounts_.has_account(agent_id) || !feed_latency_model.valid_config()) {
            return false;
        }
        for (std::size_t i = 0U; i < market_data_subscriber_count_; ++i) {
            if (market_data_subscribers_[i].agent_id == agent_id) {
                market_data_subscribers_[i].feed_latency_model = feed_latency_model;
                market_data_subscribers_[i].uses_market_latency = false;
                return true;
            }
        }
        if (market_data_subscriber_count_ == MarketDataSubscriberCapacity) {
            return false;
        }
        market_data_subscribers_[market_data_subscriber_count_++] = MarketDataSubscriber {
            .agent_id = agent_id,
            .feed_latency_model = feed_latency_model,
            .uses_market_latency = false,
        };
        return true;
    }

    [[nodiscard]] std::size_t market_data_subscriber_count() const noexcept {
        return market_data_subscriber_count_;
    }

    template <std::size_t Depth>
    [[nodiscard]] std::optional<MarketDepthSnapshot<Depth>> visible_depth(AgentID agent_id, MarketID market_id) const noexcept {
        static_assert(Depth == 1U, "Only depth-1 visible market data is implemented currently.");
        const VisibleDepthSlot* slot = find_visible_depth_slot(agent_id, market_id);
        if (slot == nullptr) {
            return std::nullopt;
        }

        MarketDepthSnapshot<Depth> snapshot {};
        copy_depth_snapshot(slot->snapshot, snapshot);
        return snapshot;
    }

#ifdef LOB_SIM_TESTING
    template <std::size_t Depth>
    [[nodiscard]] std::optional<MarketDepthSnapshot<Depth>> current_exchange_depth(MarketID market_id) const noexcept {
        static_assert(Depth == 1U, "Only depth-1 exchange depth extraction is implemented currently.");
        const MarketStateType* state = market_state(market_id);
        if (state == nullptr) {
            return std::nullopt;
        }

        MarketDepthSnapshot<Depth> snapshot {};
        snapshot.exchange_timestamp_ns = current_time_ns_;
        snapshot.receive_timestamp_ns = current_time_ns_;
        snapshot.market_id = market_id;
        state->matching_book.copy_depth(snapshot);
        return snapshot;
    }
#endif

    [[nodiscard]] bool market_data_overflowed() const noexcept {
        return market_data_overflow_;
    }

    [[nodiscard]] GatewayResult submit_order(const OrderRequest& request) noexcept {
        if (halted()) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::Halted,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }
        if (request.market_id == invalid_market_id || request.price_ticks <= 0 || request.quantity_lots == 0U) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::InvalidRequest,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }

        MarketStateType* state = mutable_market_state(request.market_id);
        if (state == nullptr) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::UnknownMarket,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }
        if (!valid_product_spec(state->product_spec)) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::InvalidRequest,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }
        if (!accounts_.has_account(request.agent_id)) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::UnknownAccount,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }

        const BalanceLots reserve_amount = required_reservation_amount(*state, request.side, request.price_ticks, request.quantity_lots);
        const AssetID reserve_asset_id = reservation_asset(*state, request.side);
        if (reserve_amount < 0) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::InvalidRequest,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }
        if (!accounts_.reserve(request.agent_id, reserve_asset_id, reserve_amount)) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::InsufficientBalance,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }

        const OrderID order_id = next_order_id_;
        if (!insert_reservation(OrderReservation {
                .order_id = order_id,
                .agent_id = request.agent_id,
                .market_id = request.market_id,
                .side = request.side,
                .reserve_asset_id = reserve_asset_id,
                .price_ticks = request.price_ticks,
                .reserved_amount_lots = reserve_amount,
            })) {
            (void)accounts_.release(request.agent_id, reserve_asset_id, reserve_amount);
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::ReservationCapacityFull,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }

        const TimeNs arrival_timestamp_ns =
            add_time_saturating(current_time_ns_, state->order_entry_latency_model.next_latency_ns());
        const bool scheduled = scheduler_.push(ScheduledEvent {
            .timestamp_ns = arrival_timestamp_ns,
            .phase = EventPhase::Market,
            .kind = EventKind::OrderArrivesAtMarket,
            .market_id = request.market_id,
            .agent_id = request.agent_id,
            .order_id = order_id,
            .side = request.side,
            .price_ticks = request.price_ticks,
            .quantity_lots = request.quantity_lots,
            .time_in_force = request.time_in_force,
            .client_timestamp_ns = request.client_timestamp_ns,
        });
        if (!scheduled) {
            release_and_remove_reservation(order_id);
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::SchedulerFull,
                .order_id = invalid_order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = arrival_timestamp_ns,
            };
        }

        ++next_order_id_;
        return GatewayResult {
            .accepted = true,
            .status = GatewayStatus::Accepted,
            .order_id = order_id,
            .market_id = request.market_id,
            .arrival_timestamp_ns = arrival_timestamp_ns,
        };
    }

    [[nodiscard]] GatewayResult cancel_order(const CancelRequest& request) noexcept {
        if (halted()) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::Halted,
                .order_id = request.order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }
        if (request.market_id == invalid_market_id || request.order_id == invalid_order_id) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::InvalidRequest,
                .order_id = request.order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }

        MarketStateType* state = mutable_market_state(request.market_id);
        if (state == nullptr) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::UnknownMarket,
                .order_id = request.order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }
        const auto* reservation = find_reservation(request.order_id);
        if (reservation == nullptr || reservation->market_id != request.market_id) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::UnknownOrder,
                .order_id = request.order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }
        if (reservation->agent_id != request.agent_id) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::Unauthorized,
                .order_id = request.order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = current_time_ns_,
            };
        }

        const TimeNs arrival_timestamp_ns =
            add_time_saturating(current_time_ns_, state->cancel_latency_model.next_latency_ns());
        const bool scheduled = scheduler_.push(ScheduledEvent {
            .timestamp_ns = arrival_timestamp_ns,
            .phase = EventPhase::Market,
            .kind = EventKind::CancelArrivesAtMarket,
            .market_id = request.market_id,
            .agent_id = request.agent_id,
            .order_id = request.order_id,
            .client_timestamp_ns = request.client_timestamp_ns,
        });
        if (!scheduled) {
            return GatewayResult {
                .accepted = false,
                .status = GatewayStatus::SchedulerFull,
                .order_id = request.order_id,
                .market_id = request.market_id,
                .arrival_timestamp_ns = arrival_timestamp_ns,
            };
        }

        return GatewayResult {
            .accepted = true,
            .status = GatewayStatus::Accepted,
            .order_id = request.order_id,
            .market_id = request.market_id,
            .arrival_timestamp_ns = arrival_timestamp_ns,
        };
    }

    [[nodiscard]] std::size_t match_result_count() const noexcept {
        return match_result_count_;
    }

    [[nodiscard]] const MatchResultType* match_result(std::size_t index) const noexcept {
        if (index >= match_result_count_) {
            return nullptr;
        }
        return &match_results_[index];
    }

    [[nodiscard]] const MatchResultType* last_match_result() const noexcept {
        if (match_result_count_ == 0U) {
            return nullptr;
        }
        return &match_results_[match_result_count_ - 1U];
    }

    [[nodiscard]] std::size_t cancel_result_count() const noexcept {
        return cancel_result_count_;
    }

    [[nodiscard]] const CancelResult* cancel_result(std::size_t index) const noexcept {
        if (index >= cancel_result_count_) {
            return nullptr;
        }
        return &cancel_results_[index];
    }

    [[nodiscard]] const CancelResult* last_cancel_result() const noexcept {
        if (cancel_result_count_ == 0U) {
            return nullptr;
        }
        return &cancel_results_[cancel_result_count_ - 1U];
    }

    [[nodiscard]] std::size_t accounting_event_count() const noexcept {
        return accounting_event_count_;
    }

    [[nodiscard]] const AccountingEvent* accounting_event(std::size_t index) const noexcept {
        if (index >= accounting_event_count_) {
            return nullptr;
        }
        return &accounting_events_[index];
    }

    [[nodiscard]] const AccountingEvent* last_accounting_event() const noexcept {
        if (accounting_event_count_ == 0U) {
            return nullptr;
        }
        return &accounting_events_[accounting_event_count_ - 1U];
    }

    [[nodiscard]] bool match_result_sink_overflowed() const noexcept {
        return match_result_sink_overflow_;
    }

    [[nodiscard]] bool cancel_result_sink_overflowed() const noexcept {
        return cancel_result_sink_overflow_;
    }

    [[nodiscard]] bool accounting_event_sink_overflowed() const noexcept {
        return accounting_event_sink_overflow_;
    }

    [[nodiscard]] bool result_sink_overflowed() const noexcept {
        return match_result_sink_overflow_ || cancel_result_sink_overflow_ || accounting_event_sink_overflow_
               || market_data_overflow_;
    }

    [[nodiscard]] std::size_t accounting_failure_count() const noexcept {
        return accounting_failure_count_;
    }

    void run_until(TimeNs end_time_ns) noexcept {
        while (status_ == SimulationStatus::Running && !scheduler_.empty()) {
            const ScheduledEvent& next = scheduler_.top();
            if (next.timestamp_ns > end_time_ns) {
                break;
            }

            ScheduledEvent event {};
            const bool popped = scheduler_.pop(event);
            if (!popped) {
                break;
            }

            current_time_ns_ = event.timestamp_ns;
            handle_event(event);
        }
    }

#ifdef LOB_SIM_TESTING
    void clear_events() noexcept {
        scheduler_.clear();
        processed_event_count_ = 0U;
    }
#endif

private:
    [[nodiscard]] MarketStateType* mutable_market_state(MarketID market_id) noexcept {
        if (static_cast<std::size_t>(market_id) >= market_state_count_) {
            return nullptr;
        }
        return &market_states_[market_id];
    }

    [[nodiscard]] bool market_product_spec_locked(MarketID market_id, const MarketStateType& state) const noexcept {
        return processed_event_count_ > 0U || scheduler_.has_event_for_market(market_id)
               || state.matching_book.resting_order_count() > 0U || has_active_reservation_for_market(market_id);
    }

    [[nodiscard]] bool has_active_reservation_for_market(MarketID market_id) const noexcept {
        for (const OrderReservation& reservation : reservations_) {
            if (reservation.active && reservation.market_id == market_id) {
                return true;
            }
        }
        return false;
    }

    void halt(SimulationStatus status) noexcept {
        if (status_ == SimulationStatus::Running) {
            status_ = status;
        }
    }

    template <std::size_t ToDepth>
    static void copy_depth_snapshot(const MarketDepthSnapshot<1U>& from, MarketDepthSnapshot<ToDepth>& to) noexcept {
        to.exchange_timestamp_ns = from.exchange_timestamp_ns;
        to.receive_timestamp_ns = from.receive_timestamp_ns;
        to.market_id = from.market_id;
        to.bids = {};
        to.asks = {};
        to.bid_count = from.bid_count < ToDepth ? from.bid_count : ToDepth;
        to.ask_count = from.ask_count < ToDepth ? from.ask_count : ToDepth;
        for (std::size_t i = 0U; i < to.bid_count; ++i) {
            to.bids[i] = from.bids[i];
        }
        for (std::size_t i = 0U; i < to.ask_count; ++i) {
            to.asks[i] = from.asks[i];
        }
    }

    [[nodiscard]] const VisibleDepthSlot* find_visible_depth_slot(AgentID agent_id, MarketID market_id) const noexcept {
        for (const VisibleDepthSlot& slot : visible_depth_slots_) {
            if (slot.active && slot.agent_id == agent_id && slot.market_id == market_id) {
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] VisibleDepthSlot* find_visible_depth_slot(AgentID agent_id, MarketID market_id) noexcept {
        for (VisibleDepthSlot& slot : visible_depth_slots_) {
            if (slot.active && slot.agent_id == agent_id && slot.market_id == market_id) {
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] VisibleDepthSlot* find_or_create_visible_depth_slot(AgentID agent_id, MarketID market_id) noexcept {
        VisibleDepthSlot* existing = find_visible_depth_slot(agent_id, market_id);
        if (existing != nullptr) {
            return existing;
        }
        for (VisibleDepthSlot& slot : visible_depth_slots_) {
            if (!slot.active) {
                slot = VisibleDepthSlot {
                    .agent_id = agent_id,
                    .market_id = market_id,
                    .snapshot = {},
                    .active = true,
                };
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] AgentSlot* find_agent_slot(AgentID agent_id) noexcept {
        for (std::size_t i = 0U; i < agent_count_; ++i) {
            AgentSlot& slot = agent_slots_[i];
            if (slot.active && slot.agent_id == agent_id) {
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const AgentSlot* find_agent_slot(AgentID agent_id) const noexcept {
        for (std::size_t i = 0U; i < agent_count_; ++i) {
            const AgentSlot& slot = agent_slots_[i];
            if (slot.active && slot.agent_id == agent_id) {
                return &slot;
            }
        }
        return nullptr;
    }

    void notify_agent_fill(const AgentFillEvent& event, AgentID agent_id) noexcept {
        AgentSlot* slot = find_agent_slot(agent_id);
        if (slot == nullptr || slot->agent == nullptr) {
            return;
        }
        AgentContextType context {*this, agent_id};
        slot->agent->on_fill(event, context);
    }

    void notify_agent_cancel(const AgentCancelEvent& event, AgentID agent_id) noexcept {
        AgentSlot* slot = find_agent_slot(agent_id);
        if (slot == nullptr || slot->agent == nullptr) {
            return;
        }
        AgentContextType context {*this, agent_id};
        slot->agent->on_cancel(event, context);
    }

    void schedule_market_data_update(MarketStateType& state, TimeNs exchange_timestamp_ns) noexcept {
        if (market_data_subscriber_count_ == 0U) {
            return;
        }
        if (scheduler_.available_capacity() < market_data_subscriber_count_) {
            market_data_overflow_ = true;
            halt(SimulationStatus::HaltedMarketDataOverflow);
            return;
        }

        MarketDepthSnapshot<1U> snapshot {};
        snapshot.exchange_timestamp_ns = exchange_timestamp_ns;
        snapshot.market_id = state.market_id;
        state.matching_book.copy_depth(snapshot);

        for (std::size_t i = 0U; i < market_data_subscriber_count_; ++i) {
            MarketDataSubscriber& subscriber = market_data_subscribers_[i];
            MarketDepthSnapshot<1U> subscriber_snapshot = snapshot;
            LatencyModel& latency_model =
                subscriber.uses_market_latency ? state.market_data_latency_model : subscriber.feed_latency_model;
            const TimeNs receive_timestamp_ns =
                add_time_saturating(exchange_timestamp_ns, latency_model.next_latency_ns());
            subscriber_snapshot.receive_timestamp_ns = receive_timestamp_ns;
            const bool scheduled = scheduler_.push(ScheduledEvent {
                .timestamp_ns = receive_timestamp_ns,
                .phase = EventPhase::PostMarket,
                .kind = EventKind::MarketDataArrives,
                .market_id = state.market_id,
                .agent_id = subscriber.agent_id,
                .depth_snapshot = subscriber_snapshot,
            });
            if (!scheduled) {
                market_data_overflow_ = true;
                halt(SimulationStatus::HaltedMarketDataOverflow);
                return;
            }
        }
    }

    [[nodiscard]] static constexpr TimeNs add_time_saturating(TimeNs lhs, TimeNs rhs) noexcept {
        if (std::numeric_limits<TimeNs>::max() - lhs < rhs) {
            return std::numeric_limits<TimeNs>::max();
        }
        return lhs + rhs;
    }

    void handle_event(const ScheduledEvent& event) noexcept {
        switch (event.kind) {
        case EventKind::OrderArrivesAtMarket:
            handle_order_arrival(event);
            break;
        case EventKind::CancelArrivesAtMarket:
            handle_cancel_arrival(event);
            break;
        case EventKind::MarketDataArrives:
            handle_market_data_arrival(event);
            break;
        case EventKind::AgentWakeup:
            handle_agent_wakeup(event);
            break;
        case EventKind::Settlement:
            break;
        }
        ++processed_event_count_;
    }

    void handle_agent_wakeup(const ScheduledEvent& event) noexcept {
        AgentSlot* slot = find_agent_slot(event.agent_id);
        if (slot == nullptr || slot->agent == nullptr) {
            ++unknown_agent_wakeup_count_;
            return;
        }
        AgentContextType context {*this, slot->agent_id};
        slot->agent->on_wakeup(context);
    }

    void handle_order_arrival(const ScheduledEvent& event) noexcept {
        MarketStateType* state = mutable_market_state(event.market_id);
        if (state == nullptr) {
            return;
        }

        const MatchResultType result = state->matching_book.submit_limit(SimOrder {
            .order_id = event.order_id,
            .agent_id = event.agent_id,
            .side = event.side,
            .price_ticks = event.price_ticks,
            .quantity_lots = event.quantity_lots,
            .timestamp_ns = event.timestamp_ns,
            .time_in_force = event.time_in_force,
        });
        append_match_result(result);
        if (halted()) {
            return;
        }
        apply_match_accounting(event, result);
        if (!halted() && result.accepted && (result.rested || result.filled_quantity_lots > 0U)) {
            schedule_market_data_update(*state, event.timestamp_ns);
        }
    }

    void handle_cancel_arrival(const ScheduledEvent& event) noexcept {
        MarketStateType* state = mutable_market_state(event.market_id);
        if (state == nullptr) {
            return;
        }

        const auto* reservation = find_reservation(event.order_id);
        if (reservation == nullptr || reservation->market_id != event.market_id || reservation->agent_id != event.agent_id) {
            append_cancel_result(CancelResult {
                .canceled = false,
                .order_id = event.order_id,
                .agent_id = event.agent_id,
                .status = OrderStatus::Rejected,
            });
            if (!halted()) {
                notify_agent_cancel(AgentCancelEvent {
                    .timestamp_ns = event.timestamp_ns,
                    .market_id = event.market_id,
                    .order_id = event.order_id,
                    .canceled = false,
                    .canceled_quantity_lots = 0U,
                }, event.agent_id);
            }
            return;
        }

        const CancelResult result = state->matching_book.cancel(event.order_id);
        append_cancel_result(result);
        if (halted()) {
            return;
        }
        if (result.canceled) {
            release_and_remove_reservation(event.order_id);
            if (!halted()) {
                notify_agent_cancel(AgentCancelEvent {
                    .timestamp_ns = event.timestamp_ns,
                    .market_id = event.market_id,
                    .order_id = event.order_id,
                    .canceled = true,
                    .canceled_quantity_lots = result.canceled_quantity_lots,
                }, event.agent_id);
                schedule_market_data_update(*state, event.timestamp_ns);
            }
        } else {
            notify_agent_cancel(AgentCancelEvent {
                .timestamp_ns = event.timestamp_ns,
                .market_id = event.market_id,
                .order_id = event.order_id,
                .canceled = false,
                .canceled_quantity_lots = 0U,
            }, event.agent_id);
        }
    }

    void handle_market_data_arrival(const ScheduledEvent& event) noexcept {
        VisibleDepthSlot* slot = find_or_create_visible_depth_slot(event.agent_id, event.market_id);
        if (slot == nullptr) {
            market_data_overflow_ = true;
            halt(SimulationStatus::HaltedMarketDataOverflow);
            return;
        }
        slot->snapshot = event.depth_snapshot;
        slot->snapshot.receive_timestamp_ns = event.timestamp_ns;
    }

    void append_match_result(const MatchResultType& result) noexcept {
        if (match_result_count_ < ResultSinkCapacity) {
            match_results_[match_result_count_++] = result;
        } else {
            match_result_sink_overflow_ = true;
            halt(SimulationStatus::HaltedSinkOverflow);
        }
    }

    void append_cancel_result(const CancelResult& result) noexcept {
        if (cancel_result_count_ < ResultSinkCapacity) {
            cancel_results_[cancel_result_count_++] = result;
        } else {
            cancel_result_sink_overflow_ = true;
            halt(SimulationStatus::HaltedSinkOverflow);
        }
    }

#ifdef LOB_SIM_TESTING
public:
    struct ReservationView {
        OrderID order_id {invalid_order_id};
        AgentID agent_id {invalid_agent_id};
        MarketID market_id {invalid_market_id};
        Side side {Side::Buy};
        AssetID reserve_asset_id {};
        PriceTicks price_ticks {};
        BalanceLots reserved_amount_lots {};
    };

    [[nodiscard]] std::size_t reservation_count() const noexcept {
        std::size_t count {};
        for (const OrderReservation& reservation : reservations_) {
            if (reservation.active) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::optional<ReservationView> reservation_by_order_id(OrderID order_id) const noexcept {
        const OrderReservation* reservation = find_reservation(order_id);
        if (reservation == nullptr) {
            return std::nullopt;
        }
        return ReservationView {
            .order_id = reservation->order_id,
            .agent_id = reservation->agent_id,
            .market_id = reservation->market_id,
            .side = reservation->side,
            .reserve_asset_id = reservation->reserve_asset_id,
            .price_ticks = reservation->price_ticks,
            .reserved_amount_lots = reservation->reserved_amount_lots,
        };
    }

    [[nodiscard]] BalanceLots total_reserved_lots(AgentID agent_id, AssetID asset_id) const noexcept {
        BalanceLots total {};
        for (const OrderReservation& reservation : reservations_) {
            if (reservation.active && reservation.agent_id == agent_id && reservation.reserve_asset_id == asset_id) {
                total += reservation.reserved_amount_lots;
            }
        }
        return total;
    }

    [[nodiscard]] bool accounting_invariants_hold() const noexcept {
        for (std::size_t i = 0U; i < accounts_.account_snapshot_count(); ++i) {
            const auto snapshot = accounts_.account_snapshot(i);
            if (!snapshot.has_value()) {
                return false;
            }
            if (snapshot->balance.reserved_lots < 0) {
                return false;
            }
            if (snapshot->balance.reserved_lots != total_reserved_lots(snapshot->agent_id, snapshot->asset_id)) {
                return false;
            }
        }

        for (const OrderReservation& reservation : reservations_) {
            if (!reservation.active) {
                continue;
            }
            const MarketStateType* state = market_state(reservation.market_id);
            if (state == nullptr || !accounts_.has_account(reservation.agent_id)) {
                return false;
            }
            if (reservation.reserve_asset_id != reservation_asset(*state, reservation.side)) {
                return false;
            }
            if (reservation.reserved_amount_lots < 0) {
                return false;
            }
            if (scheduler_.empty() && !state->matching_book.remaining_quantity(reservation.order_id).has_value()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] MarketStateType* market_state_for_testing(MarketID market_id) noexcept {
        return mutable_market_state(market_id);
    }

private:
#endif

    [[nodiscard]] static constexpr bool add_overflows(BalanceLots lhs, BalanceLots rhs) noexcept {
        if (rhs > 0 && lhs > std::numeric_limits<BalanceLots>::max() - rhs) {
            return true;
        }
        if (rhs < 0 && lhs < std::numeric_limits<BalanceLots>::min() - rhs) {
            return true;
        }
        return false;
    }

    [[nodiscard]] static constexpr bool valid_asset_id(AssetID asset_id) noexcept {
        return static_cast<std::size_t>(asset_id) < AssetCapacity;
    }

    [[nodiscard]] static constexpr bool valid_product_spec(const ProductSpec& spec) noexcept {
        return valid_asset_id(spec.base_asset_id) && valid_asset_id(spec.quote_asset_id)
               && spec.base_asset_id != spec.quote_asset_id;
    }

    void record_accounting_failure() noexcept {
        ++accounting_failure_count_;
        halt(SimulationStatus::HaltedAccountingFailure);
    }

    void record_accounting_sink_overflow() noexcept {
        accounting_event_sink_overflow_ = true;
        ++accounting_failure_count_;
        halt(SimulationStatus::HaltedSinkOverflow);
    }

    [[nodiscard]] static constexpr BalanceLots fee_lots(BalanceLots notional_lots, std::int64_t fee_bps) noexcept {
        if (notional_lots <= 0 || fee_bps == 0) {
            return 0;
        }

        const std::uint64_t abs_fee_bps =
            fee_bps < 0 ? static_cast<std::uint64_t>(-(fee_bps + 1)) + 1U : static_cast<std::uint64_t>(fee_bps);
        if (abs_fee_bps == 0U) {
            return 0;
        }
        if (static_cast<std::uint64_t>(notional_lots)
            > static_cast<std::uint64_t>(std::numeric_limits<BalanceLots>::max()) / abs_fee_bps) {
            return fee_bps > 0 ? std::numeric_limits<BalanceLots>::max()
                               : std::numeric_limits<BalanceLots>::min() + 1;
        }

        const auto abs_fee_lots = static_cast<BalanceLots>(
            (static_cast<std::uint64_t>(notional_lots) * abs_fee_bps + 9999U) / 10000U);
        return fee_bps > 0 ? abs_fee_lots : -abs_fee_lots;
    }

    [[nodiscard]] static constexpr BalanceLots notional_lots(PriceTicks price_ticks, QtyLots quantity_lots) noexcept {
        if (price_ticks <= 0) {
            return -1;
        }
        if (quantity_lots > static_cast<QtyLots>(std::numeric_limits<BalanceLots>::max() / price_ticks)) {
            return -1;
        }
        return static_cast<BalanceLots>(price_ticks * static_cast<PriceTicks>(quantity_lots));
    }

    [[nodiscard]] static constexpr std::int64_t max_positive_fee_bps(const ProductSpec& spec) noexcept {
        const std::int64_t maker_fee_bps = spec.maker_fee_bps > 0 ? spec.maker_fee_bps : 0;
        const std::int64_t taker_fee_bps = spec.taker_fee_bps > 0 ? spec.taker_fee_bps : 0;
        return maker_fee_bps > taker_fee_bps ? maker_fee_bps : taker_fee_bps;
    }

    [[nodiscard]] static constexpr AssetID reservation_asset(const MarketStateType& state, Side side) noexcept {
        return side == Side::Buy ? state.product_spec.quote_asset_id : state.product_spec.base_asset_id;
    }

    [[nodiscard]] static constexpr BalanceLots required_reservation_amount(const MarketStateType& state,
                                                                            Side side,
                                                                            PriceTicks price_ticks,
                                                                            QtyLots quantity_lots) noexcept {
        if (side == Side::Sell) {
            if (quantity_lots > static_cast<QtyLots>(std::numeric_limits<BalanceLots>::max())) {
                return -1;
            }
            return static_cast<BalanceLots>(quantity_lots);
        }

        const BalanceLots notional = notional_lots(price_ticks, quantity_lots);
        if (notional < 0) {
            return -1;
        }
        const BalanceLots fee = fee_lots(notional, max_positive_fee_bps(state.product_spec));
        if (add_overflows(notional, fee)) {
            return -1;
        }
        return notional + fee;
    }

    [[nodiscard]] OrderReservation* find_reservation(OrderID order_id) noexcept {
        for (OrderReservation& reservation : reservations_) {
            if (reservation.active && reservation.order_id == order_id) {
                return &reservation;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const OrderReservation* find_reservation(OrderID order_id) const noexcept {
        for (const OrderReservation& reservation : reservations_) {
            if (reservation.active && reservation.order_id == order_id) {
                return &reservation;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool insert_reservation(OrderReservation reservation) noexcept {
        for (OrderReservation& slot : reservations_) {
            if (!slot.active) {
                reservation.active = true;
                slot = reservation;
                return true;
            }
        }
        return false;
    }

    bool release_and_remove_reservation(OrderID order_id) noexcept {
        OrderReservation* reservation = find_reservation(order_id);
        if (reservation == nullptr) {
            return true;
        }
        const bool released =
            accounts_.release(reservation->agent_id, reservation->reserve_asset_id, reservation->reserved_amount_lots);
        if (!released) {
            record_accounting_failure();
            return false;
        }
        *reservation = {};
        return true;
    }

    void remove_empty_reservation(OrderReservation& reservation) noexcept {
        if (reservation.reserved_amount_lots == 0) {
            reservation = {};
        }
    }

    [[nodiscard]] bool consume_fill_reservation(OrderID order_id,
                                  BalanceLots actual_consumed_lots,
                                  BalanceLots reserved_for_fill_lots) noexcept {
        OrderReservation* reservation = find_reservation(order_id);
        if (reservation == nullptr) {
            record_accounting_failure();
            return false;
        }
        if (actual_consumed_lots < 0 || reserved_for_fill_lots < actual_consumed_lots
            || actual_consumed_lots > reservation->reserved_amount_lots) {
            record_accounting_failure();
            return false;
        }

        if (!accounts_.consume_reserved(reservation->agent_id, reservation->reserve_asset_id, actual_consumed_lots)) {
            record_accounting_failure();
            return false;
        }
        reservation->reserved_amount_lots -= actual_consumed_lots;

        const BalanceLots release_amount = reserved_for_fill_lots - actual_consumed_lots;
        if (release_amount > 0) {
            if (release_amount > reservation->reserved_amount_lots
                || !accounts_.release(reservation->agent_id, reservation->reserve_asset_id, release_amount)) {
                record_accounting_failure();
                return false;
            }
            reservation->reserved_amount_lots -= release_amount;
        }

        remove_empty_reservation(*reservation);
        return true;
    }

    void apply_match_accounting(const ScheduledEvent& event, const MatchResultType& result) noexcept {
        MarketStateType* state = mutable_market_state(event.market_id);
        if (state == nullptr) {
            return;
        }

        for (std::size_t i = 0U; i < result.execution_count; ++i) {
            const TradeExecution& execution = result.executions[i];
            if (execution.liquidity_role != LiquidityRole::Taker) {
                continue;
            }
            if (!apply_fill_accounting(*state, execution)) {
                return;
            }
        }

        if (!result.rested) {
            release_and_remove_reservation(event.order_id);
        }
    }

    [[nodiscard]] bool apply_fill_accounting(MarketStateType& state, const TradeExecution& taker_execution) noexcept {
        if (accounting_event_count_ == AccountingSinkCapacity) {
            record_accounting_sink_overflow();
            return false;
        }

        const BalanceLots notional = notional_lots(taker_execution.price_ticks, taker_execution.quantity_lots);
        if (notional < 0) {
            record_accounting_failure();
            return false;
        }

        const bool taker_is_buyer = taker_execution.side == Side::Buy;
        const AgentID buyer_agent_id = taker_is_buyer ? taker_execution.agent_id : taker_execution.counterparty_agent_id;
        const AgentID seller_agent_id = taker_is_buyer ? taker_execution.counterparty_agent_id : taker_execution.agent_id;
        const OrderID buyer_order_id = taker_is_buyer ? taker_execution.order_id : taker_execution.counterparty_order_id;
        const OrderID seller_order_id = taker_is_buyer ? taker_execution.counterparty_order_id : taker_execution.order_id;
        const BalanceLots buyer_fee = fee_lots(notional, taker_is_buyer ? state.product_spec.taker_fee_bps
                                                                         : state.product_spec.maker_fee_bps);
        const BalanceLots seller_fee = fee_lots(notional, taker_is_buyer ? state.product_spec.maker_fee_bps
                                                                          : state.product_spec.taker_fee_bps);
        if (add_overflows(notional, buyer_fee) || add_overflows(notional, -seller_fee)) {
            record_accounting_failure();
            return false;
        }

        const OrderReservation* buyer_reservation = find_reservation(buyer_order_id);
        const PriceTicks buyer_limit_price = buyer_reservation == nullptr ? taker_execution.price_ticks
                                                                          : buyer_reservation->price_ticks;
        const BalanceLots buyer_reserved_for_fill =
            required_reservation_amount(state, Side::Buy, buyer_limit_price, taker_execution.quantity_lots);
        if (buyer_reserved_for_fill < 0) {
            record_accounting_failure();
            return false;
        }
        if (!consume_fill_reservation(buyer_order_id, notional + buyer_fee, buyer_reserved_for_fill)) {
            return false;
        }
        if (!consume_fill_reservation(seller_order_id, static_cast<BalanceLots>(taker_execution.quantity_lots),
                                      static_cast<BalanceLots>(taker_execution.quantity_lots))) {
            return false;
        }

        if (!accounts_.apply_delta(buyer_agent_id, state.product_spec.base_asset_id,
                                   static_cast<BalanceLots>(taker_execution.quantity_lots))) {
            record_accounting_failure();
            return false;
        }
        if (!accounts_.apply_delta(seller_agent_id, state.product_spec.quote_asset_id, notional - seller_fee)) {
            record_accounting_failure();
            return false;
        }
        append_accounting_event(AccountingEvent {
            .timestamp_ns = taker_execution.timestamp_ns,
            .market_id = state.market_id,
            .buyer_agent_id = buyer_agent_id,
            .seller_agent_id = seller_agent_id,
            .price_ticks = taker_execution.price_ticks,
            .quantity_lots = taker_execution.quantity_lots,
            .buyer_fee_lots = buyer_fee,
            .seller_fee_lots = seller_fee,
        });
        if (halted()) {
            return false;
        }

        notify_agent_fill(AgentFillEvent {
            .timestamp_ns = taker_execution.timestamp_ns,
            .market_id = state.market_id,
            .order_id = buyer_order_id,
            .side = Side::Buy,
            .liquidity_role = taker_is_buyer ? LiquidityRole::Taker : LiquidityRole::Maker,
            .price_ticks = taker_execution.price_ticks,
            .quantity_lots = taker_execution.quantity_lots,
            .fee_lots = buyer_fee,
        }, buyer_agent_id);
        notify_agent_fill(AgentFillEvent {
            .timestamp_ns = taker_execution.timestamp_ns,
            .market_id = state.market_id,
            .order_id = seller_order_id,
            .side = Side::Sell,
            .liquidity_role = taker_is_buyer ? LiquidityRole::Maker : LiquidityRole::Taker,
            .price_ticks = taker_execution.price_ticks,
            .quantity_lots = taker_execution.quantity_lots,
            .fee_lots = seller_fee,
        }, seller_agent_id);
        return true;
    }

    void append_accounting_event(const AccountingEvent& event) noexcept {
        if (accounting_event_count_ < AccountingSinkCapacity) {
            accounting_events_[accounting_event_count_++] = event;
        } else {
            record_accounting_sink_overflow();
        }
    }

    TimeNs current_time_ns_ {};
    EventScheduler<SchedulerCapacity> scheduler_ {};
    MarketRegistry market_registry_ {};
    std::array<MarketStateType, MarketCapacity> market_states_ {};
    std::size_t market_state_count_ {};
    AccountStoreType accounts_ {};
    std::array<OrderReservation, ReservationCapacity> reservations_ {};
    OrderID next_order_id_ {1U};
    std::array<MatchResultType, ResultSinkCapacity> match_results_ {};
    std::array<CancelResult, ResultSinkCapacity> cancel_results_ {};
    std::array<AccountingEvent, AccountingSinkCapacity> accounting_events_ {};
    std::array<MarketDataSubscriber, MarketDataSubscriberCapacity> market_data_subscribers_ {};
    std::array<VisibleDepthSlot, VisibleDepthCapacity> visible_depth_slots_ {};
    std::array<AgentSlot, AgentCapacity> agent_slots_ {};
    SimulationStatus status_ {SimulationStatus::Running};
    std::size_t match_result_count_ {};
    std::size_t cancel_result_count_ {};
    std::size_t accounting_event_count_ {};
    std::size_t market_data_subscriber_count_ {};
    std::size_t agent_count_ {};
    bool match_result_sink_overflow_ {};
    bool cancel_result_sink_overflow_ {};
    bool accounting_event_sink_overflow_ {};
    bool market_data_overflow_ {};
    bool agents_started_ {};
    std::size_t accounting_failure_count_ {};
    std::size_t processed_event_count_ {};
    std::size_t unknown_agent_wakeup_count_ {};
};

}  // namespace lob::sim
