#include <gtest/gtest.h>

#include <order_book/feed.hpp>
#include <order_book/levels.hpp>
#include <order_book/order_book.hpp>

#include "market_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
using order_book::Shares;
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
    // Apply an event the book must accept, returning whether the BBO changed
    bool apply(std::string_view order, const Event& event)
    {
        const auto applied = book.apply(order, event);
        EXPECT_TRUE(applied.has_value()) << "rejected order " << order;
        return applied.value_or(false);
    }

    void buy(std::string_view order, Price price, Shares shares)
    {
        apply(order, AddOrder{.side = Side::Buy, .price = price, .shares = shares});
    }

    void sell(std::string_view order, Price price, Shares shares)
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

static_assert(std::is_move_constructible_v<OrderBook> && std::is_move_assignable_v<OrderBook>);

TEST_F(OrderBookTest, KeepsWorkingAfterAMove)
{
    buy("1", 1110, 150);
    sell("2", 1120, 100);
    auto moved = std::move(book);
    EXPECT_EQ(moved.apply("1", ExecuteOrder{.shares = 50}), true);
    EXPECT_EQ(moved.apply("2", DeleteOrder{}), true);
    EXPECT_EQ(moved.bbo(), bbo(Level{.price = 1110, .quantity = 100}, {}));

    OrderBook assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.apply("1", DeleteOrder{}), true);
    EXPECT_EQ(assigned.bbo(), bbo({}, {}));
    EXPECT_EQ(assigned.order_count(), 0U);
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

// Applies seeded events of every kind to a book and to MarketModel, the reference the ITCH fuzzer
// uses: adds of 0 to 100 shares, executions and decreases of any size up to past the order's
// remaining shares, deletes, fills, and no-ops, and adds of a live ID and events naming no live
// order, which must fail and leave the book as it was
class Churn {
public:
    explicit Churn(OrderBook& book) : m_book(book) {}

    // Apply one event, an add with probability two in three when `adding`, and report whether the
    // book still agrees with the model
    testing::AssertionResult step(bool adding)
    {
        const auto before = m_model.bbo(0);
        const auto orders = m_book.order_count();
        const auto levels = m_book.level_count();
        const auto kind = static_cast<unsigned>(m_random() % 8);
        if (m_live.empty() || (adding && kind != 0)) {
            return agrees(add(), before);
        }
        if (adding) {
            const auto applied = unknown();
            return rejected(applied, BookError::UnknownOrder, before, orders, levels);
        }
        const auto index = m_random() % m_live.size();
        if (kind == 0) {
            const auto applied =
                m_book.apply(std::to_string(m_live[index]),
                             AddOrder{.side = Side::Buy, .price = 1000, .shares = 1});
            return rejected(applied, BookError::DuplicateOrder, before, orders, levels);
        }
        return agrees(change(index, kind), before);
    }

    // Delete every live order, reporting whether the book agrees with the model after each delete
    testing::AssertionResult drain()
    {
        while (!m_live.empty()) {
            const auto before = m_model.bbo(0);
            const auto id = m_live.back();
            const auto applied = m_book.apply(std::to_string(id), DeleteOrder{});
            m_model.take(0, id, m_model.orders().at({0, id}).remaining);
            m_live.pop_back();
            if (const auto result = agrees(applied, before); !result) {
                return result;
            }
        }
        return testing::AssertionSuccess();
    }

    [[nodiscard]] std::size_t live() const { return m_live.size(); }

private:
    std::expected<bool, BookError> add()
    {
        const AddOrder add{
            .side = m_random() % 2 == 0 ? Side::Buy : Side::Sell,
            .price = static_cast<Price>(1000 + (m_random() % 16 * 10)),
            .shares = static_cast<Shares>(m_random() % 20 == 0 ? 0 : 1 + (m_random() % 100)),
        };
        const auto id = m_next_id++;
        m_model.add(0, id, add.side, add.price, add.shares);
        m_live.push_back(id);
        return m_book.apply(std::to_string(id), add);
    }

    // An execution or delete of an ID never used
    std::expected<bool, BookError> unknown()
    {
        const auto id = std::to_string(m_next_id + 1'000'000);
        if (m_random() % 2 == 0) {
            return m_book.apply(id, ExecuteOrder{1});
        }
        return m_book.apply(id, DeleteOrder{});
    }

    // Apply an event of `kind`, 1 to 7, to live order `index`, which the check sees as swappable
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::expected<bool, BookError> change(std::size_t index, unsigned kind)
    {
        const auto id = m_live[index];
        const auto remaining = static_cast<Shares>(m_model.orders().at({0, id}).remaining);
        const auto shares = static_cast<Shares>(m_random() % (remaining + 21));
        std::expected<bool, BookError> applied;
        if (kind <= 2) {
            applied = m_book.apply(std::to_string(id), ExecuteOrder{shares});
            m_model.take(0, id, std::min(shares, remaining));
        }
        else if (kind <= 4) {
            applied = m_book.apply(std::to_string(id), DecreaseOrder{shares});
            if (shares < remaining) {
                m_model.take(0, id, remaining - shares);
            }
        }
        else if (kind == 5 || kind == 6) {
            applied = m_book.apply(std::to_string(id),
                                   kind == 5 ? Event{DeleteOrder{}} : Event{FillOrder{}});
            m_model.take(0, id, remaining);
        }
        else {
            applied = m_book.apply(std::to_string(id), NoOp{});
        }
        if (!m_model.live(0, id)) {
            m_live[index] = m_live.back();
            m_live.pop_back();
        }
        return applied;
    }

