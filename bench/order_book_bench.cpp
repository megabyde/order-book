#include <order_book/event.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <ostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

namespace {

using order_book::Event;
using order_book::OrderBook;

constexpr uint32_t mid_price = 100000;
constexpr uint32_t tick = 100;
constexpr uint64_t lot = 100;

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
        benchmark::DoNotOptimize(Event(line));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseLine);

// Adds range(0) orders alternating sides over 16 price levels per side, then deletes them newest
// first, which is the far end of each level's FIFO
void BM_AddDelete(benchmark::State& state)
{
    constexpr std::size_t levels = 16;
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto ids = order_ids(count);
    OrderBook book;
    for (const auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = static_cast<uint32_t>((i / 2 % levels) + 1) * tick;
            if (i % 2 == 0) {
                book.buy(ids[i], mid_price - offset, lot);
            }
            else {
                book.sell(ids[i], mid_price + offset, lot);
            }
        }
        for (const auto& id : std::views::reverse(ids)) {
            book.remove(id);
        }
    }
    state.SetItemsProcessed(state.iterations() * 2 * state.range(0));
}
BENCHMARK(BM_AddDelete)->Range(64, 4096);

// Rests range(0) buy orders at one price, then executes each in FIFO order in two halves, so
// every event changes the best bid's quantity
void BM_ExecuteFront(benchmark::State& state)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto ids = order_ids(count);
    OrderBook book;
    for (const auto _ : state) {
        for (const auto& id : ids) {
            book.buy(id, mid_price, lot);
        }
        for (const auto& id : ids) {
            book.execute(id, lot / 2);
            book.execute(id, lot / 2);
        }
    }
    state.SetItemsProcessed(state.iterations() * 3 * state.range(0));
}
BENCHMARK(BM_ExecuteFront)->Range(64, 4096);

// Replays the feed named by ORDER_BOOK_REPLAY_CSV through the same entry point as the application
void BM_Replay(benchmark::State& state)
{
    const char* const path = std::getenv("ORDER_BOOK_REPLAY_CSV");
    if (path == nullptr) {
        state.SkipWithMessage("ORDER_BOOK_REPLAY_CSV is not set");
        return;
    }
    const std::ifstream file(path, std::ios::binary);
    if (!file) {
        state.SkipWithError(std::string("cannot open '") + path + "'");
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
BENCHMARK(BM_Replay)->Unit(benchmark::kMillisecond);

} // namespace
