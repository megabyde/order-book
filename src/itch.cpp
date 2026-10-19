#include <order_book/feed.hpp>
#include <order_book/itch.hpp>

#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace order_book {

std::string describe(const ItchError& error)
{
    const auto byte = static_cast<unsigned char>(error.type);
    switch (error.reason) {
    case ItchError::Reason::Empty:
        return "empty message";
    case ItchError::Reason::UnknownType:
        return std::format("unknown message type 0x{:02x}", byte);
    case ItchError::Reason::WrongSize:
        return std::format("'{}' message has {} bytes, expected {}", error.type, error.size,
                           error.value);
    case ItchError::Reason::InvalidSide:
        break;
    }
    return std::format("invalid side 0x{:02x}", byte);
}

namespace {

// Keeps whatever visit_itch() decodes
struct Keep {
    using Result = std::expected<ItchMessage, ItchError>;
    static Result add(const ItchMessage& message) { return message; }
    static Result execute(const ItchMessage& message) { return message; }
    static Result remove(const ItchMessage& message) { return message; }
    static Result replace(const ItchMessage& message) { return message; }
    static Result trade(const ItchMessage& message) { return message; }
    static Result skipped()
    {
        return ItchMessage{
            .time = 0,
            .order = 0,
            .replacement = 0,
            .stock = {},
            .shares = 0,
            .price = 0,
            .locate = 0,
            .kind = ItchKind::Skipped,
            .side = Side::Buy,
        };
    }
    static Result error(const ItchError& error) { return std::unexpected(error); }
};

} // namespace

std::expected<ItchMessage, ItchError> decode_itch(std::string_view message)
{
    return visit_itch(message, Keep{});
}

} // namespace order_book
