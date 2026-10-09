#include <order_book/feed.hpp>
#include <order_book/order_book.hpp>

#include <algorithm>
#include <expected>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>

namespace order_book {

std::expected<void, BookError> OrderBook::apply(std::string_view order, const Event& event)
{
    if (const auto* const add_order = std::get_if<AddOrder>(&event)) {
        return add(order, *add_order);
    }
    if (std::holds_alternative<NoOp>(event)) {
        return {};
    }

    const auto found = m_orders.find(order);
    if (found == m_orders.end()) {
        return std::unexpected(BookError::UnknownOrder);
    }
    const auto remaining = *found->second.order;
    // D and F take the whole remaining quantity
    auto quantity = remaining;
    if (const auto* const decrease = std::get_if<DecreaseOrder>(&event)) {
        if (decrease->shares >= remaining) {
            return {};
        }
        quantity = remaining - decrease->shares;
    }
    else if (const auto* const execute = std::get_if<ExecuteOrder>(&event)) {
        quantity = std::min(execute->shares, remaining);
    }
    reduce(found, quantity);
    return {};
}

Bbo OrderBook::bbo() const
{
    Bbo bbo;
    if (!m_bids.empty()) {
        const auto& [price, level] = *std::prev(m_bids.end());
        bbo.bid = Level{.price = price, .quantity = level.total};
    }
    if (!m_asks.empty()) {
        const auto& [price, level] = *m_asks.begin();
        bbo.ask = Level{.price = price, .quantity = level.total};
    }
    return bbo;
}

std::expected<void, BookError> OrderBook::add(std::string_view order, const AddOrder& add)
{
    const auto [entry, inserted] = m_orders.try_emplace(std::string(order));
    if (!inserted) {
        return std::unexpected(BookError::DuplicateOrder);
    }
    auto& levels = add.side == Side::Buy ? m_bids : m_asks;
    const auto level = levels.try_emplace(add.price).first;
    level->second.total += add.shares;
    const auto resting = level->second.fifo.insert(level->second.fifo.end(), add.shares);
    entry->second = Handle{.side = add.side, .level = level, .order = resting};
    return {};
}

void OrderBook::reduce(Orders::iterator order, Quantity quantity)
{
    const auto& [side, level, resting] = order->second;
    level->second.total -= quantity;
    *resting -= quantity;
    if (*resting != 0) {
        return;
    }
    level->second.fifo.erase(resting);
    if (level->second.fifo.empty()) {
        (side == Side::Buy ? m_bids : m_asks).erase(level);
    }
    m_orders.erase(order);
}

} // namespace order_book
