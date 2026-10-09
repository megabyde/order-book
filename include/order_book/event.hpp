#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <order_book/utils.hpp>

namespace order_book {

/**
 * Class parsing a market event from an equity exchange.
 */
struct Event {
    /// Message type, encoded as the feed's type letter
    enum struct Type : char {
        Buy = 'B',        ///< Buy order
        Sell = 'S',       ///< Sell order
        Decrease = 'C',   ///< Decrease the number of shares
        Delete = 'D',     ///< Delete the order
        Execute = 'E',    ///< Execute shares
        Fill = 'F',       ///< Fill the order completely
        Trade = 'T',      ///< Trade has occurred
        CrossTrade = 'X', ///< Cross trade has occurred
    };

    Event() = delete;
    /// Parse one feed line: `Time,Ticker,Order,T,Shares,Price`, then any extra columns
    ///
    /// Throws std::invalid_argument naming the offending field if the line has fewer than six
    /// fields, a field is not an unsigned decimal integer in range, or the type is not one of
    /// BSCDEFTX.
    explicit Event(const std::string& s)
    {
        const auto fields = split(s);
        if (fields.size() < 6) { // NOLINT(readability-magic-numbers)
            throw std::invalid_argument("expected at least 6 fields, got " +
                                        std::to_string(fields.size()));
        }

        time = parse_uint<uint64_t>(fields[0], "time");
        ticker = fields[1];
        order = fields[2];
        if (fields[3].size() != 1 || !std::string_view("BSCDEFTX").contains(fields[3][0])) {
            throw std::invalid_argument("unknown message type '" + fields[3] + "'");
        }
        type = static_cast<Type>(fields[3][0]);
        shares = parse_uint<uint32_t>(fields[4], "shares");
        price = parse_uint<uint32_t>(fields[5], "price"); // NOLINT(readability-magic-numbers)
    }

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    uint64_t time;      ///< Milliseconds since the start of the trading day
    std::string ticker; ///< Stock symbol
    std::string order;  ///< Unique per-order ID
    Type type;          ///< Message type
    uint32_t shares;    ///< Quantity of shares
    uint32_t price;     ///< Order price in 100th of a penny
    // NOLINTEND(misc-non-private-member-variables-in-classes)

private:
    template <typename T> static T parse_uint(const std::string& text, const char* name)
    {
        T value{};
        const auto* const end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value);
        if (ec == std::errc::result_out_of_range) {
            throw std::invalid_argument(std::string(name) + " out of range: '" + text + "'");
        }
        if (ec != std::errc{} || ptr != end) {
            throw std::invalid_argument(std::string(name) + " is not an unsigned integer: '" +
                                        text + "'");
        }
        return value;
    }
};

} // namespace order_book
