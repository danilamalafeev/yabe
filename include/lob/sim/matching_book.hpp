#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "lob/order.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim {

enum class OrderStatus : std::uint8_t {
    Rejected,
    RejectedExecutionReportCapacity,
    Resting,
    PartiallyFilled,
    PartiallyFilledExecutionReportCapacity,
    PartiallyFilledResidualRejected,
    Filled,
    Canceled,
};

enum class LiquidityRole : std::uint8_t {
    Taker,
    Maker,
};

struct SimOrder {
    OrderID order_id {};
    AgentID agent_id {invalid_agent_id};
    Side side {Side::Buy};
    std::int64_t price_ticks {};
    std::uint64_t quantity_lots {};
    TimeNs timestamp_ns {};
    TimeInForce time_in_force {TimeInForce::GTC};
};

struct TradeExecution {
    OrderID order_id {};
    OrderID counterparty_order_id {};
    AgentID agent_id {invalid_agent_id};
    AgentID counterparty_agent_id {invalid_agent_id};
    Side side {Side::Buy};
    LiquidityRole liquidity_role {LiquidityRole::Taker};
    std::int64_t price_ticks {};
    std::uint64_t quantity_lots {};
    TimeNs timestamp_ns {};
};

template <std::size_t MaxExecutionReports>
struct MatchResult {
    bool accepted {};
    bool rested {};
    bool residual_rejected {};
    bool execution_report_overflow {};
    OrderStatus status {OrderStatus::Rejected};
    OrderID order_id {};
    std::uint64_t filled_quantity_lots {};
    std::uint64_t remaining_quantity_lots {};
    std::uint64_t rejected_quantity_lots {};
    std::array<TradeExecution, MaxExecutionReports> executions {};
    std::size_t execution_count {};
};

struct CancelResult {
    bool canceled {};
    OrderID order_id {};
    AgentID agent_id {invalid_agent_id};
    Side side {Side::Buy};
    std::int64_t price_ticks {};
    std::uint64_t canceled_quantity_lots {};
    OrderStatus status {OrderStatus::Rejected};
};

template <std::size_t OrderCapacity,
          std::size_t PriceLevelCapacity,
          std::size_t MaxExecutionReports,
          std::size_t OrderIndexCapacity = (OrderCapacity * 2U) + 1U>
class MatchingBook {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    using Result = MatchResult<MaxExecutionReports>;

    MatchingBook() noexcept {
        for (std::size_t i = 0U; i < OrderCapacity; ++i) {
            free_slots_[i] = static_cast<std::uint16_t>(i);
        }
    }

    [[nodiscard]] Result submit_limit(const SimOrder& order) noexcept {
        Result result {};
        result.order_id = order.order_id;
        result.remaining_quantity_lots = order.quantity_lots;

        if (!is_valid_order(order) || find_slot(order.order_id).has_value()) {
            return result;
        }

        if (order.time_in_force == TimeInForce::FOK && !fok_preflight_passes(order)) {
            return result;
        }

        SimOrder incoming = order;
        std::uint64_t remaining = order.quantity_lots;

        while (remaining > 0U) {
            const std::size_t level_index = best_crossing_level(incoming.side, incoming.price_ticks);
            if (level_index == npos) {
                break;
            }

            PriceLevel& level = levels_[level_index];
            if (level.head_slot == npos) {
                level.active = false;
                continue;
            }

            OrderSlot& maker_slot = orders_[level.head_slot];
            const std::uint64_t fill_qty = std::min(remaining, maker_slot.remaining_quantity_lots);
            if (!can_append_fill_reports(result)) {
                result.execution_report_overflow = true;
                result.remaining_quantity_lots = remaining;
                if (result.filled_quantity_lots == 0U) {
                    result.status = OrderStatus::RejectedExecutionReportCapacity;
                    result.rejected_quantity_lots = remaining;
                } else {
                    result.accepted = true;
                    result.status = OrderStatus::PartiallyFilledExecutionReportCapacity;
                }
                return result;
            }

            remaining -= fill_qty;
            maker_slot.remaining_quantity_lots -= fill_qty;
            level.total_quantity_lots -= fill_qty;
            result.filled_quantity_lots += fill_qty;
            append_executions(result, incoming, maker_slot.order, level.price_ticks, fill_qty);

            if (maker_slot.remaining_quantity_lots == 0U) {
                remove_head_order(level_index);
            }
        }

        result.remaining_quantity_lots = remaining;

        if (remaining == 0U) {
            result.accepted = true;
            result.status = OrderStatus::Filled;
            return result;
        }

        if (result.filled_quantity_lots > 0U) {
            result.accepted = true;
            result.status = OrderStatus::PartiallyFilled;
        }

        if (incoming.time_in_force == TimeInForce::GTC) {
            incoming.quantity_lots = remaining;
            if (rest_order(incoming, remaining)) {
                result.accepted = true;
                result.rested = true;
                result.status = result.filled_quantity_lots > 0U ? OrderStatus::PartiallyFilled : OrderStatus::Resting;
                result.remaining_quantity_lots = remaining;
            } else if (result.filled_quantity_lots > 0U) {
                result.residual_rejected = true;
                result.rejected_quantity_lots = remaining;
                result.status = OrderStatus::PartiallyFilledResidualRejected;
            } else {
                result.rejected_quantity_lots = remaining;
            }
        }

        return result;
    }

