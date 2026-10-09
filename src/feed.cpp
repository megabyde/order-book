#include <order_book/feed.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace order_book {

namespace {

template <typename T>
std::expected<T, std::string> parse_uint(std::string_view text, std::string_view name)
{
    T value{};
    const auto* const end = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(std::to_address(text.begin()), end, value);
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected(std::format("{} out of range: '{}'", name, text));
    }
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected(std::format("{} is not an unsigned integer: '{}'", name, text));
    }
    return value;
}

constexpr std::size_t min_fields = 6;

} // namespace

std::expected<Message, std::string> parse_line(std::string_view line)
{
    std::array<std::string_view, min_fields> fields;
    std::size_t count = 0;
    for (const auto field : line | std::views::split(',')) {
        fields[count] = std::string_view(field.begin(), field.end());
        if (++count == fields.size()) {
            break;
        }
    }
    if (count < fields.size()) {
        return std::unexpected(
            std::format("expected at least {} fields, got {}", min_fields, count));
    }
    const auto [time_field, ticker, order, type, shares_field, price_field] = fields;

    const auto time = parse_uint<std::uint64_t>(time_field, "time");
    if (!time) {
        return std::unexpected(time.error());
    }
    if (type.size() != 1 || !std::string_view("BSCDEFTX").contains(type.front())) {
        return std::unexpected(std::format("unknown message type '{}'", type));
    }
    const auto shares = parse_uint<std::uint32_t>(shares_field, "shares");
    if (!shares) {
        return std::unexpected(shares.error());
    }
    const auto price = parse_uint<Price>(price_field, "price");
    if (!price) {
        return std::unexpected(price.error());
    }

    Message message{.time = *time, .ticker = ticker, .order = order, .event = NoOp{}};
    switch (type.front()) {
    case 'B':
        message.event = AddOrder{.side = Side::Buy, .price = *price, .shares = *shares};
        break;
    case 'S':
        message.event = AddOrder{.side = Side::Sell, .price = *price, .shares = *shares};
        break;
    case 'C':
        message.event = DecreaseOrder{.shares = *shares};
        break;
    case 'D':
        message.event = DeleteOrder{};
        break;
    case 'E':
        message.event = ExecuteOrder{.shares = *shares};
        break;
    case 'F':
        message.event = FillOrder{};
        break;
    default:
        // T and X leave the NoOp in place
        break;
    }
    return message;
}

} // namespace order_book
