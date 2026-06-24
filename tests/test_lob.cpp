#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lob/analytics.hpp"
#include "lob/csv_parser.hpp"
#include "lob/dynamic_wallet.hpp"
#include "lob/event_merger.hpp"
#include "lob/graph_arbitrage_engine.hpp"
#include "lob/l2_backtest_engine.hpp"
#include "lob/l2_market_maker_strategy.hpp"
#include "lob/l2_order_book.hpp"
#include "lob/l2_depth5_csv_parser.hpp"
#include "lob/l2_update_csv_parser.hpp"
#include "lob/market_maker_strategy.hpp"
#include "lob/multi_asset_backtest_engine.hpp"
#include "lob/order_book.hpp"
#include "lob/PolymarketFeedAdapter.hpp"
#include "lob/PolymarketOrderBook.hpp"
#include "lob/PolymarketTypes.hpp"
#include "lob/venue_manifest.hpp"
#include "lob/venue_replay.hpp"

namespace {

[[nodiscard]] std::int64_t P(double p) noexcept {
    return static_cast<std::int64_t>(std::llround(p * 100'000'000.0));
}

[[nodiscard]] std::uint64_t Q(double q) noexcept {
    return static_cast<std::uint64_t>(std::llround(q * 100'000'000.0));
}

template <std::size_t Capacity>
class FakeStreamParser {
public:
    FakeStreamParser() = default;

    explicit FakeStreamParser(std::array<std::uint64_t, Capacity> timestamps, std::size_t size)
        : timestamps_(timestamps),
          size_(size) {}

    [[nodiscard]] bool has_next() const noexcept {
        return index_ < size_;
    }

    [[nodiscard]] std::uint64_t peek_time() const noexcept {
        return timestamps_[index_];
    }

    [[nodiscard]] lob::Event pop() noexcept {
        const std::uint64_t timestamp = timestamps_[index_++];
        return lob::Event {
            .timestamp = timestamp,
            .order = lob::Order {
                .id = timestamp,
                .price = 100.0,
                .quantity = 0U,
                .side = lob::Side::Buy,
                .timestamp = timestamp,
            },
        };
    }

private:
    std::array<std::uint64_t, Capacity> timestamps_ {};
    std::size_t size_ {};
    std::size_t index_ {};
};

class CountingStrategy final : public lob::Strategy {
public:
    void on_tick(lob::AssetID asset_id, const lob::OrderBook& book, lob::OrderGateway& gateway) override {
        (void)book;
        (void)gateway;
        ++ticks_by_asset_[asset_id];
    }

