#include <fstream>
#include <unordered_map>

#include "order_book.h"

void
print(std::ostream& os, uint64_t time, const std::string& ticker, PQ best_ask, PQ best_bid)
{
    os << time << ',' << ticker << ',';
    if (best_bid.price != 0)
        os << best_bid;
    else
        os << ',';
    os << ',';
    if (best_ask.price != 0)
        os << best_ask;
    else
        os << ',';
    os << '\n';
}

int
main(int argc, const char* argv[])
{
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " FILE\n";
        return 0;
    }

    std::ifstream input(argv[1]);
    assert(input.is_open());

    // Order books for each symbol
    std::unordered_map<std::string, OrderBook> books;

    std::string line;
    // Skip the header
    getline(input, line);
    while (getline(input, line)) {
        // Skip empty strings
        if (line.find_last_not_of(" \t\n\v\f\r") == std::string::npos)
            continue;

        // Parse event message
        Event event(line);
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
            auto ask_bid = book.best_ask_bid();
            print(std::cout, event.time, event.ticker, ask_bid.first, ask_bid.second);
        }
    }

    return 0;
}
