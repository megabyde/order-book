/**
 * Tests for order book
 */
#include "../order_book.h"

#include <gmock/gmock.h>

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

    book.fill("4");
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1110, 50), PQ(1105, 100)));

    book.execute("3", 50);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(), PQ(1105, 50)));

    book.sell("6", 1120, 300);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1120, 300), PQ(1105, 50)));

    book.execute("6", 150);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1120, 150), PQ(1100, 100)));

    book.execute("6", 100);
    EXPECT_EQ(book.best_ask_bid(), std::make_pair(PQ(1120, 50), PQ()));
}
