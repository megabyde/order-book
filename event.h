#pragma once

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "utils.h"

/**
 * Class parsing a market event from an equity exchange.
 */
struct Event {
    enum struct Type : char {
        Buy        = 'B', // Buy order
        Sell       = 'S', // Sell order
        Decrease   = 'C', // Decrease the number of shares
        Delete     = 'D', // Delete the order
        Execute    = 'E', // Execute shares
        Fill       = 'F', // Fill the order completely
        Trade      = 'T', // Trade has occurred
        CrossTrade = 'X'  // Cross trade has occurred
    };

    Event() = delete;
    explicit Event(const std::string& s)
    {
        auto fields = split(s);
        assert(fields.size() >= 6);

        // We parse using the following format:
        // Time,Ticker,Order,T,Shares,Price,...
        time   = std::stoull(fields[0]);
        ticker = fields[1];
        order  = fields[2];
        assert(fields[3].length() == 1);
        type   = static_cast<Type>(fields[3][0]);
        shares = std::stoul(fields[4]);
        price  = std::stoul(fields[5]);
    }

    uint64_t    time;   // Milliseconds since the start of the trading day
    std::string ticker; // Stock symbol
    std::string order;  // Unique per-order ID
    Type        type;   // Message type
    uint32_t    shares; // Quantity of shares
    uint32_t    price;  // Order price in 100th of a penny
};
