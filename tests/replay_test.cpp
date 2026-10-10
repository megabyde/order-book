#include <gtest/gtest.h>

#include <order_book/replay.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ios>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// Hands out at most a few characters per bulk read, as a pipe or a translating file may
class TrickleBuffer : public std::stringbuf {
public:
    using std::stringbuf::stringbuf;

protected:
    std::streamsize xsgetn(char* s, std::streamsize count) override
    {
        return std::stringbuf::xsgetn(s, std::min(count, max_read));
    }

private:
    static constexpr std::streamsize max_read = 7;
};

} // namespace

TEST(ReplayTest, CountsAppliedEventsAndSkipsHeaderAndBlankLines)
{
    // CRLF rows, a blank line, a trade, and no newline after the last row
    std::istringstream in("Time,Ticker,Order,T,Shares,Price,MPID,X\r\n"
                          "1,AAA,1,B,100,1000,,Q\r\n"
                          "\r\n"
                          "2,AAA,0,T,10,1000,,Q\r\n"
                          "3,AAA,1,D,0,0,,Q");
    std::ostringstream out;
    EXPECT_EQ(order_book::replay(in, out), 3U);
    EXPECT_EQ(out.str(), "1,AAA,1000,100,,\n3,AAA,,,,\n");
}

TEST(ReplayTest, ReplaysFeedLargerThanItsBuffers)
{
    // Each add raises the best bid, so input and output both span several 64 KiB blocks and rows
    // straddle the block boundaries
    constexpr std::size_t count = 20000;
    std::string feed = "Time,Ticker,Order,T,Shares,Price\r\n";
    std::string expected;
    for (std::size_t i = 1; i <= count; ++i) {
        feed += std::format("{0},AAA,{0},B,1,{0}\r\n", i);
        expected += std::format("{0},AAA,{0},1,,\n", i);
    }
    std::istringstream in(feed);
    std::ostringstream out;
    EXPECT_EQ(order_book::replay(in, out), count);
    EXPECT_EQ(out.str(), expected);
}

TEST(ReplayTest, ReadsPastShortReads)
{
    TrickleBuffer buffer("Time,Ticker,Order,T,Shares,Price\n"
                         "1,AAA,1,B,100,1000\n"
                         "2,AAA,2,S,50,1010\n");
    std::istream in(&buffer);
    std::ostringstream out;
    EXPECT_EQ(order_book::replay(in, out), 2U);
    EXPECT_EQ(out.str(), "1,AAA,1000,100,,\n2,AAA,1000,100,1010,50\n");
}

TEST(ReplayTest, ReadsLineLongerThanItsBuffer)
{
    const std::string extra(std::size_t{200} << 10U, 'x');
    std::istringstream in("Time,Ticker,Order,T,Shares,Price,Extra\n"
                          "1,AAA,1,B,100,1000," +
                          extra +
                          "\n"
                          "2,AAA,2,S,50,1010," +
                          extra + "\n");
    std::ostringstream out;
    EXPECT_EQ(order_book::replay(in, out), 2U);
    EXPECT_EQ(out.str(), "1,AAA,1000,100,,\n2,AAA,1000,100,1010,50\n");
}

TEST(ReplayTest, WritesOutputBeforeTheFailingLine)
{
    std::istringstream in("Time,Ticker,Order,T,Shares,Price\n"
                          "1,AAA,1,B,100,1000\n"
                          "2,AAA,9,D,0,0\n"
                          "3,AAA,2,B,100,1010\n");
    std::ostringstream out;
    try {
        order_book::replay(in, out);
        FAIL() << "replay accepted an unknown order";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "line 3: unknown order '9'");
    }
    EXPECT_EQ(out.str(), "1,AAA,1000,100,,\n");
}
