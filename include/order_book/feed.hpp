#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>

namespace order_book {

/// Price in 100ths of a penny
using Price = std::uint32_t;
/// Number of shares; 64-bit so that a level total cannot overflow
using Quantity = std::uint64_t;

/// Side of the book an order rests on
enum class Side : std::uint8_t {
    Buy,  ///< Bid
    Sell, ///< Ask
};

/// B or S: add a resting order
struct AddOrder {
    Side side;       ///< Side the order rests on
    Price price;     ///< Limit price
    Quantity shares; ///< Initial quantity
    /// Memberwise equality
    friend bool operator==(const AddOrder&, const AddOrder&) = default;
};

/// C: decrease the order *to* `shares`; no effect unless smaller, removes the order at 0
struct DecreaseOrder {
    Quantity shares; ///< New quantity
    /// Memberwise equality
    friend bool operator==(const DecreaseOrder&, const DecreaseOrder&) = default;
};

/// D: delete the order
struct DeleteOrder {
    /// Every delete is equal
    friend bool operator==(const DeleteOrder&, const DeleteOrder&) = default;
};

/// E: execute `shares` of the order, clamped to what remains; removes the order at 0
struct ExecuteOrder {
    Quantity shares; ///< Executed quantity
    /// Memberwise equality
    friend bool operator==(const ExecuteOrder&, const ExecuteOrder&) = default;
};

/// F: fill, and so remove, the order
struct FillOrder {
    /// Every fill is equal
    friend bool operator==(const FillOrder&, const FillOrder&) = default;
};

/// T or X: a trade that does not touch the book
struct NoOp {
    /// Every no-op is equal
    friend bool operator==(const NoOp&, const NoOp&) = default;
};

/// What a message does to the order it names
using Event = std::variant<AddOrder, DecreaseOrder, DeleteOrder, ExecuteOrder, FillOrder, NoOp>;

/// One feed line. `ticker` and `order` view the parsed line and dangle once it goes away.
struct Message {
    std::uint64_t time;      ///< Milliseconds since midnight
    std::string_view ticker; ///< Stock symbol
    std::string_view order;  ///< Order ID, unique among live orders
    Event event;             ///< Effect on the order
};

/// Parse `Time,Ticker,Order,T,Shares,Price`, then any extra columns
///
/// Fails with a message naming the offending field if the line has fewer than six fields, a field
/// is not an unsigned decimal integer in range, or the type is not one of BSCDEFTX.
[[nodiscard]] std::expected<Message, std::string> parse_line(std::string_view line);

} // namespace order_book
