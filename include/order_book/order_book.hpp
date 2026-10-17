#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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
    /// changed; on error the book is unchanged
    [[nodiscard]] std::expected<bool, BookError> apply(std::string_view order, const Event& event);
    /// Current best bid and offer
    [[nodiscard]] Bbo bbo() const;
    /// Number of resting orders on both sides
    [[nodiscard]] std::size_t order_count() const { return m_orders.size() - m_free.size(); }
    /// Number of price levels on both sides
    [[nodiscard]] std::size_t level_count() const { return m_levels.size(); }

private:
    struct Order {
        std::string id;
        Quantity remaining;
        std::uint32_t level;
    };
    // Open-addressing index entry: where the order sits in m_orders, and the low bits of its ID's
    // hash, which place it in the table and skip most string comparisons
    struct Slot {
        std::uint32_t order;
        std::uint32_t hash;
    };
    static constexpr std::uint32_t empty_slot = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t initial_slots = 16;

    std::expected<bool, BookError> add(std::string_view order, const AddOrder& add);
    // Slot holding `id`, or the empty slot that ends its probe sequence
    [[nodiscard]] std::size_t find(std::string_view id, std::uint32_t hash) const;
    // Empty `slot`, shifting later entries of the probe run back so that no lookup stops early
    void erase(std::size_t slot);
    void grow();

    BookLevels m_book;
    LevelPool m_levels;
    // Orders by position; removed positions are listed in m_free and reused
    std::vector<Order> m_orders;
    std::vector<std::uint32_t> m_free;
    // Linear probing over a power-of-two table kept at most half full
    std::vector<Slot> m_index =
        std::vector<Slot>(initial_slots, Slot{.order = empty_slot, .hash = 0});
};

} // namespace order_book
