#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/itch_market.hpp>
#include <order_book/order_book.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace order_book {

namespace {

// Whether `lhs` is a worse price than `rhs` on `side`: lower for bids, higher for asks
bool worse(Side side, Price lhs, Price rhs)
{
    return side == Side::Buy ? lhs < rhs : lhs > rhs;
}

constexpr unsigned locate_bits = std::numeric_limits<std::uint16_t>::digits;

// Key of m_overflow. Reference numbers are below max_itch_order, so both fit.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::uint64_t overflow_key(std::uint64_t order, std::uint16_t locate)
{
    return (order << locate_bits) | locate;
}

// Ask for the cache line holding `address`. A prefetch is a hint: it cannot fault and does not
// change what the program computes, so a line that turns out not to be needed costs only the fill.
void prefetch_line(const void* address)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(address);
#else
    static_cast<void>(address);
#endif
}

} // namespace

std::expected<bool, BookError> ItchMarket::apply(const ItchMessage& message)
{
    switch (message.kind) {
    case ItchKind::Add:
        return add(message);
    case ItchKind::Execute:
        return execute(message);
    case ItchKind::Delete:
        return remove(message);
    case ItchKind::Replace:
        return replace(message);
    case ItchKind::Trade:
    case ItchKind::Skipped:
        break;
    }
    return false;
}

std::expected<bool, BookError> ItchMarket::execute(const ItchMessage& message)
{
    auto* const resting = find(message.order, message.locate);
    if (resting == nullptr) {
        return std::unexpected(BookError::UnknownOrder);
    }
    // The decoder reads shares from 32 bits, so the minimum fits an order's remaining shares
    return reduce(
        message.order, *resting,
        static_cast<std::uint32_t>(std::min<Quantity>(message.shares, resting->remaining)));
}

std::expected<bool, BookError> ItchMarket::remove(const ItchMessage& message)
{
    auto* const resting = find(message.order, message.locate);
    if (resting == nullptr) {
        return std::unexpected(BookError::UnknownOrder);
    }
    return reduce(message.order, *resting, resting->remaining);
}

std::expected<bool, BookError> ItchMarket::replace(const ItchMessage& message)
{
    auto* const resting = find(message.order, message.locate);
    if (resting == nullptr) {
        return std::unexpected(BookError::UnknownOrder);
    }
    if (message.replacement != message.order &&
        find(message.replacement, message.locate) != nullptr) {
        return std::unexpected(BookError::DuplicateOrder);
    }
    const auto before = bbo(message.locate);
    auto replacement = message;
    replacement.order = message.replacement;
    replacement.side = m_levels[resting->level].side;
    reduce(message.order, *resting, resting->remaining);
    // Cannot fail: the replacement was not live in this locate, and the order it replaces is gone
    static_cast<void>(add(replacement).value());
    return bbo(message.locate) != before;
}

Bbo ItchMarket::bbo(std::uint16_t locate) const
{
    Bbo bbo;
    if (locate >= m_books.size()) {
        return bbo;
    }
    const auto& book = m_books[locate];
    if (!book.bids.empty()) {
        const auto& best = book.bids.back();
        bbo.bid = Level{.price = best.price, .quantity = m_levels[best.level].total};
    }
    if (!book.asks.empty()) {
        const auto& best = book.asks.back();
        bbo.ask = Level{.price = best.price, .quantity = m_levels[best.level].total};
    }
    return bbo;
}

std::expected<bool, BookError> ItchMarket::add(const ItchMessage& message)
{
    const auto chunk_index = message.order >> (page_bits + chunk_bits);
    if (chunk_index >= m_chunks.size()) {
        m_chunks.resize(chunk_index + 1);
    }
    if (!m_chunks[chunk_index]) {
        m_chunks[chunk_index] = std::make_unique<Chunk>();
    }
    auto& page = this->page(message.order);
    if (!page) {
        if (m_spare_pages.empty()) {
            page = std::make_unique<Page>();
        }
        else {
            page = std::move(m_spare_pages.back());
            m_spare_pages.pop_back();
        }
    }
    auto* resting = &page->orders[message.order & (page_size - 1)];
    const auto overflow = resting->level != 0 && m_levels[resting->level].locate != message.locate;
    if (overflow) {
        const auto [entry, inserted] =
            m_overflow.try_emplace(overflow_key(message.order, message.locate));
        if (!inserted) {
            return std::unexpected(BookError::DuplicateOrder);
        }
        resting = &entry->second;
    }
    else if (resting->level != 0 || (!m_overflow.empty() && m_overflow.contains(overflow_key(
                                                                message.order, message.locate)))) {
        return std::unexpected(BookError::DuplicateOrder);
    }

    auto& levels = side(message.locate, message.side);
    auto index = position(levels, message.side, message.price);
    std::uint32_t handle = 0;
    const auto created = index == 0 || levels[index - 1].price != message.price;
    if (created) {
        if (m_free_levels.empty()) {
            handle = static_cast<std::uint32_t>(m_levels.size());
            m_levels.emplace_back();
        }
        else {
            handle = m_free_levels.back();
            m_free_levels.pop_back();
        }
        m_levels[handle] = LevelRecord{
            .total = 0,
            .price = message.price,
            .orders = 0,
            .locate = message.locate,
            .side = message.side,
        };
        levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(index),
                      Entry{.price = message.price, .level = handle});
        ++index;
    }
    else {
        handle = levels[index - 1].level;
    }
    auto& level = m_levels[handle];
    level.total += message.shares;
    ++level.orders;

    *resting = Order{.remaining = static_cast<std::uint32_t>(message.shares), .level = handle};
    if (!overflow) {
        ++page->live;
    }
    ++m_order_count;
    return index == levels.size() && (created || message.shares != 0);
}

