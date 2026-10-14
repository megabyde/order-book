#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/order_book.hpp>

namespace order_book {

/**
 * The books of every stock locate in an ITCH feed, with their orders indexed by reference number
 *
 * ITCH names each order by a reference number that is unique for the day across all stocks and
 * assigned in sequence, so orders live in pages of consecutive reference numbers, each found from
 * the reference number through a two-level directory that holds only the ranges in use, and the
 * recent orders that most messages name share a few pages. Each level is a pooled record whose
 * handle its orders keep, so executions and deletes reach it directly, and each side of a book is a
 * vector of prices and handles, worst price first, so its best level is the last one.
 *
 * Each book keeps its own reference numbers, as the CSV books keep their order IDs, so that which
 * locates share a market does not change what a feed means. An order whose reference number names
 * a live order of another locate goes to a hash map keyed by both instead of the index.
 */
class ItchMarket {
public:
    /// Apply `message` to its locate's book, returning whether that book's best bid or offer
    /// changed; on error nothing changes. An add or a replacement fails with DuplicateOrder if its
    /// reference number names a live order of the message's locate, and an execution, delete, or
    /// replace fails with UnknownOrder if its order is not live in that locate. Trades and skipped
    /// messages change nothing. Messages are expected from the decoder, so their shares fit in 32
    /// bits.
    [[nodiscard]] std::expected<bool, BookError> apply(const ItchMessage& message);
    /// apply() for an Add, without dispatching on the kind
    [[nodiscard]] std::expected<bool, BookError> add(const ItchMessage& message);
    /// apply() for an Execute
    [[nodiscard]] std::expected<bool, BookError> execute(const ItchMessage& message);
    /// apply() for a Delete
    [[nodiscard]] std::expected<bool, BookError> remove(const ItchMessage& message);
    /// apply() for a Replace
    [[nodiscard]] std::expected<bool, BookError> replace(const ItchMessage& message);
    /// Start loading the index entry of `order` and the book of `locate`, so that a message naming
    /// them a few messages later does not wait for memory
    void prefetch(std::uint64_t order, std::uint16_t locate) const;
    /// Start loading the level of `order` and the best levels of `locate`'s book, once prefetch()
    /// has loaded the index entry and the book that point to them
    void prefetch_levels(std::uint64_t order, std::uint16_t locate) const;
    /// Current best bid and offer of `locate`
    [[nodiscard]] Bbo bbo(std::uint16_t locate) const;
    /// Number of live orders in every book
    [[nodiscard]] std::size_t order_count() const { return m_order_count; }
    /// Number of price levels in every book
    [[nodiscard]] std::size_t level_count() const
    {
        return m_levels.size() - 1 - m_free_levels.size();
    }

private:
    // A level, with the book and side it belongs to, so that an order need not repeat them
    struct LevelRecord {
        Quantity total;
        Price price;
        std::uint32_t orders;
        std::uint16_t locate;
        Side side;
    };
    // An element of a side's levels, worst price first: the price beside the level's handle, so
    // that finding a price reads no level record
    struct Entry {
        Price price;
        std::uint32_t level;
    };
    struct Book {
        std::vector<Entry> bids;
        std::vector<Entry> asks;
    };
    // A resting order in 8 bytes, as ITCH gives shares in 32 bits; level 0 marks an entry of the
    // index that holds none
    struct Order {
        std::uint32_t remaining;
        std::uint32_t level;
    };
    // Small pages, because each worker keeps only the locates assigned to it, so its orders are a
    // sparse subset of every range of reference numbers, and long-lived orders keep pages alive
    static constexpr unsigned page_bits = 8;
    static constexpr std::size_t page_size = std::size_t{1} << page_bits;
    // The orders of page_size consecutive reference numbers
    struct Page {
        std::array<Order, page_size> orders{};
        std::uint32_t live = 0;
    };
    // The pages of chunk_size consecutive page numbers. The index makes a chunk only for a page it
    // uses, so a high reference number costs one chunk rather than a slot for every page below it.
    static constexpr unsigned chunk_bits = 8;
    static constexpr std::size_t chunk_size = std::size_t{1} << chunk_bits;
    using Chunk = std::array<std::unique_ptr<Page>, chunk_size>;

    // Take `quantity` off `resting`, the order `order`, removing it and then its level once they
    // reach 0; returns whether its book's best bid or offer changed. Removing an order of
    // m_overflow invalidates `resting`.
    bool reduce(std::uint64_t order, Order& resting, std::uint32_t quantity);
    // The live order `order` of `locate`, or nullptr
    [[nodiscard]] Order* find(std::uint64_t order, std::uint16_t locate);
    // The index entry of `order`, live or not, or nullptr if its page does not exist
    [[nodiscard]] const Order* indexed(std::uint64_t order) const;
    // The slot in m_chunks of the page holding `order`, whose chunk exists
    [[nodiscard]] std::unique_ptr<Page>& page(std::uint64_t order);
    [[nodiscard]] std::vector<Entry>& side(std::uint16_t locate, Side side);
    // Where the level at `price` is in `levels`, worst first, or where it would be inserted:
    // levels[result - 1] is the first from the best that is not better than `price`
    [[nodiscard]] static std::size_t position(const std::vector<Entry>& levels, Side side,
                                              Price price);

    std::vector<Book> m_books;
    // Handle 0 is never used, so that 0 can mean none
    std::vector<LevelRecord> m_levels = std::vector<LevelRecord>(1);
    std::vector<std::uint32_t> m_free_levels;
    // Chunks by the reference numbers' bits above both a page's and a chunk's; never freed
    std::vector<std::unique_ptr<Chunk>> m_chunks;
    // Emptied pages, kept for reuse
    std::vector<std::unique_ptr<Page>> m_spare_pages;
    // Orders whose slot in the index holds a live order of another locate, by reference number and
    // locate. Empty for a feed whose reference numbers are unique for the day, as ITCH's are.
    std::unordered_map<std::uint64_t, Order> m_overflow;
    std::size_t m_order_count = 0;
};

} // namespace order_book
