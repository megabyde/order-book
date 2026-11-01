// Metamorphic tests: a replay of a transformed feed must give the original's output, or its output
// transformed the same way, so they need no model of what the output should be

#include <gtest/gtest.h>

#include <order_book/replay.hpp>

#include "itch_messages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <ios>
#include <istream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTBEGIN(readability-magic-numbers)

namespace {

using order_book::Format;

// Serves bulk reads in seeded sizes from 1 byte to past a batch, as pipes and sockets may
class ChunkedBuffer : public std::stringbuf {
public:
    ChunkedBuffer(std::string text, unsigned seed) : std::stringbuf(std::move(text)), m_random(seed)
    {
    }

protected:
    std::streamsize xsgetn(char* s, std::streamsize count) override
    {
        static constexpr std::array<std::streamsize, 6> limits{
            1, 7, 4096, 65536, 1'048'576, 3'145'728,
        };
        const auto limit = limits.at(m_random() % limits.size());
        return std::stringbuf::xsgetn(
            s, std::min(count, 1 + static_cast<std::streamsize>(m_random() % limit)));
    }

private:
    std::mt19937 m_random;
};

// What replay() writes for the feed in `buffer`, with a marker after it if a record fails
std::string replay(std::streambuf& buffer, Format format, std::size_t threads)
{
    std::istream in(&buffer);
    std::ostringstream out;
    try {
        order_book::replay(in, out, format, threads);
    }
    catch (const std::invalid_argument& error) {
        return out.str() + "<failed: " + error.what() + ">";
    }
    return out.str();
}

std::string replay(const std::string& feed, Format format, std::size_t threads)
{
    std::stringbuf buffer(feed);
    return replay(buffer, format, threads);
}

// A seeded CSV feed of every event type over 30 tickers, kept as events so that it can be written
// out with its IDs, tickers, line endings, and columns transformed. Every event applies.
class CsvFeed {
public:
    struct Event {
        std::uint64_t time;
        std::string ticker;
        std::string order;
        char type;
        unsigned shares;
        unsigned price;
    };

    explicit CsvFeed(int events)
    {
        std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed)
        // Each ticker's live orders and their remaining shares
        std::map<std::string, std::map<std::string, unsigned>> live;
        for (int i = 0; i < events; ++i) {
            const auto time = static_cast<std::uint64_t>(i);
            const auto ticker = std::format("T{}", random() % 30);
            auto& orders = live[ticker];
            const auto roll = random() % 10;
            if (orders.empty() || roll < 5) {
                auto order = std::format("{}", m_events.size());
                const auto shares = static_cast<unsigned>(1 + (random() % 100));
                m_events.push_back({
                    .time = time,
                    .ticker = ticker,
                    .order = order,
                    .type = random() % 2 == 0 ? 'B' : 'S',
                    .shares = shares,
                    .price = static_cast<unsigned>(1000 + (random() % 20 * 10)),
                });
                orders.emplace(std::move(order), shares);
                continue;
            }
            const auto found =
                std::next(orders.begin(), static_cast<std::ptrdiff_t>(random() % orders.size()));
            auto& [order, remaining] = *found;
            constexpr std::string_view types = "CEFDTX";
            const auto type = types[std::min<std::size_t>(roll - 5, types.size() - 1)];
            const auto shares = static_cast<unsigned>(random() % (remaining + 20));
            m_events.push_back({
                .time = time,
                .ticker = ticker,
                .order = type == 'T' || type == 'X' ? "0" : order,
                .type = type,
                .shares = shares,
                .price = 1000,
            });
            if (type == 'C') {
                remaining = std::min(remaining, shares);
            }
            else if (type == 'E') {
                remaining -= std::min(remaining, shares);
            }
            else if (type == 'F' || type == 'D') {
                remaining = 0;
            }
            if (remaining == 0) {
                orders.erase(found);
            }
        }
    }

    // The feed with each order ID and ticker mapped, CRLF line endings if `crlf`, and two more
    // columns if `extra`
    [[nodiscard]] std::string write(const std::function<std::string(const std::string&)>& order,
                                    const std::function<std::string(const std::string&)>& ticker,
                                    bool crlf = false, bool extra = false) const
    {
        const std::string_view end = crlf ? "\r\n" : "\n";
        std::string feed =
            std::format("Time,Ticker,Order,T,Shares,Price{}{}", extra ? ",MPID,X" : "", end);
        for (const auto& event : m_events) {
            feed += std::format("{},{},{},{},{},{}{}{}", event.time, ticker(event.ticker),
                                order(event.order), event.type, event.shares, event.price,
                                extra ? ",ABCD,Q" : "", end);
        }
        return feed;
    }

private:
    std::vector<Event> m_events;
};