    // Whether the book reported `applied` for a successful event as the model expects, from
    // `before`, and holds what the model holds
    testing::AssertionResult agrees(const std::expected<bool, BookError>& applied,
                                    const Bbo& before)
    {
        const auto expected = m_model.bbo(0);
        if (!applied || *applied != (expected != before)) {
            return testing::AssertionFailure()
                   << "result " << testing::PrintToString(applied) << " for a BBO change from "
                   << testing::PrintToString(before) << " to " << testing::PrintToString(expected);
        }
        if (m_book.bbo() != expected || m_book.order_count() != m_model.orders().size() ||
            m_book.level_count() != m_model.level_count()) {
            return testing::AssertionFailure()
                   << "BBO " << testing::PrintToString(m_book.bbo()) << " with "
                   << m_book.order_count() << " orders on " << m_book.level_count()
                   << " levels, expected " << testing::PrintToString(expected) << " with "
                   << m_model.orders().size() << " on " << m_model.level_count();
        }
        return testing::AssertionSuccess();
    }

    // Whether the book rejected an event with `error` and kept the BBO and counts it had
    testing::AssertionResult rejected(const std::expected<bool, BookError>& applied,
                                      BookError error, const Bbo& before, std::size_t orders,
                                      std::size_t levels) const
    {
        if (applied != std::unexpected(error) || m_book.bbo() != before ||
            m_book.order_count() != orders || m_book.level_count() != levels) {
            return testing::AssertionFailure()
                   << "a rejected event returned " << testing::PrintToString(applied)
                   << " or changed the book";
        }
        return testing::AssertionSuccess();
    }

    OrderBook& m_book;
    MarketModel m_model;
    // std::mt19937's output sequence is fixed by the standard, unlike the distributions
    std::mt19937 m_random{1}; // NOLINT(bugprone-random-generator-seed)
    // IDs of the live orders, in no particular order
    std::vector<std::uint64_t> m_live;
    std::uint64_t m_next_id = 0;
};

// The book grows to thousands of orders, which grows its order map several times and erases entries
// throughout it, then drains
TEST_F(OrderBookTest, MatchesModelUnderChurn)
{
    Churn churn(book);
    std::size_t peak = 0;
    constexpr int steps = 60000;
    for (int step = 0; step < steps; ++step) {
        ASSERT_TRUE(churn.step(step < steps / 2)) << "step " << step;
        peak = std::max(peak, churn.live());
    }
    EXPECT_GT(peak, 2000U);
    ASSERT_TRUE(churn.drain());
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.level_count(), 0U);
}

TEST_F(OrderBookTest, ReportsWhetherBboChanged)
{
    EXPECT_TRUE(apply("1", AddOrder{.side = Side::Buy, .price = 1000, .shares = 100}));
    // Below the best bid
    EXPECT_FALSE(apply("2", AddOrder{.side = Side::Buy, .price = 990, .shares = 100}));
    // Joins the best level and raises its total
    EXPECT_TRUE(apply("3", AddOrder{.side = Side::Buy, .price = 1000, .shares = 50}));
    // Joins the best level without changing its total
    EXPECT_FALSE(apply("4", AddOrder{.side = Side::Buy, .price = 1000, .shares = 0}));
    EXPECT_FALSE(apply("1", ExecuteOrder{0}));
    EXPECT_FALSE(apply("2", ExecuteOrder{10}));
    // A decrease to the current quantity or above does nothing
    EXPECT_FALSE(apply("1", DecreaseOrder{200}));
    EXPECT_TRUE(apply("1", DecreaseOrder{60}));
    // Removes an empty order and leaves the best level's total as it was
    EXPECT_FALSE(apply("4", DeleteOrder{}));
    // Removes the only order of a level below the best
    EXPECT_FALSE(apply("2", DeleteOrder{}));
    // An empty level above the best still moves the price
    EXPECT_TRUE(apply("5", AddOrder{.side = Side::Buy, .price = 1010, .shares = 0}));
    EXPECT_TRUE(apply("5", DeleteOrder{}));
    EXPECT_FALSE(apply("0", NoOp{}));
    EXPECT_TRUE(apply("6", AddOrder{.side = Side::Sell, .price = 1100, .shares = 10}));
    EXPECT_FALSE(apply("7", AddOrder{.side = Side::Sell, .price = 1200, .shares = 10}));
    EXPECT_TRUE(apply("1", FillOrder{}));
    EXPECT_TRUE(apply("3", FillOrder{}));
    EXPECT_EQ(book.bbo(), bbo({}, Level{1100, 10}));
}

// NOLINTEND(readability-magic-numbers)

} // namespace
