#pragma once

#include <algorithm>
#include <list>

#include <order_book/order.hpp>

namespace order_book {

/**
 * Price level is a list of orders maintained as FIFO queue
 */
class PriceLevel {
public:
    /// Number of orders at this price
    [[nodiscard]] size_t size() const { return m_queue.size(); }
    /// Whether no orders remain at this price
    [[nodiscard]] bool empty() const { return m_queue.empty(); }
    /// Total quantity of shares at this price
    [[nodiscard]] uint64_t quantity() const
    {
        uint64_t total = 0;
        for (const auto& o : m_queue) {
            total += o->quantity;
        }
        return total;
    }
    /// Oldest order
    [[nodiscard]] const Order::Ptr& front() const { return m_queue.front(); }
    /// Remove the oldest order
    void pop_front() { m_queue.pop_front(); }
    /// Append an order at the back of the queue
    void push_back(const Order::Ptr& order) { m_queue.push_back(order); }
    /// Remove by ID
    void remove(const std::string id)
    {
        m_queue.remove_if([&id](const auto& o) { return o->id == id; });
    }

private:
    std::list<Order::Ptr> m_queue;
};

} // namespace order_book
