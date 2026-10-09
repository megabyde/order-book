#include <order_book/feed.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <cstddef>
#include <format>
#include <functional>
#include <ios>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace order_book {

namespace {

void append_level(std::string& buffer, const std::optional<Level>& level)
{
    if (level) {
        std::format_to(std::back_inserter(buffer), "{},{}", level->price, level->quantity);
    }
    else {
        buffer += ',';
    }
}

std::string describe(BookError error, std::string_view order)
{
    return std::format("{} order '{}'",
                       error == BookError::DuplicateOrder ? "duplicate" : "unknown", order);
}

std::invalid_argument line_error(std::size_t number, std::string_view message)
{
    const auto what = std::format("line {}: {}", number, message);
    return std::invalid_argument(what);
}

} // namespace

std::size_t replay(std::istream& in, std::ostream& out)
{
    std::unordered_map<std::string, OrderBook, StringHash, std::equal_to<>> books;
    std::string line;
    std::string buffer;
    std::size_t number = 1;
    std::size_t events = 0;

    // Skip the header
    std::getline(in, line);
    while (std::getline(in, line)) {
        ++number;
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        if (line.find_last_not_of(" \t\n\v\f\r") == std::string::npos) {
            continue;
        }

        const auto message = parse_line(line);
        if (!message) {
            throw line_error(number, message.error());
        }
        auto book = books.find(message->ticker);
        if (book == books.end()) {
            book = books.emplace(std::string(message->ticker), OrderBook{}).first;
        }

        const auto before = book->second.bbo();
        if (const auto applied = book->second.apply(message->order, message->event); !applied) {
            throw line_error(number, describe(applied.error(), message->order));
        }
        if (const auto after = book->second.bbo(); after != before) {
            buffer.clear();
            std::format_to(std::back_inserter(buffer), "{},{},", message->time, message->ticker);
            append_level(buffer, after.bid);
            buffer += ',';
            append_level(buffer, after.ask);
            buffer += '\n';
            out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        }
        ++events;
    }
    return events;
}

} // namespace order_book
