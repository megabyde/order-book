#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>

#include <order_book/feed.hpp>

namespace order_book {

/// U: replace an order by a new one on the same side
struct ReplaceOrder {
    std::uint64_t order; ///< Reference number of the new order
    Price price;         ///< New limit price
    Quantity shares;     ///< New quantity
    /// Memberwise equality
    friend bool operator==(const ReplaceOrder&, const ReplaceOrder&) = default;
};

/// A message type the book does not use, such as a system event or a stock directory entry
struct Skipped {
    /// Every skipped message is equal
    friend bool operator==(const Skipped&, const Skipped&) = default;
};

/// What an ITCH message does to the order it names
using ItchEvent = std::variant<AddOrder, DeleteOrder, ExecuteOrder, ReplaceOrder, NoOp, Skipped>;

/// One Nasdaq TotalView-ITCH 5.0 message. `stock` views the message and dangles once it goes away.
struct ItchMessage {
    std::uint64_t time;     ///< Nanoseconds since midnight
    std::uint16_t locate;   ///< Stock locate code, the day's index of the security
    std::string_view stock; ///< Stock symbol without its padding; set for adds only
    std::uint64_t order;    ///< Order reference number; 0 when the message names no order
    ItchEvent event;        ///< Effect on the order
};

/// Decode one ITCH 5.0 message, without its 2-byte length prefix
///
/// A and F become AddOrder; E, C, and X become ExecuteOrder, since each takes shares off the order;
/// D becomes DeleteOrder and U ReplaceOrder; P and Q, trades that do not touch the book, become
/// NoOp; the specification's other types become Skipped. Fails with a message naming the problem
/// on an empty message, an unknown type, a size other than the specification's for a type the book
/// uses, or a side other than B or S.
[[nodiscard]] std::expected<ItchMessage, std::string> decode_itch(std::string_view message);

} // namespace order_book
