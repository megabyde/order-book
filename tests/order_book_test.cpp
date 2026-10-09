#include <gtest/gtest.h>

#include <order_book/feed.hpp>
#include <order_book/order_book.hpp>

#include <optional>
#include <ostream>
#include <string>
#include <string_view>

using order_book::AddOrder;
using order_book::Bbo;
using order_book::BookError;
using order_book::DecreaseOrder;
using order_book::DeleteOrder;
using order_book::Event;
using order_book::ExecuteOrder;
using order_book::FillOrder;
using order_book::Level;
using order_book::NoOp;
using order_book::OrderBook;
using order_book::Price;
using order_book::Quantity;
using order_book::Side;

namespace order_book {

// Google Test prints these in failure messages. It finds them by argument-dependent lookup, which
// skips unnamed namespaces, so they need external linkage in the type's namespace.
// NOLINTBEGIN(misc-use-internal-linkage)
void PrintTo(const Level& level, std::ostream* os)
{
    *os << level.price << 'x' << level.quantity;
}

void PrintTo(const Bbo& bbo, std::ostream* os)
{
    *os << "bid ";
    if (bbo.bid) {
        PrintTo(*bbo.bid, os);
    }
    *os << ", ask ";
    if (bbo.ask) {
        PrintTo(*bbo.ask, os);
    }
}
// NOLINTEND(misc-use-internal-linkage)

} // namespace order_book

namespace {

// NOLINTBEGIN(readability-magic-numbers)

// A fresh book per test, with helpers that fail the test if the book rejects a message. The gtest
// base is a parameter so that a parametrized suite gets the same helpers through single
// inheritance.
template <typename Base = testing::Test> class BookTest : public Base {
protected:
    void apply(std::string_view order, const Event& event)
    {
        const auto applied = book.apply(order, event);
        ASSERT_TRUE(applied) << "rejected order " << order;
    }

    void buy(std::string_view order, Price price, Quantity shares)
    {
        apply(order, AddOrder{.side = Side::Buy, .price = price, .shares = shares});
    }

    void sell(std::string_view order, Price price, Quantity shares)
    {
        apply(order, AddOrder{.side = Side::Sell, .price = price, .shares = shares});
    }

    // TEST_F bodies are subclasses of the fixture and reach its state through protected members
    OrderBook book; // NOLINT(misc-non-private-member-variables-in-classes)
};

using OrderBookTest = BookTest<>;

Bbo bbo(std::optional<Level> bid, std::optional<Level> ask)
{
    return Bbo{.bid = bid, .ask = ask};
}

TEST_F(OrderBookTest, CountsOrdersAndLevels)
{
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.level_count(), 0U);

    buy("1", 1110, 150);
    buy("2", 1110, 100);
    EXPECT_EQ(book.order_count(), 2U);
    EXPECT_EQ(book.level_count(), 1U);

    sell("3", 1120, 150);
    EXPECT_EQ(book.order_count(), 3U);
    EXPECT_EQ(book.level_count(), 2U);
}

TEST_F(OrderBookTest, BestBidIsHighestLevelTotal)
{
    EXPECT_EQ(book.bbo(), bbo({}, {}));

    buy("1", 1110, 150);
    EXPECT_EQ(book.bbo(), bbo(Level{1110, 150}, {}));
    buy("2", 1110, 50);
    EXPECT_EQ(book.bbo(), bbo(Level{1110, 200}, {}));
    buy("3", 1120, 100);
    EXPECT_EQ(book.bbo(), bbo(Level{1120, 100}, {}));
    buy("4", 1100, 100);
    EXPECT_EQ(book.bbo(), bbo(Level{1120, 100}, {}));

    apply("2", DeleteOrder{});
    apply("1", DeleteOrder{});
    apply("3", DeleteOrder{});
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 100}, {}));
    apply("4", DeleteOrder{});
    EXPECT_EQ(book.bbo(), bbo({}, {}));
    EXPECT_EQ(book.level_count(), 0U);
}

TEST_F(OrderBookTest, BestAskIsLowestLevelTotal)
{
    sell("1", 1110, 150);
    EXPECT_EQ(book.bbo(), bbo({}, Level{1110, 150}));
    sell("3", 1120, 100);
    EXPECT_EQ(book.bbo(), bbo({}, Level{1110, 150}));
    sell("4", 1100, 100);
    EXPECT_EQ(book.bbo(), bbo({}, Level{1100, 100}));
}