    [[nodiscard]] CancelResult cancel(OrderID order_id) noexcept {
        CancelResult result {};
        result.order_id = order_id;

        const std::optional<std::uint16_t> slot_index = find_slot(order_id);
        if (!slot_index.has_value()) {
            return result;
        }

        OrderSlot& slot = orders_[*slot_index];
        result.canceled = true;
        result.agent_id = slot.order.agent_id;
        result.side = slot.order.side;
        result.price_ticks = slot.order.price_ticks;
        result.canceled_quantity_lots = slot.remaining_quantity_lots;
        result.status = OrderStatus::Canceled;

        unlink_order(*slot_index);
        return result;
    }

    [[nodiscard]] std::optional<std::int64_t> best_bid() const noexcept {
        const std::size_t index = best_level(Side::Buy);
        if (index == npos) {
            return std::nullopt;
        }
        return levels_[index].price_ticks;
    }

    [[nodiscard]] std::optional<std::int64_t> best_ask() const noexcept {
        const std::size_t index = best_level(Side::Sell);
        if (index == npos) {
            return std::nullopt;
        }
        return levels_[index].price_ticks;
    }

    [[nodiscard]] std::uint64_t total_quantity_at_price(Side side, std::int64_t price_ticks) const noexcept {
        const std::size_t level_index = find_level(side, price_ticks);
        if (level_index == npos) {
            return 0U;
        }
        return levels_[level_index].total_quantity_lots;
    }

    [[nodiscard]] std::optional<std::uint64_t> remaining_quantity(OrderID order_id) const noexcept {
        const std::optional<std::uint16_t> slot_index = find_slot(order_id);
        if (!slot_index.has_value()) {
            return std::nullopt;
        }
        return orders_[*slot_index].remaining_quantity_lots;
    }

    template <std::size_t Depth>
    void copy_depth(MarketDepthSnapshot<Depth>& snapshot) const noexcept {
        snapshot.bids = {};
        snapshot.asks = {};
        snapshot.bid_count = copy_side_depth(Side::Buy, snapshot.bids);
        snapshot.ask_count = copy_side_depth(Side::Sell, snapshot.asks);
    }

    [[nodiscard]] std::size_t resting_order_count() const noexcept {
        return resting_order_count_;
    }

    [[nodiscard]] std::size_t active_price_level_count() const noexcept {
        return active_price_level_count_;
    }

    void clear() noexcept {
        orders_ = {};
        levels_ = {};
        index_ = {};
        for (std::size_t i = 0U; i < OrderCapacity; ++i) {
            free_slots_[i] = static_cast<std::uint16_t>(i);
        }
        free_slots_ptr_ = 0U;
        resting_order_count_ = 0U;
        active_price_level_count_ = 0U;
    }

private:
    struct OrderSlot {
        SimOrder order {};
        std::uint64_t remaining_quantity_lots {};
        std::size_t level_index {npos};
        std::size_t prev_slot {npos};
        std::size_t next_slot {npos};
        bool active {};
    };

    struct PriceLevel {
        Side side {Side::Buy};
        std::int64_t price_ticks {};
        std::uint64_t total_quantity_lots {};
        std::size_t head_slot {npos};
        std::size_t tail_slot {npos};
        bool active {};
    };

    struct IndexEntry {
        OrderID order_id {invalid_order_id};
        std::uint16_t slot_index {0U};
        std::uint8_t psl {0U};
        bool occupied {false};
    };

    [[nodiscard]] static bool is_valid_order(const SimOrder& order) noexcept {
        return order.order_id != invalid_order_id && order.price_ticks > 0 && order.quantity_lots > 0U;
    }