    std::array<std::uint64_t, 2U> ticks_by_asset_ {};
};

TEST(OrderBookTest, InitializesSuccessfully) {
    const lob::OrderBook order_book {};
    (void)order_book;

    ASSERT_TRUE(true);
}

TEST(L2OrderBookTest, MaintainsBidAndAskSortOrder) {
    lob::L2OrderBook book {8U};

    book.update_level(true, P(99.0), Q(1.0));
    book.update_level(true, P(101.0), Q(2.0));
    book.update_level(true, P(100.0), Q(3.0));
    book.update_level(false, P(103.0), Q(4.0));
    book.update_level(false, P(101.0), Q(5.0));
    book.update_level(false, P(102.0), Q(6.0));

    ASSERT_EQ(book.bids().size(), 3U);
    EXPECT_EQ(book.bids()[0].price, P(101.0));
    EXPECT_EQ(book.bids()[1].price, P(100.0));
    EXPECT_EQ(book.bids()[2].price, P(99.0));

    ASSERT_EQ(book.asks().size(), 3U);
    EXPECT_EQ(book.asks()[0].price, P(101.0));
    EXPECT_EQ(book.asks()[1].price, P(102.0));
    EXPECT_EQ(book.asks()[2].price, P(103.0));
}

TEST(L2OrderBookTest, UpdatesAndRemovesLevels) {
    lob::L2OrderBook book {8U};

    book.update_level(true, P(100.0), Q(5.0));
    book.deplete_level(true, P(100.0), Q(2.0));
    EXPECT_EQ(book.effective_qty(true, P(100.0)), Q(3.0));

    book.update_level(true, P(100.0), Q(1.0));
    ASSERT_EQ(book.bids().size(), 1U);
    EXPECT_EQ(book.bids()[0].qty, Q(1.0));
    EXPECT_EQ(book.bids()[0].depleted_qty, Q(1.0));
    EXPECT_EQ(book.effective_qty(true, P(100.0)), 0U);

    book.update_level(true, P(100.0), 0U);
    EXPECT_TRUE(book.bids().empty());
    EXPECT_EQ(book.effective_qty(true, P(100.0)), 0U);
}

TEST(L2OrderBookTest, AllowsSimulatedDepletionBeyondVisibleQuantityUntilFeedRefresh) {
    lob::L2OrderBook book {8U};

    book.update_level(false, P(101.0), Q(5.0));
    book.deplete_level(false, P(101.0), Q(8.0));

    ASSERT_EQ(book.asks().size(), 1U);
    EXPECT_EQ(book.asks()[0].depleted_qty, Q(8.0));
    EXPECT_EQ(book.effective_qty(false, P(101.0)), 0U);

    book.update_level(false, P(101.0), Q(6.0));
    EXPECT_EQ(book.asks()[0].depleted_qty, Q(6.0));
    EXPECT_EQ(book.effective_qty(false, P(101.0)), 0U);
}

TEST(L2OrderBookTest, EnforcesCapacityByDroppingDeepLevels) {
    lob::L2OrderBook book {3U};

    book.update_level(true, P(100.0), Q(1.0));
    book.update_level(true, P(99.0), Q(1.0));
    book.update_level(true, P(98.0), Q(1.0));
    book.update_level(true, P(97.0), Q(1.0));

    ASSERT_EQ(book.bids().size(), 3U);
    EXPECT_EQ(book.bids()[0].price, P(100.0));
    EXPECT_EQ(book.bids()[1].price, P(99.0));
    EXPECT_EQ(book.bids()[2].price, P(98.0));

    book.update_level(true, P(101.0), Q(1.0));
    ASSERT_EQ(book.bids().size(), 3U);
    EXPECT_EQ(book.bids()[0].price, P(101.0));
    EXPECT_EQ(book.bids()[1].price, P(100.0));
    EXPECT_EQ(book.bids()[2].price, P(99.0));

    book.update_level(false, P(101.0), Q(1.0));
    book.update_level(false, P(102.0), Q(1.0));
    book.update_level(false, P(103.0), Q(1.0));
    book.update_level(false, P(104.0), Q(1.0));

    ASSERT_EQ(book.asks().size(), 3U);
    EXPECT_EQ(book.asks()[0].price, P(101.0));
    EXPECT_EQ(book.asks()[1].price, P(102.0));
    EXPECT_EQ(book.asks()[2].price, P(103.0));

    book.update_level(false, P(100.0), Q(1.0));
    ASSERT_EQ(book.asks().size(), 3U);
    EXPECT_EQ(book.asks()[0].price, P(100.0));
    EXPECT_EQ(book.asks()[1].price, P(101.0));
    EXPECT_EQ(book.asks()[2].price, P(102.0));
}

TEST(L2OrderBookTest, AppliesSnapshotByClearingBothSides) {
    lob::L2OrderBook book {8U};
    book.update_level(true, P(100.0), Q(1.0));
    book.update_level(false, P(101.0), Q(1.0));

    book.apply_update(lob::L2UpdateEvent {
        .timestamp_ns = 10U,
        .price = P(99.0),
        .qty = Q(2.0),
        .is_snapshot = true,
        .is_bid = true,
    });

    ASSERT_EQ(book.bids().size(), 1U);
    EXPECT_EQ(book.bids()[0].price, P(99.0));
    EXPECT_TRUE(book.asks().empty());
}

TEST(L2OrderBookTest, RemovesLevelsNotPresentInSnapshotWithoutClearingRetainedDepletion) {
    lob::L2OrderBook book {8U};
    book.update_level(true, P(101.0), Q(3.0));
    book.update_level(true, P(100.0), Q(5.0));
    book.update_level(true, P(99.0), Q(7.0));
    book.deplete_level(true, P(100.0), Q(2.0));

    const std::array<std::int64_t, 2U> retained_prices {P(101.0), P(100.0)};
    book.remove_levels_not_in(true, retained_prices);

    ASSERT_EQ(book.bids().size(), 2U);
    EXPECT_EQ(book.bids()[0].price, P(101.0));
    EXPECT_EQ(book.bids()[0].depleted_qty, 0U);
    EXPECT_EQ(book.bids()[1].price, P(100.0));
    EXPECT_EQ(book.bids()[1].depleted_qty, Q(2.0));
    EXPECT_EQ(book.effective_qty(true, P(99.0)), 0U);
}

TEST(PolymarketOrderBookTest, MaintainsDepthByCentIndexAndFindsBboWithMasks) {
    lob::PolymarketOrderBook book {};

    EXPECT_TRUE(book.apply_delta(lob::Side::Buy, lob::PriceCents {42U}, 10.0));
    EXPECT_TRUE(book.apply_delta(lob::Side::Buy, lob::PriceCents {97U}, 4.0));
    EXPECT_TRUE(book.apply_delta(lob::Side::Sell, lob::PriceCents {55U}, 7.0));
    EXPECT_TRUE(book.apply_delta(lob::Side::Sell, lob::PriceCents {12U}, 2.0));

    const lob::PolymarketOrderBook::BBO bbo = book.get_bbo();
    ASSERT_TRUE(bbo.has_bid());
    ASSERT_TRUE(bbo.has_ask());
    EXPECT_EQ(bbo.bid.price_cents, lob::PriceCents {97U});
    EXPECT_DOUBLE_EQ(bbo.bid.size, 4.0);
    EXPECT_EQ(bbo.ask.price_cents, lob::PriceCents {12U});
    EXPECT_DOUBLE_EQ(bbo.ask.size, 2.0);
    EXPECT_DOUBLE_EQ(book.bids()[97U], 4.0);
    EXPECT_DOUBLE_EQ(book.asks()[12U], 2.0);
}

TEST(PolymarketOrderBookTest, RemovesTopLevelAndRejectsOutOfRangePrices) {
    lob::PolymarketOrderBook book {};

    EXPECT_FALSE(book.apply_delta(lob::Side::Buy, lob::PriceCents {0U}, 1.0));
    EXPECT_FALSE(book.apply_delta(lob::Side::Sell, lob::PriceCents {100U}, 1.0));
    EXPECT_TRUE(book.apply_delta(lob::Side::Buy, lob::PriceCents {98U}, 1.0));
    EXPECT_TRUE(book.apply_delta(lob::Side::Buy, lob::PriceCents {99U}, 2.0));
    EXPECT_EQ(book.best_bid_price_cents(), lob::PriceCents {99U});

    EXPECT_TRUE(book.apply_delta(lob::Side::Buy, lob::PriceCents {99U}, 0.0));
    EXPECT_EQ(book.best_bid_price_cents(), lob::PriceCents {98U});
    EXPECT_DOUBLE_EQ(book.depth(lob::Side::Buy, lob::PriceCents {99U}), 0.0);
    EXPECT_DOUBLE_EQ(book.depth(lob::Side::Buy, lob::PriceCents {98U}), 1.0);
}

TEST(PolymarketFeedAdapterTest, RoutesDenseTokenUpdatesToBoundBooks) {
    lob::PolymarketOrderBook yes_book {};
    lob::PolymarketOrderBook no_book {};
    std::array<lob::PolymarketOrderBook*, 4U> books_by_token {};
    lob::PolymarketFeedAdapter adapter {books_by_token};

    ASSERT_TRUE(adapter.bind_book(lob::TokenId {1U}, yes_book));
    ASSERT_TRUE(adapter.bind_book(lob::TokenId {2U}, no_book));

    EXPECT_TRUE(adapter.on_l2_update(lob::PolymarketL2Update {
        .timestamp_ns = 1U,
        .new_size = static_cast<std::uint64_t>(std::llround(12.0 * 100'000'000.0)),
        .market_id = lob::MarketId {7U},
        .token_id = lob::TokenId {1U},
        .side = lob::Side::Buy,
        .price_cents = lob::PriceCents {64U},
        .is_snapshot = false,
    }));
    EXPECT_TRUE(adapter.on_l2_update(lob::PolymarketL2Update {
        .timestamp_ns = 2U,
        .new_size = static_cast<std::uint64_t>(std::llround(8.0 * 100'000'000.0)),
        .market_id = lob::MarketId {7U},
        .token_id = lob::TokenId {2U},
        .side = lob::Side::Sell,
        .price_cents = lob::PriceCents {36U},
        .is_snapshot = false,
    }));

    EXPECT_EQ(yes_book.best_bid_price_cents(), lob::PriceCents {64U});
    EXPECT_EQ(no_book.best_ask_price_cents(), lob::PriceCents {36U});
    EXPECT_FALSE(adapter.on_l2_update(lob::PolymarketL2Update {
        .timestamp_ns = 3U,
        .new_size = static_cast<std::uint64_t>(std::llround(1.0 * 100'000'000.0)),
        .market_id = lob::MarketId {7U},
        .token_id = lob::TokenId {3U},
        .side = lob::Side::Buy,
        .price_cents = lob::PriceCents {50U},
        .is_snapshot = false,
    }));
}

TEST(DynamicWalletTest, TracksReservedAndFreeBalances) {
    lob::DynamicWallet wallet {};
    wallet.reset(2U, 0U, 100.0);

    EXPECT_TRUE(wallet.reserve_balance(0U, 60.0));
    EXPECT_DOUBLE_EQ(wallet.balance(0U), 100.0);
    EXPECT_DOUBLE_EQ(wallet.reserved(0U), 60.0);
    EXPECT_DOUBLE_EQ(wallet.free_balance(0U), 40.0);
    EXPECT_FALSE(wallet.reserve_balance(0U, 50.0));

    EXPECT_TRUE(wallet.consume_reserved(0U, 25.0));
    EXPECT_DOUBLE_EQ(wallet.balance(0U), 75.0);
    EXPECT_DOUBLE_EQ(wallet.reserved(0U), 35.0);
    EXPECT_DOUBLE_EQ(wallet.free_balance(0U), 40.0);

    wallet.release_reserved(0U, 35.0);
    EXPECT_DOUBLE_EQ(wallet.balance(0U), 75.0);
    EXPECT_DOUBLE_EQ(wallet.reserved(0U), 0.0);
    EXPECT_DOUBLE_EQ(wallet.free_balance(0U), 75.0);
}

TEST(OrderBookTest, ProcessOrderRestsBuyOrderWhenBookDoesNotCross) {
    lob::OrderBook order_book {};
    const lob::Order order {
        .id = 1U,
        .price = 100.25,
        .quantity = 10U,
        .side = lob::Side::Buy,
        .timestamp = 1'000U,
    };

    const auto trades = order_book.process_order(order);

    ASSERT_TRUE(trades.empty());
    ASSERT_EQ(order_book.bids().size(), 1U);
    ASSERT_TRUE(order_book.asks().empty());
    ASSERT_EQ(order_book.order_index().size(), 1U);

    const auto bid_level_it = order_book.bids().find(order.price);
    ASSERT_NE(bid_level_it, order_book.bids().end());
    ASSERT_EQ(bid_level_it->second.orders.size(), 1U);
    EXPECT_EQ(bid_level_it->second.orders.back().id, order.id);
    EXPECT_EQ(order_book.order_index().at(order.id).order_iterator->id, order.id);
}

TEST(OrderBookTest, ProcessOrderRestsSellOrderWhenBookDoesNotCross) {
    lob::OrderBook order_book {};
    const lob::Order order {
        .id = 2U,
        .price = 100.50,
        .quantity = 12U,
        .side = lob::Side::Sell,
        .timestamp = 1'001U,
    };

    const auto trades = order_book.process_order(order);

    ASSERT_TRUE(trades.empty());
    ASSERT_TRUE(order_book.bids().empty());
    ASSERT_EQ(order_book.asks().size(), 1U);
    ASSERT_EQ(order_book.order_index().size(), 1U);

    const auto ask_level_it = order_book.asks().find(order.price);
    ASSERT_NE(ask_level_it, order_book.asks().end());
    ASSERT_EQ(ask_level_it->second.orders.size(), 1U);
    EXPECT_EQ(ask_level_it->second.orders.back().id, order.id);
    EXPECT_EQ(order_book.order_index().at(order.id).order_iterator->id, order.id);
}

TEST(OrderBookTest, ProcessOrderMatchesCrossSpreadUsingPriceTimePriority) {
    lob::OrderBook order_book {};

    const auto first_resting_trades = order_book.process_order(lob::Order {
        .id = 10U,
        .price = 100.00,
        .quantity = 5U,
        .side = lob::Side::Sell,
        .timestamp = 1U,
    });
    const auto second_resting_trades = order_book.process_order(lob::Order {
        .id = 11U,
        .price = 100.00,
        .quantity = 7U,
        .side = lob::Side::Sell,
        .timestamp = 2U,
    });
    const auto third_resting_trades = order_book.process_order(lob::Order {
        .id = 12U,
        .price = 101.00,
        .quantity = 4U,
        .side = lob::Side::Sell,
        .timestamp = 3U,
    });

    ASSERT_TRUE(first_resting_trades.empty());
    ASSERT_TRUE(second_resting_trades.empty());
    ASSERT_TRUE(third_resting_trades.empty());

    const auto trades = order_book.process_order(lob::Order {
        .id = 20U,
        .price = 101.00,
        .quantity = 14U,
        .side = lob::Side::Buy,
        .timestamp = 4U,
    });

    ASSERT_EQ(trades.size(), 3U);
    EXPECT_EQ(trades[0].buyer_id, 20U);
    EXPECT_EQ(trades[0].seller_id, 10U);
    EXPECT_EQ(trades[0].taker_order_id, 20U);
    EXPECT_DOUBLE_EQ(trades[0].price, 100.00);
    EXPECT_EQ(trades[0].quantity, 5U);

    EXPECT_EQ(trades[1].buyer_id, 20U);
    EXPECT_EQ(trades[1].seller_id, 11U);
    EXPECT_EQ(trades[1].taker_order_id, 20U);
    EXPECT_DOUBLE_EQ(trades[1].price, 100.00);
    EXPECT_EQ(trades[1].quantity, 7U);

    EXPECT_EQ(trades[2].buyer_id, 20U);
    EXPECT_EQ(trades[2].seller_id, 12U);
    EXPECT_EQ(trades[2].taker_order_id, 20U);
    EXPECT_DOUBLE_EQ(trades[2].price, 101.00);
    EXPECT_EQ(trades[2].quantity, 2U);

    ASSERT_TRUE(order_book.bids().empty());
    ASSERT_EQ(order_book.asks().size(), 1U);
    ASSERT_EQ(order_book.asks().count(100.00), 0U);
    ASSERT_EQ(order_book.order_index().count(10U), 0U);
    ASSERT_EQ(order_book.order_index().count(11U), 0U);
    ASSERT_EQ(order_book.order_index().count(20U), 0U);
    ASSERT_EQ(order_book.order_index().count(12U), 1U);
    ASSERT_EQ(order_book.asks().at(101.00).orders.size(), 1U);
    EXPECT_EQ(order_book.asks().at(101.00).orders.front().id, 12U);
    EXPECT_EQ(order_book.asks().at(101.00).orders.front().quantity, 2U);
}

TEST(OrderBookTest, ProcessOrderRestsResidualQuantityAfterPartialIncomingFill) {
    lob::OrderBook order_book {};

    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 30U,
        .price = 100.00,
        .quantity = 4U,
        .side = lob::Side::Sell,
        .timestamp = 10U,
    }).empty());

    const auto trades = order_book.process_order(lob::Order {
        .id = 31U,
        .price = 101.00,
        .quantity = 10U,
        .side = lob::Side::Buy,
        .timestamp = 11U,
    });

    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().buyer_id, 31U);
    EXPECT_EQ(trades.front().seller_id, 30U);
    EXPECT_DOUBLE_EQ(trades.front().price, 100.00);
    EXPECT_EQ(trades.front().quantity, 4U);

    ASSERT_TRUE(order_book.asks().empty());
    ASSERT_EQ(order_book.bids().size(), 1U);
    ASSERT_EQ(order_book.order_index().count(30U), 0U);
    ASSERT_EQ(order_book.order_index().count(31U), 1U);
    ASSERT_EQ(order_book.bids().at(101.00).orders.size(), 1U);
    EXPECT_EQ(order_book.bids().at(101.00).orders.front().quantity, 6U);
}

