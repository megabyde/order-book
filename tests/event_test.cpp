#include <gtest/gtest.h>

#include <order_book/event.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using order_book::Event;

TEST(EventTest, ParsesFeedRowAndIgnoresExtraColumns)
{
    const Event event("25210938,PRU,655228,B,30,643200,MSCO,Q");
    EXPECT_EQ(event.time, 25210938U);
    EXPECT_EQ(event.ticker, "PRU");
    EXPECT_EQ(event.order, "655228");
    EXPECT_EQ(event.type, Event::Type::Buy);
    EXPECT_EQ(event.shares, 30U);
    EXPECT_EQ(event.price, 643200U);
}

TEST(EventTest, AcceptsTheLargestValues)
{
    const Event event("18446744073709551615,PRU,1,S,4294967295,4294967295");
    EXPECT_EQ(event.time, UINT64_MAX);
    EXPECT_EQ(event.shares, UINT32_MAX);
    EXPECT_EQ(event.price, UINT32_MAX);
}

TEST(EventTest, RejectsMalformedLines)
{
    const std::vector<std::pair<std::string, std::string>> cases{
        {"1,PRU,1,B,30", "expected at least 6 fields, got 5"},
        {"x,PRU,1,B,30,100", "time is not an unsigned integer: 'x'"},
        {"18446744073709551616,PRU,1,B,1,1", "time out of range: '18446744073709551616'"},
        {"1,PRU,1,,30,100", "unknown message type ''"},
        {"1,PRU,1,BB,30,100", "unknown message type 'BB'"},
        {"1,PRU,1,Z,30,100", "unknown message type 'Z'"},
        {"1,PRU,1,B,,100", "shares is not an unsigned integer: ''"},
        {"1,PRU,1,B,-1,100", "shares is not an unsigned integer: '-1'"},
        {"1,PRU,1,B,+1,100", "shares is not an unsigned integer: '+1'"},
        {"1,PRU,1,B,30,12a", "price is not an unsigned integer: '12a'"},
        {"1,PRU,1,B,30,4294967296", "price out of range: '4294967296'"},
    };
    for (const auto& [line, message] : cases) {
        try {
            const Event event(line);
            ADD_FAILURE() << "accepted: " << line;
        }
        catch (const std::invalid_argument& error) {
            EXPECT_EQ(error.what(), message) << line;
        }
    }
}
