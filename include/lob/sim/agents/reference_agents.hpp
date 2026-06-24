#pragma once

#include <cstddef>
#include <limits>
#include <optional>

#include "lob/sim/agent_context.hpp"
#include "lob/sim/market_agent.hpp"
#include "lob/sim/order_request.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim::agents {

struct PassiveLimitAgentConfig {
    MarketID market_id {invalid_market_id};
    Side side {Side::Buy};
    PriceTicks price_ticks {};
    QtyLots quantity_lots {};
    TimeNs first_wakeup_time_ns {};
    TimeNs repeat_interval_ns {};
    std::size_t max_actions {1U};
};

template <typename Engine>
class PassiveLimitAgent final : public MarketAgent<Engine> {
public:
    explicit constexpr PassiveLimitAgent(PassiveLimitAgentConfig config) noexcept
        : config_(config) {}

    void on_start(AgentContext<Engine>& context) override {
        if (config_.max_actions == 0U) {
            return;
        }
        if (config_.first_wakeup_time_ns <= context.current_time_ns()) {
            submit_next(context);
        } else {
            last_schedule_result_ = context.schedule_wakeup(config_.first_wakeup_time_ns);
        }
    }

    void on_wakeup(AgentContext<Engine>& context) override {
        submit_next(context);
    }

    [[nodiscard]] std::size_t action_count() const noexcept {
        return action_count_;
    }

    [[nodiscard]] GatewayResult last_submit_result() const noexcept {
        return last_submit_result_;
    }

    [[nodiscard]] bool last_schedule_result() const noexcept {
        return last_schedule_result_;
    }

private:
    void submit_next(AgentContext<Engine>& context) noexcept {
        if (action_count_ >= config_.max_actions) {
            return;
        }

        last_submit_result_ = context.submit_order(config_.market_id,
                                                   config_.side,
                                                   config_.price_ticks,
                                                   config_.quantity_lots,
                                                   TimeInForce::GTC);
        ++action_count_;
        schedule_next_if_needed(context);
    }

    void schedule_next_if_needed(AgentContext<Engine>& context) noexcept {
        if (action_count_ < config_.max_actions && config_.repeat_interval_ns > 0U) {
            last_schedule_result_ =
                context.schedule_wakeup(add_time_saturating(context.current_time_ns(), config_.repeat_interval_ns));
        }
    }

    [[nodiscard]] static constexpr TimeNs add_time_saturating(TimeNs lhs, TimeNs rhs) noexcept {
        if (std::numeric_limits<TimeNs>::max() - lhs < rhs) {
            return std::numeric_limits<TimeNs>::max();
        }
        return lhs + rhs;
    }

    PassiveLimitAgentConfig config_ {};
    GatewayResult last_submit_result_ {};
    std::size_t action_count_ {};
    bool last_schedule_result_ {};
};

struct PeriodicWakeupAgentConfig {
    TimeNs first_wakeup_time_ns {};
    TimeNs repeat_interval_ns {};
    std::size_t max_wakeups {};
};

template <typename Engine>
class PeriodicWakeupAgent final : public MarketAgent<Engine> {
public:
    explicit constexpr PeriodicWakeupAgent(PeriodicWakeupAgentConfig config) noexcept
        : config_(config) {}

    void on_start(AgentContext<Engine>& context) override {
        if (config_.max_wakeups > 0U) {
            last_schedule_result_ = context.schedule_wakeup(config_.first_wakeup_time_ns);
        }
    }

    void on_wakeup(AgentContext<Engine>& context) override {
        ++wakeup_count_;
        last_wakeup_time_ns_ = context.current_time_ns();
        if (wakeup_count_ < config_.max_wakeups && config_.repeat_interval_ns > 0U) {
            last_schedule_result_ =
                context.schedule_wakeup(add_time_saturating(context.current_time_ns(), config_.repeat_interval_ns));
        }
    }

    [[nodiscard]] std::size_t wakeup_count() const noexcept {
        return wakeup_count_;
    }

    [[nodiscard]] TimeNs last_wakeup_time_ns() const noexcept {
        return last_wakeup_time_ns_;
    }

    [[nodiscard]] bool last_schedule_result() const noexcept {
        return last_schedule_result_;
    }

private:
    [[nodiscard]] static constexpr TimeNs add_time_saturating(TimeNs lhs, TimeNs rhs) noexcept {
        if (std::numeric_limits<TimeNs>::max() - lhs < rhs) {
            return std::numeric_limits<TimeNs>::max();
        }
        return lhs + rhs;
    }