TEST(OrderBookTest, CancelOrderRemovesEmptyBidPriceLevelAndIndexEntry) {
    lob::OrderBook order_book {};
    const lob::Order first_order {
        .id = 10U,
        .price = 99.75,
        .quantity = 15U,
        .side = lob::Side::Buy,
        .timestamp = 2'000U,
    };
    const lob::Order second_order {
        .id = 11U,
        .price = 99.75,
        .quantity = 20U,
        .side = lob::Side::Buy,
        .timestamp = 2'001U,
    };

    ASSERT_TRUE(order_book.process_order(first_order).empty());
    ASSERT_TRUE(order_book.process_order(second_order).empty());

    ASSERT_TRUE(order_book.cancel_order(first_order.id));
    ASSERT_EQ(order_book.bids().size(), 1U);
    ASSERT_EQ(order_book.bids().at(first_order.price).orders.size(), 1U);
    ASSERT_EQ(order_book.order_index().count(first_order.id), 0U);

    ASSERT_TRUE(order_book.cancel_order(second_order.id));
    ASSERT_TRUE(order_book.bids().empty());
    ASSERT_EQ(order_book.order_index().count(second_order.id), 0U);
}

TEST(OrderBookTest, CancelOrderReturnsFalseForUnknownId) {
    lob::OrderBook order_book {};
    EXPECT_FALSE(order_book.cancel_order(999U));
}

