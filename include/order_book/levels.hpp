#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <order_book/feed.hpp>

namespace order_book {

/// Price and total resting quantity of one level
struct Level {
    Price price;       ///< Level price
    Quantity quantity; ///< Sum of the remaining quantity of every order at this price
    /// Same price and quantity
    friend bool operator==(const Level&, const Level&) = default;
};

/// Best bid and offer; an empty side has no level
struct Bbo {
    std::optional<Level> bid; ///< Highest bid level
    std::optional<Level> ask; ///< Lowest ask level
    /// Same levels on both sides
    friend bool operator==(const Bbo&, const Bbo&) = default;
};

/// The orders resting at one price: their total, their count, and the book and side they are in
struct LevelRecord {
    Quantity total;       ///< Sum of the remaining quantity of the level's orders
    Price price;          ///< Level price
    std::uint32_t orders; ///< Number of orders at the level
    std::uint16_t locate; ///< Book the level belongs to, where a pool serves several; 0 otherwise
    Side side;            ///< Side the level is on
};

/// An element of a side's levels: the price beside the level's handle, so that finding a price
/// reads no level record
struct LevelEntry {
    Price price;         ///< Level price
    std::uint32_t level; ///< Handle of the level in its pool
};

/// One book's levels, each side worst price first, so that its best level is the last element and
/// most adds and removals touch only the end
struct BookLevels {
    std::vector<LevelEntry> bids; ///< Bid levels, highest price last
    std::vector<LevelEntry> asks; ///< Ask levels, lowest price last
};

/**
 * Level records for one or more books, reached by handle
 *
 * An order keeps its level's handle, so an execution or delete reaches the level without a search.
 * Handle 0 is never given out, so that 0 can mean none. Removed levels' handles are reused.
 */
class LevelPool {
public:
    /// The level an add went to, and whether it changed its book's best bid or offer
    struct Added {
        std::uint32_t level; ///< Handle of the level
        bool changed;        ///< Whether the best bid or offer changed
    };

    /// Add `order` to `book`, whose levels are in this pool, creating its level if the
    /// price is new; `locate` names the book where the pool serves several
    Added add(BookLevels& book, const AddOrder& order, std::uint16_t locate = 0);
    /// Take `quantity` off level `level` of `book`, and one order off it if `removed`, removing the
    /// level once it has no orders; returns whether the best bid or offer changed
    bool reduce(BookLevels& book, std::uint32_t level, Quantity quantity, bool removed);
    /// Best bid and offer of `book`
    [[nodiscard]] Bbo bbo(const BookLevels& book) const;

    /// The record of level `level`
    [[nodiscard]] const LevelRecord& operator[](std::uint32_t level) const
    {
        return m_records[level];
    }
    /// Number of levels in every book
    [[nodiscard]] std::size_t size() const { return m_records.size() - 1 - m_free.size(); }

private:
    // Whether `lhs` is a worse price than `rhs` on `side`: lower for bids, higher for asks
    [[nodiscard]] static bool worse(Side side, Price lhs, Price rhs)
    {
        return side == Side::Buy ? lhs < rhs : lhs > rhs;
    }
    // Where the level at `price` is in `levels`, worst first, or where it would be inserted:
    // levels[result - 1] is the first from the best that is not better than `price`
    [[nodiscard]] static std::size_t position(const std::vector<LevelEntry>& levels, Side side,
                                              Price price);

    std::vector<LevelRecord> m_records = std::vector<LevelRecord>(1);
    std::vector<std::uint32_t> m_free;
};

inline LevelPool::Added LevelPool::add(BookLevels& book, const AddOrder& order,
                                       std::uint16_t locate)
{
    const auto [side, price, shares] = order;
    auto& levels = side == Side::Buy ? book.bids : book.asks;
    auto index = position(levels, side, price);
    std::uint32_t handle = 0;
    const auto created = index == 0 || levels[index - 1].price != price;
    if (created) {
        if (m_free.empty()) {
            handle = static_cast<std::uint32_t>(m_records.size());
            m_records.emplace_back();
        }
        else {
            handle = m_free.back();
            m_free.pop_back();
        }
        m_records[handle] =
            LevelRecord{.total = 0, .price = price, .orders = 0, .locate = locate, .side = side};
        levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(index),
                      LevelEntry{.price = price, .level = handle});
        ++index;
    }
    else {
        handle = levels[index - 1].level;
    }
    auto& level = m_records[handle];
    level.total += shares;
    ++level.orders;
    return {.level = handle, .changed = index == levels.size() && (created || shares != 0)};
}

// A handle and a quantity, which the check sees as swappable
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline bool LevelPool::reduce(BookLevels& book, std::uint32_t level, Quantity quantity,
                              bool removed)
{
    auto& record = m_records[level];
    auto& levels = record.side == Side::Buy ? book.bids : book.asks;
    const auto best = levels.back().level == level;
    // Only the best level shows in the BBO, and only if its total changes or it goes away
    auto changed = best && quantity != 0;
    record.total -= quantity;
    if (removed && --record.orders == 0) {
        changed = best;
        if (best) {
            levels.pop_back();
        }
        else {
            const auto index = position(levels, record.side, record.price);
            levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(index - 1));
        }
        m_free.push_back(level);
    }
    return changed;
}

inline Bbo LevelPool::bbo(const BookLevels& book) const
{
    Bbo bbo;
    if (!book.bids.empty()) {
        const auto& best = book.bids.back();
        bbo.bid = Level{.price = best.price, .quantity = m_records[best.level].total};
    }
    if (!book.asks.empty()) {
        const auto& best = book.asks.back();
        bbo.ask = Level{.price = best.price, .quantity = m_records[best.level].total};
    }
    return bbo;
}

inline std::size_t LevelPool::position(const std::vector<LevelEntry>& levels, Side side,
                                       Price price)
{
    // Most adds and removals are near the top of the book, which is the end of the vector
    auto index = levels.size();
    while (index > 0 && worse(side, price, levels[index - 1].price)) {
        --index;
    }
    return index;
}

} // namespace order_book