    PeriodicWakeupAgentConfig config_ {};
    std::size_t wakeup_count_ {};
    TimeNs last_wakeup_time_ns_ {};
    bool last_schedule_result_ {};
};

using ClockAgentConfig = PeriodicWakeupAgentConfig;

template <typename Engine>
using ClockAgent = PeriodicWakeupAgent<Engine>;

struct SimpleLiquidityTakerAgentConfig {
    MarketID market_id {invalid_market_id};
    Side side {Side::Buy};
    PriceTicks price_ticks {};
    PriceTicks price_offset_ticks {};
    QtyLots quantity_lots {};
    TimeNs first_wakeup_time_ns {};
    TimeNs repeat_interval_ns {};
    std::size_t max_actions {1U};
};

template <typename Engine>
class SimpleLiquidityTakerAgent final : public MarketAgent<Engine> {
public:
    explicit constexpr SimpleLiquidityTakerAgent(SimpleLiquidityTakerAgentConfig config) noexcept
        : config_(config) {}

    void on_start(AgentContext<Engine>& context) override {
        if (config_.max_actions > 0U) {
            last_schedule_result_ = context.schedule_wakeup(config_.first_wakeup_time_ns);
        }
    }

    void on_wakeup(AgentContext<Engine>& context) override {
        submit_from_visible_depth(context);
    }

    [[nodiscard]] std::size_t action_count() const noexcept {
        return action_count_;
    }

    [[nodiscard]] GatewayResult last_submit_result() const noexcept {
        return last_submit_result_;
    }

    [[nodiscard]] bool last_had_visible_depth() const noexcept {
        return last_had_visible_depth_;
    }

    [[nodiscard]] std::optional<PriceTicks> last_visible_price_ticks() const noexcept {
        return last_visible_price_ticks_;
    }

private:
    void submit_from_visible_depth(AgentContext<Engine>& context) noexcept {
        if (action_count_ >= config_.max_actions) {
            return;
        }

        last_had_visible_depth_ = false;
        last_visible_price_ticks_ = std::nullopt;
        const auto depth = context.template visible_depth<1U>(config_.market_id);
        if (!depth.has_value()) {
            schedule_next_if_needed(context);
            return;
        }

        PriceTicks visible_price {};
        if (config_.side == Side::Buy) {
            if (depth->ask_count == 0U) {
                schedule_next_if_needed(context);
                return;
            }
            visible_price = depth->asks[0].price_ticks;
            last_submit_result_ = context.submit_order(config_.market_id,
                                                       config_.side,
                                                       visible_price + config_.price_offset_ticks,
                                                       config_.quantity_lots,
                                                       TimeInForce::IOC);
        } else {
            if (depth->bid_count == 0U) {
                schedule_next_if_needed(context);
                return;
            }
            visible_price = depth->bids[0].price_ticks;
            last_submit_result_ = context.submit_order(config_.market_id,
                                                       config_.side,
                                                       visible_price - config_.price_offset_ticks,
                                                       config_.quantity_lots,
                                                       TimeInForce::IOC);
        }

        last_had_visible_depth_ = true;
        last_visible_price_ticks_ = visible_price;
        ++action_count_;
        schedule_next_if_needed(context);
    }

    void schedule_next_if_needed(AgentContext<Engine>& context) noexcept {
        if (action_count_ < config_.max_actions && config_.repeat_interval_ns > 0U) {
            last_schedule_result_ =
                context.schedule_wakeup(add_time_saturating(context.current_time_ns(), config_.repeat_interval_ns));
        }
    }

    [[nodiscard]] static constexpr TimeNs add_time_saturating(TimeNs lhs, TimeNs rhs) noexcept {
        if (std::numeric_limits<TimeNs>::max() - lhs < rhs) {
            return std::numeric_limits<TimeNs>::max();
        }
        return lhs + rhs;
    }

    SimpleLiquidityTakerAgentConfig config_ {};
    GatewayResult last_submit_result_ {};
    std::optional<PriceTicks> last_visible_price_ticks_ {};
    std::size_t action_count_ {};
    bool last_had_visible_depth_ {};
    bool last_schedule_result_ {};
};

}  // namespace lob::sim::agents