TEST(OrderBookTest, ExposesBestPricesAndL2Snapshot) {
    lob::OrderBook order_book {};
    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 100U,
        .price = 99.50,
        .quantity = 10U,
        .side = lob::Side::Buy,
        .timestamp = 1U,
    }).empty());
    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 101U,
        .price = 99.50,
        .quantity = 15U,
        .side = lob::Side::Buy,
        .timestamp = 2U,
    }).empty());
    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 102U,
        .price = 100.50,
        .quantity = 20U,
        .side = lob::Side::Sell,
        .timestamp = 3U,
    }).empty());

    EXPECT_DOUBLE_EQ(order_book.get_best_bid(), 99.50);
    EXPECT_DOUBLE_EQ(order_book.get_best_ask(), 100.50);

    std::vector<lob::PriceLevelInfo> bids {};
    std::vector<lob::PriceLevelInfo> asks {};
    bids.reserve(5U);
    asks.reserve(5U);

    order_book.get_l2_snapshot(bids, asks, 5);

    ASSERT_EQ(bids.size(), 1U);
    ASSERT_EQ(asks.size(), 1U);
    EXPECT_DOUBLE_EQ(bids.front().price, 99.50);
    EXPECT_DOUBLE_EQ(bids.front().total_qty, 25.0);
    EXPECT_DOUBLE_EQ(asks.front().price, 100.50);
    EXPECT_DOUBLE_EQ(asks.front().total_qty, 20.0);
}

TEST(AnalyticsTest, ComputesPnlAndDrawdownFromEquityCurve) {
    const std::vector<double> equity_curve {100.0, 110.0, 105.0, 120.0};

    const lob::BacktestAnalytics analytics = lob::BacktestAnalytics::analyze(equity_curve);

    EXPECT_DOUBLE_EQ(analytics.total_realized_pnl, 20.0);
    EXPECT_DOUBLE_EQ(analytics.max_drawdown, 5.0);
    EXPECT_DOUBLE_EQ(analytics.max_drawdown_pct, 5.0 / 110.0);
    EXPECT_GT(analytics.sharpe_ratio, 0.0);
}

TEST(CsvParserTest, StreamsMmapEventsWithOneEventLookahead) {
    const std::filesystem::path csv_path = std::filesystem::temp_directory_path() / "lob_streaming_parser_test.csv";
    {
        std::ofstream csv {csv_path};
        csv << "1,100.00,0.01000000,1.00000000,1709251200000,false,true\n"
            << "2,101.25,0.02000000,2.02500000,1709251200100,true,true\n";
    }

    lob::CsvParser parser {csv_path};

    ASSERT_TRUE(parser.has_next());
    EXPECT_EQ(parser.peek_time(), 1'709'251'200'000ULL);

    const lob::Event first_event = parser.pop();
    EXPECT_EQ(first_event.timestamp, 1'709'251'200'000ULL);
    EXPECT_EQ(first_event.order.id, 1U);
    EXPECT_EQ(first_event.order.side, lob::Side::Buy);
    EXPECT_EQ(first_event.order.quantity, 1'000'000U);

    ASSERT_TRUE(parser.has_next());
    EXPECT_EQ(parser.peek_time(), 1'709'251'200'100ULL);

    const lob::Event second_event = parser.pop();
    EXPECT_EQ(second_event.order.id, 2U);
    EXPECT_EQ(second_event.order.side, lob::Side::Sell);
    EXPECT_EQ(second_event.order.quantity, 2'000'000U);
    EXPECT_FALSE(parser.has_next());

    std::filesystem::remove(csv_path);
}

TEST(L2Depth5CsvParserTest, StreamsDepth5RowsAndBackfillsBboRows) {
    const std::filesystem::path depth_path = std::filesystem::temp_directory_path() / "lob_depth5_parser_test.csv";
    {
        std::ofstream csv {depth_path};
        csv << "timestamp,b1_p,b1_q,b2_p,b2_q,b3_p,b3_q,b4_p,b4_q,b5_p,b5_q,"
               "a1_p,a1_q,a2_p,a2_q,a3_p,a3_q,a4_p,a4_q,a5_p,a5_q\n"
            << "1000,99,1,98,2,97,3,96,4,95,5,101,6,102,7,103,8,104,9,105,10\n";
    }

    lob::L2Depth5CsvParser depth_parser {depth_path};
    ASSERT_TRUE(depth_parser.has_next());
    const lob::L2Depth5Event& depth_event = depth_parser.peek();
    EXPECT_EQ(depth_event.timestamp, 1000U);
    EXPECT_DOUBLE_EQ(depth_event.bid_prices[0], 99.0);
    EXPECT_DOUBLE_EQ(depth_event.bid_qty[4], 5.0);
    EXPECT_DOUBLE_EQ(depth_event.ask_prices[0], 101.0);
    EXPECT_DOUBLE_EQ(depth_event.ask_qty[4], 10.0);
    depth_parser.advance();
    EXPECT_FALSE(depth_parser.has_next());
    std::filesystem::remove(depth_path);

    const std::filesystem::path bbo_path = std::filesystem::temp_directory_path() / "lob_depth5_bbo_parser_test.csv";
    {
        std::ofstream csv {bbo_path};
        csv << "timestamp,bid_price,bid_qty,ask_price,ask_qty\n"
            << "2000,100,2,101,3\n";
    }

    lob::L2Depth5CsvParser bbo_parser {bbo_path};
    ASSERT_TRUE(bbo_parser.has_next());
    const lob::L2Depth5Event& bbo_event = bbo_parser.peek();
    EXPECT_EQ(bbo_event.timestamp, 2000U);
    EXPECT_DOUBLE_EQ(bbo_event.bid_prices[0], 100.0);
    EXPECT_DOUBLE_EQ(bbo_event.bid_qty[0], 2.0);
    EXPECT_DOUBLE_EQ(bbo_event.ask_prices[0], 101.0);
    EXPECT_DOUBLE_EQ(bbo_event.ask_qty[0], 3.0);
    EXPECT_DOUBLE_EQ(bbo_event.bid_prices[1], 0.0);
    EXPECT_DOUBLE_EQ(bbo_event.ask_qty[1], 0.0);
    bbo_parser.advance();
    EXPECT_FALSE(bbo_parser.has_next());
    std::filesystem::remove(bbo_path);
}

