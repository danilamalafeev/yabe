#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include "lob/sim/simulation.hpp"

namespace {

static_assert(std::is_same_v<decltype(std::declval<lob::sim::SimulationEngine<8U>&>().market_registry()),
                             const lob::sim::MarketRegistry&>);
static_assert(std::is_same_v<decltype(std::declval<lob::sim::SimulationEngine<8U>&>().scheduler()),
                             const lob::sim::EventScheduler<8U>&>);

[[nodiscard]] lob::sim::ScheduledEvent make_event(lob::sim::TimeNs timestamp_ns,
                                                  lob::sim::EventPhase phase = lob::sim::EventPhase::Agent) {
    return lob::sim::ScheduledEvent {
        .timestamp_ns = timestamp_ns,
        .phase = phase,
        .kind = lob::sim::EventKind::AgentWakeup,
    };
}

using TestMatchingBook = lob::sim::MatchingBook<16U, 8U, 16U>;
using TestSimulationEngine = lob::sim::SimulationEngine<16U, 2U, 16U, 8U, 16U>;
using TestAgentContext = lob::sim::AgentContext<TestSimulationEngine>;

static_assert(std::is_same_v<decltype(std::declval<TestSimulationEngine&>().market_state(lob::sim::MarketID {})),
                             const TestSimulationEngine::MarketStateType*>);

template <typename T>
concept HasSchedulerAccessor = requires(T& context) {
    context.scheduler();
};

template <typename T>
concept HasMarketStateAccessor = requires(T& context, lob::sim::MarketID market_id) {
    context.market_state(market_id);
};

template <typename T>
concept HasCurrentExchangeDepthAccessor = requires(T& context, lob::sim::MarketID market_id) {
    context.template current_exchange_depth<1U>(market_id);
};

template <typename T>
concept HasMarketLatencySetter = requires(T& context, lob::sim::MarketID market_id) {
    context.set_market_latency(market_id, 1U);
};

template <typename T>
concept HasAccountStoreAccessor = requires(T& context) {
    context.account_store();
};

static_assert(!HasSchedulerAccessor<TestAgentContext>);
static_assert(!HasMarketStateAccessor<TestAgentContext>);
static_assert(!HasCurrentExchangeDepthAccessor<TestAgentContext>);
static_assert(!HasMarketLatencySetter<TestAgentContext>);
static_assert(!HasAccountStoreAccessor<TestAgentContext>);

[[nodiscard]] lob::sim::SimOrder make_order(lob::sim::OrderID order_id,
                                            lob::Side side,
                                            std::int64_t price_ticks,
                                            std::uint64_t quantity_lots,
                                            lob::sim::AgentID agent_id = 1U,
                                            lob::sim::TimeNs timestamp_ns = 1U) {
    return lob::sim::SimOrder {
        .order_id = order_id,
        .agent_id = agent_id,
        .side = side,
        .price_ticks = price_ticks,
        .quantity_lots = quantity_lots,
        .timestamp_ns = timestamp_ns,
    };
}

template <typename Engine>
void register_funded_account(Engine& engine,
                             lob::sim::AgentID agent_id,
                             lob::sim::BalanceLots base_lots = 1000,
                             lob::sim::BalanceLots quote_lots = 1'000'000) {
    ASSERT_TRUE(engine.register_account(agent_id));
    ASSERT_TRUE(engine.set_balance(agent_id, 0U, base_lots));
    ASSERT_TRUE(engine.set_balance(agent_id, 1U, quote_lots));
}

template <typename Engine>
void register_empty_account(Engine& engine, lob::sim::AgentID agent_id) {
    ASSERT_TRUE(engine.register_account(agent_id));
    ASSERT_TRUE(engine.set_balance(agent_id, 0U, 0));
    ASSERT_TRUE(engine.set_balance(agent_id, 1U, 0));
}

template <typename Engine>
void expect_accounting_invariants(const Engine& engine) {
    EXPECT_TRUE(engine.accounting_invariants_hold());
    EXPECT_EQ(engine.accounting_failure_count(), 0U);
    EXPECT_EQ(engine.status(), lob::sim::SimulationStatus::Running);
}

class StartCountingAgent final : public TestSimulationEngine::MarketAgentType {
public:
    void on_start(TestSimulationEngine::AgentContextType& context) override {
        ++start_count;
        last_agent_id = context.agent_id();
        last_time_ns = context.current_time_ns();
    }

    std::size_t start_count {};
    lob::sim::AgentID last_agent_id {lob::sim::invalid_agent_id};
    lob::sim::TimeNs last_time_ns {};
};

class WakeupSubmitAgent final : public TestSimulationEngine::MarketAgentType {
public:
    explicit WakeupSubmitAgent(lob::sim::MarketID market_id_in) noexcept
        : market_id(market_id_in) {}

    void on_start(TestSimulationEngine::AgentContextType& context) override {
        start_scheduled = context.schedule_wakeup(wakeup_time_ns);
    }

    void on_wakeup(TestSimulationEngine::AgentContextType& context) override {
        ++wakeup_count;
        last_wakeup_time_ns = context.current_time_ns();
        submit_result = context.submit_order(lob::sim::OrderRequest {
            .agent_id = 999U,
            .market_id = market_id,
            .side = lob::Side::Buy,
            .price_ticks = 100,
            .quantity_lots = 2U,
        });
    }

    lob::sim::MarketID market_id {lob::sim::invalid_market_id};
    lob::sim::TimeNs wakeup_time_ns {7U};
    bool start_scheduled {};
    std::size_t wakeup_count {};
    lob::sim::TimeNs last_wakeup_time_ns {};
    lob::sim::GatewayResult submit_result {};
};

class FillCountingAgent final : public TestSimulationEngine::MarketAgentType {
public:
    void on_fill(const lob::sim::AgentFillEvent& event, TestSimulationEngine::AgentContextType& context) override {
        ++fill_count;
        last_fill = event;
        last_agent_id = context.agent_id();
        last_time_ns = context.current_time_ns();
    }

    std::size_t fill_count {};
    lob::sim::AgentFillEvent last_fill {};
    lob::sim::AgentID last_agent_id {lob::sim::invalid_agent_id};
    lob::sim::TimeNs last_time_ns {};
};

class CancelCountingAgent final : public TestSimulationEngine::MarketAgentType {
public:
    void on_cancel(const lob::sim::AgentCancelEvent& event, TestSimulationEngine::AgentContextType& context) override {
        ++cancel_count;
        last_cancel = event;
        last_agent_id = context.agent_id();
        last_time_ns = context.current_time_ns();
        quote_balance_at_callback = context.account_balance(1U);
    }

    std::size_t cancel_count {};
    lob::sim::AgentCancelEvent last_cancel {};
    lob::sim::AgentID last_agent_id {lob::sim::invalid_agent_id};
    lob::sim::TimeNs last_time_ns {};
    std::optional<lob::sim::AccountBalance> quote_balance_at_callback {};
};

TEST(EventSchedulerTest, PopsEventsInTimestampOrder) {
    lob::sim::EventScheduler<8U> scheduler {};

    ASSERT_TRUE(scheduler.push(make_event(30U)));
    ASSERT_TRUE(scheduler.push(make_event(10U)));
    ASSERT_TRUE(scheduler.push(make_event(20U)));

    lob::sim::ScheduledEvent event {};
    ASSERT_TRUE(scheduler.pop(event));
    EXPECT_EQ(event.timestamp_ns, 10U);
    ASSERT_TRUE(scheduler.pop(event));
    EXPECT_EQ(event.timestamp_ns, 20U);
    ASSERT_TRUE(scheduler.pop(event));
    EXPECT_EQ(event.timestamp_ns, 30U);
    EXPECT_TRUE(scheduler.empty());
}

TEST(EventSchedulerTest, SameTimestampOrdersByPhaseThenSequenceNumber) {
    lob::sim::EventScheduler<8U> scheduler {};

    ASSERT_TRUE(scheduler.push(make_event(10U, lob::sim::EventPhase::Market)));
    ASSERT_TRUE(scheduler.push(make_event(10U, lob::sim::EventPhase::Agent)));
    ASSERT_TRUE(scheduler.push(make_event(10U, lob::sim::EventPhase::Agent)));
    ASSERT_TRUE(scheduler.push(make_event(10U, lob::sim::EventPhase::PreMarket)));

    lob::sim::ScheduledEvent event {};
    ASSERT_TRUE(scheduler.pop(event));
    EXPECT_EQ(event.phase, lob::sim::EventPhase::PreMarket);
    EXPECT_EQ(event.sequence_number, 3U);

    ASSERT_TRUE(scheduler.pop(event));
    EXPECT_EQ(event.phase, lob::sim::EventPhase::Agent);
    EXPECT_EQ(event.sequence_number, 1U);

    ASSERT_TRUE(scheduler.pop(event));
    EXPECT_EQ(event.phase, lob::sim::EventPhase::Agent);
    EXPECT_EQ(event.sequence_number, 2U);

    ASSERT_TRUE(scheduler.pop(event));
    EXPECT_EQ(event.phase, lob::sim::EventPhase::Market);
    EXPECT_EQ(event.sequence_number, 0U);
}

TEST(EventSchedulerTest, CapacityOverflowReturnsFalse) {
    lob::sim::EventScheduler<2U> scheduler {};

    EXPECT_TRUE(scheduler.push(make_event(1U)));
    EXPECT_TRUE(scheduler.push(make_event(2U)));
    EXPECT_FALSE(scheduler.push(make_event(3U)));
    EXPECT_EQ(scheduler.size(), 2U);
}

TEST(LatencyModelTest, FixedLatencyIsDeterministic) {
    lob::sim::LatencyModel latency {lob::sim::LatencyDistribution::Fixed, 17U, 99U, 123U};

    EXPECT_EQ(latency.next_latency_ns(), 17U);
    EXPECT_EQ(latency.next_latency_ns(), 17U);
    EXPECT_EQ(latency.distribution(), lob::sim::LatencyDistribution::Fixed);
}

TEST(LatencyModelTest, UniformJitterIsDeterministicWithSeed) {
    lob::sim::LatencyModel first {lob::sim::LatencyDistribution::Uniform, 10U, 5U, 42U};
    lob::sim::LatencyModel second {lob::sim::LatencyDistribution::Uniform, 10U, 5U, 42U};

    for (std::size_t i = 0U; i < 8U; ++i) {
        const auto lhs = first.next_latency_ns();
        const auto rhs = second.next_latency_ns();
        EXPECT_EQ(lhs, rhs);
        EXPECT_GE(lhs, 10U);
        EXPECT_LE(lhs, 15U);
    }
}

TEST(LatencyModelTest, ExponentialJitterIsDeterministicAndNonnegative) {
    auto first = lob::sim::LatencyModel::exponential(10U, 50.0, 42U);
    auto second = lob::sim::LatencyModel::exponential(10U, 50.0, 42U);

    ASSERT_TRUE(first.valid_config());
    ASSERT_TRUE(second.valid_config());
    for (std::size_t i = 0U; i < 8U; ++i) {
        const auto lhs = first.next_latency_ns();
        const auto rhs = second.next_latency_ns();
        EXPECT_EQ(lhs, rhs);
        EXPECT_GE(lhs, 10U);
    }
}

TEST(LatencyModelTest, LogNormalJitterIsDeterministicAndNonnegative) {
    auto first = lob::sim::LatencyModel::lognormal(10U, 3.0, 0.4, 42U);
    auto second = lob::sim::LatencyModel::lognormal(10U, 3.0, 0.4, 42U);

    ASSERT_TRUE(first.valid_config());
    ASSERT_TRUE(second.valid_config());
    for (std::size_t i = 0U; i < 8U; ++i) {
        const auto lhs = first.next_latency_ns();
        const auto rhs = second.next_latency_ns();
        EXPECT_EQ(lhs, rhs);
        EXPECT_GE(lhs, 10U);
    }
}

TEST(LatencyModelTest, SameSeedProducesSameSequenceAndDifferentSeedsDiffer) {
    auto first = lob::sim::LatencyModel::exponential(0U, 1000.0, 7U);
    auto second = lob::sim::LatencyModel::exponential(0U, 1000.0, 7U);
    auto different = lob::sim::LatencyModel::exponential(0U, 1000.0, 8U);
    std::array<lob::sim::TimeNs, 6U> first_values {};
    std::array<lob::sim::TimeNs, 6U> second_values {};
    std::array<lob::sim::TimeNs, 6U> different_values {};

    for (std::size_t i = 0U; i < first_values.size(); ++i) {
        first_values[i] = first.next_latency_ns();
        second_values[i] = second.next_latency_ns();
        different_values[i] = different.next_latency_ns();
    }

    EXPECT_EQ(first_values, second_values);
    EXPECT_NE(first_values, different_values);
}

TEST(AgentContextTest, FacadeBindsOrderAndCancelToAgentAndOwnAccount) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 3U, .product_id = 301U});
    register_funded_account(engine, 1U);
    register_funded_account(engine, 2U);
    ASSERT_TRUE(engine.register_market_data_subscriber(1U, lob::sim::LatencyModel::fixed(0U)));

    TestAgentContext context {engine, 1U};
    EXPECT_EQ(context.current_time_ns(), 0U);
    ASSERT_TRUE(context.account_balance(1U).has_value());
    EXPECT_EQ(context.account_balance(1U)->available_lots, 1'000'000);

    const auto order = context.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 1U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);

    const auto reservation = engine.reservation_by_order_id(order.order_id);
    ASSERT_TRUE(reservation.has_value());
    EXPECT_EQ(reservation->agent_id, 1U);

    const auto depth = context.visible_depth<1U>(market_id);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->bid_count, 1U);

    const auto cancel = context.cancel_order(lob::sim::CancelRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .order_id = order.order_id,
    });
    ASSERT_TRUE(cancel.accepted);
    engine.run_until(0U);
    EXPECT_FALSE(engine.reservation_by_order_id(order.order_id).has_value());
    expect_accounting_invariants(engine);
}

