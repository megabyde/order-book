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

// Reference numbers are assigned in sequence, so the table's low bits need them mixed first:
// the splitmix64 finalizer
std::uint32_t hash_id(std::uint64_t id)
{
    constexpr std::uint64_t first = 0xbf58476d1ce4e5b9;
    constexpr std::uint64_t second = 0x94d049bb133111eb;
    constexpr unsigned first_shift = 30;
    constexpr unsigned second_shift = 27;
    constexpr unsigned last_shift = 31;
    id = (id ^ (id >> first_shift)) * first;
    id = (id ^ (id >> second_shift)) * second;
    return static_cast<std::uint32_t>(id ^ (id >> last_shift));
}

} // namespace

template <typename Id>
std::expected<bool, BookError> BasicOrderBook<Id>::apply(Key order, const Event& event)
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
    const auto remaining = m_orders[m_index[slot].order].remaining;
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
    return reduce(slot, quantity);
}

// A slot index and a share count; both are unsigned, so the check sees them as swappable
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
template <typename Id> bool BasicOrderBook<Id>::reduce(std::size_t slot, Quantity quantity)
{
    const auto position = m_index[slot].order;
    auto& resting = m_orders[position];
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

template <typename Id>
std::expected<bool, BookError> BasicOrderBook<Id>::replace(Key order, Key replacement, Price price,
                                                           Quantity shares)
{
    const auto slot = find(order, hash_id(order));
    if (m_index[slot].order == empty_slot) {
        return std::unexpected(BookError::UnknownOrder);
    }
    if (replacement != order &&
        m_index[find(replacement, hash_id(replacement))].order != empty_slot) {
        return std::unexpected(BookError::DuplicateOrder);
    }
    const auto before = bbo();
    const auto& resting = m_orders[m_index[slot].order];
    const auto side = resting.side;
    reduce(slot, resting.remaining);
    // Cannot fail: the replacement was not live, and the order it replaces is gone
    static_cast<void>(
        add(replacement, AddOrder{.side = side, .price = price, .shares = shares}).value());
    return bbo() != before;
}

template <typename Id> Bbo BasicOrderBook<Id>::bbo() const
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

template <typename Id>
std::expected<bool, BookError> BasicOrderBook<Id>::add(Key order, const AddOrder& add)
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
    // A reused position keeps a string ID's buffer for the new ID
    auto& resting = m_orders[position];
    resting.id = order;
    resting.level = level;
    resting.remaining = add.shares;
    resting.side = add.side;
    m_index[slot] = Slot{.order = position, .hash = hash};
    return changed;
}

template <typename Id> std::size_t BasicOrderBook<Id>::find(Key id, std::uint32_t hash) const
{
    const auto mask = m_index.size() - 1;
    for (auto slot = hash & mask;; slot = (slot + 1) & mask) {
        const auto& entry = m_index[slot];
        if (entry.order == empty_slot || (entry.hash == hash && m_orders[entry.order].id == id)) {
            return slot;
        }
    }
}

template <typename Id> void BasicOrderBook<Id>::erase(std::size_t slot)
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

template <typename Id> void BasicOrderBook<Id>::grow()
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

template class BasicOrderBook<std::string>;
template class BasicOrderBook<std::uint64_t>;

} // namespace order_book
