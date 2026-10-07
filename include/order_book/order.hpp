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
    PQ() = default;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    PQ(uint32_t price, uint32_t quantity) : price(price), quantity(quantity) {}
    [[nodiscard]] bool equal_to(const PQ& other) const
    {
        return price == other.price && quantity == other.quantity;
    }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    uint32_t price = 0;    // Price in 100th of a penny
    uint64_t quantity = 0; // Quantity of shares
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// Let Google Test know how to print this
inline std::ostream& operator<<(std::ostream& os, const PQ& pq)
{
    os << pq.price << ',' << pq.quantity;
    return os;
}

/**
 * An abstract order
 */
struct Order {
    using Ptr = std::shared_ptr<Order>;
    enum struct Type : std::uint8_t { Buy, Sell };

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Order(std::string id, Type type, uint32_t price, uint64_t quantity)
        : id(std::move(id)), type(type), price(price), quantity(quantity)
    {
    }
    [[nodiscard]] bool is_buy() const { return type == Type::Buy; }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::string id;    // Unique per-order ID
    Type type;         // Order type (sell/buy)
    uint32_t price;    // Order price in 100th of a penny
    uint64_t quantity; // Quantity of shares (64-bit to avoid overlow when we sum 32-bit values)
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace order_book