TEST(L2UpdateCsvParserTest, StreamsRawL2UpdatesAndBooleanFormats) {
    const std::filesystem::path update_path = std::filesystem::temp_directory_path() / "lob_l2_update_parser_test.csv";
    {
        std::ofstream csv {update_path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,true,99.5,2.25\n"
            << "1000,false,0,100.5,0\n";
    }

    lob::L2UpdateCsvParser parser {update_path};
    ASSERT_TRUE(parser.has_next());
    const lob::L2UpdateEvent& first_event = parser.peek();
    EXPECT_EQ(first_event.timestamp_ns, 1000U);
    EXPECT_TRUE(first_event.is_snapshot);
    EXPECT_TRUE(first_event.is_bid);
    EXPECT_EQ(first_event.price, static_cast<std::int64_t>(std::llround(99.5 * 100'000'000.0)));
    EXPECT_EQ(first_event.qty, static_cast<std::uint64_t>(std::llround(2.25 * 100'000'000.0)));

    parser.advance();
    ASSERT_TRUE(parser.has_next());
    const lob::L2UpdateEvent& second_event = parser.peek();
    EXPECT_EQ(second_event.timestamp_ns, 1000U);
    EXPECT_FALSE(second_event.is_snapshot);
    EXPECT_FALSE(second_event.is_bid);
    EXPECT_EQ(second_event.price, static_cast<std::int64_t>(std::llround(100.5 * 100'000'000.0)));
    EXPECT_EQ(second_event.qty, 0U);

    parser.advance();
    EXPECT_FALSE(parser.has_next());
    std::filesystem::remove(update_path);
}

TEST(VenueReplayTest, RejectsSequenceGapsAndReorderingPerProduct) {
    lob::ReplaySequenceValidator validator {};
    lob::FeedEnvelope first {
        .venue_id = 1U,
        .product_id = 10U,
        .sequence = 100U,
        .snapshot_epoch = 1U,
    };
    lob::FeedEnvelope second = first;
    second.sequence = 101U;
    lob::FeedEnvelope gap = first;
    gap.sequence = 103U;
    lob::FeedEnvelope reordered = first;
    reordered.sequence = 100U;

    EXPECT_EQ(validator.validate(first), lob::ReplayValidationStatus::Accepted);
    EXPECT_EQ(validator.validate(second), lob::ReplayValidationStatus::Accepted);
    EXPECT_EQ(validator.validate(gap), lob::ReplayValidationStatus::SequenceGap);
    EXPECT_EQ(validator.validate(reordered), lob::ReplayValidationStatus::SequenceReorder);
}

TEST(VenueReplayTest, AcceptsNewSnapshotEpochForSameProduct) {
    lob::ReplaySequenceValidator validator {};
    lob::FeedEnvelope first {
        .venue_id = 1U,
        .product_id = 10U,
        .sequence = 100U,
        .snapshot_epoch = 1U,
    };
    lob::FeedEnvelope new_epoch = first;
    new_epoch.sequence = 1U;
    new_epoch.snapshot_epoch = 2U;

    EXPECT_EQ(validator.validate(first), lob::ReplayValidationStatus::Accepted);
    EXPECT_EQ(validator.validate(new_epoch), lob::ReplayValidationStatus::Accepted);
}

TEST(VenueManifestTest, DescribesVenueProductScalesAndCosts) {
    lob::VenueManifest manifest {};
    manifest.assets.push_back(lob::AssetManifestEntry {
        .asset_id = 0U,
        .symbol = "USDC",
        .settlement_domain_id = 1U,
    });
    manifest.cost_models.push_back(lob::CostModelManifestEntry {
        .cost_model_id = 1U,
        .kind = lob::CostModelKind::Proportional,
        .proportional_fee_bps = 7.5,
    });
    manifest.products.push_back(lob::ProductManifestEntry {
        .product_id = 42U,
        .venue_id = 2U,
        .kind = lob::ProductKind::PredictionMarketOutcome,
        .symbol = "EVENT-YES",
        .base_asset_id = 1U,
        .quote_asset_id = 0U,
        .price_tick_scale = 100,
        .qty_lot_scale = 1'000'000,
        .cost_model_id = 1U,
        .settlement_domain_id = 1U,
    });

    ASSERT_EQ(manifest.products.size(), 1U);
    EXPECT_EQ(manifest.products[0].product_id, 42U);
    EXPECT_EQ(manifest.products[0].price_tick_scale, 100);
    EXPECT_EQ(manifest.cost_models[0].proportional_fee_bps, 7.5);
}

class CountingL2Strategy final : public lob::Strategy {
public:
    void on_tick(lob::AssetID asset_id, const lob::OrderBook& book, lob::OrderGateway& gateway) override {
        last_asset_id = asset_id;
        last_timestamp = gateway.current_timestamp();
        last_best_bid = book.get_best_bid();
        last_best_ask = book.get_best_ask();
        ++ticks;
    }

    std::uint64_t ticks {};
    lob::AssetID last_asset_id {};
    std::uint64_t last_timestamp {};
    double last_best_bid {};
    double last_best_ask {};
};

class SubmitOnceL2Strategy final : public lob::Strategy {
public:
    SubmitOnceL2Strategy(lob::Side side, double price, std::uint64_t quantity)
        : side_(side), price_(price), quantity_(quantity) {}

    void on_start(lob::OrderGateway& gateway) override {
        if (submit_on_start) {
            order_id = gateway.submit_order(0U, side_, price_, quantity_, gateway.current_timestamp());
            submitted = true;
        }
    }

    void on_tick(lob::AssetID asset_id, const lob::OrderBook& book, lob::OrderGateway& gateway) override {
        (void)book;
        ++ticks;
        if (!submitted) {
            order_id = gateway.submit_order(asset_id, side_, price_, quantity_, gateway.current_timestamp());
            submitted = true;
        }
    }

    void on_fill(const lob::StrategyFill& fill, lob::OrderGateway& gateway) override {
        (void)gateway;
        ++fills;
        last_fill = fill;
    }

    bool submit_on_start {};
    bool submitted {};
    std::uint64_t ticks {};
    std::uint64_t fills {};
    std::uint64_t order_id {};
    lob::StrategyFill last_fill {};

private:
    lob::Side side_ {lob::Side::Buy};
    double price_ {};
    std::uint64_t quantity_ {};
};

class CountingNativeL2Strategy final : public lob::L2Strategy {
public:
    void on_tick(lob::AssetID asset_id, const lob::L2OrderBook& book, lob::OrderGateway& gateway) override {
        last_asset_id = asset_id;
        last_timestamp = gateway.current_timestamp();
        last_best_bid = static_cast<double>(book.best_bid()) / 100'000'000.0;
        last_best_ask = static_cast<double>(book.best_ask()) / 100'000'000.0;
        last_bid_visible_qty = static_cast<double>(book.bid_effective_qty()) / 100'000'000.0;
        last_ask_visible_qty = static_cast<double>(book.ask_effective_qty()) / 100'000'000.0;
        ++ticks;
    }

    std::uint64_t ticks {};
    lob::AssetID last_asset_id {};
    std::uint64_t last_timestamp {};
    double last_best_bid {};
    double last_best_ask {};
    double last_bid_visible_qty {};
    double last_ask_visible_qty {};
};

class SubmitOnceNativeL2Strategy final : public lob::L2Strategy {
public:
    SubmitOnceNativeL2Strategy(lob::Side side, double price, std::uint64_t quantity)
        : side_(side), price_(price), quantity_(quantity) {}

    void on_tick(lob::AssetID asset_id, const lob::L2OrderBook& book, lob::OrderGateway& gateway) override {
        (void)book;
        ++ticks;
        if (!submitted) {
            order_id = gateway.submit_order(asset_id, side_, price_, quantity_, gateway.current_timestamp());
            submitted = true;
        }
    }

    void on_fill(const lob::StrategyFill& fill, lob::OrderGateway& gateway) override {
        (void)gateway;
        ++fills;
        last_fill = fill;
    }

    bool submitted {};
    std::uint64_t ticks {};
    std::uint64_t fills {};
    std::uint64_t order_id {};
    lob::StrategyFill last_fill {};

private:
    lob::Side side_ {lob::Side::Buy};
    double price_ {};
    std::uint64_t quantity_ {};
};

TEST(L2BacktestEngineTest, InitializesFromSnapshotAndInvokesStrategyOnBatch) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_snapshot.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,2\n"
            << "1000,1,0,101,3\n";
    }

    CountingL2Strategy strategy {};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(result.events_processed, 2U);
    EXPECT_EQ(result.market_batches_processed, 1U);
    EXPECT_EQ(result.strategy_ticks, 1U);
    EXPECT_EQ(strategy.ticks, 1U);
    EXPECT_DOUBLE_EQ(strategy.last_best_bid, 100.0);
    EXPECT_DOUBLE_EQ(strategy.last_best_ask, 101.0);
    EXPECT_DOUBLE_EQ(result.final_best_bid, 100.0);
    EXPECT_DOUBLE_EQ(result.final_best_ask, 101.0);

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, AppliesIncrementalUpdateAndDelete) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_update_delete.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,2\n"
            << "1000,1,0,101,3\n"
            << "2000,0,1,100,0\n"
            << "2000,0,1,99,4\n";
    }

    CountingL2Strategy strategy {};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(result.events_processed, 4U);
    EXPECT_EQ(result.market_batches_processed, 2U);
    EXPECT_EQ(strategy.ticks, 2U);
    EXPECT_DOUBLE_EQ(strategy.last_best_bid, 99.0);
    EXPECT_DOUBLE_EQ(strategy.last_best_ask, 101.0);
    EXPECT_DOUBLE_EQ(result.final_best_bid, 99.0);

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, MarketMakerPlacesPassiveQuotesWithoutImmediateFill) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_mm_quotes.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,10\n"
            << "1000,1,0,102,10\n";
    }

    lob::MarketMakerStrategy strategy {lob::MarketMakerStrategy::Config {
        .quote_offset = 0.5,
        .quote_quantity = 2U,
        .refresh_interval_ns = 1'000'000'000ULL,
    }};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(result.orders_submitted, 2U);
    EXPECT_EQ(result.active_orders, 2U);
    EXPECT_EQ(result.execution.maker_fills_count, 0U);
    EXPECT_EQ(result.execution.taker_fills_count, 0U);

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, LatencyDelayedAggressiveOrderFillsAgainstVisibleDepth) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_latency_fill.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,10\n"
            << "1000,1,0,101,10\n";
    }

    SubmitOnceL2Strategy strategy {lob::Side::Buy, 101.0, 3U};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .taker_fee_bps = 0.0,
        .latency_ns = 100U,
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(strategy.fills, 1U);
    EXPECT_EQ(result.execution.taker_fills_count, 1U);
    EXPECT_EQ(result.last_fill_timestamp, 1100U);
    EXPECT_EQ(result.final_position, 3);
    EXPECT_DOUBLE_EQ(result.final_cash, 100'000'000.0 - (101.0 * 3.0));

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, DoesNotFillBeforeBookExists) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_no_prebook_fill.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,10\n"
            << "1000,1,0,101,10\n";
    }

    SubmitOnceL2Strategy strategy {lob::Side::Buy, 101.0, 3U};
    strategy.submit_on_start = true;
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(strategy.fills, 1U);
    EXPECT_EQ(result.last_fill_timestamp, 1000U);
    EXPECT_GT(result.last_fill_timestamp, 0U);

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, NativeStrategyConsumesL2BookWithoutAdapter) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_native_snapshot.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,2\n"
            << "1000,1,1,99,3\n"
            << "1000,1,0,101,4\n"
            << "1000,1,0,102,5\n";
    }

    CountingNativeL2Strategy strategy {};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(result.strategy_ticks, 1U);
    EXPECT_EQ(strategy.ticks, 1U);
    EXPECT_DOUBLE_EQ(strategy.last_best_bid, 100.0);
    EXPECT_DOUBLE_EQ(strategy.last_best_ask, 101.0);
    EXPECT_DOUBLE_EQ(strategy.last_bid_visible_qty, 5.0);
    EXPECT_DOUBLE_EQ(strategy.last_ask_visible_qty, 9.0);

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, NativeMarketMakerPlacesQuotes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_native_mm.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,10\n"
            << "1000,1,0,102,10\n";
    }

    lob::L2MarketMakerStrategy strategy {lob::L2MarketMakerStrategy::Config {
        .quote_offset = 0.5,
        .quote_quantity = 2U,
        .refresh_interval_ns = 1'000'000'000ULL,
    }};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(result.orders_submitted, 2U);
    EXPECT_EQ(result.active_orders, 2U);
    EXPECT_EQ(result.execution.maker_fills_count, 0U);
    EXPECT_EQ(result.execution.taker_fills_count, 0U);

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, NativeLatencyDelayedOrderFills) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_native_latency.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,10\n"
            << "1000,1,0,101,10\n";
    }

    SubmitOnceNativeL2Strategy strategy {lob::Side::Buy, 101.0, 3U};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .latency_ns = 100U,
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    EXPECT_EQ(strategy.fills, 1U);
    EXPECT_EQ(result.execution.taker_fills_count, 1U);
    EXPECT_EQ(result.last_fill_timestamp, 1100U);
    EXPECT_EQ(result.final_position, 3);

    std::filesystem::remove(path);
}

