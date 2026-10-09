#include <gtest/gtest.h>

#include <order_book/feed.hpp>

#include <cstdint>
#include <string>
#include <string_view>

using order_book::AddOrder;
using order_book::DecreaseOrder;
using order_book::DeleteOrder;
using order_book::Event;
using order_book::ExecuteOrder;
using order_book::FillOrder;
using order_book::NoOp;
using order_book::parse_line;
using order_book::Side;
using testing::TestParamInfo;
using testing::TestWithParam;
using testing::Values;

namespace {

TEST(FeedTest, ParsesFeedRowAndIgnoresExtraColumns)
{
    const auto message = parse_line("25210938,PRU,655228,B,30,643200,MSCO,Q");
    ASSERT_TRUE(message) << message.error();
    EXPECT_EQ(message->time, 25210938U);
    EXPECT_EQ(message->ticker, "PRU");
    EXPECT_EQ(message->order, "655228");
    const Event expected = AddOrder{.side = Side::Buy, .price = 643200, .shares = 30};
    EXPECT_EQ(message->event, expected);
}

TEST(FeedTest, AcceptsTheLargestValues)
{
    const auto message = parse_line("18446744073709551615,PRU,1,S,4294967295,4294967295");
    ASSERT_TRUE(message) << message.error();
    EXPECT_EQ(message->time, UINT64_MAX);
    const Event expected = AddOrder{.side = Side::Sell, .price = UINT32_MAX, .shares = UINT32_MAX};
    EXPECT_EQ(message->event, expected);
}

struct TypeCase {
    std::string_view name;
    std::string_view line;
    Event event;
};

class FeedTypeTest : public TestWithParam<TypeCase> {};

TEST_P(FeedTypeTest, MapsTypeToEvent)
{
    const auto message = parse_line(GetParam().line);
    ASSERT_TRUE(message) << message.error();
    EXPECT_EQ(message->event, GetParam().event);
}

INSTANTIATE_TEST_SUITE_P(Types, FeedTypeTest,
                         Values(TypeCase{"Buy", "1,PRU,1,B,30,100",
                                         AddOrder{.side = Side::Buy, .price = 100, .shares = 30}},
                                TypeCase{"Sell", "1,PRU,1,S,30,100",
                                         AddOrder{.side = Side::Sell, .price = 100, .shares = 30}},
                                TypeCase{"Decrease", "1,PRU,1,C,20,0", DecreaseOrder{20}},
                                TypeCase{"Delete", "1,PRU,1,D,0,0", DeleteOrder{}},
                                TypeCase{"Execute", "1,PRU,1,E,10,0", ExecuteOrder{10}},
                                TypeCase{"Fill", "1,PRU,1,F,0,0", FillOrder{}},
                                TypeCase{"Trade", "1,PRU,0,T,10,100", NoOp{}},
                                TypeCase{"CrossTrade", "1,PRU,1,X,0,0", NoOp{}}),
                         [](const TestParamInfo<TypeCase>& info) {
                             return std::string(info.param.name);
                         });

struct MalformedCase {
    std::string_view name;
    std::string_view line;
    std::string_view error;
};

class FeedMalformedTest : public TestWithParam<MalformedCase> {};

TEST_P(FeedMalformedTest, RejectsWithMessage)
{
    const auto message = parse_line(GetParam().line);
    ASSERT_FALSE(message) << "accepted: " << GetParam().line;
    EXPECT_EQ(message.error(), GetParam().error);
}

INSTANTIATE_TEST_SUITE_P(
    Lines, FeedMalformedTest,
    Values(MalformedCase{"TooFewFields", "1,PRU,1,B,30", "expected at least 6 fields, got 5"},
           MalformedCase{"TimeNotNumber", "x,PRU,1,B,30,100",
                         "time is not an unsigned integer: 'x'"},
           MalformedCase{"TimeOutOfRange", "18446744073709551616,PRU,1,B,1,1",
                         "time out of range: '18446744073709551616'"},
           MalformedCase{"EmptyType", "1,PRU,1,,30,100", "unknown message type ''"},
           MalformedCase{"LongType", "1,PRU,1,BB,30,100", "unknown message type 'BB'"},
           MalformedCase{"UnknownType", "1,PRU,1,Z,30,100", "unknown message type 'Z'"},
           MalformedCase{"EmptyShares", "1,PRU,1,B,,100", "shares is not an unsigned integer: ''"},
           MalformedCase{"NegativeShares", "1,PRU,1,B,-1,100",
                         "shares is not an unsigned integer: '-1'"},
           MalformedCase{"SignedShares", "1,PRU,1,B,+1,100",
                         "shares is not an unsigned integer: '+1'"},
           MalformedCase{"SharesOutOfRange", "1,PRU,1,B,4294967296,100",
                         "shares out of range: '4294967296'"},
           MalformedCase{"PriceNotNumber", "1,PRU,1,B,30,12a",
                         "price is not an unsigned integer: '12a'"},
           MalformedCase{"PriceOutOfRange", "1,PRU,1,B,30,4294967296",
                         "price out of range: '4294967296'"}),
    [](const TestParamInfo<MalformedCase>& info) { return std::string(info.param.name); });

} // namespace