const auto same = [](const std::string& text) { return text; };

// Whether `output` came from a replay that applied every record and wrote at least `lines` lines,
// so that a comparison with it covers the whole feed
testing::AssertionResult complete(const std::string& output, std::ptrdiff_t lines)
{
    if (const auto failed = output.find("<failed"); failed != std::string::npos) {
        return testing::AssertionFailure() << output.substr(failed);
    }
    if (const auto written = std::ranges::count(output, '\n'); written < lines) {
        return testing::AssertionFailure() << written << " lines, expected " << lines;
    }
    return testing::AssertionSuccess();
}

TEST(MetamorphicTest, CsvOutputDoesNotDependOnHowReadsAreCut)
{
    const auto feed = CsvFeed(60'000).write(same, same);
    const auto expected = replay(feed, Format::Csv, 1);
    ASSERT_TRUE(complete(expected, 3'000));
    for (const unsigned seed : {1U, 2U, 3U}) {
        for (const std::size_t threads : {1, 3}) {
            ChunkedBuffer buffer(feed, seed);
            EXPECT_TRUE(replay(buffer, Format::Csv, threads) == expected)
                << "seed " << seed << ", threads " << threads;
        }
    }
}

TEST(MetamorphicTest, CsvOutputDoesNotDependOnOrderIdsLineEndingsOrExtraColumns)
{
    const CsvFeed feed(60'000);
    const auto expected = replay(feed.write(same, same), Format::Csv, 3);
    ASSERT_TRUE(complete(expected, 3'000));
    // A bijection on IDs that changes their length and every character position
    const auto renamed = [](const std::string& order) {
        return "id-" + std::string(order.rbegin(), order.rend()) + "-" + order;
    };
    EXPECT_TRUE(replay(feed.write(renamed, same), Format::Csv, 3) == expected);
    EXPECT_TRUE(replay(feed.write(same, same, true, true), Format::Csv, 3) == expected);
}

TEST(MetamorphicTest, CsvOutputFollowsRenamedTickers)
{
    const CsvFeed feed(60'000);
    const auto rename = [](const std::string& ticker) { return "SYM" + ticker + "X"; };
    const auto expected = replay(feed.write(same, same), Format::Csv, 3);
    ASSERT_TRUE(complete(expected, 3'000));
    // Each output line starts with the time and the ticker
    std::string mapped;
    std::istringstream lines(expected);
    for (std::string line; std::getline(lines, line);) {
        const auto first = line.find(',');
        const auto second = line.find(',', first + 1);
        mapped += first == std::string::npos
                      ? line
                      : line.substr(0, first + 1) +
                            rename(line.substr(first + 1, second - first - 1)) +
                            line.substr(second);
        mapped += '\n';
    }
    if (!expected.ends_with('\n')) {
        mapped.pop_back();
    }
    EXPECT_TRUE(replay(feed.write(same, rename), Format::Csv, 3) == mapped);
}

// A seeded native ITCH feed over a dozen locates, kept as message fields so that it can be written
// out with its reference numbers and locates mapped. Every message applies, and a third of the new
// orders take a reference number whose order is gone.
class ItchFeed {
public:
    struct Message {
        char type;
        std::uint16_t locate;
        std::uint64_t order;
        std::uint64_t replacement = 0;
        char side = 'B';
        std::uint32_t shares = 0;
        std::uint32_t price = 0;
    };

    explicit ItchFeed(int messages)
    {
        std::mt19937 random(9); // NOLINT(bugprone-random-generator-seed)
        // Each locate's live orders and their remaining shares, and reference numbers whose orders
        // are gone, which a new order may take again
        std::map<std::uint16_t, std::map<std::uint64_t, std::uint32_t>> live;
        std::map<std::uint16_t, std::vector<std::uint64_t>> dead;
        std::uint64_t next = 1;
        const auto fresh = [&](std::uint16_t locate) {
            auto& free = dead[locate];
            if (free.empty() || random() % 3 != 0) {
                return next++;
            }
            const auto index = random() % free.size();
            const auto order = free[index];
            free.erase(free.begin() + static_cast<std::ptrdiff_t>(index));
            return order;
        };
        for (int i = 0; i < messages; ++i) {
            const auto locate = static_cast<std::uint16_t>(1 + (random() % 12));
            auto& orders = live[locate];
            const auto roll = random() % 10;
            const auto shares = static_cast<std::uint32_t>(1 + (random() % 100));
            const auto price = static_cast<std::uint32_t>(1000 + (random() % 10 * 10));
            if (orders.empty() || roll < 4) {
                const auto order = fresh(locate);
                m_messages.push_back({
                    .type = 'A',
                    .locate = locate,
                    .order = order,
                    .side = random() % 2 == 0 ? 'B' : 'S',
                    .shares = shares,
                    .price = price,
                });
                orders.emplace(order, shares);
                continue;
            }
            const auto found =
                std::next(orders.begin(), static_cast<std::ptrdiff_t>(random() % orders.size()));
            const auto [order, remaining] = *found;
            orders.erase(found);
            if (roll < 6) {
                const auto taken = static_cast<std::uint32_t>(random() % (remaining + 20));
                m_messages.push_back(
                    {.type = 'E', .locate = locate, .order = order, .shares = taken});
                if (taken < remaining) {
                    orders.emplace(order, remaining - taken);
                }
                else {
                    dead[locate].push_back(order);
                }
            }
            else if (roll < 8) {
                m_messages.push_back({.type = 'D', .locate = locate, .order = order});
                dead[locate].push_back(order);
            }
            else {
                const auto replacement = fresh(locate);
                m_messages.push_back({
                    .type = 'U',
                    .locate = locate,
                    .order = order,
                    .replacement = replacement,
                    .shares = shares,
                    .price = price,
                });
                orders.emplace(replacement, shares);
                dead[locate].push_back(order);
            }
        }
    }

    // The feed with each reference number and locate mapped
    [[nodiscard]] std::string write(const std::function<std::uint64_t(std::uint64_t)>& reference,
                                    const std::function<std::uint16_t(std::uint16_t)>& locate) const
    {
        std::string feed;
        std::uint64_t time = 0;
        for (const auto& message : m_messages) {
            ++time;
            const auto at = locate(message.locate);
            const auto symbol = std::format("S{}", message.locate);
            switch (message.type) {
            case 'A':
                feed += itch::framed(itch::add(at, time, reference(message.order), message.side,
                                               message.shares, symbol, message.price));
                break;
            case 'E':
                feed += itch::framed(
                    itch::executed(at, time, reference(message.order), message.shares));
                break;
            case 'D':
                feed += itch::framed(itch::remove(at, time, reference(message.order)));
                break;
            default:
                feed += itch::framed(itch::replace(at, time, reference(message.order),
                                                   reference(message.replacement), message.shares,
                                                   message.price));
            }
        }
        return feed;
    }

private:
    std::vector<Message> m_messages;
};

const auto same_reference = [](std::uint64_t order) { return order; };
const auto same_locate = [](std::uint16_t locate) { return locate; };

TEST(MetamorphicTest, ItchOutputDoesNotDependOnHowReadsAreCut)
{
    const auto feed = ItchFeed(60'000).write(same_reference, same_locate);
    const auto expected = replay(feed, Format::Itch, 1);
    ASSERT_TRUE(complete(expected, 7'000));
    for (const unsigned seed : {1U, 2U, 3U}) {
        for (const std::size_t threads : {1, 3}) {
            ChunkedBuffer buffer(feed, seed);
            EXPECT_TRUE(replay(buffer, Format::Itch, threads) == expected)
                << "seed " << seed << ", threads " << threads;
        }
    }
}

TEST(MetamorphicTest, ItchOutputDoesNotDependOnReferenceNumbersOrLocates)
{
    const ItchFeed feed(60'000);
    const auto expected = replay(feed.write(same_reference, same_locate), Format::Itch, 3);
    ASSERT_TRUE(complete(expected, 7'000));
    // Every order moves from the paged index to the hash map beyond it, and back for the largest
    const auto beyond = [](std::uint64_t order) { return order ^ (std::uint64_t{1} << 40U); };
    const auto top = [](std::uint64_t order) { return ~order; };
    // Locates reversed and spread out, so they hash to other workers
    const auto moved = [](std::uint16_t locate) {
        return static_cast<std::uint16_t>(5000 - (locate * 97));
    };
    EXPECT_TRUE(replay(feed.write(beyond, same_locate), Format::Itch, 3) == expected);
    EXPECT_TRUE(replay(feed.write(top, same_locate), Format::Itch, 3) == expected);
    EXPECT_TRUE(replay(feed.write(same_reference, moved), Format::Itch, 3) == expected);
}

} // namespace

// NOLINTEND(readability-magic-numbers)