TEST(MarketAgentTest, RegisteredAgentReceivesStartAndDuplicateRegistrationFails) {
    TestSimulationEngine engine {};
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    StartCountingAgent first {};
    StartCountingAgent duplicate {};
    StartCountingAgent missing_account {};

    EXPECT_TRUE(engine.register_agent(1U, first));
    EXPECT_FALSE(engine.register_agent(1U, duplicate));
    EXPECT_FALSE(engine.register_agent(3U, missing_account));
    EXPECT_EQ(engine.agent_count(), 1U);

    EXPECT_TRUE(engine.start());
    EXPECT_EQ(first.start_count, 1U);
    EXPECT_EQ(first.last_agent_id, 1U);
    EXPECT_EQ(first.last_time_ns, 0U);

    EXPECT_TRUE(engine.start());
    EXPECT_EQ(first.start_count, 1U);
    EXPECT_FALSE(engine.register_agent(2U, duplicate));
    expect_accounting_invariants(engine);
}

TEST(MarketAgentTest, WakeupFiresAtSimulatedTimeAndSubmitUsesBoundAgent) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 3U, .product_id = 302U});
    register_funded_account(engine, 1U);
    WakeupSubmitAgent agent {market_id};

    ASSERT_TRUE(engine.register_agent(1U, agent));
    ASSERT_TRUE(engine.start());
    EXPECT_TRUE(agent.start_scheduled);

    engine.run_until(6U);
    EXPECT_EQ(agent.wakeup_count, 0U);

    engine.run_until(7U);
    EXPECT_EQ(agent.wakeup_count, 1U);
    EXPECT_EQ(agent.last_wakeup_time_ns, 7U);
    ASSERT_TRUE(agent.submit_result.accepted);
    EXPECT_EQ(agent.submit_result.arrival_timestamp_ns, 7U);

    engine.run_until(7U);
    const auto reservation = engine.reservation_by_order_id(agent.submit_result.order_id);
    ASSERT_TRUE(reservation.has_value());
    EXPECT_EQ(reservation->agent_id, 1U);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(agent.submit_result.order_id), 2U);
    expect_accounting_invariants(engine);
}

TEST(MarketAgentTest, FillNotificationsReachMakerAndTakerWithPerspective) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 3U, .product_id = 303U});
    ASSERT_TRUE(engine.set_market_product_spec(market_id, lob::sim::ProductSpec {
        .base_asset_id = 0U,
        .quote_asset_id = 1U,
        .maker_fee_bps = 2,
        .taker_fee_bps = 5,
    }));
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 10));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 10'000));
    FillCountingAgent maker {};
    FillCountingAgent taker {};
    ASSERT_TRUE(engine.register_agent(1U, maker));
    ASSERT_TRUE(engine.register_agent(2U, taker));
    ASSERT_TRUE(engine.start());

    const auto maker_order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(maker_order.accepted);
    engine.run_until(0U);

    const auto taker_order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(taker_order.accepted);
    engine.run_until(0U);

    ASSERT_EQ(maker.fill_count, 1U);
    EXPECT_EQ(maker.last_agent_id, 1U);
    EXPECT_EQ(maker.last_fill.timestamp_ns, 0U);
    EXPECT_EQ(maker.last_fill.market_id, market_id);
    EXPECT_EQ(maker.last_fill.order_id, maker_order.order_id);
    EXPECT_EQ(maker.last_fill.side, lob::Side::Sell);
    EXPECT_EQ(maker.last_fill.liquidity_role, lob::sim::LiquidityRole::Maker);
    EXPECT_EQ(maker.last_fill.price_ticks, 100);
    EXPECT_EQ(maker.last_fill.quantity_lots, 3U);
    EXPECT_EQ(maker.last_fill.fee_lots, 1);

    ASSERT_EQ(taker.fill_count, 1U);
    EXPECT_EQ(taker.last_agent_id, 2U);
    EXPECT_EQ(taker.last_fill.timestamp_ns, 0U);
    EXPECT_EQ(taker.last_fill.market_id, market_id);
    EXPECT_EQ(taker.last_fill.order_id, taker_order.order_id);
    EXPECT_EQ(taker.last_fill.side, lob::Side::Buy);
    EXPECT_EQ(taker.last_fill.liquidity_role, lob::sim::LiquidityRole::Taker);
    EXPECT_EQ(taker.last_fill.price_ticks, 100);
    EXPECT_EQ(taker.last_fill.quantity_lots, 3U);
    EXPECT_EQ(taker.last_fill.fee_lots, 1);
    expect_accounting_invariants(engine);
}

TEST(MarketAgentTest, CancelNotificationDeliveredToOwner) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 3U, .product_id = 304U});
    register_funded_account(engine, 1U);
    CancelCountingAgent agent {};
    ASSERT_TRUE(engine.register_agent(1U, agent));
    ASSERT_TRUE(engine.start());

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    }).accepted);
    engine.run_until(0U);

    ASSERT_EQ(agent.cancel_count, 1U);
    EXPECT_EQ(agent.last_agent_id, 1U);
    EXPECT_EQ(agent.last_cancel.timestamp_ns, 0U);
    EXPECT_EQ(agent.last_cancel.market_id, market_id);
    EXPECT_EQ(agent.last_cancel.order_id, order.order_id);
    EXPECT_TRUE(agent.last_cancel.canceled);
    EXPECT_EQ(agent.last_cancel.canceled_quantity_lots, 5U);
    ASSERT_TRUE(agent.quote_balance_at_callback.has_value());
    EXPECT_EQ(agent.quote_balance_at_callback->available_lots, 1'000'000);
    EXPECT_EQ(agent.quote_balance_at_callback->reserved_lots, 0);
    expect_accounting_invariants(engine);
}

TEST(MarketAgentTest, UnknownAgentWakeupIsDroppedAndCounted) {
    TestSimulationEngine engine {};
    ASSERT_TRUE(engine.schedule_event(lob::sim::ScheduledEvent {
        .timestamp_ns = 5U,
        .phase = lob::sim::EventPhase::Agent,
        .kind = lob::sim::EventKind::AgentWakeup,
        .agent_id = 99U,
    }));

    engine.run_until(5U);

    EXPECT_EQ(engine.unknown_agent_wakeup_count(), 1U);
    EXPECT_EQ(engine.processed_event_count(), 1U);
    expect_accounting_invariants(engine);
}

