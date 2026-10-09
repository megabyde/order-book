/**
 * Tests for order book
 */
#include <gtest/gtest.h>

#include <order_book/order.hpp>
#include <order_book/order_book.hpp>

#include <stdexcept>
#include <utility>

using order_book::OrderBook;
using order_book::PQ;

// NOLINTBEGIN(readability-magic-numbers)

TEST(OrderBookTest, TestStatistics)
{
    OrderBook book;

    EXPECT_EQ(book.num_ask_orders(), 0);
    EXPECT_EQ(book.num_bid_orders(), 0);
    EXPECT_EQ(book.num_orders(), 0);

    EXPECT_EQ(book.num_ask_price_levels(), 0);
    EXPECT_EQ(book.num_bid_price_levels(), 0);
    EXPECT_EQ(book.num_price_levels(), 0);

    // New buy order
    book.buy("1", 1110, 150);

    EXPECT_EQ(book.num_ask_orders(), 0);
    EXPECT_EQ(book.num_bid_orders(), 1);
    EXPECT_EQ(book.num_orders(), 1);

    EXPECT_EQ(book.num_ask_price_levels(), 0);
    EXPECT_EQ(book.num_bid_price_levels(), 1);
    EXPECT_EQ(book.num_price_levels(), 1);

    // New buy order at the same price
    book.buy("2", 1110, 100);

    EXPECT_EQ(book.num_ask_orders(), 0);
    EXPECT_EQ(book.num_bid_orders(), 2);
    EXPECT_EQ(book.num_orders(), 2);

    EXPECT_EQ(book.num_ask_price_levels(), 0);
    EXPECT_EQ(book.num_bid_price_levels(), 1);
    EXPECT_EQ(book.num_price_levels(), 1);

    // New sell order
    book.sell("3", 1120, 150);

    EXPECT_EQ(book.num_ask_orders(), 1);
    EXPECT_EQ(book.num_bid_orders(), 2);
    EXPECT_EQ(book.num_orders(), 3);

    EXPECT_EQ(book.num_ask_price_levels(), 1);
    EXPECT_EQ(book.num_bid_price_levels(), 1);
    EXPECT_EQ(book.num_price_levels(), 2);
}

TEST(OrderBookTest, TestBestBid)
{
    OrderBook book;
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ()));

    book.buy("1", 1110, 150);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1110, 150)));

    // New buy order at the same price
    book.buy("2", 1110, 50);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1110, 200)));

    // New buy order at a higher price
    book.buy("3", 1120, 100);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1120, 100)));

    // New buy order at a lower price
    book.buy("4", 1100, 100);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1120, 100)));

    // Remove/decrease orders
    book.remove("2");
    book.remove("1");
    book.remove("3");
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1100, 100)));
    book.decrease("4", 50);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1100, 50)));
    book.remove("4");
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ()));
}

TEST(OrderBookTest, TestBestAsk)
{
    OrderBook book;
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ()));

    book.sell("1", 1110, 150);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1110, 150), PQ()));

    // New sell order at a higher price
    book.sell("3", 1120, 100);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1110, 150), PQ()));

    // New sell order at a lower price
    book.sell("4", 1100, 100);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1100, 100), PQ()));
}

TEST(OrderBookTest, TestTrading)
{
    OrderBook book;
    book.sell("1", 1110, 150);
    book.sell("2", 1108, 100);
    book.buy("3", 1105, 100);
    book.buy("4", 1105, 200);
    book.buy("5", 1100, 200);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1108, 100), PQ(1105, 300)));

    // An execution reduces the named resting order and nothing on the other side
    book.execute("2", 40);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1108, 60), PQ(1105, 300)));
    book.execute("4", 50);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1108, 60), PQ(1105, 250)));

    book.fill("3");
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1108, 60), PQ(1105, 150)));

    // Executing the remainder removes the order and its now empty level
    book.execute("2", 60);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1110, 150), PQ(1105, 150)));

    // An execution beyond the remaining quantity is clamped to it
    book.execute("4", 1000);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1110, 150), PQ(1100, 200)));

    // Emptying the last bid level
    book.fill("5");
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1110, 150), PQ()));
    book.fill("1");
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ()));
    EXPECT_EQ(book.num_orders(), 0U);
    EXPECT_EQ(book.num_price_levels(), 0U);
}

TEST(OrderBookTest, TestDecreaseTo)
{
    OrderBook book;
    book.buy("1", 1100, 100);
    book.buy("2", 1100, 50);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1100, 150)));

    // Shares is the new quantity, not the amount to subtract
    book.decrease("1", 40);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1100, 90)));

    // A decrease to the current quantity or above changes nothing
    book.decrease("1", 40);
    book.decrease("1", 70);
    EXPECT_FALSE(book.changed());
    EXPECT_EQ(book.num_bid_orders(), 2U);

    // A decrease to 0 removes the order
    book.decrease("1", 0);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1100, 50)));
    EXPECT_EQ(book.num_bid_orders(), 1U);
}

TEST(OrderBookTest, TestLevelQuantityAbove32Bits)
{
    OrderBook book;
    book.sell("1", 1100, 4'000'000'000);
    book.sell("2", 1100, 4'000'000'000);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1100, 8'000'000'000), PQ()));
}

TEST(OrderBookTest, TestUnknownOrder)
{
    OrderBook book;
    book.buy("1", 1100, 100);
    static_cast<void>(book.best_ask_bid());

    EXPECT_THROW(book.decrease("2", 10), std::invalid_argument);
    EXPECT_THROW(book.remove("2"), std::invalid_argument);
    EXPECT_THROW(book.execute("2", 10), std::invalid_argument);
    EXPECT_THROW(book.fill("2"), std::invalid_argument);
    try {
        book.remove("2");
        FAIL() << "expected std::invalid_argument";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "unknown order '2'");
    }
    EXPECT_FALSE(book.changed());
    EXPECT_EQ(book.num_orders(), 1U);
}

TEST(OrderBookTest, TestDuplicateAdd)
{
    OrderBook book;
    book.buy("1", 1100, 100);
    static_cast<void>(book.best_ask_bid());

    try {
        book.sell("1", 1200, 10);
        FAIL() << "expected std::invalid_argument";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "duplicate order '1'");
    }
    EXPECT_FALSE(book.changed());
    EXPECT_EQ(book.num_orders(), 1U);

    // The ID is free again once the order is gone
    book.remove("1");
    book.sell("1", 1200, 10);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1200, 10), PQ()));
}

// NOLINTEND(readability-magic-numbers)
