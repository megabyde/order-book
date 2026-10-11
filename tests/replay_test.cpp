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

// NOLINTBEGIN(readability-magic-numbers)

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

// 37 interleaved tickers, each adding an order on even steps and deleting it on odd ones, so
// every event changes its ticker's BBO
std::string multi_ticker_feed(std::size_t events)
{
    constexpr std::size_t tickers = 37;
    std::string feed = "Time,Ticker,Order,T,Shares,Price\n";
    for (std::size_t i = 0; i < events; ++i) {
        const auto step = i / tickers;
        if (step % 2 == 0) {
            feed +=
                std::format("{},T{},{},{},{},{}\n", i, i % tickers, step,
                            step / 2 % 2 == 0 ? 'B' : 'S', 100 + (step % 7), 1000 + (step % 50));
        }
        else {
            feed += std::format("{},T{},{},D,0,0\n", i, i % tickers, step - 1);
        }
    }
    return feed;
}

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
    // Each add raises the best bid, so input and output both span several 1 MiB batches and rows
    // straddle the batch boundaries
    constexpr std::size_t count = 100000;
    std::string feed = "Time,Ticker,Order,T,Shares,Price\r\n";
    std::string expected;
    for (std::size_t i = 1; i <= count; ++i) {
        feed += std::format("{0},AAA,{0},B,1,{0}\r\n", i);
        expected += std::format("{0},AAA,{0},1,,\n", i);
    }
    std::istringstream in(feed);
    std::ostringstream out;
    EXPECT_EQ(order_book::replay(in, out), count);
    EXPECT_TRUE(out.str() == expected);
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
    const std::string extra(std::size_t{5} << 19U, 'x');
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

TEST(ReplayTest, OutputDoesNotDependOnThreads)
{
    constexpr std::size_t events = 150000;
    const auto feed = multi_ticker_feed(events);
    std::istringstream sequential_in(feed);
    std::ostringstream sequential_out;
    ASSERT_EQ(order_book::replay(sequential_in, sequential_out, 1), events);
    const auto expected = sequential_out.str();
    ASSERT_EQ(std::ranges::count(expected, '\n'), events);

    for (const std::size_t threads : {0, 2, 3, 8}) {
        std::istringstream in(feed);
        std::ostringstream out;
        EXPECT_EQ(order_book::replay(in, out, threads), events) << threads << " threads";
        EXPECT_TRUE(out.str() == expected) << threads << " threads";
    }
}

TEST(ReplayTest, StopsAtFailingLineWhateverTheThreads)
{
    // Unknown orders on eight tickers two thirds into a multi-batch feed, followed by every
    // ticker's later lines. With several workers, more than one of them fails in the same batch.
    auto feed = multi_ticker_feed(150000);
    const auto cut = feed.find('\n', feed.size() * 2 / 3) + 1;
    const auto prefix = feed.substr(0, cut);
    const auto failing_line = std::ranges::count(prefix, '\n') + 1;
    std::string unknown;
    for (int ticker = 5; ticker < 13; ++ticker) {
        unknown += std::format("0,T{0},missing{0},D,0,0\n", ticker);
    }
    feed.insert(cut, unknown);

    std::istringstream prefix_in(prefix);
    std::ostringstream prefix_out;
    order_book::replay(prefix_in, prefix_out, 1);

    for (const std::size_t threads : {1, 4}) {
        std::istringstream in(feed);
        std::ostringstream out;
        try {
            order_book::replay(in, out, threads);
            ADD_FAILURE() << "replay accepted an unknown order with " << threads << " threads";
        }
        catch (const std::invalid_argument& error) {
            EXPECT_EQ(error.what(), std::format("line {}: unknown order 'missing5'", failing_line));
        }
        EXPECT_TRUE(out.str() == prefix_out.str()) << threads << " threads";
    }
}

TEST(ReplayTest, ReportsLineWithoutTickerOnceWhateverTheThreads)
{
    for (const std::size_t threads : {1, 4}) {
        std::istringstream in("Time,Ticker,Order,T,Shares,Price\n"
                              "1,AAA,1,B,100,1000\n"
                              "garbage\n"
                              "2,BBB,2,S,50,1010\n");
        std::ostringstream out;
        try {
            order_book::replay(in, out, threads);
            ADD_FAILURE() << "replay accepted a line without fields with " << threads << " threads";
        }
        catch (const std::invalid_argument& error) {
            EXPECT_STREQ(error.what(), "line 3: expected at least 6 fields, got 1");
        }
        EXPECT_EQ(out.str(), "1,AAA,1000,100,,\n") << threads << " threads";
    }
}

// NOLINTEND(readability-magic-numbers)