bool ItchMarket::reduce(std::uint64_t order, Order& resting, std::uint32_t quantity)
{
    auto& level = m_levels[resting.level];
    auto& levels = side(level.locate, level.side);
    const auto best = levels.back().level == resting.level;
    // Only the best level shows in the BBO, and only if its total changes or it goes away
    auto changed = best && quantity != 0;
    level.total -= quantity;
    resting.remaining -= quantity;
    if (resting.remaining != 0) {
        return changed;
    }

    if (--level.orders == 0) {
        changed = best;
        if (best) {
            levels.pop_back();
        }
        else {
            const auto index = position(levels, level.side, level.price);
            levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(index - 1));
        }
        m_free_levels.push_back(resting.level);
    }
    --m_order_count;
    // A live order's chunk exists, whether the order is in the index or in m_overflow
    auto& page = this->page(order);
    if (page && &resting == &page->orders[order & (page_size - 1)]) {
        resting.level = 0;
        if (--page->live == 0) {
            m_spare_pages.push_back(std::move(page));
        }
    }
    else {
        m_overflow.erase(overflow_key(order, level.locate));
    }
    return changed;
}

// A reference number and a locate, which the check sees as swappable
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ItchMarket::Order* ItchMarket::find(std::uint64_t order, std::uint16_t locate)
{
    const auto chunk = order >> (page_bits + chunk_bits);
    if (chunk < m_chunks.size() && m_chunks[chunk]) {
        if (const auto& page = this->page(order)) {
            auto& resting = page->orders[order & (page_size - 1)];
            if (resting.level != 0 && m_levels[resting.level].locate == locate) {
                return &resting;
            }
        }
    }
    if (m_overflow.empty()) {
        return nullptr;
    }
    const auto found = m_overflow.find(overflow_key(order, locate));
    return found == m_overflow.end() ? nullptr : &found->second;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ItchMarket::prefetch(std::uint64_t order, std::uint16_t locate) const
{
    if (const auto* const resting = indexed(order)) {
        prefetch_line(resting);
    }
    if (locate < m_books.size()) {
        prefetch_line(&m_books[locate]);
    }
}

// The best entries are read here rather than prefetched, since prefetch() could only fetch the
// book, whose vectors point to them. That read may miss, but only these prefetches wait for it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ItchMarket::prefetch_levels(std::uint64_t order, std::uint16_t locate) const
{
    if (const auto* const resting = indexed(order); resting != nullptr && resting->level != 0) {
        prefetch_line(&m_levels[resting->level]);
    }
    if (locate < m_books.size()) {
        for (const auto* const levels : {&m_books[locate].bids, &m_books[locate].asks}) {
            if (!levels->empty()) {
                prefetch_line(&m_levels[levels->back().level]);
            }
        }
    }
}

// The lookup reads the directory's chunk and page pointers itself. They are in cache for the
// recent reference numbers that most messages name, and for an older order only the prefetch
// waits on them.
const ItchMarket::Order* ItchMarket::indexed(std::uint64_t order) const
{
    const auto chunk = order >> (page_bits + chunk_bits);
    if (chunk >= m_chunks.size() || !m_chunks[chunk]) {
        return nullptr;
    }
    const auto& page = (*m_chunks[chunk])[(order >> page_bits) & (chunk_size - 1)];
    return page ? &page->orders[order & (page_size - 1)] : nullptr;
}

std::unique_ptr<ItchMarket::Page>& ItchMarket::page(std::uint64_t order)
{
    return (*m_chunks[order >> (page_bits + chunk_bits)])[(order >> page_bits) & (chunk_size - 1)];
}

std::vector<ItchMarket::Entry>& ItchMarket::side(std::uint16_t locate, Side side)
{
    if (locate >= m_books.size()) {
        m_books.resize(locate + std::size_t{1});
    }
    auto& book = m_books[locate];
    return side == Side::Buy ? book.bids : book.asks;
}

std::size_t ItchMarket::position(const std::vector<Entry>& levels, Side side, Price price)
{
    // Most adds and removals are near the top of the book, which is the end of the vector
    auto index = levels.size();
    while (index > 0 && worse(side, price, levels[index - 1].price)) {
        --index;
    }
    return index;
}

} // namespace order_book
