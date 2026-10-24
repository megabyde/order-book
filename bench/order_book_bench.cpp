#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/itch_market.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using order_book::AddOrder;
using order_book::DeleteOrder;
using order_book::ExecuteOrder;
using order_book::OrderBook;
using order_book::Side;

constexpr order_book::Price mid_price = 100000;
constexpr order_book::Price tick = 100;
constexpr order_book::Quantity lot = 100;

// Discards everything written to it, so BM_Replay still pays for formatting the output
class NullBuffer : public std::streambuf {
protected:
    int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
    std::streamsize xsputn(const char* /*s*/, std::streamsize count) override { return count; }
};

std::vector<std::string> order_ids(std::size_t count)
{
    std::vector<std::string> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(std::to_string(i));
    }
    return ids;
}

void BM_ParseLine(benchmark::State& state)
{
    const std::string line = "57240167,PRU,9871234,B,100,1172400,,Q";
    for (const auto _ : state) {
        benchmark::DoNotOptimize(order_book::parse_line(line));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseLine);

// Adds range(0) orders alternating sides over 16 price levels per side, then deletes them newest
// first
void BM_AddDelete(benchmark::State& state)
{
    constexpr std::size_t levels = 16;
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto ids = order_ids(count);
    OrderBook book;
    for (const auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = static_cast<order_book::Price>((i / 2 % levels) + 1) * tick;
            const auto add =
                i % 2 == 0
                    ? AddOrder{.side = Side::Buy, .price = mid_price - offset, .shares = lot}
                    : AddOrder{.side = Side::Sell, .price = mid_price + offset, .shares = lot};
            benchmark::DoNotOptimize(book.apply(ids[i], add));
        }
        for (const auto& id : std::views::reverse(ids)) {
            benchmark::DoNotOptimize(book.apply(id, DeleteOrder{}));
        }
    }
    state.SetItemsProcessed(state.iterations() * 2 * state.range(0));
}
BENCHMARK(BM_AddDelete)->Range(64, 4096);

// Rests range(0) buy orders at one price, then executes each in the order added in two halves, so
// every event changes the best bid's quantity
void BM_ExecuteFront(benchmark::State& state)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto ids = order_ids(count);
    OrderBook book;
    for (const auto _ : state) {
        for (const auto& id : ids) {
            benchmark::DoNotOptimize(
                book.apply(id, AddOrder{.side = Side::Buy, .price = mid_price, .shares = lot}));
        }
        for (const auto& id : ids) {
            benchmark::DoNotOptimize(book.apply(id, ExecuteOrder{lot / 2}));
            benchmark::DoNotOptimize(book.apply(id, ExecuteOrder{lot / 2}));
        }
    }
    state.SetItemsProcessed(state.iterations() * 3 * state.range(0));
}
BENCHMARK(BM_ExecuteFront)->Range(64, 4096);

// The whole file at `path`, or nothing after skipping `state` with the reason. Read in one call
// into a string of the file's size, since a real day is several GB and a growing buffer would
// briefly hold it twice.
std::optional<std::string> read_feed(benchmark::State& state, const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        state.SkipWithError("cannot open '" + path + "'");
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(file.tellg()), '\0');
    file.seekg(0);
    if (!file.read(contents.data(), static_cast<std::streamsize>(contents.size()))) {
        state.SkipWithError("cannot read '" + path + "'");
        return std::nullopt;
    }
    return contents;
}

// Replays the feed at `path` in `format` with range(0) worker threads through the entry point the
// application uses
void BM_Replay(benchmark::State& state, const std::string& path, order_book::Format format)
{
    const auto threads = static_cast<std::size_t>(state.range(0));
    auto feed = read_feed(state, path);
    if (!feed) {
        return;
    }
    const auto bytes = static_cast<int64_t>(feed->size());

    // The stream takes the feed rather than copying it, which for a real day saves several GB
    std::istringstream in(std::move(*feed));
    NullBuffer sink;
    std::ostream out(&sink);
    // An untimed first pass rejects a malformed feed and counts its events
    std::size_t events = 0;
    try {
        events = order_book::replay(in, out, format, threads);
    }
    catch (const std::invalid_argument& error) {
        state.SkipWithError(error.what());
        return;
    }
    for (const auto _ : state) {
        in.clear();
        in.seekg(0);
        order_book::replay(in, out, format, threads);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(events));
    state.SetBytesProcessed(state.iterations() * bytes);
}

// Offset of the ITCH message after the one whose 2-byte length is at `offset` in `feed`
std::size_t next_message(std::string_view feed, std::size_t offset)
{
    constexpr unsigned byte_bits = std::numeric_limits<unsigned char>::digits;
    const std::size_t high = static_cast<unsigned char>(feed[offset]);
    const std::size_t low = static_cast<unsigned char>(feed[offset + 1]);
    return offset + 2 + ((high << byte_bits) | low);
}

std::string_view message_at(std::string_view feed, std::size_t offset)
{
    return feed.substr(offset + 2, next_message(feed, offset) - offset - 2);
}

// Applies each message as replay()'s workers do: visit_itch() calls the ItchMarket method for its
// kind, and a change of the BBO reads it, but neither formats a line nor tracks the stock symbol.
// Records why a message fails.
class EngineHandler {
public:
    explicit EngineHandler(order_book::ItchMarket& market) : m_market(market) {}

    bool add(const order_book::ItchMessage& message)
    {
        return applied(message, m_market.add(message));
    }
    bool execute(const order_book::ItchMessage& message)
    {
        return applied(message, m_market.execute(message));
    }
    bool remove(const order_book::ItchMessage& message)
    {
        return applied(message, m_market.remove(message));
    }
    bool replace(const order_book::ItchMessage& message)
    {
        return applied(message, m_market.replace(message));
    }
    static bool trade(const order_book::ItchMessage& /*message*/) { return true; }
    static bool skipped() { return true; }
    bool error(const order_book::ItchError& error)
    {
        m_failure = describe(error);
        return false;
    }

