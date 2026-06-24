#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "lob/order.hpp"
#include "lob/venue_replay.hpp"

namespace lob::sim {

using AgentID = std::uint32_t;
using MarketID = std::uint32_t;
using OrderID = std::uint64_t;
using TimeNs = std::uint64_t;
using EventSequence = std::uint64_t;

inline constexpr AgentID invalid_agent_id = std::numeric_limits<AgentID>::max();
inline constexpr MarketID invalid_market_id = std::numeric_limits<MarketID>::max();
inline constexpr OrderID invalid_order_id = std::numeric_limits<OrderID>::max();

struct MarketKey {
    VenueID venue_id {};
    ProductID product_id {};

    [[nodiscard]] friend constexpr bool operator==(MarketKey lhs, MarketKey rhs) noexcept = default;
};

enum class TimeInForce : std::uint8_t {
    GTC,
    IOC,
    FOK,
};

enum class EventKind : std::uint8_t {
    AgentWakeup,
    OrderArrivesAtMarket,
    CancelArrivesAtMarket,
    MarketDataArrives,
    Settlement,
};

struct DepthLevel {
    std::int64_t price_ticks {};
    std::uint64_t quantity_lots {};
};

template <std::size_t Depth>
struct MarketDepthSnapshot {
    TimeNs exchange_timestamp_ns {};
    TimeNs receive_timestamp_ns {};
    MarketID market_id {invalid_market_id};
    std::array<DepthLevel, Depth> bids {};
    std::array<DepthLevel, Depth> asks {};
    std::size_t bid_count {};
    std::size_t ask_count {};
};

enum class EventPhase : std::uint8_t {
    PreMarket = 0U,
    Agent = 1U,
    Market = 2U,
    PostMarket = 3U,
};

struct ScheduledEvent {
    TimeNs timestamp_ns {};
    EventPhase phase {EventPhase::Agent};
    EventKind kind {EventKind::AgentWakeup};
    MarketID market_id {invalid_market_id};
    AgentID agent_id {invalid_agent_id};
    OrderID order_id {invalid_order_id};
    Side side {Side::Buy};
    std::int64_t price_ticks {};
    std::uint64_t quantity_lots {};
    TimeInForce time_in_force {TimeInForce::GTC};
    TimeNs client_timestamp_ns {};
    MarketDepthSnapshot<1U> depth_snapshot {};
    EventSequence sequence_number {};
};

}  // namespace lob::sim