TEST(MarketRegistryTest, ReturnsStableDenseIds) {
    lob::sim::MarketRegistry registry {};

    const auto first = registry.register_market(lob::sim::MarketKey {.venue_id = 1U, .product_id = 101U});
    const auto second = registry.register_market(lob::sim::MarketKey {.venue_id = 1U, .product_id = 102U});
    const auto first_again = registry.register_market(lob::sim::MarketKey {.venue_id = 1U, .product_id = 101U});

    EXPECT_EQ(first, 0U);
    EXPECT_EQ(second, 1U);
    EXPECT_EQ(first_again, first);
    EXPECT_EQ(registry.size(), 2U);

    const auto found = registry.find_market(lob::sim::MarketKey {.venue_id = 1U, .product_id = 102U});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, second);

    const lob::sim::MarketKey* key = registry.market_key(first);
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(key->venue_id, 1U);
    EXPECT_EQ(key->product_id, 101U);
}

TEST(AccountStoreTest, ReservesReleasesConsumesAndAppliesDeltas) {
    lob::sim::AccountStore<2U, 2U> accounts {};

    ASSERT_TRUE(accounts.register_account(1U));
    ASSERT_TRUE(accounts.set_balance(1U, 0U, 100));
    ASSERT_TRUE(accounts.reserve(1U, 0U, 40));
    ASSERT_EQ(accounts.balance(1U, 0U)->available_lots, 60);
    ASSERT_EQ(accounts.balance(1U, 0U)->reserved_lots, 40);

    ASSERT_TRUE(accounts.release(1U, 0U, 10));
    EXPECT_EQ(accounts.balance(1U, 0U)->available_lots, 70);
    EXPECT_EQ(accounts.balance(1U, 0U)->reserved_lots, 30);

    ASSERT_TRUE(accounts.consume_reserved(1U, 0U, 20));
    EXPECT_EQ(accounts.balance(1U, 0U)->available_lots, 70);
    EXPECT_EQ(accounts.balance(1U, 0U)->reserved_lots, 10);

    ASSERT_TRUE(accounts.apply_delta(1U, 0U, 5));
    EXPECT_EQ(accounts.balance(1U, 0U)->available_lots, 75);
    EXPECT_EQ(accounts.balance(1U, 0U)->reserved_lots, 10);
}

TEST(AccountStoreTest, ApplyDeltaRejectsOverflow) {
    lob::sim::AccountStore<1U, 1U> accounts {};

    ASSERT_TRUE(accounts.register_account(1U));
    ASSERT_TRUE(accounts.set_balance(1U, 0U, std::numeric_limits<lob::sim::BalanceLots>::max()));

    EXPECT_FALSE(accounts.apply_delta(1U, 0U, 1));
    EXPECT_EQ(accounts.balance(1U, 0U)->available_lots, std::numeric_limits<lob::sim::BalanceLots>::max());
}

TEST(SimulationEngineTest, AdvancesCurrentTimeByEventOrder) {
    lob::sim::SimulationEngine<8U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 2U, .product_id = 200U});

    ASSERT_TRUE(engine.schedule_event(lob::sim::ScheduledEvent {
        .timestamp_ns = 30U,
        .phase = lob::sim::EventPhase::Agent,
        .kind = lob::sim::EventKind::AgentWakeup,
        .market_id = market_id,
    }));
    ASSERT_TRUE(engine.schedule_event(lob::sim::ScheduledEvent {
        .timestamp_ns = 10U,
        .phase = lob::sim::EventPhase::Agent,
        .kind = lob::sim::EventKind::AgentWakeup,
        .market_id = market_id,
    }));
    ASSERT_TRUE(engine.schedule_event(lob::sim::ScheduledEvent {
        .timestamp_ns = 20U,
        .phase = lob::sim::EventPhase::Agent,
        .kind = lob::sim::EventKind::AgentWakeup,
        .market_id = market_id,
    }));

    engine.run_until(20U);
    EXPECT_EQ(engine.current_time_ns(), 20U);
    EXPECT_EQ(engine.processed_event_count(), 2U);
    ASSERT_FALSE(engine.scheduler().empty());
    EXPECT_EQ(engine.scheduler().top().timestamp_ns, 30U);

    engine.run_until(30U);
    EXPECT_EQ(engine.current_time_ns(), 30U);
    EXPECT_EQ(engine.processed_event_count(), 3U);
    EXPECT_TRUE(engine.scheduler().empty());
}

TEST(SimulationEngineTest, RegisteredMarketStateOwnsMatchingBook) {
    lob::sim::SimulationEngine<8U, 2U, 8U, 4U, 8U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 2U, .product_id = 201U});

    const auto* market_state = engine.market_state(market_id);
    ASSERT_NE(market_state, nullptr);
    EXPECT_EQ(market_state->market_id, market_id);
    EXPECT_EQ(market_state->market_key.venue_id, 2U);
    EXPECT_EQ(market_state->market_key.product_id, 201U);

    ASSERT_TRUE(engine.register_account(10U));
    ASSERT_TRUE(engine.set_balance(10U, 1U, 10'000));
    const auto result = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 10U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(result.accepted);
    engine.run_until(0U);

    EXPECT_EQ(market_state->matching_book.best_bid(), 1000);
}

TEST(SimulationEngineTest, MarketRegistryIsReadOnlyFromEngine) {
    lob::sim::SimulationEngine<8U, 2U, 8U, 4U, 8U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 3U, .product_id = 301U});

    const lob::sim::MarketRegistry& registry = engine.market_registry();
    const auto found = registry.find_market(lob::sim::MarketKey {.venue_id = 3U, .product_id = 301U});

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, market_id);
    EXPECT_EQ(engine.market_state_count(), 1U);
}

TEST(SimulationEngineTest, SubmittedOrderDoesNotRestBeforeLatencyElapses) {
    lob::sim::SimulationEngine<8U, 1U, 8U, 4U, 8U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 401U});
    ASSERT_NE(market_id, lob::sim::invalid_market_id);
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_funded_account(engine, 1U);
    ASSERT_TRUE(engine.set_market_latency(market_id, 10U));

    const auto gateway_result = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 5U,
        .time_in_force = lob::sim::TimeInForce::GTC,
        .client_timestamp_ns = 0U,
    });

    ASSERT_TRUE(gateway_result.accepted);
    EXPECT_EQ(gateway_result.order_id, 1U);
    EXPECT_EQ(gateway_result.arrival_timestamp_ns, 10U);
    EXPECT_FALSE(engine.market_state(market_id)->matching_book.best_bid().has_value());

    engine.run_until(9U);
    EXPECT_FALSE(engine.market_state(market_id)->matching_book.best_bid().has_value());
    EXPECT_EQ(engine.match_result_count(), 0U);

    engine.run_until(10U);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.best_bid(), 1000);
    ASSERT_EQ(engine.match_result_count(), 1U);
    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_EQ(engine.last_match_result()->order_id, 1U);
    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::Resting);
}

TEST(SimulationEngineTest, CrossingOrderExecutesOnlyAtArrivalTimestamp) {
    lob::sim::SimulationEngine<8U, 1U, 8U, 4U, 8U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 402U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_funded_account(engine, 1U);
    register_funded_account(engine, 2U);
    const auto* market_state = engine.market_state(market_id);
    ASSERT_TRUE(engine.set_market_latency(market_id, 0U));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);
    EXPECT_EQ(market_state->matching_book.best_ask(), 1000);

    ASSERT_TRUE(engine.set_market_latency(market_id, 5U));
    const auto crossing = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(crossing.accepted);
    EXPECT_EQ(crossing.arrival_timestamp_ns, 5U);

    engine.run_until(4U);
    EXPECT_EQ(market_state->matching_book.best_ask(), 1000);
    EXPECT_EQ(engine.match_result_count(), 1U);

    engine.run_until(5U);
    EXPECT_FALSE(market_state->matching_book.best_ask().has_value());
    ASSERT_EQ(engine.match_result_count(), 2U);
    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::Filled);
    EXPECT_EQ(engine.last_match_result()->filled_quantity_lots, 3U);
}

TEST(SimulationEngineTest, SameTimeOrderArrivalsPreserveSchedulerSequenceOrder) {
    lob::sim::SimulationEngine<16U, 1U, 8U, 4U, 16U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 403U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_funded_account(engine, 1U);
    register_funded_account(engine, 2U);
    register_funded_account(engine, 3U);
    const auto* market_state = engine.market_state(market_id);
    ASSERT_TRUE(engine.set_market_latency(market_id, 10U));

    const auto first = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 1U,
    });
    const auto second = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 1U,
    });
    ASSERT_TRUE(first.accepted);
    ASSERT_TRUE(second.accepted);
    EXPECT_EQ(first.arrival_timestamp_ns, second.arrival_timestamp_ns);

    engine.run_until(10U);
    EXPECT_EQ(market_state->matching_book.total_quantity_at_price(lob::Side::Buy, 1000), 2U);

    ASSERT_TRUE(engine.set_market_latency(market_id, 0U));
    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 3U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 2U,
    }).accepted);
    engine.run_until(10U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    const auto& result = *engine.last_match_result();
    ASSERT_EQ(result.execution_count, 4U);
    EXPECT_EQ(result.executions[0].counterparty_order_id, first.order_id);
    EXPECT_EQ(result.executions[2].counterparty_order_id, second.order_id);
}

TEST(SimulationEngineTest, CancelRequestIsLatencyDelayed) {
    lob::sim::SimulationEngine<8U, 1U, 8U, 4U, 8U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 404U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_funded_account(engine, 1U);
    const auto* market_state = engine.market_state(market_id);
    ASSERT_TRUE(engine.set_market_latency(market_id, 0U));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);
    ASSERT_EQ(market_state->matching_book.best_bid(), 1000);

    ASSERT_TRUE(engine.set_market_latency(market_id, 5U));
    const auto cancel = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    });
    ASSERT_TRUE(cancel.accepted);
    EXPECT_EQ(cancel.arrival_timestamp_ns, 5U);

    engine.run_until(4U);
    EXPECT_EQ(market_state->matching_book.best_bid(), 1000);
    EXPECT_EQ(engine.cancel_result_count(), 0U);

    engine.run_until(5U);
    EXPECT_FALSE(market_state->matching_book.best_bid().has_value());
    ASSERT_EQ(engine.cancel_result_count(), 1U);
    ASSERT_NE(engine.last_cancel_result(), nullptr);
    EXPECT_TRUE(engine.last_cancel_result()->canceled);
}

