#include <order_book/feed.hpp>
#include <order_book/levels.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace order_book {

LevelPool::Added LevelPool::create(std::vector<LevelEntry>& levels, std::size_t index,
                                   const AddOrder& order, std::uint16_t locate)
{
    std::uint32_t handle = 0;
    if (m_free.empty()) {
        handle = static_cast<std::uint32_t>(m_records.size());
        m_records.emplace_back();
    }
    else {
        handle = m_free.back();
        m_free.pop_back();
    }
    m_records[handle] = LevelRecord{
        .total = order.shares,
        .price = order.price,
        .orders = 1,
        .locate = locate,
        .side = order.side,
    };
    const auto best = index == levels.size();
    levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(index),
                  LevelEntry{.price = order.price, .level = handle});
    return {.level = handle, .changed = best};
}

} // namespace order_book
