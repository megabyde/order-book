#include <order_book/feed.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <ostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
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

// Replays the feed at `path` through the same entry point as the application
void BM_Replay(benchmark::State& state, const std::string& path)
{
    const std::ifstream file(path, std::ios::binary);
    if (!file) {
        state.SkipWithError("cannot open '" + path + "'");
        return;
    }
    std::stringstream contents;
    contents << file.rdbuf();
    const std::string feed = contents.str();

    std::istringstream in(feed);
    NullBuffer sink;
    std::ostream out(&sink);
    // An untimed first pass rejects a malformed feed and counts its events
    std::size_t events = 0;
    try {
        events = order_book::replay(in, out);
    }
    catch (const std::invalid_argument& error) {
        state.SkipWithError(error.what());
        return;
    }
    for (const auto _ : state) {
        in.clear();
        in.seekg(0);
        order_book::replay(in, out);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(events));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(feed.size()));
}

#ifdef _WIN32
constexpr char list_separator = ';';
#else
constexpr char list_separator = ':';
#endif

// Registers BM_Replay/<file name> for each feed in ORDER_BOOK_REPLAY_CSV, a list separated like
// PATH, so that one process and one --benchmark_out file cover every feed
void register_replays()
{
    const char* const feeds = std::getenv("ORDER_BOOK_REPLAY_CSV");
    if (feeds == nullptr) {
        benchmark::RegisterBenchmark("BM_Replay", [](benchmark::State& state) {
            state.SkipWithMessage("ORDER_BOOK_REPLAY_CSV is not set");
        });
        return;
    }
    for (const auto feed : std::string_view(feeds) | std::views::split(list_separator)) {
        const std::string path(feed.begin(), feed.end());
        const auto name = "BM_Replay/" + std::filesystem::path(path).filename().string();
        benchmark::RegisterBenchmark(name, BM_Replay, path)->Unit(benchmark::kMillisecond);
    }
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
    register_replays();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
