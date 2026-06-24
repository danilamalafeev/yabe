#pragma once

#include "lob/sim/account_store.hpp"
#include "lob/sim/matching_book.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim {

template <typename Engine>
class AgentContext;

struct AgentFillEvent {
    TimeNs timestamp_ns {};
    MarketID market_id {invalid_market_id};
    OrderID order_id {invalid_order_id};
    Side side {Side::Buy};
    LiquidityRole liquidity_role {LiquidityRole::Taker};
    PriceTicks price_ticks {};
    QtyLots quantity_lots {};
    BalanceLots fee_lots {};
};

struct AgentCancelEvent {
    TimeNs timestamp_ns {};
    MarketID market_id {invalid_market_id};
    OrderID order_id {invalid_order_id};
    bool canceled {};
    QtyLots canceled_quantity_lots {};
};

template <typename Engine>
class MarketAgent {
public:
    virtual ~MarketAgent() = default;

    virtual void on_start(AgentContext<Engine>& /*context*/) {}
    virtual void on_wakeup(AgentContext<Engine>& /*context*/) {}
    virtual void on_fill(const AgentFillEvent& /*event*/, AgentContext<Engine>& /*context*/) {}
    virtual void on_cancel(const AgentCancelEvent& /*event*/, AgentContext<Engine>& /*context*/) {}
};

}  // namespace lob::sim
