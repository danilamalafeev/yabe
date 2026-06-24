#pragma once

#include <cstdint>

#include "lob/order.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim {

struct OrderRequest {
    AgentID agent_id {invalid_agent_id};
    MarketID market_id {invalid_market_id};
    Side side {Side::Buy};
    std::int64_t price_ticks {};
    std::uint64_t quantity_lots {};
    TimeInForce time_in_force {TimeInForce::GTC};
    TimeNs client_timestamp_ns {};
};

struct CancelRequest {
    AgentID agent_id {invalid_agent_id};
    MarketID market_id {invalid_market_id};
    OrderID order_id {invalid_order_id};
    TimeNs client_timestamp_ns {};
};

enum class GatewayStatus : std::uint8_t {
    Accepted,
    UnknownMarket,
    SchedulerFull,
    InsufficientBalance,
    UnknownAccount,
    ReservationCapacityFull,
    InvalidRequest,
    UnknownOrder,
    Unauthorized,
    Halted,
};

struct GatewayResult {
    bool accepted {};
    GatewayStatus status {GatewayStatus::InvalidRequest};
    OrderID order_id {invalid_order_id};
    MarketID market_id {invalid_market_id};
    TimeNs arrival_timestamp_ns {};
};

}  // namespace lob::sim
