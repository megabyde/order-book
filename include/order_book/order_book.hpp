#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <order_book/feed.hpp>

namespace order_book {

/// Hash that lets string-keyed unordered containers look up a std::string_view without a copy
struct StringHash {
    using is_transparent = void; ///< Enables heterogeneous lookup
    /// Hash of the characters, equal for a std::string and a view of it
    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept
    {
        return std::hash<std::string_view>{}(text);
    }
};

/// Price and total resting quantity of one level
struct Level {
    Price price;       ///< Level price
    Quantity quantity; ///< Sum of the remaining quantity of every order at this price
    /// Same price and quantity
    friend bool operator==(const Level&, const Level&) = default;
};

/// Best bid and offer; an empty side has no level
struct Bbo {
    std::optional<Level> bid; ///< Highest bid level
    std::optional<Level> ask; ///< Lowest ask level
    /// Same levels on both sides
    friend bool operator==(const Bbo&, const Bbo&) = default;
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
 * names, and the book never matches orders itself. Each price level keeps its orders in arrival
 * order and a running total, and every order is reachable from its ID in constant time.
 */
class OrderBook {
public:
    /// Apply `event` to the order with ID `order`; on error the book is unchanged
    [[nodiscard]] std::expected<void, BookError> apply(std::string_view order, const Event& event);
    /// Current best bid and offer
    [[nodiscard]] Bbo bbo() const;
    /// Number of resting orders on both sides
    [[nodiscard]] std::size_t order_count() const { return m_orders.size(); }
    /// Number of price levels on both sides
    [[nodiscard]] std::size_t level_count() const { return m_bids.size() + m_asks.size(); }

private:
    struct PriceLevel {
        Quantity total = 0;
        std::list<Quantity> fifo;
    };
    using Levels = std::map<Price, PriceLevel>;
    // Where an order rests; std::map and std::list iterators survive inserts and erases elsewhere
    struct Handle {
        Side side;
        Levels::iterator level;
        std::list<Quantity>::iterator order;
    };
    using Orders = std::unordered_map<std::string, Handle, StringHash, std::equal_to<>>;

    std::expected<void, BookError> add(std::string_view order, const AddOrder& add);
    // Take `quantity` off the order, removing it and then its level once they reach 0
    void reduce(Orders::iterator order, Quantity quantity);

    Levels m_bids, m_asks;
    Orders m_orders;
};

} // namespace order_book