TEST(L2BacktestEngineTest, RecordsFeatureRowsForMlExports) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "l2_backtest_features.csv";
    {
        std::ofstream csv {path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,100,2\n"
            << "1000,1,0,101,3\n"
            << "2000,0,1,100,4\n"
            << "2000,0,0,101,6\n";
    }

    CountingNativeL2Strategy strategy {};
    lob::L2BacktestEngine engine {strategy, lob::L2BacktestEngine::Config {
        .quantity_scale = 1.0,
        .max_book_levels_per_side = 10U,
        .record_features = true,
        .feature_reserve = 2U,
    }};

    const lob::L2BacktestEngine::Result result = engine.run(path);

    ASSERT_EQ(result.features.size(), 2U);
    EXPECT_EQ(result.features[0].timestamp, 1000U);
    EXPECT_DOUBLE_EQ(result.features[0].best_bid, 100.0);
    EXPECT_DOUBLE_EQ(result.features[0].best_ask, 101.0);
    EXPECT_DOUBLE_EQ(result.features[0].bid_qty_1, 2.0);
    EXPECT_DOUBLE_EQ(result.features[0].ask_qty_1, 3.0);
    EXPECT_NEAR(result.features[0].imbalance_1, -0.2, 1e-12);
    EXPECT_EQ(result.features[1].timestamp, 2000U);
    EXPECT_DOUBLE_EQ(result.features[1].bid_qty_1, 4.0);
    EXPECT_DOUBLE_EQ(result.features[1].ask_qty_1, 6.0);

    std::filesystem::remove(path);
}

TEST(EventMergerTest, MergesSmallStreamSetWithFastPath) {
    using Parser = FakeStreamParser<3U>;
    lob::EventMerger<3U, Parser> merger {std::array<Parser, 3U> {
        Parser {{1U, 4U, 7U}, 3U},
        Parser {{2U, 5U, 8U}, 3U},
        Parser {{3U, 6U, 9U}, 3U},
    }};

    for (std::uint64_t expected_timestamp = 1U; expected_timestamp <= 9U; ++expected_timestamp) {
        ASSERT_TRUE(merger.has_next());
        const lob::Event event = merger.get_next();
        EXPECT_EQ(event.timestamp, expected_timestamp);
        EXPECT_EQ(event.asset_id, static_cast<lob::AssetID>((expected_timestamp - 1U) % 3U));
    }

    EXPECT_FALSE(merger.has_next());
}

TEST(EventMergerTest, MergesLargeStreamSetWithHeapPath) {
    using Parser = FakeStreamParser<2U>;
    lob::EventMerger<5U, Parser> merger {std::array<Parser, 5U> {
        Parser {{5U, 10U}, 2U},
        Parser {{1U, 6U}, 2U},
        Parser {{3U, 8U}, 2U},
        Parser {{2U, 7U}, 2U},
        Parser {{4U, 9U}, 2U},
    }};

    for (std::uint64_t expected_timestamp = 1U; expected_timestamp <= 10U; ++expected_timestamp) {
        ASSERT_TRUE(merger.has_next());
        const lob::Event event = merger.get_next();
        EXPECT_EQ(event.timestamp, expected_timestamp);
    }

    EXPECT_FALSE(merger.has_next());
}