TEST(SimulationEngineTest, OrderArrivalUpdatesExchangeDepthBeforePublicFeedArrives) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 406U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U));
    ASSERT_TRUE(engine.set_order_entry_latency(market_id, 5U));
    ASSERT_TRUE(engine.set_market_data_latency(market_id, 7U));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);

    ASSERT_TRUE(engine.current_exchange_depth<1U>(market_id).has_value());
    EXPECT_EQ(engine.current_exchange_depth<1U>(market_id)->bid_count, 0U);
    engine.run_until(4U);
    EXPECT_EQ(engine.current_exchange_depth<1U>(market_id)->bid_count, 0U);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());

    engine.run_until(5U);
    ASSERT_TRUE(engine.current_exchange_depth<1U>(market_id).has_value());
    EXPECT_EQ(engine.current_exchange_depth<1U>(market_id)->bid_count, 1U);
    EXPECT_EQ(engine.current_exchange_depth<1U>(market_id)->bids[0].price_ticks, 100);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());

    engine.run_until(11U);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());
    engine.run_until(12U);
    const auto visible = engine.visible_depth<1U>(2U, market_id);
    ASSERT_TRUE(visible.has_value());
    EXPECT_EQ(visible->exchange_timestamp_ns, 5U);
    EXPECT_EQ(visible->receive_timestamp_ns, 12U);
    EXPECT_EQ(visible->bid_count, 1U);
    EXPECT_EQ(visible->bids[0].price_ticks, 100);
    EXPECT_EQ(visible->bids[0].quantity_lots, 3U);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, TwoSubscribersReceiveSameDelayedDepth) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 407U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U));
    ASSERT_TRUE(engine.register_market_data_subscriber(3U));
    ASSERT_TRUE(engine.set_market_data_latency(market_id, 5U));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 101,
        .quantity_lots = 4U,
    }).accepted);
    engine.run_until(4U);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());
    EXPECT_FALSE(engine.visible_depth<1U>(3U, market_id).has_value());
    engine.run_until(5U);

    const auto first = engine.visible_depth<1U>(2U, market_id);
    const auto second = engine.visible_depth<1U>(3U, market_id);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->exchange_timestamp_ns, second->exchange_timestamp_ns);
    EXPECT_EQ(first->receive_timestamp_ns, second->receive_timestamp_ns);
    EXPECT_EQ(first->bid_count, second->bid_count);
    EXPECT_EQ(first->bids[0].price_ticks, second->bids[0].price_ticks);
    EXPECT_EQ(first->bids[0].quantity_lots, second->bids[0].quantity_lots);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, SubscribersCanReceiveSameSnapshotAtDifferentFeedLatencies) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 417U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U, lob::sim::LatencyModel::fixed(3U)));
    ASSERT_TRUE(engine.register_market_data_subscriber(3U, lob::sim::LatencyModel::fixed(7U)));
    ASSERT_TRUE(engine.set_market_data_latency(market_id, 99U));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 101,
        .quantity_lots = 4U,
    }).accepted);

    engine.run_until(2U);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());
    EXPECT_FALSE(engine.visible_depth<1U>(3U, market_id).has_value());
    engine.run_until(3U);
    const auto fast = engine.visible_depth<1U>(2U, market_id);
    ASSERT_TRUE(fast.has_value());
    EXPECT_EQ(fast->exchange_timestamp_ns, 0U);
    EXPECT_EQ(fast->receive_timestamp_ns, 3U);
    EXPECT_FALSE(engine.visible_depth<1U>(3U, market_id).has_value());

    engine.run_until(7U);
    const auto slow = engine.visible_depth<1U>(3U, market_id);
    ASSERT_TRUE(slow.has_value());
    EXPECT_EQ(slow->exchange_timestamp_ns, fast->exchange_timestamp_ns);
    EXPECT_EQ(slow->receive_timestamp_ns, 7U);
    EXPECT_EQ(slow->bid_count, fast->bid_count);
    EXPECT_EQ(slow->bids[0].price_ticks, fast->bids[0].price_ticks);
    EXPECT_EQ(slow->bids[0].quantity_lots, fast->bids[0].quantity_lots);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, ExplicitSubscriberLatencySeedReproducesReceiveSequence) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 418U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    auto expected_latency = lob::sim::LatencyModel::uniform(1U, 5U, 44U);
    const auto first_latency = expected_latency.next_latency_ns();
    const auto second_latency = expected_latency.next_latency_ns();

    ASSERT_TRUE(engine.register_market_data_subscriber(2U, lob::sim::LatencyModel::uniform(1U, 5U, 44U)));
    ASSERT_TRUE(engine.register_market_data_subscriber(3U, lob::sim::LatencyModel::uniform(1U, 5U, 44U)));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 101,
        .quantity_lots = 4U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(first_latency);

    const auto first_subscriber = engine.visible_depth<1U>(2U, market_id);
    const auto second_subscriber = engine.visible_depth<1U>(3U, market_id);
    ASSERT_TRUE(first_subscriber.has_value());
    ASSERT_TRUE(second_subscriber.has_value());
    EXPECT_EQ(first_subscriber->receive_timestamp_ns, first_latency);
    EXPECT_EQ(second_subscriber->receive_timestamp_ns, first_latency);

    ASSERT_TRUE(engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    }).accepted);
    const auto cancel_exchange_timestamp = first_latency;
    engine.run_until(cancel_exchange_timestamp + second_latency);

    const auto first_after_cancel = engine.visible_depth<1U>(2U, market_id);
    const auto second_after_cancel = engine.visible_depth<1U>(3U, market_id);
    ASSERT_TRUE(first_after_cancel.has_value());
    ASSERT_TRUE(second_after_cancel.has_value());
    EXPECT_EQ(first_after_cancel->exchange_timestamp_ns, cancel_exchange_timestamp);
    EXPECT_EQ(second_after_cancel->exchange_timestamp_ns, cancel_exchange_timestamp);
    EXPECT_EQ(first_after_cancel->receive_timestamp_ns, cancel_exchange_timestamp + second_latency);
    EXPECT_EQ(second_after_cancel->receive_timestamp_ns, cancel_exchange_timestamp + second_latency);
    EXPECT_EQ(first_after_cancel->bid_count, 0U);
    EXPECT_EQ(second_after_cancel->bid_count, 0U);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, ExplicitSubscriberLatencyBeatsRegistrationOrder) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 419U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U, lob::sim::LatencyModel::fixed(10U)));
    ASSERT_TRUE(engine.register_market_data_subscriber(3U, lob::sim::LatencyModel::fixed(2U)));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 101,
        .quantity_lots = 4U,
    }).accepted);

    engine.run_until(2U);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());
    const auto fast = engine.visible_depth<1U>(3U, market_id);
    ASSERT_TRUE(fast.has_value());
    EXPECT_EQ(fast->receive_timestamp_ns, 2U);

    engine.run_until(10U);
    const auto slow = engine.visible_depth<1U>(2U, market_id);
    ASSERT_TRUE(slow.has_value());
    EXPECT_EQ(slow->receive_timestamp_ns, 10U);
    EXPECT_EQ(slow->exchange_timestamp_ns, fast->exchange_timestamp_ns);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, CancelProducesDelayedDepthUpdate) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 408U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U));
    ASSERT_TRUE(engine.set_market_data_latency(market_id, 5U));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(5U);
    ASSERT_TRUE(engine.visible_depth<1U>(2U, market_id).has_value());
    EXPECT_EQ(engine.visible_depth<1U>(2U, market_id)->bid_count, 1U);

    ASSERT_TRUE(engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    }).accepted);
    engine.run_until(9U);
    EXPECT_EQ(engine.visible_depth<1U>(2U, market_id)->bid_count, 1U);
    engine.run_until(10U);

    const auto visible = engine.visible_depth<1U>(2U, market_id);
    ASSERT_TRUE(visible.has_value());
    EXPECT_EQ(visible->exchange_timestamp_ns, 5U);
    EXPECT_EQ(visible->receive_timestamp_ns, 10U);
    EXPECT_EQ(visible->bid_count, 0U);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, SeparateOrderCancelAndMarketDataLatencyChannelsCanDiffer) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 409U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U));
    ASSERT_TRUE(engine.set_order_entry_latency(market_id, 3U));
    ASSERT_TRUE(engine.set_cancel_latency(market_id, 7U));
    ASSERT_TRUE(engine.set_market_data_latency(market_id, 11U));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(order.accepted);
    EXPECT_EQ(order.arrival_timestamp_ns, 3U);
    engine.run_until(3U);

    const auto cancel = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    });
    ASSERT_TRUE(cancel.accepted);
    EXPECT_EQ(cancel.arrival_timestamp_ns, 10U);

    engine.run_until(13U);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());
    engine.run_until(14U);
    ASSERT_TRUE(engine.visible_depth<1U>(2U, market_id).has_value());
    EXPECT_EQ(engine.visible_depth<1U>(2U, market_id)->exchange_timestamp_ns, 3U);
    EXPECT_EQ(engine.visible_depth<1U>(2U, market_id)->bid_count, 1U);
    engine.run_until(21U);
    EXPECT_EQ(engine.visible_depth<1U>(2U, market_id)->exchange_timestamp_ns, 10U);
    EXPECT_EQ(engine.visible_depth<1U>(2U, market_id)->bid_count, 0U);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, LatencySettersRejectInvalidDistributionConfigs) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 410U});
    register_empty_account(engine, 2U);

    EXPECT_FALSE(engine.set_order_entry_latency_model(market_id, lob::sim::LatencyModel::exponential(0U, 0.0, 1U)));
    EXPECT_FALSE(engine.set_cancel_latency_model(market_id, lob::sim::LatencyModel::exponential(0U, -1.0, 1U)));
    EXPECT_FALSE(engine.set_market_data_latency_model(market_id, lob::sim::LatencyModel::lognormal(0U, 0.0, 0.0, 1U)));
    EXPECT_FALSE(engine.set_market_latency_model(market_id, lob::sim::LatencyModel::lognormal(0U, 0.0, -1.0, 1U)));
    EXPECT_FALSE(engine.register_market_data_subscriber(2U, lob::sim::LatencyModel::exponential(0U, 0.0, 1U)));
    EXPECT_TRUE(engine.set_order_entry_latency_model(market_id, lob::sim::LatencyModel::exponential(0U, 1.0, 1U)));
    EXPECT_TRUE(engine.register_market_data_subscriber(2U, lob::sim::LatencyModel::lognormal(0U, 1.0, 0.1, 1U)));
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, LatencyChannelsCanUseDifferentDistributionsAndSeeds) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 411U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U));

    auto expected_cancel_latency = lob::sim::LatencyModel::exponential(0U, 5.0, 7U);
    auto expected_market_data_latency = lob::sim::LatencyModel::lognormal(0U, 2.0, 0.2, 11U);
    const auto cancel_latency_ns = expected_cancel_latency.next_latency_ns();
    const auto market_data_latency_ns = expected_market_data_latency.next_latency_ns();

    ASSERT_TRUE(engine.set_order_entry_latency_model(market_id, lob::sim::LatencyModel::fixed(3U, 5U)));
    ASSERT_TRUE(engine.set_cancel_latency_model(market_id, lob::sim::LatencyModel::exponential(0U, 5.0, 7U)));
    ASSERT_TRUE(engine.set_market_data_latency_model(market_id, lob::sim::LatencyModel::lognormal(0U, 2.0, 0.2, 11U)));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(order.accepted);
    EXPECT_EQ(order.arrival_timestamp_ns, 3U);
    engine.run_until(3U + market_data_latency_ns);
    ASSERT_TRUE(engine.visible_depth<1U>(2U, market_id).has_value());
    EXPECT_EQ(engine.visible_depth<1U>(2U, market_id)->receive_timestamp_ns, 3U + market_data_latency_ns);

    const auto cancel = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    });
    ASSERT_TRUE(cancel.accepted);
    EXPECT_EQ(cancel.arrival_timestamp_ns, 3U + market_data_latency_ns + cancel_latency_ns);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, SchedulingOverflowReturnsCleanRejection) {
    lob::sim::SimulationEngine<1U, 1U, 8U, 4U, 8U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 4U, .product_id = 405U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_funded_account(engine, 1U);
    register_funded_account(engine, 2U);
    register_funded_account(engine, 3U);
    ASSERT_TRUE(engine.set_market_latency(market_id, 10U));

    const auto first = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 1U,
    });
    const auto rejected = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 990,
        .quantity_lots = 1U,
    });

    ASSERT_TRUE(first.accepted);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.status, lob::sim::GatewayStatus::SchedulerFull);
    EXPECT_EQ(rejected.order_id, lob::sim::invalid_order_id);
    ASSERT_TRUE(engine.account_balance(2U, 1U).has_value());
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 1'000'000);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 0);

    engine.run_until(10U);
    ASSERT_TRUE(engine.set_market_latency(market_id, 0U));
    const auto after_capacity_frees = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 3U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 980,
        .quantity_lots = 1U,
    });

    EXPECT_TRUE(after_capacity_frees.accepted);
    EXPECT_EQ(after_capacity_frees.order_id, 2U);
}

