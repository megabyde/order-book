#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

/// Why a book rejected a message
enum class BookError : std::uint8_t {
    DuplicateOrder, ///< An add or a replacement reuses the ID of a live order
    UnknownOrder,   ///< The message names no live order
};

/**
 * Order book for one ticker, replayed from the exchange's own feed
 *
 * The exchange has already matched the orders: each message applies to the resting order it
 * names, and the book never matches orders itself. Each price level keeps a running total and a
 * count of its orders, and every order is reachable from its ID in expected constant time.
 *
 * `Id` is how the feed names orders: std::string for the CSV feed's text IDs, std::uint64_t for
 * ITCH order reference numbers.
 */
template <typename Id> class BasicOrderBook {
public:
    /// How callers pass an order's ID: a view for string IDs, the number itself otherwise
    using Key = std::conditional_t<std::is_same_v<Id, std::string>, std::string_view, Id>;

    /// An empty book
    BasicOrderBook() = default;
    /// Not copyable: orders keep iterators into the levels, which a copy would leave pointing
    /// into the original. A container of books then moves them even where std::map's move
    /// constructor may throw, as on MSVC.
    BasicOrderBook(const BasicOrderBook&) = delete;
    /// Not copyable
    BasicOrderBook& operator=(const BasicOrderBook&) = delete;
    /// Takes `other`'s orders and levels, whose iterators stay valid
    BasicOrderBook(BasicOrderBook&& other) = default;
    /// Takes `other`'s orders and levels
    BasicOrderBook& operator=(BasicOrderBook&& other) = default;
    /// Frees the orders and levels
    ~BasicOrderBook() = default;

    /// Apply `event` to the order with ID `order`, returning whether the best bid or offer
    /// changed; on error the book is unchanged
    [[nodiscard]] std::expected<bool, BookError> apply(Key order, const Event& event);
    /// Replace the order with ID `order` by `replacement`, resting on the same side at `price` with
    /// `shares`, returning whether the best bid or offer changed; on error the book is unchanged
    [[nodiscard]] std::expected<bool, BookError> replace(Key order, Key replacement, Price price,
                                                         Quantity shares);
    /// Current best bid and offer
    [[nodiscard]] Bbo bbo() const;
    /// Number of resting orders on both sides
    [[nodiscard]] std::size_t order_count() const { return m_orders.size() - m_free.size(); }
    /// Number of price levels on both sides
    [[nodiscard]] std::size_t level_count() const { return m_bids.size() + m_asks.size(); }

private:
    struct PriceLevel {
        Quantity total = 0;
        std::size_t orders = 0;
    };
    // Orders a side's prices best first, so its best level is begin(), which std::map keeps in
    // constant time; one type for both sides lets an order hold a level of either
    class BestFirst {
    public:
        explicit BestFirst(Side side) : m_descending(side == Side::Buy) {}
        [[nodiscard]] bool operator()(Price lhs, Price rhs) const
        {
            return m_descending ? lhs > rhs : lhs < rhs;
        }

    private:
        bool m_descending;
    };
    // std::map iterators survive inserts and erases of other levels
    using Levels = std::map<Price, PriceLevel, BestFirst>;
    struct Order {
        Id id;
        Levels::iterator level;
        Quantity remaining;
        Side side;
    };
    // Open-addressing index entry: where the order sits in m_orders, and the low bits of its ID's
    // hash, which place it in the table and skip most ID comparisons
    struct Slot {
        std::uint32_t order;
        std::uint32_t hash;
    };
    static constexpr std::uint32_t empty_slot = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t initial_slots = 16;

    std::expected<bool, BookError> add(Key order, const AddOrder& add);
    // Take `quantity` off the order in `slot`, removing it and then its level once they reach 0;
    // returns whether the best bid or offer changed
    bool reduce(std::size_t slot, Quantity quantity);
    // Slot holding `id`, or the empty slot that ends its probe sequence
    [[nodiscard]] std::size_t find(Key id, std::uint32_t hash) const;
    // Empty `slot`, shifting later entries of the probe run back so that no lookup stops early
    void erase(std::size_t slot);
    void grow();

    Levels m_bids{BestFirst(Side::Buy)};
    Levels m_asks{BestFirst(Side::Sell)};
    // Orders by position; removed positions are listed in m_free and reused
    std::vector<Order> m_orders;
    std::vector<std::uint32_t> m_free;
    // Linear probing over a power-of-two table kept at most half full
    std::vector<Slot> m_index =
        std::vector<Slot>(initial_slots, Slot{.order = empty_slot, .hash = 0});
};

/// Book of a feed that names orders by text, such as the CSV feed
using OrderBook = BasicOrderBook<std::string>;
/// Book of an ITCH feed, which names orders by 64-bit reference number
using ItchOrderBook = BasicOrderBook<std::uint64_t>;

extern template class BasicOrderBook<std::string>;
extern template class BasicOrderBook<std::uint64_t>;

} // namespace order_book