TEST(MultiAssetBacktestEngineTest, RoutesMergedEventsToAssetBooksAndStrategy) {
    using Parser = FakeStreamParser<2U>;
    CountingStrategy strategy {};
    lob::MultiAssetBacktestEngine<2U, Parser> engine {strategy, std::array<Parser, 2U> {
        Parser {{1U, 3U}, 2U},
        Parser {{2U, 4U}, 2U},
    }};

    const auto result = engine.run();

    EXPECT_EQ(result.events_processed, 4U);
    EXPECT_EQ(strategy.ticks_by_asset_[0], 2U);
    EXPECT_EQ(strategy.ticks_by_asset_[1], 2U);
}

TEST(GraphArbitrageEngineTest, DetectsAndExecutesNegativeCycleThroughL2OrderBook) {
    const std::filesystem::path btc_usdt_path = std::filesystem::temp_directory_path() / "btc_usdt_graph_test.csv";
    const std::filesystem::path eth_usdt_path = std::filesystem::temp_directory_path() / "eth_usdt_graph_test.csv";
    const std::filesystem::path eth_btc_path = std::filesystem::temp_directory_path() / "eth_btc_graph_test.csv";

    {
        std::ofstream csv {btc_usdt_path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,49900,10\n"
            << "1000,1,0,50000,10\n"
            << "2000,1,1,49900,10\n"
            << "2000,1,0,50000,10\n";
    }
    {
        std::ofstream csv {eth_usdt_path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,2900,100\n"
            << "1000,1,0,3000,100\n"
            << "2000,1,1,2900,100\n"
            << "2000,1,0,3000,100\n";
    }
    {
        std::ofstream csv {eth_btc_path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,0.049,100\n"
            << "1000,1,0,0.050,100\n"
            << "2000,1,1,0.049,100\n"
            << "2000,1,0,0.050,100\n";
    }

    lob::GraphArbitrageEngine::Config config {
        .initial_usdt = 100'000.0,
        .quote_asset = "USDT",
        .latency_ns = 0U,
        .intra_leg_latency_ns = 0U,
        .taker_fee_bps = 0.0,
        .max_cycle_notional_usdt = 10'000.0,
        .max_adverse_obi = 1.0,
        .max_spread_bps = 10'000.0,
        .min_depth_usdt = 0.0,
        .min_cycle_edge_bps = 0.0,
        .cycle_snapshot_reserve = 100U,
    };

    lob::GraphArbitrageEngine engine {config};
    engine.add_pair("BTC", "USDT", btc_usdt_path.string());
    engine.add_pair("ETH", "USDT", eth_usdt_path.string());
    engine.add_pair("ETH", "BTC", eth_btc_path.string());

    const lob::GraphArbitrageEngine::Result result = engine.run();

    EXPECT_EQ(result.events_processed, 12U);
    EXPECT_EQ(result.market_batches_processed, 2U);
    EXPECT_EQ(result.cycle_searches, 2U);
    EXPECT_GT(result.completed_cycles, 0U);
    EXPECT_GT(result.final_nav, 100'000.0);

    std::filesystem::remove(btc_usdt_path);
    std::filesystem::remove(eth_usdt_path);
    std::filesystem::remove(eth_btc_path);
}

TEST(GraphArbitrageEngineTest, MaxBookLevelsPerSideCapsDepthUsedByGraph) {
    const auto run_with_cap = [](std::size_t max_levels_per_side) {
        const std::filesystem::path ausdt_path = std::filesystem::temp_directory_path() /
            ("ausdt_depth_cap_test_" + std::to_string(max_levels_per_side) + ".csv");
        const std::filesystem::path ba_path = std::filesystem::temp_directory_path() /
            ("ba_depth_cap_test_" + std::to_string(max_levels_per_side) + ".csv");
        const std::filesystem::path busdt_path = std::filesystem::temp_directory_path() /
            ("busdt_depth_cap_test_" + std::to_string(max_levels_per_side) + ".csv");

        {
            std::ofstream csv {ausdt_path};
            csv << "timestamp,is_snapshot,is_bid,price,qty\n"
                << "1000,1,1,9,1\n"
                << "1000,1,1,8.9,100\n"
                << "1000,1,0,10,1\n"
                << "1000,1,0,10.1,100\n";
        }
        {
            std::ofstream csv {ba_path};
            csv << "timestamp,is_snapshot,is_bid,price,qty\n"
                << "1000,1,1,1.9,1\n"
                << "1000,1,1,1.8,100\n"
                << "1000,1,0,2,1\n"
                << "1000,1,0,2.01,100\n";
        }
        {
            std::ofstream csv {busdt_path};
            csv << "timestamp,is_snapshot,is_bid,price,qty\n"
                << "1000,1,1,25,1\n"
                << "1000,1,1,24.9,100\n"
                << "1000,1,0,26,1\n"
                << "1000,1,0,26.1,100\n";
        }

        lob::GraphArbitrageEngine::Config config {
            .initial_usdt = 100'000.0,
            .quote_asset = "USDT",
            .latency_ns = 0U,
            .intra_leg_latency_ns = 0U,
            .taker_fee_bps = 0.0,
            .max_cycle_notional_usdt = 1'000.0,
            .max_adverse_obi = 1.0,
            .max_spread_bps = 100'000.0,
            .min_depth_usdt = 500.0,
            .min_cycle_edge_bps = 0.0,
            .cycle_snapshot_reserve = 100U,
            .max_book_levels_per_side = max_levels_per_side,
        };

        lob::GraphArbitrageEngine engine {config};
        engine.add_pair("A", "USDT", ausdt_path.string());
        engine.add_pair("B", "A", ba_path.string());
        engine.add_pair("B", "USDT", busdt_path.string());

        const lob::GraphArbitrageEngine::Result result = engine.run();

        std::filesystem::remove(ausdt_path);
        std::filesystem::remove(ba_path);
        std::filesystem::remove(busdt_path);
        return result;
    };

    const lob::GraphArbitrageEngine::Result capped_top_level = run_with_cap(1U);
    const lob::GraphArbitrageEngine::Result capped_two_levels = run_with_cap(2U);

    EXPECT_EQ(capped_top_level.completed_cycles, 0U);
    EXPECT_GT(capped_two_levels.completed_cycles, 0U);
}

TEST(GraphArbitrageEngineTest, SparseLookupEngineMatchesDenseEngineOnL2Scenario) {
    const auto run_engine = []<typename Engine>(const std::string& suffix) {
        const std::filesystem::path btc_usdt_path = std::filesystem::temp_directory_path() /
            ("btc_usdt_sparse_match_" + suffix + ".csv");
        const std::filesystem::path eth_usdt_path = std::filesystem::temp_directory_path() /
            ("eth_usdt_sparse_match_" + suffix + ".csv");
        const std::filesystem::path eth_btc_path = std::filesystem::temp_directory_path() /
            ("eth_btc_sparse_match_" + suffix + ".csv");

        {
            std::ofstream csv {btc_usdt_path};
            csv << "timestamp,is_snapshot,is_bid,price,qty\n"
                << "1000,1,1,49900,10\n"
                << "1000,1,0,50000,10\n"
                << "2000,0,1,49910,10\n"
                << "2000,0,0,50010,10\n";
        }
        {
            std::ofstream csv {eth_usdt_path};
            csv << "timestamp,is_snapshot,is_bid,price,qty\n"
                << "1000,1,1,2900,100\n"
                << "1000,1,0,3000,100\n"
                << "2000,0,1,2910,100\n"
                << "2000,0,0,3010,100\n";
        }
        {
            std::ofstream csv {eth_btc_path};
            csv << "timestamp,is_snapshot,is_bid,price,qty\n"
                << "1000,1,1,0.049,100\n"
                << "1000,1,0,0.050,100\n"
                << "2000,0,1,0.049,100\n"
                << "2000,0,0,0.050,100\n";
        }

        typename Engine::Config config {
            .initial_usdt = 100'000.0,
            .quote_asset = "USDT",
            .latency_ns = 0U,
            .intra_leg_latency_ns = 0U,
            .taker_fee_bps = 0.0,
            .max_cycle_notional_usdt = 10'000.0,
            .max_adverse_obi = 1.0,
            .max_spread_bps = 10'000.0,
            .min_depth_usdt = 0.0,
            .min_cycle_edge_bps = 0.0,
            .cycle_snapshot_reserve = 100U,
            .max_book_levels_per_side = 20U,
        };

        Engine engine {config};
        engine.add_pair("BTC", "USDT", btc_usdt_path.string());
        engine.add_pair("ETH", "USDT", eth_usdt_path.string());
        engine.add_pair("ETH", "BTC", eth_btc_path.string());

        const auto result = engine.run();

        std::filesystem::remove(btc_usdt_path);
        std::filesystem::remove(eth_usdt_path);
        std::filesystem::remove(eth_btc_path);
        return result;
    };

    const auto dense = run_engine.template operator()<lob::GraphArbitrageEngine>("dense");
    const auto sparse = run_engine.template operator()<lob::GraphArbitrageEngineLarge>("sparse");

    EXPECT_EQ(sparse.events_processed, dense.events_processed);
    EXPECT_EQ(sparse.cycles_detected, dense.cycles_detected);
    EXPECT_EQ(sparse.completed_cycles, dense.completed_cycles);
    EXPECT_EQ(sparse.panic_closes, dense.panic_closes);
    EXPECT_NEAR(sparse.final_nav, dense.final_nav, 1e-9);
}

TEST(GraphArbitrageEngineTest, DetectsCompletedCycleRegardlessOfL2UpdateOrder) {
    struct PairFile {
        const char* key;
        const char* base;
        const char* quote;
        double bid;
        double ask;
        double qty;
    };

    const std::array<PairFile, 3U> pairs {{
        {"AUSDT", "A", "USDT", 9.0, 10.0, 100.0},
        {"BA", "B", "A", 1.9, 2.0, 100.0},
        {"BUSDT", "B", "USDT", 25.0, 26.0, 100.0},
    }};
    const std::array<std::array<const char*, 3U>, 3U> update_orders {{
        {{"AUSDT", "BA", "BUSDT"}},
        {{"BUSDT", "BA", "AUSDT"}},
        {{"BA", "BUSDT", "AUSDT"}},
    }};

    for (std::size_t order_index = 0U; order_index < update_orders.size(); ++order_index) {
        std::array<std::filesystem::path, 3U> paths {};
        for (std::size_t pair_index = 0U; pair_index < pairs.size(); ++pair_index) {
            paths[pair_index] = std::filesystem::temp_directory_path() /
                ("graph_order_repro_" + std::to_string(order_index) + "_" + pairs[pair_index].key + ".csv");

            std::uint64_t timestamp = 0U;
            for (std::size_t update_index = 0U; update_index < update_orders[order_index].size(); ++update_index) {
                if (std::string {update_orders[order_index][update_index]} == pairs[pair_index].key) {
                    timestamp = 1000U + static_cast<std::uint64_t>(update_index) * 1000U;
                    break;
                }
            }

            std::ofstream csv {paths[pair_index]};
            csv << "timestamp,is_snapshot,is_bid,price,qty\n"
                << timestamp << ",1,1," << pairs[pair_index].bid << "," << pairs[pair_index].qty << "\n"
                << timestamp << ",1,0," << pairs[pair_index].ask << "," << pairs[pair_index].qty << "\n";
        }

        lob::GraphArbitrageEngine::Config config {
            .initial_usdt = 100'000.0,
            .quote_asset = "USDT",
            .latency_ns = 0U,
            .intra_leg_latency_ns = 0U,
            .taker_fee_bps = 0.0,
            .max_cycle_notional_usdt = 1'000.0,
            .max_adverse_obi = 1.0,
            .max_spread_bps = 100'000.0,
            .min_depth_usdt = 0.0,
            .min_cycle_edge_bps = 0.0,
            .cycle_snapshot_reserve = 100U,
        };

        lob::GraphArbitrageEngine engine {config};
        for (std::size_t pair_index = 0U; pair_index < pairs.size(); ++pair_index) {
            engine.add_pair(pairs[pair_index].base, pairs[pair_index].quote, paths[pair_index].string());
        }

        const lob::GraphArbitrageEngine::Result result = engine.run();

        EXPECT_GT(result.completed_cycles, 0U) << "order_index=" << order_index;
        EXPECT_GT(result.final_nav, config.initial_usdt) << "order_index=" << order_index;

        for (const auto& path : paths) {
            std::filesystem::remove(path);
        }
    }
}

TEST(GraphArbitrageEngineTest, RevalidatesVisibleDepthAtPendingLegExecution) {
    const std::filesystem::path ausdt_path = std::filesystem::temp_directory_path() / "ausdt_stale_depth_test.csv";
    const std::filesystem::path ba_path = std::filesystem::temp_directory_path() / "ba_stale_depth_test.csv";
    const std::filesystem::path busdt_path = std::filesystem::temp_directory_path() / "busdt_stale_depth_test.csv";

    {
        std::ofstream csv {ausdt_path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,9,100\n"
            << "1000,1,0,10,100\n";
    }
    {
        std::ofstream csv {ba_path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,1.9,100\n"
            << "1000,1,0,2,100\n";
    }
    {
        std::ofstream csv {busdt_path};
        csv << "timestamp,is_snapshot,is_bid,price,qty\n"
            << "1000,1,1,25,100\n"
            << "1000,1,0,26,100\n"
            << "1050,0,1,25,10\n";
    }

    lob::GraphArbitrageEngine::Config config {
        .initial_usdt = 100'000.0,
        .quote_asset = "USDT",
        .latency_ns = 100'000'000U,
        .intra_leg_latency_ns = 0U,
        .taker_fee_bps = 0.0,
        .max_cycle_notional_usdt = 1'000.0,
        .max_adverse_obi = 1.0,
        .max_spread_bps = 100'000.0,
        .min_depth_usdt = 500.0,
        .min_cycle_edge_bps = 0.0,
        .cycle_snapshot_reserve = 100U,
    };

    lob::GraphArbitrageEngine engine {config};
    engine.add_pair("A", "USDT", ausdt_path.string());
    engine.add_pair("B", "A", ba_path.string());
    engine.add_pair("B", "USDT", busdt_path.string());

    const lob::GraphArbitrageEngine::Result result = engine.run();

    EXPECT_EQ(result.completed_cycles, 0U);
    EXPECT_GT(result.panic_closes, 0U);

    std::filesystem::remove(ausdt_path);
    std::filesystem::remove(ba_path);
    std::filesystem::remove(busdt_path);
}

}  // namespace
