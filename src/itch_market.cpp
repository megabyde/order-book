#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/itch_market.hpp>
#include <order_book/levels.hpp>
#include <order_book/order_book.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace order_book {

namespace {

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
    return reduce(message.order, *resting, std::min(message.shares, resting->remaining));
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
    return locate < m_books.size() ? m_levels.bbo(m_books[locate]) : Bbo{};
}

std::expected<bool, BookError> ItchMarket::add(const ItchMessage& message)
{
    Order* resting = nullptr;
    std::uint8_t* live = nullptr;
    if (message.order < paged_orders) {
        const auto chunk_index = message.order >> (page_bits + chunk_bits);
        if (chunk_index >= m_chunks.size()) {
            m_chunks.resize(chunk_index + 1);
        }
        if (!m_chunks[chunk_index]) {
            m_chunks[chunk_index] = std::make_unique<Chunk>();
        }
        auto& chunk = *m_chunks[chunk_index];
        const auto slot = page_slot(message.order);
        auto& page = chunk.pages[slot];
        if (!page) {
            if (m_spare_pages.empty()) {
                page = std::make_unique<Page>();
            }
            else {
                page = std::move(m_spare_pages.back());
                m_spare_pages.pop_back();
            }
        }
        resting = &(*page)[message.order & (page_size - 1)];
        live = &chunk.live[slot];
    }
    const std::pair key{message.order, message.locate};
    const auto overflow = resting == nullptr || (resting->level != 0 &&
                                                 m_levels[resting->level].locate != message.locate);
    if (overflow) {
        const auto [entry, inserted] = m_overflow.try_emplace(key);
        if (!inserted) {
            return std::unexpected(BookError::DuplicateOrder);
        }
        resting = &entry->second;
    }
    else if (resting->level != 0 || (!m_overflow.empty() && m_overflow.contains(key))) {
        return std::unexpected(BookError::DuplicateOrder);
    }

    const AddOrder add{.side = message.side, .price = message.price, .shares = message.shares};
    const auto [level, changed] = m_levels.add(book(message.locate), add, message.locate);
    *resting = Order{.remaining = message.shares, .level = level};
    if (!overflow) {
        ++*live;
    }
    ++m_order_count;
    return changed;
}

bool ItchMarket::reduce(std::uint64_t order, Order& resting, Shares quantity)
{
    const auto locate = m_levels[resting.level].locate;
    resting.remaining -= quantity;
    const auto removed = resting.remaining == 0;
    const auto changed = m_levels.reduce(m_books[locate], resting.level, quantity, removed);
    if (!removed) {
        return changed;
    }

    --m_order_count;
    // A live order below paged_orders has its chunk, whether it is in the index or in m_overflow
    if (order < paged_orders) {
        auto& chunk = this->chunk(order);
        const auto slot = page_slot(order);
        auto& page = chunk.pages[slot];
        if (page && &resting == &(*page)[order & (page_size - 1)]) {
            resting.level = 0;
            if (--chunk.live[slot] == 0) {
                m_spare_pages.push_back(std::move(page));
            }
            return changed;
        }
    }
    m_overflow.erase(std::pair{order, locate});
    return changed;
}

// A reference number and a locate, which the check sees as swappable
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ItchMarket::Order* ItchMarket::find(std::uint64_t order, std::uint16_t locate)
{
    const auto chunk = order >> (page_bits + chunk_bits);
    if (chunk < m_chunks.size() && m_chunks[chunk]) {
        if (const auto& page = m_chunks[chunk]->pages[page_slot(order)]) {
            auto& resting = (*page)[order & (page_size - 1)];
            if (resting.level != 0 && m_levels[resting.level].locate == locate) {
                return &resting;
            }
        }
    }
    if (m_overflow.empty()) {
        return nullptr;
    }
    const auto found = m_overflow.find(std::pair{order, locate});
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
    const auto& page = m_chunks[chunk]->pages[page_slot(order)];
    return page ? &(*page)[order & (page_size - 1)] : nullptr;
}

ItchMarket::Chunk& ItchMarket::chunk(std::uint64_t order)
{
    return *m_chunks[order >> (page_bits + chunk_bits)];
}

BookLevels& ItchMarket::book(std::uint16_t locate)
{
    if (locate >= m_books.size()) {
        m_books.resize(locate + std::size_t{1});
    }
    return m_books[locate];
}

} // namespace order_book
