#include <gtest/gtest.h>

#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/itch_market.hpp>
#include <order_book/levels.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include "itch_messages.hpp"
#include "market_model.hpp"
#include "stream_buffers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using order_book::decode_itch;
using order_book::Format;
using order_book::ItchKind;
using order_book::ItchMessage;
using order_book::Price;
using order_book::Quantity;
using order_book::Side;
using testing::TestParamInfo;
using testing::TestWithParam;
using testing::Values;

// The message builders take each kind's fields positionally, as the specification lists them
// NOLINTBEGIN(readability-magic-numbers,bugprone-easily-swappable-parameters)

namespace {

std::size_t replay_itch(std::string_view feed, std::string& output, std::size_t threads)
{
    std::istringstream in{std::string(feed)};
    std::ostringstream out;
    const auto events = order_book::replay(in, out, Format::Itch, threads);
    output = out.str();
    return events;
}

// What replay() throws for `feed`, after checking the output it wrote before the failure
std::string replay_error(std::string_view feed, std::size_t threads, std::string_view written)
{
    std::istringstream in{std::string(feed)};
    std::ostringstream out;
    try {
        order_book::replay(in, out, Format::Itch, threads);
    }
    catch (const std::invalid_argument& error) {
        EXPECT_EQ(out.str(), written) << "threads " << threads;
        return error.what();
    }
    ADD_FAILURE() << "accepted with threads " << threads;
    return {};
}

// What decode_itch() makes of a message with the builders' locate 1 and time 2, given the fields
// its kind sets
ItchMessage decoded(ItchKind kind, std::uint64_t order, std::uint64_t replacement,
                    std::string_view stock, Quantity shares, Price price, Side side = Side::Buy)
{
    return ItchMessage{
        .time = 2,
        .order = order,
        .replacement = replacement,
        .stock = stock,
        .shares = shares,
        .price = price,
        .locate = 1,
        .kind = kind,
        .side = side,
    };
}

TEST(ItchTest, DecodesAddsAndTheHeader)
{
    // A timestamp above 32 bits exercises both of its parts
    constexpr std::uint64_t time = 0x123456789abc;
    // The decoded stock views the message, so the message must outlive it
    const auto plain_bytes = itch::add(7, time, 42, 'B', 300, "AAPL", 1'234'500);
    const auto plain = decode_itch(plain_bytes);
    ASSERT_TRUE(plain) << describe(plain.error());
    auto expected = decoded(ItchKind::Add, 42, 0, "AAPL    ", 300, 1'234'500);
    expected.time = time;
    expected.locate = 7;
    EXPECT_EQ(*plain, expected);

    // F carries an attribution and the same fields
    const auto attributed_bytes = itch::add(1, 2, 43, 'S', 5, "ABCDEFGH", 10, "MPID");
    const auto attributed = decode_itch(attributed_bytes);
    ASSERT_TRUE(attributed) << describe(attributed.error());
    EXPECT_EQ(*attributed, decoded(ItchKind::Add, 43, 0, "ABCDEFGH", 5, 10, Side::Sell));
}

struct KindCase {
    std::string_view name;
    std::string message;
    ItchMessage expected;
};

class ItchKindTest : public TestWithParam<KindCase> {};

TEST_P(ItchKindTest, MapsTypeToKind)
{
    const auto message = decode_itch(GetParam().message);
    ASSERT_TRUE(message) << describe(message.error());
    EXPECT_EQ(*message, GetParam().expected);
}

const ItchMessage skipped{
    .time = 0,
    .order = 0,
    .replacement = 0,
    .stock = {},
    .shares = 0,
    .price = 0,
    .locate = 0,
    .kind = ItchKind::Skipped,
    .side = Side::Buy,
};

INSTANTIATE_TEST_SUITE_P(
    Types, ItchKindTest,
    Values(KindCase{"Executed", itch::executed(1, 2, 42, 30),
                    decoded(ItchKind::Execute, 42, 0, {}, 30, 0)},
           KindCase{"ExecutedWithPrice", itch::executed(1, 2, 42, 30, 999),
                    decoded(ItchKind::Execute, 42, 0, {}, 30, 0)},
           KindCase{"Cancel", itch::cancel(1, 2, 42, 20),
                    decoded(ItchKind::Execute, 42, 0, {}, 20, 0)},
           KindCase{"Delete", itch::remove(1, 2, 42), decoded(ItchKind::Delete, 42, 0, {}, 0, 0)},
           KindCase{"Replace", itch::replace(1, 2, 42, 43, 70, 1005),
                    decoded(ItchKind::Replace, 42, 43, {}, 70, 1005)},
           KindCase{"Trade", itch::trade(1, 2, 10, "AAPL", 1000),
                    decoded(ItchKind::Trade, 0, 0, {}, 0, 0)},
           KindCase{"Cross", itch::cross(1, 2, 10, "AAPL", 1000),
                    decoded(ItchKind::Trade, 0, 0, {}, 0, 0)},
           KindCase{"SystemEvent", itch::system_event(2, 'O'), skipped},
           KindCase{"Directory", itch::header('R', 1, 2) + std::string(28, ' '), skipped}),
    [](const TestParamInfo<KindCase>& info) { return std::string(info.param.name); });

struct MalformedCase {
    std::string_view name;
    std::string message;
    std::string_view error;
};

class ItchMalformedTest : public TestWithParam<MalformedCase> {};

TEST_P(ItchMalformedTest, RejectsWithMessage)
{
    const auto message = decode_itch(GetParam().message);
    ASSERT_FALSE(message) << "accepted: " << GetParam().name;
    EXPECT_EQ(describe(message.error()), GetParam().error);
}

INSTANTIATE_TEST_SUITE_P(
    Messages, ItchMalformedTest,
    Values(MalformedCase{"Empty", "", "empty message"},
           MalformedCase{"UnknownType", itch::header('Z', 1, 2), "unknown message type 0x5a"},
           MalformedCase{"ShortAdd", itch::add(1, 2, 3, 'B', 4, "A", 5).substr(0, 35),
                         "'A' message has 35 bytes, expected 36"},
           MalformedCase{"LongDelete", itch::remove(1, 2, 3) + 'x',
                         "'D' message has 20 bytes, expected 19"},
           MalformedCase{"BadSide", itch::add(1, 2, 3, 'X', 4, "A", 5), "invalid side 0x58"}),
    [](const TestParamInfo<MalformedCase>& info) { return std::string(info.param.name); });

// The first reference number the market keeps outside its index, and the last of all
constexpr auto first_unindexed = std::uint64_t{1} << 32U;
constexpr auto last_reference = std::numeric_limits<std::uint64_t>::max();

TEST(ItchTest, DecodesReferencesOfAll64Bits)
{
    const auto removed = decode_itch(itch::remove(1, 2, last_reference));
    ASSERT_TRUE(removed) << describe(removed.error());
    EXPECT_EQ(removed->order, last_reference);
    const auto replaced = decode_itch(itch::replace(1, 2, first_unindexed, last_reference, 4, 5));
    ASSERT_TRUE(replaced) << describe(replaced.error());
    EXPECT_EQ(replaced->order, first_unindexed);
    EXPECT_EQ(replaced->replacement, last_reference);
}

TEST(ItchTest, PeeksTheLocateAndOrderReference)
{
    using order_book::ItchPeek;
    using order_book::peek_itch;
    EXPECT_EQ(peek_itch(itch::add(1, 2, 42, 'B', 300, "AAPL", 100)),
              (ItchPeek{.order = 42, .locate = 1}));
    EXPECT_EQ(peek_itch(itch::replace(0xbeef, 2, 7, 8, 100, 10)),
              (ItchPeek{.order = 7, .locate = 0xbeef}));
    EXPECT_EQ(peek_itch(itch::remove(3, 2, last_reference)),
              (ItchPeek{.order = last_reference, .locate = 3}));

    // Too short to hold a reference, cut off inside it, or of a type the book does not use
    EXPECT_EQ(peek_itch(""), ItchPeek{});
    EXPECT_EQ(peek_itch(itch::system_event(2, 'O')), ItchPeek{});
    EXPECT_EQ(peek_itch(itch::remove(1, 2, 42).substr(0, 18)), ItchPeek{});
    auto directory = itch::remove(1, 2, 42);
    directory.front() = 'R';
    EXPECT_EQ(peek_itch(directory), ItchPeek{});
}

ItchMessage add_message(std::uint16_t locate, std::uint64_t order, Side side, Price price,
                        Quantity shares)
{
    auto message = decoded(ItchKind::Add, order, 0, "S       ", shares, price, side);
    message.locate = locate;
    return message;
}

ItchMessage order_message(ItchKind kind, std::uint16_t locate, std::uint64_t order,
                          Quantity shares = 0)
{
    auto message = decoded(kind, order, 0, {}, shares, 0);
    message.locate = locate;
    return message;
}

ItchMessage replace_message(std::uint16_t locate, std::uint64_t order, std::uint64_t replacement,
                            Quantity shares, Price price)
{
    auto message = decoded(ItchKind::Replace, order, replacement, {}, shares, price);
    message.locate = locate;
    return message;
}

order_book::Bbo bbo(std::optional<order_book::Level> bid, std::optional<order_book::Level> ask)
{
    return order_book::Bbo{.bid = bid, .ask = ask};
}

using order_book::BookError;
using order_book::ItchMarket;
using order_book::Level;

TEST(ItchMarketTest, ReplaceKeepsTheSideAndMovesTheOrder)
{
    ItchMarket market;
    ASSERT_TRUE(market.apply(add_message(1, 1, Side::Buy, 1000, 100)));
    ASSERT_TRUE(market.apply(add_message(1, 2, Side::Sell, 1010, 50)));

    // A new best bid: the replacement rests on the buy side at its own price and size
    EXPECT_EQ(market.apply(replace_message(1, 1, 3, 70, 1005)), true);
    EXPECT_EQ(market.bbo(1), bbo(Level{1005, 70}, Level{1010, 50}));
    EXPECT_EQ(market.order_count(), 2U);
    EXPECT_EQ(market.apply(order_message(ItchKind::Delete, 1, 1)),
              std::unexpected(BookError::UnknownOrder));

    // Replacing an order by the same reference number is a replacement like any other
    EXPECT_EQ(market.apply(replace_message(1, 3, 3, 60, 1005)), true);
    EXPECT_EQ(market.bbo(1), bbo(Level{1005, 60}, Level{1010, 50}));

    // Below the best, a replacement leaves the BBO alone
    ASSERT_TRUE(market.apply(add_message(1, 4, Side::Buy, 990, 10)));
    EXPECT_EQ(market.apply(replace_message(1, 4, 5, 20, 980)), false);
    EXPECT_EQ(market.bbo(1), bbo(Level{1005, 60}, Level{1010, 50}));
    EXPECT_EQ(market.level_count(), 3U);
}

// Orders 1 and 2 in locates 1 and 2, the starting point of the rejection tests
ItchMarket two_orders()
{
    ItchMarket market;
    EXPECT_TRUE(market.apply(add_message(1, 1, Side::Buy, 1000, 100)));
    EXPECT_TRUE(market.apply(add_message(2, 2, Side::Sell, 1010, 10)));
    return market;
}

testing::AssertionResult unchanged(const ItchMarket& market)
{
    if (market.bbo(1) != bbo(Level{.price = 1000, .quantity = 100}, {}) ||
        market.bbo(2) != bbo({}, Level{.price = 1010, .quantity = 10}) ||
        market.order_count() != 2) {
        return testing::AssertionFailure() << "the market changed";
    }
    return testing::AssertionSuccess();
}

TEST(ItchMarketTest, RejectsLiveReferencesAndChangesNothing)
{
    auto market = two_orders();
    EXPECT_EQ(market.apply(add_message(1, 1, Side::Sell, 900, 5)),
              std::unexpected(BookError::DuplicateOrder));
    EXPECT_EQ(market.apply(add_message(2, 2, Side::Buy, 900, 5)),
              std::unexpected(BookError::DuplicateOrder));
    EXPECT_TRUE(unchanged(market));
    EXPECT_EQ(market.bbo(3), bbo({}, {}));
    EXPECT_EQ(market.bbo(4000), bbo({}, {}));
}

// Order 1 in three locates at once, as one order ID may be in several CSV tickers
ItchMarket shared_order()
{
    ItchMarket market;
    EXPECT_EQ(market.apply(add_message(1, 1, Side::Buy, 1000, 100)), true);
    EXPECT_EQ(market.apply(add_message(2, 1, Side::Sell, 2000, 10)), true);
    EXPECT_EQ(market.apply(add_message(3, 1, Side::Buy, 3000, 30)), true);
    return market;
}

TEST(ItchMarketTest, KeepsReferenceNumbersPerLocate)
{
    auto market = shared_order();
    EXPECT_EQ(market.order_count(), 3U);
    EXPECT_EQ(market.apply(add_message(2, 1, Side::Buy, 900, 5)),
              std::unexpected(BookError::DuplicateOrder));

    // Deleting the order that holds the index slot, and with it its page, leaves the others
    EXPECT_EQ(market.apply(order_message(ItchKind::Delete, 1, 1)), true);
    EXPECT_EQ(market.apply(add_message(2, 1, Side::Buy, 900, 5)),
              std::unexpected(BookError::DuplicateOrder));
    EXPECT_EQ(market.apply(order_message(ItchKind::Execute, 2, 1, 4)), true);
    EXPECT_EQ(market.bbo(2), bbo({}, Level{2000, 6}));
    EXPECT_EQ(market.bbo(3), bbo(Level{3000, 30}, {}));
    EXPECT_EQ(market.order_count(), 2U);
}

TEST(ItchMarketTest, ReplacesIntoReferenceNumbersOnlyOtherLocatesHold)
{
    auto market = shared_order();
    ASSERT_TRUE(market.apply(add_message(3, 2, Side::Buy, 2990, 1)));
    EXPECT_EQ(market.apply(replace_message(3, 2, 1, 1, 1)),
              std::unexpected(BookError::DuplicateOrder));
    EXPECT_EQ(market.apply(replace_message(2, 1, 2, 7, 1990)), true);
    EXPECT_EQ(market.bbo(2), bbo({}, Level{1990, 7}));
    EXPECT_EQ(market.bbo(3), bbo(Level{3000, 30}, {}));
    EXPECT_EQ(market.apply(order_message(ItchKind::Delete, 2, 1)),
              std::unexpected(BookError::UnknownOrder));
    EXPECT_EQ(market.order_count(), 4U);
}

// Delete each order of `orders`, by locate and reference number, each changing its BBO
testing::AssertionResult
delete_best(ItchMarket& market,
            std::initializer_list<std::pair<std::uint16_t, std::uint64_t>> orders)
{
    for (const auto& [locate, order] : orders) {
        if (market.apply(order_message(ItchKind::Delete, locate, order)) != true) {
            return testing::AssertionFailure()
                   << "delete of order " << order << " in locate " << locate;
        }
    }
    return testing::AssertionSuccess();
}

TEST(ItchMarketTest, EmptiesOrdersSharingReferenceNumbers)
{
    // Orders 1 and 2 each in the index once and in the hash map for the other locates
    auto market = shared_order();
    ASSERT_TRUE(market.apply(add_message(3, 2, Side::Buy, 2990, 1)));
    ASSERT_TRUE(market.apply(add_message(2, 2, Side::Sell, 1990, 1)));
    EXPECT_TRUE(delete_best(market, {{3, 1}, {1, 1}, {2, 2}, {3, 2}, {2, 1}}));
    EXPECT_EQ(market.order_count(), 0U);
    EXPECT_EQ(market.level_count(), 0U);
    // Nothing is left behind in the index or the hash map
    EXPECT_EQ(market.apply(add_message(1, 1, Side::Buy, 1000, 1)), true);
    EXPECT_EQ(market.apply(add_message(2, 1, Side::Buy, 1000, 1)), true);
    EXPECT_EQ(market.apply(add_message(3, 1, Side::Buy, 1000, 1)), true);
}

class ItchUnknownOrderTest : public TestWithParam<ItchKind> {};

TEST_P(ItchUnknownOrderTest, RejectsAndChangesNothing)
{
    auto market = two_orders();
    // Order 9 does not exist, order 2 lives in locate 2, not 1, and order 10^9 lies far past any
    // page yet allocated
    for (const std::uint64_t order : {9, 2, 1'000'000'000}) {
        EXPECT_EQ(market.apply(order_message(GetParam(), 1, order, 1)),
                  std::unexpected(BookError::UnknownOrder))
            << "order " << order;
    }
    EXPECT_TRUE(unchanged(market));
}

std::string kind_name(const TestParamInfo<ItchKind>& info)
{
    constexpr std::array<std::string_view, 6> names{
        "Add", "Execute", "Delete", "Replace", "Trade", "Skipped",
    };
    return std::string(names.at(static_cast<std::size_t>(info.param)));
}

INSTANTIATE_TEST_SUITE_P(Kinds, ItchUnknownOrderTest,
                         Values(ItchKind::Execute, ItchKind::Delete, ItchKind::Replace), kind_name);

TEST(ItchMarketTest, ReportsWhetherBboChanged)
{
    ItchMarket market;
    EXPECT_EQ(market.apply(add_message(1, 1, Side::Buy, 1000, 100)), true);
    // A deeper level, and a 0-share add at the best price, leave the BBO as it was
    EXPECT_EQ(market.apply(add_message(1, 2, Side::Buy, 990, 100)), false);
    EXPECT_EQ(market.apply(add_message(1, 3, Side::Buy, 1000, 0)), false);
    // An execution of 0 shares at the best changes nothing
    EXPECT_EQ(market.apply(order_message(ItchKind::Execute, 1, 1, 0)), false);
    // A 0-share add above the best still moves the price, and executing its 0 shares takes all
    // that remains, removing it
    EXPECT_EQ(market.apply(add_message(1, 4, Side::Buy, 1010, 0)), true);
    EXPECT_EQ(market.apply(order_message(ItchKind::Execute, 1, 4, 0)), true);
    EXPECT_EQ(market.apply(order_message(ItchKind::Delete, 1, 4)),
              std::unexpected(BookError::UnknownOrder));
    // Executions are clamped; emptying a level below the best changes nothing
    EXPECT_EQ(market.apply(order_message(ItchKind::Execute, 1, 2, 1000)), false);
    EXPECT_EQ(market.apply(order_message(ItchKind::Execute, 1, 1, 40)), true);
    EXPECT_EQ(market.bbo(1), bbo(Level{1000, 60}, {}));
    // Trades and skipped messages touch nothing
    EXPECT_EQ(market.apply(order_message(ItchKind::Trade, 1, 0)), false);
    EXPECT_EQ(market.apply(order_message(ItchKind::Skipped, 0, 0)), false);
    EXPECT_EQ(market.level_count(), 1U);
}

// Add `count` one-share orders over 7 levels, then delete them all
testing::AssertionResult fill_and_empty(ItchMarket& market, std::uint64_t count)
{
    for (std::uint64_t order = 1; order <= count; ++order) {
        const auto price = static_cast<Price>(1000 + (order % 7));
        if (!market.apply(add_message(1, order, Side::Sell, price, 1))) {
            return testing::AssertionFailure() << "add " << order << " failed";
        }
    }
    if (market.order_count() != count || market.level_count() != 7) {
        return testing::AssertionFailure()
               << market.order_count() << " orders, " << market.level_count() << " levels";
    }
    for (std::uint64_t order = 1; order <= count; ++order) {
        if (!market.apply(order_message(ItchKind::Delete, 1, order))) {
            return testing::AssertionFailure() << "delete " << order << " failed";
        }
    }
    return testing::AssertionSuccess();
}

TEST(ItchMarketTest, ReusesEmptiedPagesAndSlots)
{
    // Several pages of reference numbers, emptied and filled again
    ItchMarket market;
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(fill_and_empty(market, 30'000)) << "round " << round;
        EXPECT_EQ(market.order_count(), 0U);
        EXPECT_EQ(market.level_count(), 0U);
        EXPECT_EQ(market.bbo(1), bbo({}, {}));
    }
}

// Both sides of a 128-reference page boundary, of a 65,536-reference chunk boundary, and of the
// end of the index, and the last reference
constexpr std::array<std::uint64_t, 8> spread_orders{
    1, 127, 128, 65'535, 65'536, first_unindexed - 1, first_unindexed, last_reference,
};

// Sell spread_orders[i] at 1000 + i, 10 * (i + 1) shares, so the orders' asks rise in their order
ItchMarket spread_market()
{
    ItchMarket market;
    for (std::size_t i = 0; i < spread_orders.size(); ++i) {
        EXPECT_TRUE(market.apply(add_message(1, spread_orders[i], Side::Sell,
                                             static_cast<Price>(1000 + i), 10 * (i + 1))))
            << spread_orders[i];
    }
    return market;
}

// Delete the best ask of spread_market() until none is left, checking each order is the one shown
testing::AssertionResult delete_spread(ItchMarket& market)
{
    for (std::size_t i = 0; i < spread_orders.size(); ++i) {
        const auto ask = Level{.price = static_cast<Price>(1000 + i), .quantity = 10 * (i + 1)};
        if (market.bbo(1) != bbo({}, ask)) {
            return testing::AssertionFailure() << "best ask before deleting " << spread_orders[i];
        }
        if (market.apply(order_message(ItchKind::Delete, 1, spread_orders[i])) != true) {
            return testing::AssertionFailure() << "delete " << spread_orders[i];
        }
    }
    return testing::AssertionSuccess();
}

TEST(ItchMarketTest, IndexesTheWholeReferenceRange)
{
    auto market = spread_market();
    EXPECT_EQ(market.order_count(), spread_orders.size());
    // An unused page of a used chunk, an unused chunk, and a locate the orders are not in
    const auto unknown = std::unexpected(BookError::UnknownOrder);
    EXPECT_EQ(market.apply(order_message(ItchKind::Delete, 1, 65'536 + 128)), unknown);
    EXPECT_EQ(market.apply(order_message(ItchKind::Delete, 1, std::uint64_t{1} << 20U)), unknown);
    EXPECT_EQ(market.apply(order_message(ItchKind::Delete, 2, spread_orders.back())), unknown);
    EXPECT_TRUE(delete_spread(market));
    EXPECT_EQ(market.order_count(), 0U);
}

// Orders first_unindexed and last_reference in locate 1 and last_reference in locate 2, with none
// below first_unindexed, so the index has no chunk at all
ItchMarket unindexed_orders()
{
    ItchMarket market;
    market.prefetch(last_reference, 1);
    market.prefetch_levels(last_reference, 1);
    EXPECT_EQ(market.apply(add_message(1, first_unindexed, Side::Buy, 1000, 100)), true);
    EXPECT_EQ(market.apply(add_message(1, last_reference, Side::Sell, 1010, 50)), true);
    EXPECT_EQ(market.apply(add_message(2, last_reference, Side::Buy, 990, 7)), true);
    market.prefetch(last_reference, 1);
    market.prefetch_levels(last_reference, 1);
    return market;
}

TEST(ItchMarketTest, KeepsOrdersBeyondTheIndex)
{
    auto market = unindexed_orders();
    EXPECT_EQ(market.apply(add_message(1, last_reference, Side::Buy, 900, 5)),
              std::unexpected(BookError::DuplicateOrder));

    // A partial execution keeps the order and a full one removes it
    EXPECT_EQ(market.apply(order_message(ItchKind::Execute, 1, first_unindexed, 40)), true);
    EXPECT_EQ(market.bbo(1), bbo(Level{1000, 60}, Level{1010, 50}));
    EXPECT_EQ(market.apply(order_message(ItchKind::Execute, 1, first_unindexed, 60)), true);
    EXPECT_EQ(market.bbo(1), bbo({}, Level{1010, 50}));

    EXPECT_TRUE(delete_best(market, {{1, last_reference}, {2, last_reference}}));
    EXPECT_EQ(market.order_count(), 0U);
    EXPECT_EQ(market.level_count(), 0U);
    // Nothing is left behind
    EXPECT_EQ(market.apply(add_message(1, last_reference, Side::Buy, 1000, 1)), true);
    EXPECT_EQ(market.apply(add_message(2, first_unindexed, Side::Buy, 1000, 1)), true);
}

// Whether a delete of each order of `orders` in `locate` fails as unknown
testing::AssertionResult none_live(ItchMarket& market, std::uint16_t locate,
                                   std::initializer_list<std::uint64_t> orders)
{
    for (const auto order : orders) {
        if (market.apply(order_message(ItchKind::Delete, locate, order)) !=
            std::unexpected(BookError::UnknownOrder)) {
            return testing::AssertionFailure() << "order " << order << " is live";
        }
    }
    return testing::AssertionSuccess();
}

TEST(ItchMarketTest, ReplacesOrdersIntoTheIndexAndOut)
{
    // From the last reference number into the index, to its last reference number, and out again
    auto market = unindexed_orders();
    EXPECT_EQ(market.apply(replace_message(1, last_reference, 5, 50, 1005)), true);
    EXPECT_EQ(market.apply(replace_message(1, 5, first_unindexed - 1, 20, 1003)), true);
    EXPECT_EQ(market.apply(replace_message(1, first_unindexed - 1, first_unindexed + 1, 10, 1001)),
              true);
    EXPECT_EQ(market.bbo(1), bbo(Level{1000, 100}, Level{1001, 10}));
    EXPECT_EQ(market.bbo(2), bbo(Level{990, 7}, {}));
    EXPECT_TRUE(none_live(market, 1, {last_reference, 5, first_unindexed - 1}));
    EXPECT_TRUE(
        delete_best(market, {{1, first_unindexed + 1}, {1, first_unindexed}, {2, last_reference}}));
    EXPECT_EQ(market.order_count(), 0U);
}

// One random add, execution, replacement, or delete, applied to the market and the model, then
// checked against the model's BBO before and after. Some adds and replacements take a reference
// number that another locate holds.
class Churn {
public:
    testing::AssertionResult step(bool adding)
    {
        if (m_live.empty() || (adding && m_random() % 3 != 0)) {
            return add();
        }
        const auto index = m_random() % m_live.size();
        const auto [locate, order] = m_live[index];
        const auto& resting = m_model.orders().at({locate, order});
        const auto before = m_model.bbo(locate);
        std::expected<bool, BookError> changed = false;
        switch (m_random() % 3) {
        case 0: {
            const auto shares = 1 + (m_random() % resting.remaining);
            changed = m_market.apply(order_message(ItchKind::Execute, locate, order, shares));
            m_model.take(locate, order, shares);
            break;
        }
        case 1: {
            const auto side = resting.side;
            const auto price = random_price();
            const auto shares = random_shares();
            const auto replacement = reference(locate);
            changed = m_market.apply(replace_message(locate, order, replacement, shares, price));
            m_model.take(locate, order, resting.remaining);
            m_model.add(locate, replacement, side, price, shares);
            m_live.emplace_back(locate, replacement);
            break;
        }
        default:
            changed = m_market.apply(order_message(ItchKind::Delete, locate, order));
            m_model.take(locate, order, resting.remaining);
            break;
        }
        if (!m_model.live(locate, order)) {
            m_live[index] = m_live.back();
            m_live.pop_back();
        }
        return check(locate, before, changed);
    }

    [[nodiscard]] std::size_t orders() const { return m_market.order_count(); }
    [[nodiscard]] std::size_t model_orders() const { return m_model.orders().size(); }

private:
    testing::AssertionResult add()
    {
        const auto locate = static_cast<std::uint16_t>(1 + (m_random() % 5));
        const auto side = m_random() % 2 == 0 ? Side::Buy : Side::Sell;
        const auto price = random_price();
        const auto shares = random_shares();
        const auto order = reference(locate);
        const auto before = m_model.bbo(locate);
        const auto changed = m_market.apply(add_message(locate, order, side, price, shares));
        m_model.add(locate, order, side, price, shares);
        m_live.emplace_back(locate, order);
        return check(locate, before, changed);
    }

    // A reference number not live in `locate`: one in four times that of a random live order, if
    // that is of another locate, else a new one
    std::uint64_t reference(std::uint16_t locate)
    {
        if (!m_live.empty() && m_random() % 4 == 0) {
            const auto order = m_live[m_random() % m_live.size()].second;
            if (!m_model.live(locate, order)) {
                return order;
            }
        }
        // One new reference number in eight counts up across the end of the index, and one counts
        // down from the last; the rest skip some, so the orders spread over pages
        switch (m_random() % 8) {
        case 0:
            return m_beyond++;
        case 1:
            return m_last--;
        default: {
            const auto order = m_next;
            m_next += 1 + (m_random() % 3);
            return order;
        }
        }
    }

    testing::AssertionResult check(std::uint16_t locate, const order_book::Bbo& before,
                                   const std::expected<bool, BookError>& changed)
    {
        const auto after = m_model.bbo(locate);
        if (!changed || m_market.bbo(locate) != after || *changed != (after != before)) {
            return testing::AssertionFailure() << "locate " << locate << " disagrees";
        }
        return testing::AssertionSuccess();
    }

    Price random_price() { return static_cast<Price>(1000 + (m_random() % 40 * 10)); }
    Quantity random_shares() { return static_cast<Quantity>(1 + (m_random() % 100)); }

    // std::mt19937's output sequence is fixed by the standard, unlike the distributions
    std::mt19937 m_random{7}; // NOLINT(bugprone-random-generator-seed)
    ItchMarket m_market;
    MarketModel m_model;
    std::vector<std::pair<std::uint16_t, std::uint64_t>> m_live;
    std::uint64_t m_next = 1;
    std::uint64_t m_beyond = first_unindexed - 100;
    std::uint64_t m_last = last_reference;
};

TEST(ItchMarketTest, MatchesModelUnderChurn)
{
    // Two adds in three during the first half, then removals and replacements only
    Churn churn;
    constexpr int steps = 60'000;
    for (int step = 0; step < steps; ++step) {
        ASSERT_TRUE(churn.step(step < steps / 2)) << "step " << step;
    }
    EXPECT_EQ(churn.orders(), churn.model_orders());
}

TEST(ItchReplayTest, AppliesEachMessageTypeAndSkipsTheRest)
{
    using itch::framed;
    const std::string feed =
        framed(itch::system_event(1, 'O')) +
        framed(itch::header('R', 1, 2) + std::string(28, ' ')) +
        framed(itch::add(1, 10, 1, 'B', 100, "AAPL", 1000)) +
        framed(itch::add(1, 11, 2, 'S', 50, "AAPL", 1010, "MPID")) +
        framed(itch::executed(1, 12, 2, 30)) + framed(itch::executed(1, 13, 2, 5, 1010)) +
        // The replacement keeps the order's side and rests at its own price and size
        framed(itch::replace(1, 14, 1, 7, 70, 1005)) + framed(itch::cancel(1, 15, 7, 20)) +
        framed(itch::trade(1, 16, 10, "AAPL", 1005)) + framed(itch::cross(1, 17, 0, "AAPL", 0)) +
        // A cancel beyond what remains is clamped, removing the order
        framed(itch::cancel(1, 18, 2, 100)) + framed(itch::remove(1, 19, 7)) +
        framed(itch::system_event(20, 'C'));
    for (std::size_t threads = 1; threads <= 3; ++threads) {
        std::string output;
        // Every message but the three the book skips
        EXPECT_EQ(replay_itch(feed, output, threads), 10U) << "threads " << threads;
        EXPECT_EQ(output, "10,AAPL,1000,100,,\n"
                          "11,AAPL,1000,100,1010,50\n"
                          "12,AAPL,1000,100,1010,20\n"
                          "13,AAPL,1000,100,1010,15\n"
                          "14,AAPL,1005,70,1010,15\n"
                          "15,AAPL,1005,50,1010,15\n"
                          "18,AAPL,1005,50,,\n"
                          "19,AAPL,,,,\n")
            << "threads " << threads;
    }
}

struct FailureCase {
    std::string_view name;
    std::string tail;
    std::string_view error;
};

class ItchFailureTest : public TestWithParam<FailureCase> {};

// Two good messages, then the failing one: whatever the threads, the output of the first two is
// written and the error names message 3
TEST_P(ItchFailureTest, NamesTheFailingMessage)
{
    const auto feed = itch::framed(itch::add(1, 1, 1, 'B', 100, "AAA", 1000)) +
                      itch::framed(itch::add(2, 2, 2, 'S', 50, "BBB", 1010)) + GetParam().tail;
    for (std::size_t threads = 1; threads <= 3; ++threads) {
        EXPECT_EQ(replay_error(feed, threads, "1,AAA,1000,100,,\n2,BBB,,,1010,50\n"),
                  GetParam().error)
            << "threads " << threads;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Messages, ItchFailureTest,
    Values(FailureCase{"UnknownOrder", itch::framed(itch::executed(1, 3, 9, 10)),
                       "message 3: unknown order '9'"},
           FailureCase{"DuplicateAdd", itch::framed(itch::add(2, 3, 2, 'B', 1, "BBB", 1)),
                       "message 3: duplicate order '2'"},
           FailureCase{"ReplaceUnknown", itch::framed(itch::replace(1, 3, 9, 10, 1, 1)),
                       "message 3: unknown order '9'"},
           FailureCase{"ReplaceIntoLiveOrder",
                       itch::framed(itch::add(1, 3, 3, 'B', 1, "AAA", 1)) +
                           itch::framed(itch::replace(1, 4, 1, 3, 1, 1)),
                       "message 4: duplicate order '3'"},
           FailureCase{"Malformed", itch::framed(itch::add(1, 3, 3, 'X', 1, "AAA", 1)),
                       "message 3: invalid side 0x58"},
           // Too short to name a locate
           FailureCase{"OneByte", itch::framed("A"),
                       "message 3: 'A' message has 1 bytes, expected 36"},
           FailureCase{"TruncatedLength", std::string(1, '\0'),
                       "message 3: truncated message length"},
           FailureCase{"TruncatedBody", itch::framed(itch::remove(1, 3, 1)).substr(0, 10),
                       "message 3: truncated message: 8 of 19 bytes"}),
    [](const TestParamInfo<FailureCase>& info) { return std::string(info.param.name); });

TEST(ItchReplayTest, ReplaysWholeMessagesBeforeAReadFailure)
{
    // The read fails within message 3, which a truncated feed would report instead
    const auto feed = itch::framed(itch::add(1, 1, 1, 'B', 100, "AAA", 1000)) +
                      itch::framed(itch::add(1, 2, 2, 'S', 50, "AAA", 1010));
    const auto cut = itch::framed(itch::remove(1, 3, 1));
    for (std::size_t threads = 1; threads <= 3; ++threads) {
        streams::ReadFailure buffer(feed + cut, feed.size() + 10);
        EXPECT_EQ(streams::written_before_read_failure(buffer, Format::Itch, threads),
                  "1,AAA,1000,100,,\n2,AAA,1000,100,1010,50\n")
            << "threads " << threads;
    }
}

TEST(ItchReplayTest, OutputDoesNotDependOnThreads)
{
    // 37 interleaved locates, each adding an order on even steps and deleting it on odd ones
    constexpr std::uint16_t locates = 37;
    std::string feed;
    for (std::uint64_t i = 0; i < 20'000; ++i) {
        const auto locate = static_cast<std::uint16_t>((i % locates) + 1);
        const auto step = i / locates;
        const auto order = (step * locates) + locate;
        feed +=
            itch::framed(step % 2 == 0 ? itch::add(locate, i, order, step / 2 % 2 == 0 ? 'B' : 'S',
                                                   static_cast<std::uint32_t>(100 + (step % 7)),
                                                   std::format("S{}", locate),
                                                   static_cast<std::uint32_t>(1000 + (step % 50)))
                                       : itch::remove(locate, i, order - locates));
    }
    std::string single;
    replay_itch(feed, single, 1);
    EXPECT_EQ(std::ranges::count(single, '\n'), 20'000);
    for (std::size_t threads = 2; threads <= 5; ++threads) {
        std::string output;
        replay_itch(feed, output, threads);
        EXPECT_EQ(output, single) << "threads " << threads;
    }
}

TEST(ItchReplayTest, KeepsReferenceNumbersPerLocate)
{
    // Three locates add order 1, whichever workers they go to
    using itch::framed;
    const std::string feed = framed(itch::add(1, 1, 1, 'B', 100, "AAA", 1000)) +
                             framed(itch::add(2, 2, 1, 'S', 50, "BBB", 2000)) +
                             framed(itch::add(3, 3, 1, 'B', 10, "CCC", 3000)) +
                             framed(itch::executed(2, 4, 1, 20)) + framed(itch::remove(1, 5, 1)) +
                             framed(itch::replace(3, 6, 1, 2, 15, 3005)) +
                             framed(itch::add(1, 7, 1, 'S', 5, "AAA", 1010)) +
                             framed(itch::add(3, 8, 2, 'B', 1, "CCC", 1));
    for (std::size_t threads = 1; threads <= 4; ++threads) {
        EXPECT_EQ(replay_error(feed, threads,
                               "1,AAA,1000,100,,\n2,BBB,,,2000,50\n3,CCC,3000,10,,\n"
                               "4,BBB,,,2000,30\n5,AAA,,,,\n6,CCC,3005,15,,\n7,AAA,,,1010,5\n"),
                  "message 8: duplicate order '2'")
            << "threads " << threads;
    }
}

TEST(ItchReplayTest, ReplaysFeedLargerThanItsBuffers)
{
    // Each add raises the best bid, so input and output both span several 1 MiB batches and
    // messages straddle the batch boundaries
    constexpr std::uint32_t count = 100'000;
    std::string feed;
    std::string expected;
    for (std::uint32_t i = 1; i <= count; ++i) {
        feed += itch::framed(itch::add(1, i, i, 'B', 1, "AAA", i));
        expected += std::format("{0},AAA,{0},1,,\n", i);
    }
    std::string output;
    EXPECT_EQ(replay_itch(feed, output, 2), count);
    EXPECT_EQ(output, expected);
}

} // namespace

// NOLINTEND(readability-magic-numbers,bugprone-easily-swappable-parameters)