TEST(SimulationEngineTest, BuyOrderRejectedWhenQuoteBalanceInsufficient) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 501U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 99));

    const auto result = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 1U,
    });

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.status, lob::sim::GatewayStatus::InsufficientBalance);
    EXPECT_EQ(engine.scheduler().size(), 0U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 99);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 0);
}

TEST(SimulationEngineTest, SellOrderRejectedWhenBaseBalanceInsufficient) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 502U});
    register_empty_account(engine, 1U);

    const auto result = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 1U,
    });

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.status, lob::sim::GatewayStatus::InsufficientBalance);
    EXPECT_EQ(engine.scheduler().size(), 0U);
    EXPECT_EQ(engine.account_balance(1U, 0U)->available_lots, 0);
    EXPECT_EQ(engine.account_balance(1U, 0U)->reserved_lots, 0);
}

TEST(SimulationEngineTest, InvalidPriceOrdersRejectedWithoutMutation) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 511U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 10));
    ASSERT_TRUE(engine.set_balance(1U, 1U, 10'000));

    const auto negative_price = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = -1,
        .quantity_lots = 1U,
    });
    const auto zero_price = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 0,
        .quantity_lots = 1U,
    });

    EXPECT_FALSE(negative_price.accepted);
    EXPECT_EQ(negative_price.status, lob::sim::GatewayStatus::InvalidRequest);
    EXPECT_FALSE(zero_price.accepted);
    EXPECT_EQ(zero_price.status, lob::sim::GatewayStatus::InvalidRequest);
    EXPECT_EQ(engine.scheduler().size(), 0U);
    EXPECT_EQ(engine.reservation_count(), 0U);
    EXPECT_FALSE(engine.market_state(market_id)->matching_book.best_bid().has_value());
    EXPECT_FALSE(engine.market_state(market_id)->matching_book.best_ask().has_value());
    EXPECT_EQ(engine.account_balance(1U, 0U)->available_lots, 10);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 10'000);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, BuyOrderRejectedWhenNotionalOverflows) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 512U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, std::numeric_limits<lob::sim::BalanceLots>::max()));

    const auto result = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = std::numeric_limits<lob::sim::PriceTicks>::max(),
        .quantity_lots = 2U,
    });

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.status, lob::sim::GatewayStatus::InvalidRequest);
    EXPECT_EQ(engine.scheduler().size(), 0U);
    EXPECT_EQ(engine.reservation_count(), 0U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 0);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, AcceptedBuyReservesQuoteBeforeArrival) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 503U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    ASSERT_TRUE(engine.set_market_latency(market_id, 10U));
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    const auto result = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
    });

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 500);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);
    EXPECT_FALSE(engine.market_state(market_id)->matching_book.best_bid().has_value());
}

TEST(SimulationEngineTest, CancelReleasesReservation) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 504U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.account_balance(1U, 1U)->available_lots, 500);
    ASSERT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);

    ASSERT_TRUE(engine.set_market_latency(market_id, 5U));
    ASSERT_TRUE(engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    }).accepted);
    engine.run_until(4U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 500);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);

    engine.run_until(5U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 1000);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 0);
}

TEST(SimulationEngineTest, CrossingBuyTransfersBaseAndQuote) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 505U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 10));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 7000);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 0);
    EXPECT_EQ(engine.account_balance(1U, 0U)->available_lots, 7);
    EXPECT_EQ(engine.account_balance(1U, 0U)->reserved_lots, 0);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 3000);
    ASSERT_EQ(engine.accounting_event_count(), 1U);
    ASSERT_NE(engine.last_accounting_event(), nullptr);
    EXPECT_EQ(engine.last_accounting_event()->buyer_agent_id, 2U);
    EXPECT_EQ(engine.last_accounting_event()->seller_agent_id, 1U);
}

TEST(SimulationEngineTest, CrossingSellTransfersBaseAndQuote) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 506U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 10'000));
    ASSERT_TRUE(engine.set_balance(2U, 0U, 10));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    EXPECT_EQ(engine.account_balance(1U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 7000);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 0);
    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 7);
    EXPECT_EQ(engine.account_balance(2U, 0U)->reserved_lots, 0);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 3000);
    ASSERT_EQ(engine.accounting_event_count(), 1U);
    ASSERT_NE(engine.last_accounting_event(), nullptr);
    EXPECT_EQ(engine.last_accounting_event()->buyer_agent_id, 1U);
    EXPECT_EQ(engine.last_accounting_event()->seller_agent_id, 2U);
}

TEST(SimulationEngineTest, PartialFillWithRestingResidualKeepsResidualReservation) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 507U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 10));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 7U,
    }).accepted);
    engine.run_until(0U);

    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 3000);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 4000);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(2U), 4U);
}

TEST(SimulationEngineTest, ReportCapacityStopReleasesIncomingUnusedReservation) {
    lob::sim::SimulationEngine<16U, 1U, 8U, 4U, 2U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 509U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 10));
    ASSERT_TRUE(engine.set_balance(2U, 0U, 10));
    ASSERT_TRUE(engine.set_balance(3U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 3U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 5U,
    }).accepted);
    ASSERT_EQ(engine.account_balance(3U, 1U)->available_lots, 5000);
    ASSERT_EQ(engine.account_balance(3U, 1U)->reserved_lots, 5000);
    engine.run_until(0U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::PartiallyFilledExecutionReportCapacity);
    EXPECT_EQ(engine.account_balance(3U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(3U, 1U)->available_lots, 7000);
    EXPECT_EQ(engine.account_balance(3U, 1U)->reserved_lots, 0);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(2U), 3U);
}

TEST(SimulationEngineTest, TinyAccountingSinkSurfacesOverflow) {
    lob::sim::SimulationEngine<16U, 1U, 8U, 4U, 16U, 16U, 32U, 16U, 0U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 510U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 1000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    ASSERT_TRUE(engine.schedule_event(lob::sim::ScheduledEvent {
        .timestamp_ns = 10U,
        .phase = lob::sim::EventPhase::Agent,
        .kind = lob::sim::EventKind::AgentWakeup,
        .market_id = market_id,
    }));
    const auto processed_before_halt = engine.processed_event_count();
    engine.run_until(10U);

    EXPECT_TRUE(engine.accounting_event_sink_overflowed());
    EXPECT_TRUE(engine.result_sink_overflowed());
    EXPECT_GT(engine.accounting_failure_count(), 0U);
    EXPECT_EQ(engine.accounting_event_count(), 0U);
    EXPECT_EQ(engine.status(), lob::sim::SimulationStatus::HaltedSinkOverflow);
    EXPECT_EQ(engine.processed_event_count(), processed_before_halt + 1U);
    ASSERT_FALSE(engine.scheduler().empty());
    EXPECT_EQ(engine.scheduler().top().timestamp_ns, 10U);

    const auto rejected_submit = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 1U,
    });
    const auto rejected_cancel = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = 1U,
    });
    EXPECT_FALSE(rejected_submit.accepted);
    EXPECT_EQ(rejected_submit.status, lob::sim::GatewayStatus::Halted);
    EXPECT_FALSE(rejected_cancel.accepted);
    EXPECT_EQ(rejected_cancel.status, lob::sim::GatewayStatus::Halted);
}

TEST(SimulationEngineTest, TinyMatchAndCancelSinksSurfaceOverflow) {
    lob::sim::SimulationEngine<16U, 1U, 8U, 4U, 16U, 0U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 513U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);

    EXPECT_TRUE(engine.match_result_sink_overflowed());
    EXPECT_EQ(engine.status(), lob::sim::SimulationStatus::HaltedSinkOverflow);
    EXPECT_EQ(engine.match_result_count(), 0U);

    const auto rejected_cancel = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    });
    engine.run_until(0U);

    EXPECT_FALSE(rejected_cancel.accepted);
    EXPECT_EQ(rejected_cancel.status, lob::sim::GatewayStatus::Halted);
    EXPECT_FALSE(engine.cancel_result_sink_overflowed());
    EXPECT_TRUE(engine.result_sink_overflowed());
    EXPECT_EQ(engine.cancel_result_count(), 0U);
}

