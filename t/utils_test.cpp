/**
 * Tests for utils
 */
#include "../utils.h"

#include <gmock/gmock.h>

TEST(SplitTest, TestSplit)
{
    std::vector<std::string> expected{"52930489", "aaa", "", "1222"};
    EXPECT_EQ(split("52930489,aaa,,1222"), expected);
}

TEST(SplitTest, TestEmpty)
{
    std::vector<std::string> expected{""};
    EXPECT_EQ(split(""), expected);
}

TEST(SplitTest, TestNoDelimiter)
{
    std::vector<std::string> expected{"no delimiter"};
    EXPECT_EQ(split("no delimiter"), expected);
}
