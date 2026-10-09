#include <order_book/event.hpp>
#include <order_book/order.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace order_book {

namespace {

void print(std::ostream& os, uint64_t time, const std::string& ticker, PQ best_ask, PQ best_bid)
{
    os << time << ',' << ticker << ',';
    if (best_bid.price != 0) {
        os << best_bid;
    }
    else {
        os << ',';
    }
    os << ',';
    if (best_ask.price != 0) {
        os << best_ask;
    }
    else {
        os << ',';
    }
    os << '\n';
}

} // namespace

std::size_t replay(std::istream& in, std::ostream& out)
{
    // Order books for each symbol
    std::unordered_map<std::string, OrderBook> books;

    std::string line;
    std::size_t number = 1;
    std::size_t events = 0;
    // Skip the header
    getline(in, line);
    while (getline(in, line)) {
        ++number;
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        // Skip empty strings
        if (line.find_last_not_of(" \t\n\v\f\r") == std::string::npos) {
            continue;
        }

        try {
            // Parse event message
            const Event event(line);
            // Get the order book for this symbol
            auto& book = books[event.ticker];

            switch (event.type) {
            case Event::Type::Buy:
                book.buy(event.order, event.price, event.shares);
                break;
            case Event::Type::Sell:
                book.sell(event.order, event.price, event.shares);
                break;
            case Event::Type::Decrease:
                book.decrease(event.order, event.shares);
                break;
            case Event::Type::Delete:
                book.remove(event.order);
                break;
            case Event::Type::Execute:
                book.execute(event.order, event.shares);
                break;
            case Event::Type::Fill:
                book.fill(event.order);
                break;
            default:
                // Trades and cross-trades don't affect the order book
                break;
            }

            if (book.changed()) {
                const auto ask_bid = book.best_ask_bid();
                print(out, event.time, event.ticker, ask_bid.first, ask_bid.second);
            }
            ++events;
        }
        catch (const std::invalid_argument& error) {
            throw std::invalid_argument("line " + std::to_string(number) + ": " + error.what());
        }
    }
    return events;
}

} // namespace order_book