TEST(SimulationEngineTest, MarketDataSchedulingOverflowHalts) {
    lob::sim::SimulationEngine<1U, 1U, 8U, 4U, 8U, 64U, 32U, 16U, 64U, 16U, 2U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 514U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.register_market_data_subscriber(2U));
    ASSERT_TRUE(engine.register_market_data_subscriber(3U));
    ASSERT_TRUE(engine.set_market_data_latency(market_id, 10U));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    EXPECT_TRUE(engine.market_data_overflowed());
    EXPECT_TRUE(engine.result_sink_overflowed());
    EXPECT_EQ(engine.status(), lob::sim::SimulationStatus::HaltedMarketDataOverflow);
    EXPECT_EQ(engine.processed_event_count(), 1U);
    EXPECT_EQ(engine.scheduler().size(), 0U);
    EXPECT_FALSE(engine.visible_depth<1U>(2U, market_id).has_value());
    EXPECT_FALSE(engine.visible_depth<1U>(3U, market_id).has_value());
}

TEST(SimulationEngineTest, UnrestedGtcRejectionReleasesReservation) {
    lob::sim::SimulationEngine<16U, 1U, 1U, 1U, 16U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 5U, .product_id = 508U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 1U,
    }).accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 1000);

    const auto rejected = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 990,
        .quantity_lots = 1U,
    });
    ASSERT_TRUE(rejected.accepted);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 1990);
    engine.run_until(0U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_FALSE(engine.last_match_result()->accepted);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 9000);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 1000);
}

TEST(SimulationEngineTest, IocBuyReleasesUnfilledReservationAfterArrival) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 601U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 5));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 1000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 2U,
    }).accepted);
    engine.run_until(0U);

    const auto ioc = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
        .time_in_force = lob::sim::TimeInForce::IOC,
    });
    ASSERT_TRUE(ioc.accepted);
    ASSERT_TRUE(engine.reservation_by_order_id(ioc.order_id).has_value());
    engine.run_until(0U);

    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::PartiallyFilled);
    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 2);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 800);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 0);
    EXPECT_FALSE(engine.reservation_by_order_id(ioc.order_id).has_value());
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, IocSellReleasesUnfilledReservationAfterArrival) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 602U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));
    ASSERT_TRUE(engine.set_balance(2U, 0U, 5));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 2U,
    }).accepted);
    engine.run_until(0U);

    const auto ioc = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 5U,
        .time_in_force = lob::sim::TimeInForce::IOC,
    });
    ASSERT_TRUE(ioc.accepted);
    ASSERT_TRUE(engine.reservation_by_order_id(ioc.order_id).has_value());
    engine.run_until(0U);

    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::PartiallyFilled);
    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(2U, 0U)->reserved_lots, 0);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 200);
    EXPECT_FALSE(engine.reservation_by_order_id(ioc.order_id).has_value());
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, FokRejectionReleasesReservation) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 603U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 2));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 1000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 2U,
    }).accepted);
    engine.run_until(0U);

    const auto fok = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
        .time_in_force = lob::sim::TimeInForce::FOK,
    });
    ASSERT_TRUE(fok.accepted);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 500);
    engine.run_until(0U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_FALSE(engine.last_match_result()->accepted);
    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::Rejected);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 1000);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 0);
    EXPECT_FALSE(engine.reservation_by_order_id(fok.order_id).has_value());
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, FokRejectsWhenExecutionReportCapacityCannotRepresentAllFills) {
    lob::sim::SimulationEngine<16U, 1U, 8U, 4U, 2U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 613U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(3U, 1U, 1000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    const auto fok = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 3U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
        .time_in_force = lob::sim::TimeInForce::FOK,
    });
    ASSERT_TRUE(fok.accepted);
    ASSERT_TRUE(engine.reservation_by_order_id(fok.order_id).has_value());
    engine.run_until(0U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_FALSE(engine.last_match_result()->accepted);
    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::Rejected);
    EXPECT_EQ(engine.last_match_result()->execution_count, 0U);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(1U), 3U);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(2U), 3U);
    EXPECT_EQ(engine.account_balance(3U, 1U)->available_lots, 1000);
    EXPECT_EQ(engine.account_balance(3U, 1U)->reserved_lots, 0);
    EXPECT_FALSE(engine.reservation_by_order_id(fok.order_id).has_value());
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, FokSucceedsWhenExecutionReportCapacityCanRepresentAllFills) {
    lob::sim::SimulationEngine<16U, 1U, 8U, 4U, 4U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 614U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    register_empty_account(engine, 3U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(3U, 1U, 1000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 3U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
        .time_in_force = lob::sim::TimeInForce::FOK,
    }).accepted);
    engine.run_until(0U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_TRUE(engine.last_match_result()->accepted);
    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::Filled);
    EXPECT_EQ(engine.last_match_result()->execution_count, 4U);
    EXPECT_EQ(engine.accounting_event_count(), 2U);
    EXPECT_EQ(engine.account_balance(3U, 0U)->available_lots, 5);
    EXPECT_EQ(engine.account_balance(3U, 1U)->available_lots, 500);
    EXPECT_EQ(engine.account_balance(3U, 1U)->reserved_lots, 0);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(2U), 1U);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, DuplicateCancelDoesNotReleaseReservationTwice) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 604U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);

    ASSERT_TRUE(engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    }).accepted);
    ASSERT_TRUE(engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = order.order_id,
    }).accepted);
    engine.run_until(0U);

    ASSERT_EQ(engine.cancel_result_count(), 2U);
    EXPECT_TRUE(engine.cancel_result(0U)->canceled);
    EXPECT_FALSE(engine.cancel_result(1U)->canceled);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 1000);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 0);
    EXPECT_FALSE(engine.reservation_by_order_id(order.order_id).has_value());
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, NonOwnerCannotCancelRestingOrder) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 611U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.account_balance(1U, 1U)->available_lots, 500);
    ASSERT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);

    const auto rejected = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .order_id = order.order_id,
    });

    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.status, lob::sim::GatewayStatus::Unauthorized);
    EXPECT_EQ(engine.cancel_result_count(), 0U);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(order.order_id), 5U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 500);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);
    EXPECT_EQ(engine.reservation_by_order_id(order.order_id)->agent_id, 1U);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, NonOwnerCancelAtArrivalDoesNotMutateOrderOrReservation) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 612U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(order.order_id), 5U);

    ASSERT_TRUE(engine.schedule_event(lob::sim::ScheduledEvent {
        .timestamp_ns = 5U,
        .phase = lob::sim::EventPhase::Market,
        .kind = lob::sim::EventKind::CancelArrivesAtMarket,
        .market_id = market_id,
        .agent_id = 2U,
        .order_id = order.order_id,
    }));
    engine.run_until(5U);

    ASSERT_EQ(engine.cancel_result_count(), 1U);
    EXPECT_FALSE(engine.last_cancel_result()->canceled);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(order.order_id), 5U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 500);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);
    EXPECT_EQ(engine.reservation_by_order_id(order.order_id)->reserved_amount_lots, 500);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, CancelUnknownOrderHasNoAccountingSideEffect) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 605U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    const auto order = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 5U,
    });
    ASSERT_TRUE(order.accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);

    const auto rejected = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = 999U,
    });

    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.status, lob::sim::GatewayStatus::UnknownOrder);
    EXPECT_EQ(engine.cancel_result_count(), 0U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 500);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 500);
    EXPECT_EQ(engine.reservation_by_order_id(order.order_id)->reserved_amount_lots, 500);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, CancelAfterFullFillHasNoAccountingSideEffect) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 606U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 5));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 1000));

    const auto maker = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    });
    ASSERT_TRUE(maker.accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);
    ASSERT_FALSE(engine.reservation_by_order_id(maker.order_id).has_value());
    const auto seller_base = engine.account_balance(1U, 0U);
    const auto seller_quote = engine.account_balance(1U, 1U);

    const auto rejected = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .order_id = maker.order_id,
    });

    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.status, lob::sim::GatewayStatus::UnknownOrder);
    EXPECT_EQ(engine.cancel_result_count(), 0U);
    EXPECT_EQ(engine.account_balance(1U, 0U), seller_base);
    EXPECT_EQ(engine.account_balance(1U, 1U), seller_quote);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, PartialFillThenCancelReleasesOnlyResidualReservation) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 607U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    const auto buy = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 7U,
    });
    ASSERT_TRUE(buy.accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.account_balance(2U, 1U)->available_lots, 3000);
    ASSERT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 4000);
    ASSERT_EQ(engine.reservation_by_order_id(buy.order_id)->reserved_amount_lots, 4000);

    ASSERT_TRUE(engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .order_id = buy.order_id,
    }).accepted);
    engine.run_until(0U);

    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 7000);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 0);
    EXPECT_FALSE(engine.reservation_by_order_id(buy.order_id).has_value());
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, ProductSpecCanChangeBeforeActivity) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 616U});

    EXPECT_TRUE(engine.set_market_product_spec(market_id, lob::sim::ProductSpec {
                                                       .base_asset_id = 0U,
                                                       .quote_asset_id = 1U,
                                                       .maker_fee_bps = 2,
                                                       .taker_fee_bps = 5,
                                                   }));
    ASSERT_NE(engine.market_state(market_id), nullptr);
    EXPECT_EQ(engine.market_state(market_id)->product_spec.maker_fee_bps, 2);
    EXPECT_EQ(engine.market_state(market_id)->product_spec.taker_fee_bps, 5);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, ProductSpecChangeFailsWhileOrderEventPending) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 617U});
    ASSERT_TRUE(engine.set_market_latency(market_id, 5U));
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 1000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);

    EXPECT_FALSE(engine.set_market_product_spec(market_id, lob::sim::ProductSpec {
                                                            .base_asset_id = 0U,
                                                            .quote_asset_id = 1U,
                                                            .maker_fee_bps = 7,
                                                            .taker_fee_bps = 9,
                                                        }));
    EXPECT_EQ(engine.market_state(market_id)->product_spec.maker_fee_bps, 0);
    EXPECT_EQ(engine.market_state(market_id)->product_spec.taker_fee_bps, 0);

    engine.run_until(5U);
    EXPECT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(1U), 3U);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, ProductSpecChangeAfterRestingOrderFailsAndDoesNotAffectFillAccounting) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 618U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 1000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.market_state(market_id)->matching_book.remaining_quantity(1U), 3U);
    ASSERT_TRUE(engine.reservation_by_order_id(1U).has_value());

    EXPECT_FALSE(engine.set_market_product_spec(market_id, lob::sim::ProductSpec {
                                                            .base_asset_id = 0U,
                                                            .quote_asset_id = 1U,
                                                            .maker_fee_bps = 100,
                                                            .taker_fee_bps = 100,
                                                        }));
    EXPECT_EQ(engine.market_state(market_id)->product_spec.maker_fee_bps, 0);
    EXPECT_EQ(engine.market_state(market_id)->product_spec.taker_fee_bps, 0);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 700);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 300);
    ASSERT_EQ(engine.accounting_event_count(), 1U);
    EXPECT_EQ(engine.last_accounting_event()->buyer_fee_lots, 0);
    EXPECT_EQ(engine.last_accounting_event()->seller_fee_lots, 0);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, MakerAndTakerFeesApplyToCorrectAgentsWithCeilingRounding) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 608U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    ASSERT_TRUE(engine.set_market_product_spec(market_id, lob::sim::ProductSpec {
                                                            .base_asset_id = 0U,
                                                            .quote_asset_id = 1U,
                                                            .maker_fee_bps = 1,
                                                            .taker_fee_bps = 3,
                                                        }));
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1667,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1667,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_EQ(engine.accounting_event_count(), 1U);
    ASSERT_NE(engine.last_accounting_event(), nullptr);
    EXPECT_EQ(engine.last_accounting_event()->buyer_agent_id, 2U);
    EXPECT_EQ(engine.last_accounting_event()->seller_agent_id, 1U);
    EXPECT_EQ(engine.last_accounting_event()->buyer_fee_lots, 2);
    EXPECT_EQ(engine.last_accounting_event()->seller_fee_lots, 1);
    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 4997);
    EXPECT_EQ(engine.account_balance(1U, 0U)->available_lots, 0);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 5000);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, MakerRebateAndTakerFeeApplyInSameFill) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 609U});
    ASSERT_NE(engine.market_state(market_id), nullptr);
    ASSERT_TRUE(engine.set_market_product_spec(market_id, lob::sim::ProductSpec {
                                                            .base_asset_id = 0U,
                                                            .quote_asset_id = 1U,
                                                            .maker_fee_bps = -2,
                                                            .taker_fee_bps = 3,
                                                        }));
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_EQ(engine.accounting_event_count(), 1U);
    ASSERT_NE(engine.last_accounting_event(), nullptr);
    EXPECT_EQ(engine.last_accounting_event()->buyer_agent_id, 2U);
    EXPECT_EQ(engine.last_accounting_event()->seller_agent_id, 1U);
    EXPECT_EQ(engine.last_accounting_event()->buyer_fee_lots, 1);
    EXPECT_EQ(engine.last_accounting_event()->seller_fee_lots, -1);
    EXPECT_EQ(engine.account_balance(2U, 0U)->available_lots, 3);
    EXPECT_EQ(engine.account_balance(2U, 1U)->available_lots, 6999);
    EXPECT_EQ(engine.account_balance(2U, 1U)->reserved_lots, 0);
    EXPECT_EQ(engine.account_balance(1U, 0U)->available_lots, 0);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 3001);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, CorruptProductAssetCausesVisibleAccountingFailure) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 615U});
    register_empty_account(engine, 1U);
    register_empty_account(engine, 2U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 3));
    ASSERT_TRUE(engine.set_balance(2U, 1U, 1000));
    ASSERT_TRUE(engine.set_market_latency(market_id, 0U));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Sell,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    engine.run_until(0U);

    ASSERT_TRUE(engine.set_market_latency(market_id, 5U));
    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 3U,
    }).accepted);
    auto* mutable_state = engine.market_state_for_testing(market_id);
    ASSERT_NE(mutable_state, nullptr);
    mutable_state->product_spec.base_asset_id = 99U;
    engine.run_until(5U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_EQ(engine.last_match_result()->status, lob::sim::OrderStatus::Filled);
    EXPECT_EQ(engine.accounting_event_count(), 0U);
    EXPECT_GT(engine.accounting_failure_count(), 0U);
    EXPECT_EQ(engine.status(), lob::sim::SimulationStatus::HaltedAccountingFailure);
}

