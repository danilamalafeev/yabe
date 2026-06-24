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
    static constexpr bool uses_deep_level_index = PriceLevelCapacity > 32U;

    using Result = MatchResult<MaxExecutionReports>;

    MatchingBook() noexcept {
        static_assert(OrderCapacity <= std::numeric_limits<std::uint16_t>::max());
        static_assert(PriceLevelCapacity <= std::numeric_limits<std::uint16_t>::max());
        for (std::size_t i = 0U; i < OrderCapacity; ++i) {
            free_slots_[i] = static_cast<std::uint16_t>(i);
        }
        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            free_level_indices_[i] = static_cast<std::uint16_t>(i);
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

    template <typename Visitor>
    void for_each_level(Side side, Visitor&& visitor) const noexcept {
        if constexpr (uses_deep_level_index) {
            const auto& indices = sorted_indices_for(side);
            const std::size_t count = sorted_count_for(side);
            for (std::size_t position = 0U; position < count; ++position) {
                const PriceLevel& level = levels_[indices[position]];
                if (!visitor(level.price_ticks, level.total_quantity_lots)) {
                    break;
                }
            }
        } else {
            std::array<bool, PriceLevelCapacity> visited {};
            while (true) {
                const std::size_t index = best_unvisited_level(side, visited);
                if (index == npos) {
                    break;
                }
                visited[index] = true;
                const PriceLevel& level = levels_[index];
                if (!visitor(level.price_ticks, level.total_quantity_lots)) {
                    break;
                }
            }
        }
    }

    void clear() noexcept {
        orders_ = {};
        levels_ = {};
        index_ = {};
        level_index_ = {};
        sorted_bid_level_indices_ = {};
        sorted_ask_level_indices_ = {};
        for (std::size_t i = 0U; i < OrderCapacity; ++i) {
            free_slots_[i] = static_cast<std::uint16_t>(i);
        }
        for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
            free_level_indices_[i] = static_cast<std::uint16_t>(i);
        }
        free_slots_ptr_ = 0U;
        free_level_indices_ptr_ = 0U;
        index_size_ = 0U;
        level_index_size_ = 0U;
        sorted_bid_level_count_ = 0U;
        sorted_ask_level_count_ = 0U;
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
        std::size_t psl {0U};
        bool occupied {false};
    };

    struct LevelIndexEntry {
        Side side {Side::Buy};
        std::int64_t price_ticks {};
        std::uint16_t level_index {0U};
        std::size_t psl {0U};
        bool occupied {false};
    };

    static constexpr std::size_t LevelIndexCapacity =
        uses_deep_level_index ? ((PriceLevelCapacity * 2U) + 1U) : 1U;

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

    [[nodiscard]] bool fok_preflight_passes(const SimOrder& order) const noexcept {
        std::uint64_t remaining = order.quantity_lots;
        std::size_t report_count {};

        if constexpr (uses_deep_level_index) {
            const Side resting_side = order.side == Side::Buy ? Side::Sell : Side::Buy;
            const auto& sorted_indices = sorted_indices_for(resting_side);
            const std::size_t level_count = sorted_count_for(resting_side);
            for (std::size_t position = 0U; position < level_count && remaining > 0U; ++position) {
                const PriceLevel& level = levels_[sorted_indices[position]];
                if (!crosses(order.side, order.price_ticks, level.price_ticks)) {
                    break;
                }
                if (!consume_level_for_fok(level, remaining, report_count)) {
                    return false;
                }
            }
            return remaining == 0U;
        } else {
            std::array<bool, PriceLevelCapacity> visited_levels {};
            while (remaining > 0U) {
                const std::size_t level_index =
                    best_crossing_unvisited_level(order.side, order.price_ticks, visited_levels);
                if (level_index == npos) {
                    return false;
                }
                if (!consume_level_for_fok(levels_[level_index], remaining, report_count)) {
                    return false;
                }
                visited_levels[level_index] = true;
            }
            return true;
        }
    }

    [[nodiscard]] std::size_t best_crossing_level(Side incoming_side, std::int64_t incoming_price) const noexcept {
        const Side resting_side = incoming_side == Side::Buy ? Side::Sell : Side::Buy;
        if constexpr (uses_deep_level_index) {
            const std::size_t level_index = best_level(resting_side);
            if (level_index == npos || !crosses(incoming_side, incoming_price, levels_[level_index].price_ticks)) {
                return npos;
            }
            return level_index;
        } else {
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
        if constexpr (uses_deep_level_index) {
            const std::size_t count = sorted_count_for(side);
            return count == 0U ? npos : sorted_indices_for(side)[0U];
        } else {
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
    }

    template <std::size_t Depth>
    [[nodiscard]] std::size_t copy_side_depth(Side side, std::array<DepthLevel, Depth>& depth) const noexcept {
        if constexpr (uses_deep_level_index) {
            const auto& sorted_indices = sorted_indices_for(side);
            const std::size_t copied = std::min(Depth, sorted_count_for(side));
            for (std::size_t i = 0U; i < copied; ++i) {
                const PriceLevel& level = levels_[sorted_indices[i]];
                depth[i] = DepthLevel {
                    .price_ticks = level.price_ticks,
                    .quantity_lots = level.total_quantity_lots,
                };
            }
            return copied;
        } else {
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
        if constexpr (uses_deep_level_index) {
            return find_level_in_index(side, price_ticks);
        } else {
            for (std::size_t i = 0U; i < PriceLevelCapacity; ++i) {
                if (levels_[i].active && levels_[i].side == side && levels_[i].price_ticks == price_ticks) {
                    return i;
                }
            }
            return npos;
        }
    }

    [[nodiscard]] std::size_t find_or_create_level(Side side, std::int64_t price_ticks) noexcept {
        const std::size_t existing = find_level(side, price_ticks);
        if (existing != npos) {
            return existing;
        }

        if (free_level_indices_ptr_ >= PriceLevelCapacity) {
            return npos;
        }

        const std::size_t level_index = free_level_indices_[free_level_indices_ptr_++];
        levels_[level_index] = PriceLevel {
            .side = side,
            .price_ticks = price_ticks,
            .total_quantity_lots = 0U,
            .head_slot = npos,
            .tail_slot = npos,
            .active = true,
        };

        if constexpr (uses_deep_level_index) {
            if (!insert_level_index(side, price_ticks, static_cast<std::uint16_t>(level_index))) {
                levels_[level_index] = {};
                free_level_indices_[--free_level_indices_ptr_] = static_cast<std::uint16_t>(level_index);
                return npos;
            }
            insert_sorted_level_index(side, static_cast<std::uint16_t>(level_index));
        }

        ++active_price_level_count_;
        return level_index;
    }

    [[nodiscard]] bool consume_level_for_fok(const PriceLevel& level,
                                             std::uint64_t& remaining,
                                             std::size_t& report_count) const noexcept {
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
        return true;
    }

    [[nodiscard]] const std::array<std::uint16_t, PriceLevelCapacity>&
    sorted_indices_for(Side side) const noexcept {
        return side == Side::Buy ? sorted_bid_level_indices_ : sorted_ask_level_indices_;
    }

    [[nodiscard]] std::array<std::uint16_t, PriceLevelCapacity>& sorted_indices_for(Side side) noexcept {
        return side == Side::Buy ? sorted_bid_level_indices_ : sorted_ask_level_indices_;
    }

    [[nodiscard]] std::size_t sorted_count_for(Side side) const noexcept {
        return side == Side::Buy ? sorted_bid_level_count_ : sorted_ask_level_count_;
    }

    [[nodiscard]] std::size_t& sorted_count_for(Side side) noexcept {
        return side == Side::Buy ? sorted_bid_level_count_ : sorted_ask_level_count_;
    }

    void insert_sorted_level_index(Side side, std::uint16_t level_index) noexcept {
        auto& indices = sorted_indices_for(side);
        std::size_t& count = sorted_count_for(side);
        const std::int64_t price_ticks = levels_[level_index].price_ticks;
        const auto begin = indices.begin();
        const auto position_it = std::lower_bound(
            begin,
            begin + static_cast<std::ptrdiff_t>(count),
            price_ticks,
            [this, side](std::uint16_t existing_level_index, std::int64_t price) noexcept {
                return is_better_price(side, levels_[existing_level_index].price_ticks, price);
            });
        const std::size_t position = static_cast<std::size_t>(position_it - begin);
        for (std::size_t i = count; i > position; --i) {
            indices[i] = indices[i - 1U];
        }
        indices[position] = level_index;
        ++count;
    }

    void remove_sorted_level_index(Side side, std::uint16_t level_index) noexcept {
        auto& indices = sorted_indices_for(side);
        std::size_t& count = sorted_count_for(side);
        std::size_t position = 0U;
        while (position < count && indices[position] != level_index) {
            ++position;
        }
        if (position == count) {
            return;
        }
        for (std::size_t i = position + 1U; i < count; ++i) {
            indices[i - 1U] = indices[i];
        }
        indices[--count] = 0U;
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

        const std::size_t order_level_index = slot.level_index;
        PriceLevel& level = levels_[order_level_index];
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
            const Side side = level.side;
            const std::int64_t price_ticks = level.price_ticks;
            const std::uint16_t level_index = static_cast<std::uint16_t>(order_level_index);
            if constexpr (uses_deep_level_index) {
                remove_level_index(side, price_ticks);
                remove_sorted_level_index(side, level_index);
            }
            level = {};
            free_level_indices_[--free_level_indices_ptr_] = level_index;
            --active_price_level_count_;
        }
    }

    [[nodiscard]] std::optional<std::uint16_t> find_slot(OrderID order_id) const noexcept {
        if constexpr (OrderIndexCapacity == 0U) {
            return std::nullopt;
        } else {
            const std::size_t start = hash_order_id(order_id);
            for (std::size_t probe = 0U; probe < OrderIndexCapacity; ++probe) {
                const std::size_t index = (start + probe) % OrderIndexCapacity;
                const IndexEntry& entry = index_[index];
                if (!entry.occupied || entry.psl < probe) {
                    return std::nullopt;
                }
                if (entry.order_id == order_id) {
                    return entry.slot_index;
                }
            }
            return std::nullopt;
        }
    }

    [[nodiscard]] bool insert_index(OrderID order_id, std::uint16_t slot_index) noexcept {
        if constexpr (OrderIndexCapacity == 0U) {
            return false;
        } else {
            if (index_size_ >= OrderIndexCapacity) {
                return false;
            }

            IndexEntry incoming {
                .order_id = order_id,
                .slot_index = slot_index,
                .psl = 0U,
                .occupied = true,
            };
            std::size_t index = hash_order_id(order_id);
            for (std::size_t visited = 0U; visited < OrderIndexCapacity; ++visited) {
                IndexEntry& entry = index_[index];
                if (!entry.occupied) {
                    entry = incoming;
                    ++index_size_;
                    return true;
                }
                if (entry.order_id == incoming.order_id) {
                    return false;
                }
                if (entry.psl < incoming.psl) {
                    std::swap(entry, incoming);
                }
                index = (index + 1U) % OrderIndexCapacity;
                ++incoming.psl;
            }
            return false;
        }
    }

    void remove_index(OrderID order_id) noexcept {
        if constexpr (OrderIndexCapacity == 0U) {
            return;
        } else {
            const std::size_t start = hash_order_id(order_id);
            for (std::size_t probe = 0U; probe < OrderIndexCapacity; ++probe) {
                const std::size_t index = (start + probe) % OrderIndexCapacity;
                const IndexEntry& entry = index_[index];
                if (!entry.occupied || entry.psl < probe) {
                    return;
                }
                if (entry.order_id == order_id) {
                    backward_shift_order_index(index);
                    --index_size_;
                    return;
                }
            }
        }
    }

    void backward_shift_order_index(std::size_t hole) noexcept {
        std::size_t next = (hole + 1U) % OrderIndexCapacity;
        while (index_[next].occupied && index_[next].psl > 0U) {
            index_[hole] = index_[next];
            --index_[hole].psl;
            hole = next;
            next = (next + 1U) % OrderIndexCapacity;
        }
        index_[hole] = {};
    }

    [[nodiscard]] std::size_t find_level_in_index(Side side, std::int64_t price_ticks) const noexcept {
        const std::size_t start = hash_level_key(side, price_ticks);
        for (std::size_t probe = 0U; probe < LevelIndexCapacity; ++probe) {
            const std::size_t index = (start + probe) % LevelIndexCapacity;
            const LevelIndexEntry& entry = level_index_[index];
            if (!entry.occupied || entry.psl < probe) {
                return npos;
            }
            if (entry.side == side && entry.price_ticks == price_ticks) {
                return entry.level_index;
            }
        }
        return npos;
    }

    [[nodiscard]] bool insert_level_index(Side side,
                                          std::int64_t price_ticks,
                                          std::uint16_t level_index) noexcept {
        if (level_index_size_ >= LevelIndexCapacity) {
            return false;
        }

        LevelIndexEntry incoming {
            .side = side,
            .price_ticks = price_ticks,
            .level_index = level_index,
            .psl = 0U,
            .occupied = true,
        };
        std::size_t index = hash_level_key(side, price_ticks);
        for (std::size_t visited = 0U; visited < LevelIndexCapacity; ++visited) {
            LevelIndexEntry& entry = level_index_[index];
            if (!entry.occupied) {
                entry = incoming;
                ++level_index_size_;
                return true;
            }
            if (entry.side == incoming.side && entry.price_ticks == incoming.price_ticks) {
                return false;
            }
            if (entry.psl < incoming.psl) {
                std::swap(entry, incoming);
            }
            index = (index + 1U) % LevelIndexCapacity;
            ++incoming.psl;
        }
        return false;
    }

    void remove_level_index(Side side, std::int64_t price_ticks) noexcept {
        const std::size_t start = hash_level_key(side, price_ticks);
        for (std::size_t probe = 0U; probe < LevelIndexCapacity; ++probe) {
            const std::size_t index = (start + probe) % LevelIndexCapacity;
            const LevelIndexEntry& entry = level_index_[index];
            if (!entry.occupied || entry.psl < probe) {
                return;
            }
            if (entry.side == side && entry.price_ticks == price_ticks) {
                backward_shift_level_index(index);
                --level_index_size_;
                return;
            }
        }
    }

    void backward_shift_level_index(std::size_t hole) noexcept {
        std::size_t next = (hole + 1U) % LevelIndexCapacity;
        while (level_index_[next].occupied && level_index_[next].psl > 0U) {
            level_index_[hole] = level_index_[next];
            --level_index_[hole].psl;
            hole = next;
            next = (next + 1U) % LevelIndexCapacity;
        }
        level_index_[hole] = {};
    }

    [[nodiscard]] static constexpr std::size_t hash_order_id(OrderID order_id) noexcept {
        if constexpr (OrderIndexCapacity == 0U) {
            return 0U;
        } else {
            return static_cast<std::size_t>((order_id ^ (order_id >> 32U)) % OrderIndexCapacity);
        }
    }

    [[nodiscard]] static constexpr std::size_t hash_level_key(Side side, std::int64_t price_ticks) noexcept {
        std::uint64_t value = static_cast<std::uint64_t>(price_ticks);
        value ^= side == Side::Buy ? 0x9e3779b97f4a7c15ULL : 0xd1b54a32d192ed03ULL;
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<std::size_t>(value % LevelIndexCapacity);
    }

    std::array<OrderSlot, OrderCapacity> orders_ {};
    std::array<PriceLevel, PriceLevelCapacity> levels_ {};
    std::array<IndexEntry, OrderIndexCapacity> index_ {};
    std::array<LevelIndexEntry, LevelIndexCapacity> level_index_ {};
    std::array<std::uint16_t, PriceLevelCapacity> sorted_bid_level_indices_ {};
    std::array<std::uint16_t, PriceLevelCapacity> sorted_ask_level_indices_ {};
    std::array<std::uint16_t, OrderCapacity> free_slots_ {};
    std::array<std::uint16_t, PriceLevelCapacity> free_level_indices_ {};
    std::uint16_t free_slots_ptr_ {0U};
    std::uint16_t free_level_indices_ptr_ {0U};
    std::size_t index_size_ {};
    std::size_t level_index_size_ {};
    std::size_t sorted_bid_level_count_ {};
    std::size_t sorted_ask_level_count_ {};
    std::size_t resting_order_count_ {};
    std::size_t active_price_level_count_ {};
};

}  // namespace lob::sim
