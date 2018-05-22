#include "order_book.h"

/**
 * Add order of the given type, price and quantity
 */
void
OrderBook::add(const std::string& id, uint32_t price, uint64_t quantity, Order::Type type)
{
#ifndef NDEBUG
    // Check for duplicate add
    auto it = m_orders.find(id);
    assert(it == m_orders.end());
#endif

    // Create a new order
    auto  order       = std::make_shared<Order>(id, type, price, quantity);
    auto& side        = order->is_buy() ? m_bids : m_asks;
    auto& price_level = side[price];

    // Add the order to the cache
    m_orders[id] = order;
    // Put the new order to the end of the price level
    price_level.push_back(order);
    // Recalculate best bid/ask
    update();
}

/**
 * Decrease the number of shares of the order to the given quantity
 */
void
OrderBook::decrease(const std::string& id, uint64_t quantity)
{
    auto it = m_orders.find(id);
    if (it == m_orders.end())
        return;

    assert(!it->second.expired());
    auto order = it->second.lock();
    // Update the order
    assert(quantity > 0);
    order->quantity = std::min(quantity, order->quantity);
    // Recalculate best bid/ask
    update();
}

/**
 * Delete the order
 */
void
OrderBook::remove(const std::string& id)
{
    auto it = m_orders.find(id);
    if (it == m_orders.end())
        return;

    assert(!it->second.expired());
    auto  order       = it->second.lock();
    auto& side        = order->is_buy() ? m_bids : m_asks;
    auto& price_level = side[order->price];

    // Delete our order from the price level
    price_level.remove(id);
    // If it was the last order, drop the price level
    if (price_level.empty())
        side.erase(order->price);
    // Finally, delete the order from the cache
    m_orders.erase(it);
    // Recalculate best bid/ask
    update();
}

/**
 * Execute given quantity of shares of the order.
 * If less amount of shares is available in the order, execute the rest.
 */
void
OrderBook::execute(const std::string& id, uint64_t quantity)
{
    auto it = m_orders.find(id);
    if (it == m_orders.end())
        return;

    assert(!it->second.expired());
    auto order = it->second.lock();
    quantity   = std::min(quantity, order->quantity);

    // For buy orders, we need to match with asks and vice versa
    auto& side = order->is_buy() ? m_asks : m_bids;
    // We start from the lowest sell price
    auto lowest = m_asks.begin();
    // We start from highest buy price
    auto highest = m_bids.end();
    if (!m_bids.empty())
        --highest;
    while (quantity > 0 && order->quantity > 0 && !side.empty()) {
        auto& price_level = order->is_buy() ? lowest->second : highest->second;
        auto  oldest      = price_level.front().get();
        // We have enough shares on the oldest order
        if (oldest->quantity > quantity) {
            oldest->quantity -= quantity;
            order->quantity -= quantity;
            break;
        }
        else {
            order->quantity -= oldest->quantity;
            quantity -= oldest->quantity;

            m_orders.erase(oldest->id);
            price_level.pop_front();
            // If the price level is empty, delete it and go to the next one
            if (price_level.empty()) {
                if (order->is_buy())
                    m_asks.erase(lowest++);
                else
                    m_bids.erase(highest--);
            }
        }
    }
    // Remove our order if it was fully filled
    if (order->quantity == 0)
        remove(id);
    // Recalculate best bid/ask
    update();
}

/**
 * Fill the order completely
 */
void
OrderBook::fill(const std::string& id)
{
    auto it = m_orders.find(id);
    if (it == m_orders.end())
        return;

    assert(!it->second.expired());
    auto order = it->second.lock();
    execute(id, order->quantity);
}

/**
 * Recalculate best ask and bid
 */
void
OrderBook::update()
{
    // The lowest sell price level
    auto a        = m_asks.begin();
    auto best_ask = a != m_asks.end() ? PQ(a->first, a->second.quantity()) : PQ();
    // The highest (inverse order) buy price
    auto b        = m_bids.rbegin();
    auto best_bid = b != m_bids.rend() ? PQ(b->first, b->second.quantity()) : PQ();

    if (m_best_ask != best_ask || m_best_bid != best_bid) {
        m_changed  = true;
        m_best_ask = best_ask;
        m_best_bid = best_bid;
    }
}