    [[nodiscard]] static bool crosses(Side incoming_side,
                                      std::int64_t incoming_price,
                                      std::int64_t resting_price) noexcept {
        if (incoming_side == Side::Buy) {
            return resting_price <= incoming_price;
        }
        return resting_price >= incoming_price;
    }

    [[nodiscard]] std::uint64_t fillable_quantity(const SimOrder& order) const noexcept {
        std::uint64_t fillable {};
        const Side resting_side = order.side == Side::Buy ? Side::Sell : Side::Buy;

        for (const PriceLevel& level : levels_) {
            if (!level.active || level.side != resting_side || level.total_quantity_lots == 0U) {
                continue;
            }
            if (!crosses(order.side, order.price_ticks, level.price_ticks)) {
                continue;
            }

            const std::uint64_t needed = order.quantity_lots - fillable;
            fillable += std::min(needed, level.total_quantity_lots);
            if (fillable == order.quantity_lots) {
                break;
            }
        }
        return fillable;
    }

    [[nodiscard]] bool fok_preflight_passes(const SimOrder& order) const noexcept {
        std::uint64_t remaining = order.quantity_lots;
        std::size_t report_count {};
        std::array<bool, PriceLevelCapacity> visited_levels {};

        while (remaining > 0U) {
            const std::size_t level_index = best_crossing_unvisited_level(order.side, order.price_ticks, visited_levels);
            if (level_index == npos) {
                return false;
            }

            const PriceLevel& level = levels_[level_index];
            std::size_t slot_index = level.head_slot;
            while (slot_index != npos && remaining > 0U) {
                const OrderSlot& maker_slot = orders_[slot_index];
                const std::uint64_t fill_qty = std::min(remaining, maker_slot.remaining_quantity_lots);
                if (MaxExecutionReports - report_count < 2U) {
                    return false;
                }
                report_count += 2U;
                remaining -= fill_qty;
                slot_index = maker_slot.next_slot;
            }
            visited_levels[level_index] = true;
        }

        return true;
    }

    [[nodiscard]] std::size_t best_crossing_level(Side incoming_side, std::int64_t incoming_price) const noexcept {
        const Side resting_side = incoming_side == Side::Buy ? Side::Sell : Side::Buy;
        std::size_t best = npos;

        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            const PriceLevel& level = levels_[i];
            if (!level.active || level.side != resting_side || level.total_quantity_lots == 0U) {
                continue;
            }
            if (!crosses(incoming_side, incoming_price, level.price_ticks)) {
                continue;
            }
            if (best == npos || is_better_price(resting_side, level.price_ticks, levels_[best].price_ticks)) {
                best = i;
            }
        }