TEST(SimulationEngineTest, MatchingBookArrivalRejectionReleasesReservation) {
    lob::sim::SimulationEngine<16U, 1U, 1U, 1U, 16U> engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 610U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 1U, 10'000));

    ASSERT_TRUE(engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 1000,
        .quantity_lots = 1U,
    }).accepted);
    engine.run_until(0U);
    ASSERT_EQ(engine.reservation_count(), 1U);

    const auto rejected = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 990,
        .quantity_lots = 1U,
    });
    ASSERT_TRUE(rejected.accepted);
    ASSERT_EQ(engine.reservation_count(), 2U);
    engine.run_until(0U);

    ASSERT_NE(engine.last_match_result(), nullptr);
    EXPECT_FALSE(engine.last_match_result()->accepted);
    EXPECT_FALSE(engine.reservation_by_order_id(rejected.order_id).has_value());
    EXPECT_EQ(engine.reservation_count(), 1U);
    EXPECT_EQ(engine.account_balance(1U, 1U)->available_lots, 9000);
    EXPECT_EQ(engine.account_balance(1U, 1U)->reserved_lots, 1000);
    expect_accounting_invariants(engine);
}

TEST(SimulationEngineTest, InvalidMarketAndAccountRequestsDoNotMutateAccounting) {
    TestSimulationEngine engine {};
    const auto market_id = engine.register_market(lob::sim::MarketKey {.venue_id = 6U, .product_id = 610U});
    register_empty_account(engine, 1U);
    ASSERT_TRUE(engine.set_balance(1U, 0U, 10));
    ASSERT_TRUE(engine.set_balance(1U, 1U, 10'000));

    const auto before_base = engine.account_balance(1U, 0U);
    const auto before_quote = engine.account_balance(1U, 1U);

    const auto invalid_market = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = lob::sim::invalid_market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 1U,
    });
    const auto unknown_market = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 1U,
        .market_id = market_id + 1U,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 1U,
    });
    const auto unknown_account = engine.submit_order(lob::sim::OrderRequest {
        .agent_id = 2U,
        .market_id = market_id,
        .side = lob::Side::Buy,
        .price_ticks = 100,
        .quantity_lots = 1U,
    });
    const auto invalid_cancel = engine.cancel_order(lob::sim::CancelRequest {
        .agent_id = 1U,
        .market_id = lob::sim::invalid_market_id,
        .order_id = 1U,
    });

    EXPECT_FALSE(invalid_market.accepted);
    EXPECT_EQ(invalid_market.status, lob::sim::GatewayStatus::InvalidRequest);
    EXPECT_FALSE(unknown_market.accepted);
    EXPECT_EQ(unknown_market.status, lob::sim::GatewayStatus::UnknownMarket);
    EXPECT_FALSE(unknown_account.accepted);
    EXPECT_EQ(unknown_account.status, lob::sim::GatewayStatus::UnknownAccount);
    EXPECT_FALSE(invalid_cancel.accepted);
    EXPECT_EQ(engine.scheduler().size(), 0U);
    EXPECT_EQ(engine.reservation_count(), 0U);
    EXPECT_EQ(engine.account_balance(1U, 0U), before_base);
    EXPECT_EQ(engine.account_balance(1U, 1U), before_quote);
    expect_accounting_invariants(engine);
}

TEST(MatchingBookTest, BuyLimitRestsWhenNotCrossing) {
    TestMatchingBook book {};

    const auto result = book.submit_limit(make_order(1U, lob::Side::Buy, 1000, 10U));

    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::Resting);
    EXPECT_EQ(result.execution_count, 0U);
    EXPECT_EQ(book.best_bid(), 1000);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.total_quantity_at_price(lob::Side::Buy, 1000), 10U);
}

TEST(MatchingBookTest, SellLimitRestsWhenNotCrossing) {
    TestMatchingBook book {};

    const auto result = book.submit_limit(make_order(1U, lob::Side::Sell, 1010, 7U));

    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::Resting);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.best_ask(), 1010);
    EXPECT_EQ(book.total_quantity_at_price(lob::Side::Sell, 1010), 7U);
}

TEST(MatchingBookTest, InvalidPricesAreRejectedWithoutMutation) {
    TestMatchingBook book {};

    const auto negative_price = book.submit_limit(make_order(1U, lob::Side::Buy, -1, 10U));
    const auto zero_price = book.submit_limit(make_order(2U, lob::Side::Sell, 0, 7U));

    EXPECT_FALSE(negative_price.accepted);
    EXPECT_EQ(negative_price.status, lob::sim::OrderStatus::Rejected);
    EXPECT_FALSE(zero_price.accepted);
    EXPECT_EQ(zero_price.status, lob::sim::OrderStatus::Rejected);
    EXPECT_EQ(book.resting_order_count(), 0U);
    EXPECT_EQ(book.active_price_level_count(), 0U);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(MatchingBookTest, CrossingBuyMatchesBestAskAndReturnsBothExecutions) {
    TestMatchingBook book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Sell, 1010, 5U, 11U, 1U)).accepted);
    ASSERT_TRUE(book.submit_limit(make_order(2U, lob::Side::Sell, 1005, 5U, 12U, 2U)).accepted);

    const auto result = book.submit_limit(make_order(3U, lob::Side::Buy, 1010, 4U, 13U, 3U));

    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::Filled);
    EXPECT_EQ(result.filled_quantity_lots, 4U);
    EXPECT_EQ(result.execution_count, 2U);
    EXPECT_EQ(result.executions[0].order_id, 3U);
    EXPECT_EQ(result.executions[0].counterparty_order_id, 2U);
    EXPECT_EQ(result.executions[0].liquidity_role, lob::sim::LiquidityRole::Taker);
    EXPECT_EQ(result.executions[0].price_ticks, 1005);
    EXPECT_EQ(result.executions[1].order_id, 2U);
    EXPECT_EQ(result.executions[1].counterparty_order_id, 3U);
    EXPECT_EQ(result.executions[1].liquidity_role, lob::sim::LiquidityRole::Maker);
    EXPECT_EQ(book.remaining_quantity(2U), 1U);
    EXPECT_EQ(book.best_ask(), 1005);
}

