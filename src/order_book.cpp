#include <order_book/order.hpp>
#include <order_book/order_book.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace order_book {

/**
 * Add order of the given type, price and quantity
 */
void OrderBook::add(const std::string& id, uint32_t price, uint64_t quantity, Order::Type type)
{
    if (m_orders.contains(id)) {
        throw std::invalid_argument("duplicate order '" + id + "'");
    }

    // Create a new order
    const auto order = std::make_shared<Order>(id, type, price, quantity);
    auto& side = order->is_buy() ? m_bids : m_asks;
    auto& price_level = side[price];

    // Add the order to the cache
    m_orders[id] = order;
    // Put the new order to the end of the price level
    price_level.push_back(order);
    // Recalculate best bid/ask
    update();
}

/**
 * Resting order with the given ID
 */
std::shared_ptr<Order> OrderBook::find(const std::string& id) const
{
    const auto it = m_orders.find(id);
    if (it == m_orders.end()) {
        throw std::invalid_argument("unknown order '" + id + "'");
    }
    return it->second.lock();
}

/**
 * Decrease the number of shares of the order to the given quantity
 */
void OrderBook::decrease(const std::string& id, uint64_t quantity)
{
    const auto order = find(id);
    if (quantity >= order->quantity) {
        return;
    }
    if (quantity == 0) {
        remove(id);
        return;
    }
    order->quantity = quantity;
    // Recalculate best bid/ask
    update();
}

/**
 * Delete the order
 */
void OrderBook::remove(const std::string& id)
{
    const auto order = find(id);
    auto& side = order->is_buy() ? m_bids : m_asks;
    auto& price_level = side[order->price];

    // Delete our order from the price level
    price_level.remove(id);
    // If it was the last order, drop the price level
    if (price_level.empty()) {
        side.erase(order->price);
    }
    // Finally, delete the order from the cache
    m_orders.erase(id);
    // Recalculate best bid/ask
    update();
}

/**
 * Execute given quantity of shares of the order.
 * If less amount of shares is available in the order, execute the rest.
 */
void OrderBook::execute(const std::string& id, uint64_t quantity)
{
    const auto order = find(id);
    if (quantity >= order->quantity) {
        remove(id);
        return;
    }
    order->quantity -= quantity;
    // Recalculate best bid/ask
    update();
}

/**
 * Fill the order completely
 */
void OrderBook::fill(const std::string& id)
{
    remove(id);
}

/**
 * Recalculate best ask and bid
 */
void OrderBook::update()
{
    // The lowest sell price level
    const auto a = m_asks.begin();
    const auto best_ask = a != m_asks.end() ? PQ(a->first, a->second.quantity()) : PQ();
    // The highest (inverse order) buy price
    const auto b = m_bids.rbegin();
    const auto best_bid = b != m_bids.rend() ? PQ(b->first, b->second.quantity()) : PQ();

    if (m_best_ask != best_ask || m_best_bid != best_bid) {
        m_changed = true;
        m_best_ask = best_ask;
        m_best_bid = best_bid;
    }
}

} // namespace order_book