    [[nodiscard]] const std::string& failure() const { return m_failure; }

private:
    bool applied(const order_book::ItchMessage& message,
                 const std::expected<bool, order_book::BookError>& result)
    {
        if (!result) {
            const auto duplicate = result.error() == order_book::BookError::DuplicateOrder;
            m_failure = std::format("{} order", duplicate ? "duplicate" : "unknown");
            return false;
        }
        if (*result) {
            auto bbo = m_market.bbo(message.locate);
            benchmark::DoNotOptimize(bbo);
        }
        return true;
    }

    order_book::ItchMarket& m_market;
    std::string m_failure;
};

// Apply every message of `feed` to `market` on this thread through EngineHandler, starting the
// loads of later messages as replay()'s workers do. Returns why the first message that fails does,
// if one does.
std::optional<std::string> apply_itch(std::string_view feed, order_book::ItchMarket& market)
{
    constexpr auto distance = order_book::ItchMarket::prefetch_distance;
    std::size_t ahead = 0;
    std::size_t half = 0;
    for (std::size_t skipped = 0; skipped < distance && ahead < feed.size(); ++skipped) {
        ahead = next_message(feed, ahead);
        half = skipped < distance / 2 ? next_message(feed, half) : half;
    }
    EngineHandler handler(market);
    std::size_t number = 1;
    for (std::size_t offset = 0; offset < feed.size(); offset = next_message(feed, offset)) {
        if (ahead < feed.size()) {
            const auto peeked = order_book::peek_itch(message_at(feed, ahead));
            market.prefetch(peeked.order, peeked.locate);
            ahead = next_message(feed, ahead);
        }
        if constexpr (order_book::ItchMarket::prefetch_levels_enabled) {
            if (half < feed.size()) {
                const auto peeked = order_book::peek_itch(message_at(feed, half));
                market.prefetch_levels(peeked.order, peeked.locate);
                half = next_message(feed, half);
            }
        }
        if (!order_book::visit_itch(message_at(feed, offset), handler)) {
            return std::format("message {}: {}", number, handler.failure());
        }
        ++number;
    }
    return std::nullopt;
}

// Decodes and applies the ITCH feed at `path` to one ItchMarket on one thread, from memory and
// without formatting output: the region that per-core comparisons with other ITCH books time. Each
// iteration starts from an empty market, made and destroyed outside the timing.
void BM_ItchEngine(benchmark::State& state, const std::string& path)
{
    const auto feed = read_feed(state, path);
    if (!feed) {
        return;
    }
    // An untimed first pass rejects a truncated or malformed feed and counts its messages
    std::size_t messages = 0;
    for (std::size_t offset = 0; offset < feed->size(); offset = next_message(*feed, offset)) {
        if (offset + 2 > feed->size() || next_message(*feed, offset) > feed->size()) {
            state.SkipWithError(std::format("message {}: truncated", messages + 1));
            return;
        }
        ++messages;
    }
    if (const auto error = apply_itch(*feed, *std::make_unique<order_book::ItchMarket>())) {
        state.SkipWithError(*error);
        return;
    }
    for (const auto _ : state) {
        state.PauseTiming();
        auto market = std::make_unique<order_book::ItchMarket>();
        state.ResumeTiming();
        benchmark::DoNotOptimize(apply_itch(*feed, *market));
        state.PauseTiming();
        market.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(messages));
    state.counters["per_message"] = benchmark::Counter(
        static_cast<double>(messages),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

constexpr int max_replay_threads = 8;

#ifdef _WIN32
constexpr char list_separator = ';';
#else
constexpr char list_separator = ':';
#endif

void register_feeds(std::string_view feeds, order_book::Format format);

// Registers BM_Replay/<path>/threads:N for each CSV feed in ORDER_BOOK_REPLAY_CSV and each ITCH
// feed in ORDER_BOOK_REPLAY_ITCH, lists separated like PATH, and BM_ItchEngine/<path> for each ITCH
// feed, so that one process and one --benchmark_out file cover every feed
void register_replays()
{
    const char* const csv = std::getenv("ORDER_BOOK_REPLAY_CSV");
    const char* const itch = std::getenv("ORDER_BOOK_REPLAY_ITCH");
    if (csv == nullptr && itch == nullptr) {
        benchmark::RegisterBenchmark("BM_Replay", [](benchmark::State& state) {
            state.SkipWithMessage(
                "neither ORDER_BOOK_REPLAY_CSV nor ORDER_BOOK_REPLAY_ITCH is set");
        });
        return;
    }
    for (const auto& [feeds, format] :
         {std::pair{csv, order_book::Format::Csv}, std::pair{itch, order_book::Format::Itch}}) {
        if (feeds != nullptr) {
            register_feeds(feeds, format);
        }
    }
}

void register_feeds(std::string_view feeds, order_book::Format format)
{
    for (const auto feed : feeds | std::views::split(list_separator)) {
        const std::string path(feed.begin(), feed.end());
        benchmark::RegisterBenchmark("BM_Replay/" + path, BM_Replay, path, format)
            ->ArgName("threads")
            ->RangeMultiplier(2)
            ->Range(1, max_replay_threads)
            ->MeasureProcessCPUTime()
            ->UseRealTime()
            ->Unit(benchmark::kMillisecond);
        if (format == order_book::Format::Itch) {
            benchmark::RegisterBenchmark("BM_ItchEngine/" + path, BM_ItchEngine, path)
                ->Unit(benchmark::kMillisecond);
        }
    }
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    register_replays();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
