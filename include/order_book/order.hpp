#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

namespace order_book {

/**
 * A class template to express an equality comparison interface
 */
template <typename T> class EqualComparable {
    EqualComparable() = default;
    friend T;
    friend bool operator==(const T& lhs, const T& rhs) { return lhs.equal_to(rhs); }
    friend bool operator!=(const T& lhs, const T& rhs) { return !lhs.equal_to(rhs); }
};

/**
 * Price-Quantity pair
 */
struct PQ : private EqualComparable<PQ> {
    /// Empty level: price 0 means no orders on that side
    PQ() = default;
    /// Level at a price with a total quantity
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    PQ(uint32_t price, uint64_t quantity) : price(price), quantity(quantity) {}
    /// Whether both price and quantity match
    [[nodiscard]] bool equal_to(const PQ& other) const
    {
        return price == other.price && quantity == other.quantity;
    }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    uint32_t price = 0;    ///< Price in 100th of a penny
    uint64_t quantity = 0; ///< Quantity of shares
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

/// Print as `price,quantity`; Google Test uses it for failure messages
inline std::ostream& operator<<(std::ostream& os, const PQ& pq)
{
    os << pq.price << ',' << pq.quantity;
    return os;
}

/**
 * An abstract order
 */
struct Order {
    /// Shared ownership handle; the price level owns the order
    using Ptr = std::shared_ptr<Order>;
    /// Order side
    enum struct Type : std::uint8_t { Buy, Sell };

    /// Order with the given ID, side, limit price, and quantity
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Order(std::string id, Type type, uint32_t price, uint64_t quantity)
        : id(std::move(id)), type(type), price(price), quantity(quantity)
    {
    }
    /// Whether the order is on the bid side
    [[nodiscard]] bool is_buy() const { return type == Type::Buy; }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::string id;    ///< Unique per-order ID
    Type type;         ///< Order type (sell/buy)
    uint32_t price;    ///< Order price in 100th of a penny
    uint64_t quantity; ///< Quantity of shares (64-bit to avoid overlow when we sum 32-bit values)
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace order_book
