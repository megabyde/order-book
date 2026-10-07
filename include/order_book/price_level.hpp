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
    [[nodiscard]] size_t size() const { return m_queue.size(); }
    [[nodiscard]] bool empty() const { return m_queue.empty(); }
    [[nodiscard]] uint64_t quantity() const
    {
        uint64_t total = 0;
        for (const auto& o : m_queue) {
            total += o->quantity;
        }
        return total;
    }
    [[nodiscard]] const Order::Ptr& front() const { return m_queue.front(); }
    // FIFO access
    void pop_front() { m_queue.pop_front(); }
    void push_back(const Order::Ptr& order) { m_queue.push_back(order); }
    // Remove by ID
    void remove(const std::string id)
    {
        m_queue.remove_if([&id](const auto& o) { return o->id == id; });
    }

private:
    std::list<Order::Ptr> m_queue;
};

} // namespace order_book
