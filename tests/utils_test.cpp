/**
 * Tests for utils
 */
#include <gtest/gtest.h>

#include <order_book/utils.hpp>

#include <string>
#include <vector>

using order_book::split;

TEST(SplitTest, TestSplit)
{
    const std::vector<std::string> expected{"52930489", "aaa", "", "1222"};
    EXPECT_EQ(split("52930489,aaa,,1222"), expected);
}

TEST(SplitTest, TestEmpty)
{
    const std::vector<std::string> expected{""};
    EXPECT_EQ(split(""), expected);
}

TEST(SplitTest, TestNoDelimiter)
{
    const std::vector<std::string> expected{"no delimiter"};
    EXPECT_EQ(split("no delimiter"), expected);
}
