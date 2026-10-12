#include <order_book/feed.hpp>
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

namespace {

std::uint32_t hash_id(std::string_view id)
{
    return static_cast<std::uint32_t>(std::hash<std::string_view>{}(id));
}

} // namespace

std::expected<bool, BookError> OrderBook::apply(std::string_view order, const Event& event)
{
    if (const auto* const add_order = std::get_if<AddOrder>(&event)) {
        return add(order, *add_order);
    }
    if (std::holds_alternative<NoOp>(event)) {
        return false;
    }

    const auto slot = find(order, hash_id(order));
    if (m_index[slot].order == empty_slot) {
        return std::unexpected(BookError::UnknownOrder);
    }
    const auto position = m_index[slot].order;
    auto& resting = m_orders[position];
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

    auto& levels = resting.side == Side::Buy ? m_bids : m_asks;
    // Only the best level shows in the BBO, and only if its total changes or it goes away
    auto changed = resting.level == levels.begin() && quantity != 0;
    auto& level = resting.level->second;
    level.total -= quantity;
    resting.remaining -= quantity;
    if (resting.remaining == 0) {
        if (--level.orders == 0) {
            changed = resting.level == levels.begin();
            levels.erase(resting.level);
        }
        m_free.push_back(position);
        erase(slot);
    }
    return changed;
}

Bbo OrderBook::bbo() const
{
    Bbo bbo;
    if (!m_bids.empty()) {
        const auto& [price, level] = *m_bids.begin();
        bbo.bid = Level{.price = price, .quantity = level.total};
    }
    if (!m_asks.empty()) {
        const auto& [price, level] = *m_asks.begin();
        bbo.ask = Level{.price = price, .quantity = level.total};
    }
    return bbo;
}

std::expected<bool, BookError> OrderBook::add(std::string_view order, const AddOrder& add)
{
    const auto hash = hash_id(order);
    auto slot = find(order, hash);
    if (m_index[slot].order != empty_slot) {
        return std::unexpected(BookError::DuplicateOrder);
    }
    if (2 * (order_count() + 1) > m_index.size()) {
        grow();
        slot = find(order, hash);
    }

    auto& levels = add.side == Side::Buy ? m_bids : m_asks;
    const auto [level, created] = levels.try_emplace(add.price);
    level->second.total += add.shares;
    ++level->second.orders;
    const auto changed = level == levels.begin() && (created || add.shares != 0);

    std::uint32_t position = 0;
    if (m_free.empty()) {
        position = static_cast<std::uint32_t>(m_orders.size());
        m_orders.emplace_back();
    }
    else {
        position = m_free.back();
        m_free.pop_back();
    }
    // A reused position keeps its string's buffer for the new ID
    auto& resting = m_orders[position];
    resting.id = order;
    resting.level = level;
    resting.remaining = add.shares;
    resting.side = add.side;
    m_index[slot] = Slot{.order = position, .hash = hash};
    return changed;
}

std::size_t OrderBook::find(std::string_view id, std::uint32_t hash) const
{
    const auto mask = m_index.size() - 1;
    for (auto slot = hash & mask;; slot = (slot + 1) & mask) {
        const auto& entry = m_index[slot];
        if (entry.order == empty_slot || (entry.hash == hash && m_orders[entry.order].id == id)) {
            return slot;
        }
    }
}

void OrderBook::erase(std::size_t slot)
{
    const auto mask = m_index.size() - 1;
    auto hole = slot;
    for (auto next = (slot + 1) & mask; m_index[next].order != empty_slot;
         next = (next + 1) & mask) {
        // An entry may fill the hole only if the hole lies between its home slot and where it is,
        // or a lookup for it would stop at the hole
        const auto home = m_index[next].hash & mask;
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            m_index[hole] = m_index[next];
            hole = next;
        }
    }
    m_index[hole].order = empty_slot;
}

void OrderBook::grow()
{
    std::vector<Slot> index(2 * m_index.size(), Slot{.order = empty_slot, .hash = 0});
    const auto mask = index.size() - 1;
    for (const auto& entry : m_index) {
        if (entry.order == empty_slot) {
            continue;
        }
        auto slot = entry.hash & mask;
        while (index[slot].order != empty_slot) {
            slot = (slot + 1) & mask;
        }
        index[slot] = entry;
    }
    m_index = std::move(index);
}

} // namespace order_book
