#include <gtest/gtest.h>

#include <order_book/replay.hpp>

#include <sstream>

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
