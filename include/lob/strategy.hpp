#pragma once

#include <cstdint>

#include "lob/event_merger.hpp"
#include "lob/order_gateway.hpp"
#include "lob/fixed_matching_book.hpp"
#include "lob/order.hpp"
#include "lob/trade.hpp"

namespace lob {

template <typename Gateway>
class Strategy {
public:
    virtual ~Strategy() = default;

    virtual void on_start(Gateway& gateway) {
        (void)gateway;
    }

    virtual void on_tick(AssetID asset_id, const FixedMatchingBook& book, Gateway& gateway) {
        (void)asset_id;
        on_tick(book, gateway);
    }

    virtual void on_tick(const FixedMatchingBook& book, Gateway& gateway) {
        (void)book;
        (void)gateway;
    }

    virtual void on_fill(const StrategyFill& fill, Gateway& gateway) {
        (void)fill;
        (void)gateway;
    }
};

}  // namespace lob
