#pragma once

#include <optional>

#include "lob/sim/account_store.hpp"
#include "lob/sim/order_request.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim {

template <typename Engine>
class AgentContext {
public:
    AgentContext(Engine& engine, AgentID agent_id) noexcept
        : engine_(&engine),
          agent_id_(agent_id) {}

    [[nodiscard]] AgentID agent_id() const noexcept {
        return agent_id_;
    }

    [[nodiscard]] TimeNs current_time_ns() const noexcept {
        return engine_->current_time_ns();
    }

    [[nodiscard]] GatewayResult submit_order(OrderRequest request) noexcept {
        request.agent_id = agent_id_;
        return engine_->submit_order(request);
    }

    [[nodiscard]] GatewayResult submit_order(MarketID market_id,
                                             Side side,
                                             PriceTicks price_ticks,
                                             QtyLots quantity_lots,
                                             TimeInForce time_in_force = TimeInForce::GTC,
                                             TimeNs client_timestamp_ns = 0U) noexcept {
        return submit_order(OrderRequest {
            .agent_id = agent_id_,
            .market_id = market_id,
            .side = side,
            .price_ticks = price_ticks,
            .quantity_lots = quantity_lots,
            .time_in_force = time_in_force,
            .client_timestamp_ns = client_timestamp_ns,
        });
    }

    [[nodiscard]] GatewayResult cancel_order(CancelRequest request) noexcept {
        request.agent_id = agent_id_;
        return engine_->cancel_order(request);
    }

    [[nodiscard]] GatewayResult cancel_order(MarketID market_id,
                                             OrderID order_id,
                                             TimeNs client_timestamp_ns = 0U) noexcept {
        return cancel_order(CancelRequest {
            .agent_id = agent_id_,
            .market_id = market_id,
            .order_id = order_id,
            .client_timestamp_ns = client_timestamp_ns,
        });
    }

    [[nodiscard]] bool schedule_wakeup(TimeNs wakeup_time_ns) noexcept {
        return engine_->schedule_agent_wakeup(agent_id_, wakeup_time_ns);
    }

    template <std::size_t Depth>
    [[nodiscard]] std::optional<MarketDepthSnapshot<Depth>> visible_depth(MarketID market_id) const noexcept {
        return engine_->template visible_depth<Depth>(agent_id_, market_id);
    }

    [[nodiscard]] std::optional<AccountBalance> account_balance(AssetID asset_id) const noexcept {
        return engine_->account_balance(agent_id_, asset_id);
    }

private:
    Engine* engine_ {};
    AgentID agent_id_ {invalid_agent_id};
};

}  // namespace lob::sim
