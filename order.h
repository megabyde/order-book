#pragma once

#include <memory>
#include <string>
#include <utility>

/**
 * A class template to express an equality comparison interface
 */
template <typename T> class EqualComparable {
    friend bool operator==(const T& lhs, const T& rhs) { return lhs.equal_to(rhs); }
    friend bool operator!=(const T& lhs, const T& rhs) { return !lhs.equal_to(rhs); }
};

/**
 * Price-Quantity pair
 */
struct PQ : private EqualComparable<PQ> {
    PQ() = default;
    PQ(uint32_t price, uint32_t quantity) : price(price), quantity(quantity) {}
    bool equal_to(const PQ& other) const
    {
        return price == other.price && quantity == other.quantity;
    }
    uint32_t price    = 0; // Price in 100th of a penny
    uint64_t quantity = 0; // Quantity of shares
};

// Let Google Test know how to print this
inline std::ostream&
operator<<(std::ostream& os, const PQ& pq)
{
    os << pq.price << ',' << pq.quantity;
    return os;
}

/**
 * An abstract order
 */
struct Order {
    using Ptr = std::shared_ptr<Order>;
    enum struct Type { Buy, Sell };

    Order(std::string id, Type type, uint32_t price, uint64_t quantity)
        : id(std::move(id)), type(type), price(price), quantity(quantity)
    {
    }
    bool        is_buy() const { return type == Type::Buy; }
    std::string id;       // Unique per-order ID
    Type        type;     // Order type (sell/buy)
    uint32_t    price;    // Order price in 100th of a penny
    uint64_t    quantity; // Quantity of shares (64-bit to avoid overlow when we sum 32-bit values)
};
