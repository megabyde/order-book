#include <order_book/feed.hpp>
#include <order_book/levels.hpp>
#include <order_book/order_book.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace order_book {

std::expected<bool, BookError> OrderBook::apply(std::string_view order, const Event& event)
{
    if (const auto* const add_order = std::get_if<AddOrder>(&event)) {
        return add(order, *add_order);
    }
    if (std::holds_alternative<NoOp>(event)) {
        return false;
    }

    const auto found = m_orders.find(order);
    if (found == m_orders.end()) {
        return std::unexpected(BookError::UnknownOrder);
    }
    auto& resting = found->second;
    const auto remaining = resting.remaining;
    // D and F take the whole remaining quantity
    auto quantity = remaining;
    if (const auto* const decrease = std::get_if<DecreaseOrder>(&event)) {
        if (decrease->shares >= remaining) {
            return false;
        }
        quantity = remaining - decrease->shares;
    }
    else if (const auto* const execute = std::get_if<ExecuteOrder>(&event)) {
        quantity = std::min(execute->shares, remaining);
    }

    resting.remaining -= quantity;
    const auto removed = resting.remaining == 0;
    const auto changed = m_levels.reduce(m_book, resting.level, quantity, removed);
    if (removed) {
        m_orders.erase(found);
    }
    return changed;
}

Bbo OrderBook::bbo() const
{
    return m_levels.bbo(m_book);
}

std::expected<bool, BookError> OrderBook::add(std::string_view order, const AddOrder& add)
{
    const auto [resting, inserted] = m_orders.try_emplace(order);
    if (!inserted) {
        return std::unexpected(BookError::DuplicateOrder);
    }
    const auto [level, changed] = m_levels.add(m_book, add);
    resting->second = Order{.remaining = add.shares, .level = level};
    return changed;
}

} // namespace order_book
