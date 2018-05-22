#pragma once

#include <iostream>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

#include "event.h"
#include "order.h"
#include "price_level.h"

/**
 * Order book for a particular symbol
 */
class OrderBook {
public:
    // Add new buy order at the price and quantity of shares
    void buy(const std::string& id, uint32_t price, uint64_t quantity)
    {
        add(id, price, quantity, Order::Type::Buy);
    }
    // Add new sell order at the price and quantity of shares
    void sell(const std::string& id, uint32_t price, uint64_t quantity)
    {
        add(id, price, quantity, Order::Type::Sell);
    }
    // Decrease the number of shares of the order to the given quantity
    void decrease(const std::string& id, uint64_t quantity);
    // Delete the order
    void remove(const std::string& id);
    // Execute given quantity of shares of the order
    void execute(const std::string& id, uint64_t quantity);
    // Fill the order completely
    void fill(const std::string& id);

    // Statistics
    size_t num_ask_orders() const
    {
        size_t total = 0;
        for (const auto& kv : m_asks)
            total += kv.second.size();
        return total;
    }
    size_t num_bid_orders() const
    {
        size_t total = 0;
        for (const auto& kv : m_bids)
            total += kv.second.size();
        return total;
    }
    size_t num_orders() const { return num_ask_orders() + num_bid_orders(); }
    size_t num_ask_price_levels() const { return m_asks.size(); }
    size_t num_bid_price_levels() const { return m_bids.size(); }
    size_t num_price_levels() const { return num_ask_price_levels() + num_bid_price_levels(); }
    // Inner market
    bool changed() const { return m_changed; }
    std::pair<PQ, PQ> best_ask_bid()
    {
        m_changed = false;
        return std::make_pair(m_best_ask, m_best_bid);
    }

private:
    // Recalculate best ask and bid
    void update();
    // Add order of the given type, price and quantity
    void add(const std::string& id, uint32_t price, uint64_t quantity, Order::Type type);

    // Price levels for both buy/sell sides
    std::map<uint32_t, PriceLevel> m_asks, m_bids;
    // Cache for fast order access using ID.
    // We use weak pointers since the cache doesn't 'own' m_orders
    std::unordered_map<std::string, std::weak_ptr<Order>> m_orders;
    // Current best ask and bid
    PQ m_best_ask, m_best_bid;
    // Flag indicating best ask/bid change
    bool m_changed = false;
};
