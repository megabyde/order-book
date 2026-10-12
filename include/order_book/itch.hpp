#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <order_book/feed.hpp>

namespace order_book {

/// Order reference numbers must be below this. Nasdaq assigns them in sequence from the start of
/// each day, so a day's stay far below it, and the cap bounds the replay's reference index.
inline constexpr std::uint64_t max_itch_order = std::uint64_t{1} << 32U;

/// What an ITCH message does
enum class ItchKind : std::uint8_t {
    Add,     ///< A or F: add an order
    Execute, ///< E, C, or X: take `shares` off the order, clamped to what remains
    Delete,  ///< D: delete the order
    Replace, ///< U: replace the order by `replacement` on the same side
    Trade,   ///< P or Q: a trade that does not touch the book
    Skipped, ///< Any other type of the specification, such as a system event or stock directory
};

/// One Nasdaq TotalView-ITCH 5.0 message. `stock` views the message and dangles once it goes away;
/// fields a kind does not use are 0.
struct ItchMessage {
    std::uint64_t time;        ///< Nanoseconds since midnight
    std::uint64_t order;       ///< Order reference number
    std::uint64_t replacement; ///< Reference number of the new order, for Replace
    std::string_view stock;    ///< Stock symbol, right-padded with spaces to 8 bytes, for Add
    Quantity shares;           ///< Shares added, taken off, or of the replacement
    Price price;               ///< Limit price of an add or a replacement
    std::uint16_t locate;      ///< Stock locate code, the day's index of the security
    ItchKind kind;             ///< What the message does
    Side side;                 ///< Side of an add
    /// Memberwise equality
    friend bool operator==(const ItchMessage&, const ItchMessage&) = default;
};

/// Why decode_itch() rejected a message
struct ItchError {
    /// The rule the message broke
    enum class Reason : std::uint8_t {
        Empty,           ///< No bytes at all
        UnknownType,     ///< A type byte outside the specification
        WrongSize,       ///< A size other than the specification's for its type
        InvalidSide,     ///< A side other than B or S
        OrderOutOfRange, ///< An order reference number of max_itch_order or above
    };
    Reason reason;    ///< The rule broken
    char type;        ///< The message type, or the offending byte for UnknownType and InvalidSide
    std::size_t size; ///< The message's size, for WrongSize
    std::uint64_t value; ///< The expected size for WrongSize, the reference for OrderOutOfRange
    /// Memberwise equality
    friend bool operator==(const ItchError&, const ItchError&) = default;
};

/// The message an ItchError stands for, such as `'A' message has 35 bytes, expected 36`
[[nodiscard]] std::string describe(const ItchError& error);

namespace itch_detail {

// Field offsets from the specification. Every message starts with its type, stock locate,
// tracking number, and timestamp, and every type the book uses then names an order.
constexpr std::size_t locate_offset = 1;
constexpr std::size_t time_offset = 5;
constexpr std::size_t order_offset = 11;
// A and F
constexpr std::size_t side_offset = 19;
constexpr std::size_t add_shares_offset = 20;
constexpr std::size_t stock_offset = 24;
constexpr std::size_t stock_size = 8;
constexpr std::size_t add_price_offset = 32;
// E, C, and X
constexpr std::size_t shares_offset = 19;
// U
constexpr std::size_t new_order_offset = 19;
constexpr std::size_t replace_shares_offset = 27;
constexpr std::size_t replace_price_offset = 31;

// The specification's message types the book does not use
constexpr std::string_view skipped_types = "SRHYLVWKJhBINO";

struct Size {
    char type;
    std::uint8_t size;
};
// Size of each message type the book uses
constexpr std::array sizes{
    Size{.type = 'A', .size = 36}, Size{.type = 'F', .size = 40}, Size{.type = 'E', .size = 31},
    Size{.type = 'C', .size = 36}, Size{.type = 'X', .size = 23}, Size{.type = 'D', .size = 19},
    Size{.type = 'U', .size = 35}, Size{.type = 'P', .size = 44}, Size{.type = 'Q', .size = 40},
};
// Sizes by type byte, so each message costs one lookup: 0 for a type the book does not use
constexpr auto size_by_type = [] {
    std::array<std::uint8_t, 256> table{};
    for (const auto& [type, size] : sizes) {
        table[static_cast<unsigned char>(type)] = size;
    }
    return table;
}();

constexpr std::size_t message_size(char type)
{
    return size_by_type[static_cast<unsigned char>(type)];
}

// Big-endian unsigned integer at `offset`, which the caller has checked lies within `message`
template <typename T> T read(std::string_view message, std::size_t offset)
{
    T value{};
    std::memcpy(&value, message.data() + offset, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        value = std::byteswap(value);
    }
    return value;
}

// The 6-byte timestamp
inline std::uint64_t read_time(std::string_view message)
{
    constexpr unsigned high_shift = 32;
    return (std::uint64_t{read<std::uint16_t>(message, time_offset)} << high_shift) |
           read<std::uint32_t>(message, time_offset + 2);
}

} // namespace itch_detail

/// Decode one ITCH 5.0 message, without its 2-byte length prefix, and pass it to the `handler`
/// method for its kind: `add`, `execute`, `remove`, `replace`, or `trade` with the ItchMessage,
/// `skipped()` for a type the book does not use, or `error` with an ItchError. Returns what that
/// method returns, so a caller dispatches on the type byte once. decode_itch() describes the
/// mapping and the errors.
template <typename Handler> decltype(auto) visit_itch(std::string_view message, Handler&& handler)
{
    using namespace itch_detail;
    if (message.empty()) {
        return handler.error(
            ItchError{.reason = ItchError::Reason::Empty, .type = 0, .size = 0, .value = 0});
    }
    const auto type = message.front();
    const auto size = message_size(type);
    if (size == 0) {
        if (skipped_types.contains(type)) {
            return handler.skipped();
        }
        return handler.error(ItchError{
            .reason = ItchError::Reason::UnknownType,
            .type = type,
            .size = 0,
            .value = 0,
        });
    }
    if (message.size() != size) {
        return handler.error(ItchError{
            .reason = ItchError::Reason::WrongSize,
            .type = type,
            .size = message.size(),
            .value = size,
        });
    }

    ItchMessage decoded{
        .time = read_time(message),
        .order = 0,
        .replacement = 0,
        .stock = {},
        .shares = 0,
        .price = 0,
        .locate = read<std::uint16_t>(message, locate_offset),
        .kind = ItchKind::Trade,
        .side = Side::Buy,
    };
    if (type == 'P' || type == 'Q') {
        // Trades, whose order field, if any, names no order on the book
        return handler.trade(decoded);
    }
    decoded.order = read<std::uint64_t>(message, order_offset);
    if (type == 'U') {
        decoded.replacement = read<std::uint64_t>(message, new_order_offset);
    }
    if ((decoded.order | decoded.replacement) >= max_itch_order) [[unlikely]] {
        return handler.error(ItchError{
            .reason = ItchError::Reason::OrderOutOfRange,
            .type = type,
            .size = 0,
            .value = decoded.order >= max_itch_order ? decoded.order : decoded.replacement,
        });
    }
    switch (type) {
    case 'A':
    case 'F': {
        const auto side = message[side_offset];
        if (side != 'B' && side != 'S') {
            return handler.error(ItchError{
                .reason = ItchError::Reason::InvalidSide,
                .type = side,
                .size = 0,
                .value = 0,
            });
        }
        decoded.kind = ItchKind::Add;
        decoded.stock = message.substr(stock_offset, stock_size);
        decoded.shares = read<std::uint32_t>(message, add_shares_offset);
        decoded.price = read<std::uint32_t>(message, add_price_offset);
        decoded.side = side == 'B' ? Side::Buy : Side::Sell;
        return handler.add(decoded);
    }
    case 'D':
        decoded.kind = ItchKind::Delete;
        return handler.remove(decoded);
    case 'U':
        decoded.kind = ItchKind::Replace;
        decoded.shares = read<std::uint32_t>(message, replace_shares_offset);
        decoded.price = read<std::uint32_t>(message, replace_price_offset);
        return handler.replace(decoded);
    default:
        // E, C, and X
        decoded.kind = ItchKind::Execute;
        decoded.shares = read<std::uint32_t>(message, shares_offset);
        return handler.execute(decoded);
    }
}

/// Decode one ITCH 5.0 message, without its 2-byte length prefix
///
/// A and F become Add; E, C, and X become Execute, since each takes shares off the order; D becomes
/// Delete and U Replace; P and Q, trades that do not touch the book, become Trade; the
/// specification's other types become Skipped. Fails on an empty message, an unknown type, a size
/// other than the specification's for a type the book uses, a side other than B or S, or an order
/// reference of max_itch_order or above.
[[nodiscard]] std::expected<ItchMessage, ItchError> decode_itch(std::string_view message);

} // namespace order_book
