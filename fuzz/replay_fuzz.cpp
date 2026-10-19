// Replays the input as a feed in ORDER_BOOK_FUZZ_FORMAT on one worker and on three, which must
// count the same events, print the same lines, and fail with the same error

#include <order_book/replay.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct Outcome {
    std::size_t events = 0;
    std::string output;
    std::string error;
    friend bool operator==(const Outcome&, const Outcome&) = default;
};

Outcome replay(std::string_view feed, std::size_t threads)
{
    std::istringstream in{std::string(feed)};
    std::ostringstream out;
    Outcome outcome;
    try {
        outcome.events =
            order_book::replay(in, out, order_book::Format::ORDER_BOOK_FUZZ_FORMAT, threads);
    }
    catch (const std::invalid_argument& error) {
        outcome.error = error.what();
    }
    outcome.output = std::move(out).str();
    return outcome;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view feed(reinterpret_cast<const char*>(data), size);
    if (replay(feed, 1) != replay(feed, 3)) {
        std::abort();
    }
    return 0;
}
