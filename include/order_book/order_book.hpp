#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include <ankerl/unordered_dense.h>

#include <order_book/feed.hpp>
#include <order_book/levels.hpp>

namespace order_book {

/// Hash that lets string-keyed unordered containers look up a std::string_view without a copy
struct StringHash {
    using is_transparent = void; ///< Enables heterogeneous lookup
    using is_avalanching = void; ///< Tells unordered_dense that the hash needs no extra mixing
    /// Hash of the characters, equal for a std::string and a view of it
    [[nodiscard]] std::uint64_t operator()(std::string_view text) const noexcept
    {
        return ankerl::unordered_dense::hash<std::string_view>{}(text);
    }
};

/// Why OrderBook::apply() rejected a message
enum class BookError : std::uint8_t {
    DuplicateOrder, ///< An add reuses the ID of a live order
    UnknownOrder,   ///< C/D/E/F names no live order
};

/**
 * Order book for one ticker, replayed from the exchange's own feed
 *
 * The exchange has already matched the orders: each message applies to the resting order it
 * names, and the book never matches orders itself. Each price level keeps a running total and a
 * count of its orders, and every order is reachable from its ID in expected constant time. Each
 * side lists its levels worst price first, so most adds and removals touch only its end.
 */
class OrderBook {
public:
    /// Apply `event` to the order with ID `order`, returning whether the best bid or offer
    /// changed; on a BookError the book is unchanged. An exception, such as running out of memory,
    /// can leave the book inconsistent, so the book must not be used after one.
    [[nodiscard]] std::expected<bool, BookError> apply(std::string_view order, const Event& event);
    /// Current best bid and offer
    [[nodiscard]] Bbo bbo() const;
    /// Number of resting orders on both sides
    [[nodiscard]] std::size_t order_count() const { return m_orders.size(); }
    /// Number of price levels on both sides
    [[nodiscard]] std::size_t level_count() const { return m_levels.size(); }

private:
    struct Order {
        Shares remaining;
        std::uint32_t level;
    };

    std::expected<bool, BookError> add(std::string_view order, const AddOrder& add);

    BookLevels m_book;
    LevelPool m_levels;
    ankerl::unordered_dense::map<std::string, Order, StringHash, std::equal_to<>> m_orders;
};

} // namespace order_book