TEST_F(OrderBookTest, ExecutionsReduceOnlyTheNamedOrder)
{
    sell("1", 1110, 150);
    sell("2", 1108, 100);
    buy("3", 1105, 100);
    buy("4", 1105, 200);
    buy("5", 1100, 200);
    EXPECT_EQ(book.bbo(), bbo(Level{1105, 300}, Level{1108, 100}));

    apply("2", ExecuteOrder{40});
    EXPECT_EQ(book.bbo(), bbo(Level{1105, 300}, Level{1108, 60}));
    apply("4", ExecuteOrder{50});
    EXPECT_EQ(book.bbo(), bbo(Level{1105, 250}, Level{1108, 60}));

    apply("3", FillOrder{});
    EXPECT_EQ(book.bbo(), bbo(Level{1105, 150}, Level{1108, 60}));

    // Executing the remainder removes the order and its now empty level
    apply("2", ExecuteOrder{60});
    EXPECT_EQ(book.bbo(), bbo(Level{1105, 150}, Level{1110, 150}));

    // An execution beyond the remaining quantity is clamped to it
    apply("4", ExecuteOrder{1000});
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 200}, Level{1110, 150}));

    apply("5", FillOrder{});
    EXPECT_EQ(book.bbo(), bbo({}, Level{1110, 150}));
    apply("1", FillOrder{});
    EXPECT_EQ(book.bbo(), bbo({}, {}));
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.level_count(), 0U);
}

TEST_F(OrderBookTest, DecreaseSetsTheNewQuantity)
{
    buy("1", 1100, 100);
    buy("2", 1100, 50);

    // Shares is the new quantity, not the amount to subtract
    apply("1", DecreaseOrder{40});
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 90}, {}));

    // A decrease to the current quantity or above changes nothing
    apply("1", DecreaseOrder{40});
    apply("1", DecreaseOrder{70});
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 90}, {}));
    EXPECT_EQ(book.order_count(), 2U);

    // A decrease to 0 removes the order
    apply("1", DecreaseOrder{0});
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 50}, {}));
    EXPECT_EQ(book.order_count(), 1U);
}

TEST_F(OrderBookTest, LevelTotalExceeds32Bits)
{
    sell("1", 1100, 4'000'000'000);
    sell("2", 1100, 4'000'000'000);
    EXPECT_EQ(book.bbo(), bbo({}, Level{1100, 8'000'000'000}));
}

TEST_F(OrderBookTest, TradesNeedNoLiveOrder)
{
    buy("1", 1100, 100);
    apply("0", NoOp{});
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 100}, {}));
}

struct UnknownOrderCase {
    std::string_view name;
    Event event;
};

class UnknownOrderTest : public BookTest<testing::TestWithParam<UnknownOrderCase>> {};

TEST_P(UnknownOrderTest, RejectsAndLeavesBookUnchanged)
{
    buy("1", 1100, 100);
    const auto applied = book.apply("2", GetParam().event);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error(), BookError::UnknownOrder);
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 100}, {}));
    EXPECT_EQ(book.order_count(), 1U);
}

INSTANTIATE_TEST_SUITE_P(Events, UnknownOrderTest,
                         testing::Values(UnknownOrderCase{"Decrease", DecreaseOrder{10}},
                                         UnknownOrderCase{"Delete", DeleteOrder{}},
                                         UnknownOrderCase{"Execute", ExecuteOrder{10}},
                                         UnknownOrderCase{"Fill", FillOrder{}}),
                         [](const testing::TestParamInfo<UnknownOrderCase>& info) {
                             return std::string(info.param.name);
                         });

TEST_F(OrderBookTest, RejectsDuplicateAddAndLeavesBookUnchanged)
{
    buy("1", 1100, 100);

    const auto applied = book.apply("1", AddOrder{.side = Side::Sell, .price = 1200, .shares = 10});
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error(), BookError::DuplicateOrder);
    EXPECT_EQ(book.bbo(), bbo(Level{1100, 100}, {}));
    EXPECT_EQ(book.order_count(), 1U);

    // The ID is free again once the order is gone
    apply("1", DeleteOrder{});
    sell("1", 1200, 10);
    EXPECT_EQ(book.bbo(), bbo({}, Level{1200, 10}));
}

// NOLINTEND(readability-magic-numbers)

} // namespace
