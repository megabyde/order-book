#include <gtest/gtest.h>

#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/replay.hpp>

#include "itch_messages.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using order_book::AddOrder;
using order_book::decode_itch;
using order_book::DeleteOrder;
using order_book::ExecuteOrder;
using order_book::Format;
using order_book::ItchEvent;
using order_book::NoOp;
using order_book::ReplaceOrder;
using order_book::Side;
using order_book::Skipped;
using testing::TestParamInfo;
using testing::TestWithParam;
using testing::Values;

// NOLINTBEGIN(readability-magic-numbers)

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

TEST(ItchTest, DecodesAddsAndTheHeader)
{
    // A timestamp above 32 bits exercises both of its parts
    constexpr std::uint64_t time = 0x123456789abc;
    // The decoded stock views the message, so the message must outlive it
    const auto plain_bytes = itch::add(7, time, 42, 'B', 300, "AAPL", 1'234'500);
    const auto plain = decode_itch(plain_bytes);
    ASSERT_TRUE(plain) << plain.error();
    EXPECT_EQ(plain->time, time);
    EXPECT_EQ(plain->locate, 7);
    EXPECT_EQ(plain->stock, "AAPL");
    EXPECT_EQ(plain->order, 42U);
    const ItchEvent expected = AddOrder{.side = Side::Buy, .price = 1'234'500, .shares = 300};
    EXPECT_EQ(plain->event, expected);

    // F carries an attribution and the same fields; a full 8-character symbol has no padding
    const auto attributed_bytes = itch::add(8, 1, 43, 'S', 5, "ABCDEFGH", 10, "MPID");
    const auto attributed = decode_itch(attributed_bytes);
    ASSERT_TRUE(attributed) << attributed.error();
    EXPECT_EQ(attributed->stock, "ABCDEFGH");
    const ItchEvent sell = AddOrder{.side = Side::Sell, .price = 10, .shares = 5};
    EXPECT_EQ(attributed->event, sell);
}

struct EventCase {
    std::string_view name;
    std::string message;
    std::uint64_t order;
    ItchEvent event;
};

class ItchEventTest : public TestWithParam<EventCase> {};

TEST_P(ItchEventTest, MapsTypeToEvent)
{
    const auto message = decode_itch(GetParam().message);
    ASSERT_TRUE(message) << message.error();
    EXPECT_EQ(message->order, GetParam().order);
    EXPECT_EQ(message->event, GetParam().event);
}

INSTANTIATE_TEST_SUITE_P(
    Types, ItchEventTest,
    Values(EventCase{"Executed", itch::executed(1, 2, 42, 30), 42, ExecuteOrder{30}},
           EventCase{"ExecutedWithPrice", itch::executed(1, 2, 42, 30, 999), 42, ExecuteOrder{30}},
           EventCase{"Cancel", itch::cancel(1, 2, 42, 20), 42, ExecuteOrder{20}},
           EventCase{"Delete", itch::remove(1, 2, 42), 42, DeleteOrder{}},
           EventCase{"Replace", itch::replace(1, 2, 42, 43, 70, 1005), 42,
                     ReplaceOrder{.order = 43, .price = 1005, .shares = 70}},
           EventCase{"Trade", itch::trade(1, 2, 10, "AAPL", 1000), 0, NoOp{}},
           EventCase{"Cross", itch::cross(1, 2, 10, "AAPL", 1000), 0, NoOp{}},
           EventCase{"SystemEvent", itch::system_event(2, 'O'), 0, Skipped{}},
           EventCase{"Directory", itch::header('R', 1, 2) + std::string(28, ' '), 0, Skipped{}}),
    [](const TestParamInfo<EventCase>& info) { return std::string(info.param.name); });

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
    EXPECT_EQ(message.error(), GetParam().error);
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

// NOLINTEND(readability-magic-numbers)