TEST(MatchingBookTest, CrossingSellMatchesBestBid) {
    TestMatchingBook book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Buy, 990, 5U)).accepted);
    ASSERT_TRUE(book.submit_limit(make_order(2U, lob::Side::Buy, 1000, 5U)).accepted);

    const auto result = book.submit_limit(make_order(3U, lob::Side::Sell, 990, 5U));

    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::Filled);
    EXPECT_EQ(result.filled_quantity_lots, 5U);
    ASSERT_EQ(result.execution_count, 2U);
    EXPECT_EQ(result.executions[0].counterparty_order_id, 2U);
    EXPECT_EQ(result.executions[0].price_ticks, 1000);
    EXPECT_FALSE(book.remaining_quantity(2U).has_value());
    EXPECT_EQ(book.best_bid(), 990);
}

TEST(MatchingBookTest, FIFOAtSamePrice) {
    TestMatchingBook book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Sell, 1000, 3U, 11U, 1U)).accepted);
    ASSERT_TRUE(book.submit_limit(make_order(2U, lob::Side::Sell, 1000, 3U, 12U, 2U)).accepted);

    const auto result = book.submit_limit(make_order(3U, lob::Side::Buy, 1000, 5U, 13U, 3U));

    ASSERT_EQ(result.execution_count, 4U);
    EXPECT_EQ(result.executions[0].counterparty_order_id, 1U);
    EXPECT_EQ(result.executions[0].quantity_lots, 3U);
    EXPECT_EQ(result.executions[2].counterparty_order_id, 2U);
    EXPECT_EQ(result.executions[2].quantity_lots, 2U);
    EXPECT_FALSE(book.remaining_quantity(1U).has_value());
    EXPECT_EQ(book.remaining_quantity(2U), 1U);
}

TEST(MatchingBookTest, PartialFillLeavesResidualResting) {
    TestMatchingBook book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Sell, 1000, 3U)).accepted);

    const auto result = book.submit_limit(make_order(2U, lob::Side::Buy, 1000, 7U));

    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::PartiallyFilled);
    EXPECT_EQ(result.filled_quantity_lots, 3U);
    EXPECT_EQ(result.remaining_quantity_lots, 4U);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.best_bid(), 1000);
    EXPECT_EQ(book.remaining_quantity(2U), 4U);
}

TEST(MatchingBookTest, ReportCapacityRejectsUnstartedCrossWithoutBookMutation) {
    lob::sim::MatchingBook<4U, 2U, 1U> book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Sell, 1000, 5U)).accepted);

    const auto result = book.submit_limit(make_order(2U, lob::Side::Buy, 1000, 3U));

    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(result.execution_report_overflow);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::RejectedExecutionReportCapacity);
    EXPECT_EQ(result.execution_count, 0U);
    EXPECT_EQ(result.filled_quantity_lots, 0U);
    EXPECT_EQ(result.remaining_quantity_lots, 3U);
    EXPECT_EQ(result.rejected_quantity_lots, 3U);
    EXPECT_EQ(book.remaining_quantity(1U), 5U);
    EXPECT_FALSE(book.remaining_quantity(2U).has_value());
    EXPECT_EQ(book.best_ask(), 1000);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(MatchingBookTest, ReportCapacityAllowsExactlyOneCompleteFill) {
    lob::sim::MatchingBook<4U, 2U, 2U> book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Sell, 1000, 5U)).accepted);

    const auto result = book.submit_limit(make_order(2U, lob::Side::Buy, 1000, 3U));

    EXPECT_TRUE(result.accepted);
    EXPECT_FALSE(result.execution_report_overflow);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::Filled);
    EXPECT_EQ(result.execution_count, 2U);
    EXPECT_EQ(result.filled_quantity_lots, 3U);
    EXPECT_EQ(book.remaining_quantity(1U), 2U);
    EXPECT_FALSE(book.remaining_quantity(2U).has_value());
    EXPECT_EQ(book.best_ask(), 1000);
}

TEST(MatchingBookTest, ReportCapacityStopsBeforeSecondFillWithoutMutation) {
    lob::sim::MatchingBook<8U, 2U, 2U> book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Sell, 1000, 3U, 11U, 1U)).accepted);
    ASSERT_TRUE(book.submit_limit(make_order(2U, lob::Side::Sell, 1000, 3U, 12U, 2U)).accepted);

    const auto result = book.submit_limit(make_order(3U, lob::Side::Buy, 1000, 5U, 13U, 3U));

    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.execution_report_overflow);
    EXPECT_FALSE(result.rested);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::PartiallyFilledExecutionReportCapacity);
    EXPECT_EQ(result.execution_count, 2U);
    EXPECT_EQ(result.filled_quantity_lots, 3U);
    EXPECT_EQ(result.remaining_quantity_lots, 2U);
    EXPECT_FALSE(book.remaining_quantity(1U).has_value());
    EXPECT_EQ(book.remaining_quantity(2U), 3U);
    EXPECT_FALSE(book.remaining_quantity(3U).has_value());
    EXPECT_EQ(book.total_quantity_at_price(lob::Side::Sell, 1000), 3U);
    EXPECT_EQ(book.best_ask(), 1000);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(MatchingBookTest, CancelRemovesRestingOrder) {
    TestMatchingBook book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Buy, 1000, 10U)).accepted);

    const auto cancel = book.cancel(1U);

    EXPECT_TRUE(cancel.canceled);
    EXPECT_EQ(cancel.status, lob::sim::OrderStatus::Canceled);
    EXPECT_EQ(cancel.canceled_quantity_lots, 10U);
    EXPECT_FALSE(book.remaining_quantity(1U).has_value());
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.resting_order_count(), 0U);
}

TEST(MatchingBookTest, CancelUnknownOrderFailsCleanly) {
    TestMatchingBook book {};

    const auto cancel = book.cancel(999U);

    EXPECT_FALSE(cancel.canceled);
    EXPECT_EQ(cancel.status, lob::sim::OrderStatus::Rejected);
}

TEST(MatchingBookTest, BestBidAndAskUpdateAfterFillsAndCancels) {
    TestMatchingBook book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Buy, 1000, 5U)).accepted);
    ASSERT_TRUE(book.submit_limit(make_order(2U, lob::Side::Buy, 990, 5U)).accepted);
    ASSERT_TRUE(book.submit_limit(make_order(3U, lob::Side::Sell, 1010, 5U)).accepted);
    ASSERT_TRUE(book.submit_limit(make_order(4U, lob::Side::Sell, 1020, 5U)).accepted);

    EXPECT_EQ(book.best_bid(), 1000);
    EXPECT_EQ(book.best_ask(), 1010);

    ASSERT_TRUE(book.cancel(1U).canceled);
    EXPECT_EQ(book.best_bid(), 990);

    const auto fill = book.submit_limit(make_order(5U, lob::Side::Buy, 1010, 5U));
    EXPECT_EQ(fill.status, lob::sim::OrderStatus::Filled);
    EXPECT_EQ(book.best_ask(), 1020);
}

TEST(MatchingBookTest, IndexOverflowDoesNotCreateEmptyPriceLevel) {
    lob::sim::MatchingBook<2U, 2U, 4U, 1U> book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Buy, 1000, 5U)).accepted);

    const auto rejected = book.submit_limit(make_order(2U, lob::Side::Sell, 1010, 5U));

    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.status, lob::sim::OrderStatus::Rejected);
    EXPECT_EQ(book.active_price_level_count(), 1U);
    EXPECT_EQ(book.resting_order_count(), 1U);
    EXPECT_EQ(book.best_bid(), 1000);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.total_quantity_at_price(lob::Side::Sell, 1010), 0U);
    EXPECT_FALSE(book.remaining_quantity(2U).has_value());
}

TEST(MatchingBookTest, PriceLevelOverflowRollsBackOrderIndex) {
    lob::sim::MatchingBook<4U, 1U, 4U, 4U> book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Buy, 1000, 5U)).accepted);

    const auto rejected = book.submit_limit(make_order(2U, lob::Side::Sell, 1010, 5U));

    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.status, lob::sim::OrderStatus::Rejected);
    EXPECT_EQ(book.active_price_level_count(), 1U);
    EXPECT_EQ(book.resting_order_count(), 1U);
    EXPECT_FALSE(book.remaining_quantity(2U).has_value());
    EXPECT_FALSE(book.cancel(2U).canceled);
}

TEST(MatchingBookTest, PartialFillResidualRestsWhenMatchingFreesCapacity) {
    lob::sim::MatchingBook<1U, 1U, 4U, 1U> book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Sell, 1000, 3U)).accepted);

    const auto result = book.submit_limit(make_order(2U, lob::Side::Buy, 1000, 7U));

    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.rested);
    EXPECT_FALSE(result.residual_rejected);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::PartiallyFilled);
    EXPECT_EQ(result.filled_quantity_lots, 3U);
    EXPECT_EQ(result.remaining_quantity_lots, 4U);
    EXPECT_EQ(result.rejected_quantity_lots, 0U);
    EXPECT_EQ(result.execution_count, 2U);
    EXPECT_EQ(book.resting_order_count(), 1U);
    EXPECT_EQ(book.active_price_level_count(), 1U);
    EXPECT_EQ(book.best_bid(), 1000);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.remaining_quantity(2U), 4U);
}

TEST(MatchingBookTest, UnfilledGtcRejectsResidualWhenPostMatchRestCapacityUnavailable) {
    lob::sim::MatchingBook<1U, 1U, 4U, 4U> book {};
    ASSERT_TRUE(book.submit_limit(make_order(1U, lob::Side::Buy, 1000, 3U)).accepted);

    const auto result = book.submit_limit(make_order(2U, lob::Side::Buy, 990, 2U));

    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.rested);
    EXPECT_FALSE(result.residual_rejected);
    EXPECT_EQ(result.status, lob::sim::OrderStatus::Rejected);
    EXPECT_EQ(result.filled_quantity_lots, 0U);
    EXPECT_EQ(result.remaining_quantity_lots, 2U);
    EXPECT_EQ(result.rejected_quantity_lots, 2U);
    EXPECT_EQ(result.execution_count, 0U);
    EXPECT_EQ(book.resting_order_count(), 1U);
    EXPECT_EQ(book.active_price_level_count(), 1U);
    EXPECT_EQ(book.best_bid(), 1000);
    EXPECT_FALSE(book.remaining_quantity(2U).has_value());
}

}  // namespace