        return best;
    }

    [[nodiscard]] std::size_t best_crossing_unvisited_level(
        Side incoming_side,
        std::int64_t incoming_price,
        const std::array<bool, PriceLevelCapacity>& visited_levels) const noexcept {
        const Side resting_side = incoming_side == Side::Buy ? Side::Sell : Side::Buy;
        std::size_t best = npos;

        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            const PriceLevel& level = levels_[i];
            if (visited_levels[i] || !level.active || level.side != resting_side || level.total_quantity_lots == 0U) {
                continue;
            }
            if (!crosses(incoming_side, incoming_price, level.price_ticks)) {
                continue;
            }
            if (best == npos || is_better_price(resting_side, level.price_ticks, levels_[best].price_ticks)) {
                best = i;
            }
        }

        return best;
    }

    [[nodiscard]] std::size_t best_level(Side side) const noexcept {
        std::size_t best = npos;
        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            const PriceLevel& level = levels_[i];
            if (!level.active || level.side != side || level.total_quantity_lots == 0U) {
                continue;
            }
            if (best == npos || is_better_price(side, level.price_ticks, levels_[best].price_ticks)) {
                best = i;
            }
        }
        return best;
    }

    template <std::size_t Depth>
    [[nodiscard]] std::size_t copy_side_depth(Side side, std::array<DepthLevel, Depth>& depth) const noexcept {
        std::array<bool, PriceLevelCapacity> visited_levels {};
        std::size_t copied {};

        while (copied < Depth) {
            const std::size_t level_index = best_unvisited_level(side, visited_levels);
            if (level_index == npos) {
                break;
            }
            const PriceLevel& level = levels_[level_index];
            depth[copied++] = DepthLevel {
                .price_ticks = level.price_ticks,
                .quantity_lots = level.total_quantity_lots,
            };
            visited_levels[level_index] = true;
        }

        return copied;
    }

    [[nodiscard]] std::size_t best_unvisited_level(Side side,
                                                   const std::array<bool, PriceLevelCapacity>& visited_levels) const noexcept {
        std::size_t best = npos;
        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            const PriceLevel& level = levels_[i];
            if (visited_levels[i] || !level.active || level.side != side || level.total_quantity_lots == 0U) {
                continue;
            }
            if (best == npos || is_better_price(side, level.price_ticks, levels_[best].price_ticks)) {
                best = i;
            }
        }
        return best;
    }

    [[nodiscard]] static bool is_better_price(Side side, std::int64_t lhs, std::int64_t rhs) noexcept {
        if (side == Side::Buy) {
            return lhs > rhs;
        }
        return lhs < rhs;
    }

    [[nodiscard]] std::size_t find_level(Side side, std::int64_t price_ticks) const noexcept {
        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            if (levels_[i].active && levels_[i].side == side && levels_[i].price_ticks == price_ticks) {
                return i;
            }
        }
        return npos;
    }

    [[nodiscard]] std::size_t find_or_create_level(Side side, std::int64_t price_ticks) noexcept {
        const std::size_t existing = find_level(side, price_ticks);
        if (existing != npos) {
            return existing;
        }

        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            if (!levels_[i].active) {
                levels_[i] = PriceLevel {
                    .side = side,
                    .price_ticks = price_ticks,
                    .total_quantity_lots = 0U,
                    .head_slot = npos,
                    .tail_slot = npos,
                    .active = true,
                };
                ++active_price_level_count_;
                return i;
            }
        }

        return npos;
    }

    [[nodiscard]] static constexpr bool can_append_fill_reports(const Result& result) noexcept {
        return MaxExecutionReports - result.execution_count >= 2U;
    }

    [[nodiscard]] bool rest_order(const SimOrder& order, std::uint64_t remaining_quantity_lots) noexcept {
        if (free_slots_ptr_ >= OrderCapacity) {
            return false;
        }
        const std::size_t slot_index = free_slots_[free_slots_ptr_++];

        if (!insert_index(order.order_id, static_cast<std::uint16_t>(slot_index))) {
            free_slots_[--free_slots_ptr_] = static_cast<std::uint16_t>(slot_index);
            return false;
        }

        const std::size_t level_index = find_or_create_level(order.side, order.price_ticks);
        if (level_index == npos) {
            remove_index(order.order_id);
            free_slots_[--free_slots_ptr_] = static_cast<std::uint16_t>(slot_index);
            return false;
        }

        PriceLevel& level = levels_[level_index];
        OrderSlot& slot = orders_[slot_index];
        slot = OrderSlot {
            .order = order,
            .remaining_quantity_lots = remaining_quantity_lots,
            .level_index = level_index,
            .prev_slot = level.tail_slot,
            .next_slot = npos,
            .active = true,
        };

        if (level.tail_slot != npos) {
            orders_[level.tail_slot].next_slot = slot_index;
        } else {
            level.head_slot = slot_index;
        }
        level.tail_slot = slot_index;
        level.total_quantity_lots += remaining_quantity_lots;
        ++resting_order_count_;
        return true;
    }

    void append_executions(Result& result,
                           const SimOrder& taker,
                           const SimOrder& maker,
                           std::int64_t price_ticks,
                           std::uint64_t quantity_lots) noexcept {
        append_execution(result, TradeExecution {
                                     .order_id = taker.order_id,
                                     .counterparty_order_id = maker.order_id,
                                     .agent_id = taker.agent_id,
                                     .counterparty_agent_id = maker.agent_id,
                                     .side = taker.side,
                                     .liquidity_role = LiquidityRole::Taker,
                                     .price_ticks = price_ticks,
                                     .quantity_lots = quantity_lots,
                                     .timestamp_ns = taker.timestamp_ns,
                                 });
        append_execution(result, TradeExecution {
                                     .order_id = maker.order_id,
                                     .counterparty_order_id = taker.order_id,
                                     .agent_id = maker.agent_id,
                                     .counterparty_agent_id = taker.agent_id,
                                     .side = maker.side,
                                     .liquidity_role = LiquidityRole::Maker,
                                     .price_ticks = price_ticks,
                                     .quantity_lots = quantity_lots,
                                     .timestamp_ns = taker.timestamp_ns,
                                 });
    }

    void append_execution(Result& result, const TradeExecution& execution) noexcept {
        if (result.execution_count >= MaxExecutionReports) {
            result.execution_report_overflow = true;
            return;
        }

        result.executions[result.execution_count++] = execution;
    }

    void remove_head_order(std::size_t level_index) noexcept {
        PriceLevel& level = levels_[level_index];
        if (level.head_slot == npos) {
            return;
        }

        unlink_order(level.head_slot);
    }

    void unlink_order(std::size_t slot_index) noexcept {
        OrderSlot& slot = orders_[slot_index];
        if (!slot.active) {
            return;
        }

        PriceLevel& level = levels_[slot.level_index];
        if (slot.prev_slot != npos) {
            orders_[slot.prev_slot].next_slot = slot.next_slot;
        } else {
            level.head_slot = slot.next_slot;
        }

        if (slot.next_slot != npos) {
            orders_[slot.next_slot].prev_slot = slot.prev_slot;
        } else {
            level.tail_slot = slot.prev_slot;
        }

        level.total_quantity_lots -= slot.remaining_quantity_lots;
        remove_index(slot.order.order_id);
        slot = {};
        free_slots_[--free_slots_ptr_] = static_cast<std::uint16_t>(slot_index);
        --resting_order_count_;

        if (level.head_slot == npos) {
            level = {};
            --active_price_level_count_;
        }
    }

    [[nodiscard]] std::optional<std::uint16_t> find_slot(OrderID order_id) const noexcept {
        if constexpr (OrderIndexCapacity == 0U) {
            return std::nullopt;
        }

        const std::size_t start = hash_order_id(order_id);
        for (std::size_t probe = 0U; probe < OrderIndexCapacity; ++probe) {
            const std::size_t index = (start + probe) % OrderIndexCapacity;
            const IndexEntry& entry = index_[index];
            if (!entry.occupied) {
                return std::nullopt;
            }
            if (entry.order_id == order_id) {
                return entry.slot_index;
            }
            if (entry.psl < static_cast<std::uint8_t>(probe)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool insert_index(OrderID order_id, std::uint16_t slot_index) noexcept {
        if constexpr (OrderIndexCapacity == 0U) {
            return false;
        }

        const std::size_t start = hash_order_id(order_id);
        OrderID rid = order_id;
        std::uint16_t rslot = slot_index;
        std::uint8_t rpsl = 0U;

        for (std::size_t probe = 0U; probe < OrderIndexCapacity; ++probe) {
            const std::size_t index = (start + probe) % OrderIndexCapacity;
            IndexEntry& entry = index_[index];
            if (!entry.occupied) {
                entry = IndexEntry {
                    .order_id = rid,
                    .slot_index = rslot,
                    .psl = rpsl,
                    .occupied = true,
                };
                return true;
            }
            if (entry.order_id == rid) {
                return false;
            }
            if (entry.psl < rpsl) {
                std::swap(rid, entry.order_id);
                std::swap(rslot, entry.slot_index);
                std::swap(rpsl, entry.psl);
            }
            ++rpsl;
        }
        return false;
    }

    void remove_index(OrderID order_id) noexcept {
        if constexpr (OrderIndexCapacity == 0U) {
            return;
        }

        const std::size_t start = hash_order_id(order_id);
        for (std::size_t probe = 0U; probe < OrderIndexCapacity; ++probe) {
            const std::size_t index = (start + probe) % OrderIndexCapacity;
            IndexEntry& entry = index_[index];
            if (!entry.occupied) {
                return;
            }
            if (entry.order_id == order_id) {
                entry.occupied = false;
                std::size_t curr = index;
                std::size_t next = (curr + 1U) % OrderIndexCapacity;
                while (index_[next].occupied && index_[next].psl > 0U) {
                    index_[curr] = index_[next];
                    index_[curr].psl = static_cast<std::uint8_t>(index_[curr].psl - 1U);
                    index_[next].occupied = false;
                    curr = next;
                    next = (next + 1U) % OrderIndexCapacity;
                }
                return;
            }
        }
    }

    [[nodiscard]] static constexpr std::size_t hash_order_id(OrderID order_id) noexcept {
        return static_cast<std::size_t>((order_id ^ (order_id >> 32U)) % OrderIndexCapacity);
    }

    std::array<OrderSlot, OrderCapacity> orders_ {};
    std::array<PriceLevel, PriceLevelCapacity> levels_ {};
    std::array<IndexEntry, OrderIndexCapacity> index_ {};
    std::array<std::uint16_t, OrderCapacity> free_slots_ {};
    std::uint16_t free_slots_ptr_ {0U};
    std::size_t resting_order_count_ {};
    std::size_t active_price_level_count_ {};
};

}  // namespace lob::sim
