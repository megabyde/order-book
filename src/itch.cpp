#include <order_book/feed.hpp>
#include <order_book/itch.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace order_book {

namespace {

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
    std::size_t size;
};
// Size of each message type the book uses
constexpr std::array sizes{
    Size{.type = 'A', .size = 36}, Size{.type = 'F', .size = 40}, Size{.type = 'E', .size = 31},
    Size{.type = 'C', .size = 36}, Size{.type = 'X', .size = 23}, Size{.type = 'D', .size = 19},
    Size{.type = 'U', .size = 35}, Size{.type = 'P', .size = 44}, Size{.type = 'Q', .size = 40},
};

// 0 for a type the book does not use
constexpr std::size_t message_size(char type)
{
    for (const auto& [message_type, size] : sizes) {
        if (message_type == type) {
            return size;
        }
    }
    return 0;
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
std::uint64_t read_time(std::string_view message)
{
    constexpr unsigned high_shift = 32;
    return (std::uint64_t{read<std::uint16_t>(message, time_offset)} << high_shift) |
           read<std::uint32_t>(message, time_offset + 2);
}

} // namespace

std::expected<ItchMessage, std::string> decode_itch(std::string_view message)
{
    if (message.empty()) {
        return std::unexpected("empty message");
    }
    const auto type = message.front();
    const auto size = message_size(type);
    if (size == 0) {
        if (skipped_types.contains(type)) {
            return ItchMessage{.time = 0, .locate = 0, .stock = {}, .order = 0, .event = Skipped{}};
        }
        return std::unexpected(
            std::format("unknown message type 0x{:02x}", static_cast<unsigned char>(type)));
    }
    if (message.size() != size) {
        return std::unexpected(
            std::format("'{}' message has {} bytes, expected {}", type, message.size(), size));
    }

    ItchMessage decoded{
        .time = read_time(message),
        .locate = read<std::uint16_t>(message, locate_offset),
        .stock = {},
        .order = read<std::uint64_t>(message, order_offset),
        .event = NoOp{},
    };
    switch (type) {
    case 'A':
    case 'F': {
        const auto side = message[side_offset];
        if (side != 'B' && side != 'S') {
            return std::unexpected(
                std::format("invalid side 0x{:02x}", static_cast<unsigned char>(side)));
        }
        const auto stock = message.substr(stock_offset, stock_size);
        decoded.stock = stock.substr(0, stock.find_last_not_of(' ') + 1);
        decoded.event = AddOrder{
            .side = side == 'B' ? Side::Buy : Side::Sell,
            .price = read<std::uint32_t>(message, add_price_offset),
            .shares = read<std::uint32_t>(message, add_shares_offset),
        };
        break;
    }
    case 'E':
    case 'C':
    case 'X':
        decoded.event = ExecuteOrder{.shares = read<std::uint32_t>(message, shares_offset)};
        break;
    case 'D':
        decoded.event = DeleteOrder{};
        break;
    case 'U':
        decoded.event = ReplaceOrder{
            .order = read<std::uint64_t>(message, new_order_offset),
            .price = read<std::uint32_t>(message, replace_price_offset),
            .shares = read<std::uint32_t>(message, replace_shares_offset),
        };
        break;
    default:
        // P and Q, trades that leave the NoOp in place
        decoded.order = 0;
        break;
    }
    return decoded;
}

} // namespace order_book
