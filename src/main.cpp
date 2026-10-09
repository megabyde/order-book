#include <order_book/event.hpp>
#include <order_book/order.hpp>
#include <order_book/order_book.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

using order_book::Event;
using order_book::OrderBook;
using order_book::PQ;

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

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, const char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " FILE\n";
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "error: cannot open '" << argv[1] << "'\n";
        return 1;
    }

    // Order books for each symbol
    std::unordered_map<std::string, OrderBook> books;

    std::string line;
    std::size_t number = 1;
    // Skip the header
    getline(input, line);
    while (getline(input, line)) {
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
                print(std::cout, event.time, event.ticker, ask_bid.first, ask_bid.second);
            }
        }
        catch (const std::invalid_argument& error) {
            std::cerr << "error: line " << number << ": " << error.what() << '\n';
            return 1;
        }
    }

    return 0;
}
