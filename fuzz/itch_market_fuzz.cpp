// Applies the operations the input encodes to an ItchMarket and to MarketModel, which must agree
// on every result, every BBO, and the numbers of live orders and price levels

#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/itch_market.hpp>
#include <order_book/order_book.hpp>

#include "market_model.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <span>

namespace {

using order_book::BookError;
using order_book::ItchKind;
using order_book::ItchMessage;

// Few enough reference numbers that operations collide, on both sides of a page, of a chunk, and of
// the end of the index, and the last one
constexpr std::array<std::uint64_t, 10> references{
    1,
    2,
    127,
    128,
    65'535,
    65'536,
    (std::uint64_t{1} << 32U) - 1,
    std::uint64_t{1} << 32U,
    std::uint64_t{1} << 63U,
    std::numeric_limits<std::uint64_t>::max(),
};

constexpr std::uint32_t max_shares = 100;
constexpr order_book::Price price_levels = 8;
constexpr order_book::Price tick = 10;

void check(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

// Reads the input front to back, one byte a value. An exhausted input reads as zeros.
class Input {
public:
    explicit Input(std::span<const std::uint8_t> data) : m_data(data) {}

    [[nodiscard]] bool empty() const { return m_data.empty(); }

    // A value in [0, n), for n of at most 256
    template <std::integral T> T below(T n)
    {
        if (m_data.empty()) {
            return 0;
        }
        const auto value = m_data.front();
        m_data = m_data.subspan(1);
        return static_cast<T>(value % n);
    }

private:
    std::span<const std::uint8_t> m_data;
};

class Session {
public:
    explicit Session(Input& input) : m_input(input) {}

    void step()
    {
        ItchMessage message{};
        message.locate = m_input.below<std::uint16_t>(4);
        message.order = reference();
        message.shares = m_input.below(max_shares + 1);
        message.price = (m_input.below(price_levels) + 1) * tick;
        m_market.prefetch(message.order, message.locate);
        m_market.prefetch_levels(message.order, message.locate);

        const auto before = m_model.bbo(message.locate);
        const auto want = expect(message);
        const auto result = m_market.apply(message);
        check(result.has_value() == want.has_value());
        if (want) {
            check(*result == (m_model.bbo(message.locate) != before));
        }
        else {
            check(result.error() == want.error());
        }
        check(m_market.bbo(message.locate) == m_model.bbo(message.locate));
        check(m_market.order_count() == m_model.orders().size());
        check(m_market.level_count() == m_model.level_count());
    }

private:
    std::uint64_t reference() { return references.at(m_input.below(references.size())); }

    // Applies `message` to the model, filling in what the kind needs; the error the market must
    // report, if any
    std::expected<void, BookError> expect(ItchMessage& message)
    {
        const auto locate = message.locate;
        const auto order = message.order;
        switch (m_input.below(4)) {
        case 0:
            message.kind = ItchKind::Add;
            message.side = m_input.below(2) == 0 ? order_book::Side::Buy : order_book::Side::Sell;
            if (m_model.live(locate, order)) {
                return std::unexpected(BookError::DuplicateOrder);
            }
            m_model.add(locate, order, message.side, message.price, message.shares);
            return {};
        case 1:
            message.kind = ItchKind::Execute;
            if (!m_model.live(locate, order)) {
                return std::unexpected(BookError::UnknownOrder);
            }
            m_model.take(locate, order,
                         std::min<order_book::Quantity>(
                             message.shares, m_model.orders().at({locate, order}).remaining));
            return {};
        case 2:
            message.kind = ItchKind::Delete;
            if (!m_model.live(locate, order)) {
                return std::unexpected(BookError::UnknownOrder);
            }
            m_model.take(locate, order, m_model.orders().at({locate, order}).remaining);
            return {};
        default: {
            message.kind = ItchKind::Replace;
            message.replacement = reference();
            if (!m_model.live(locate, order)) {
                return std::unexpected(BookError::UnknownOrder);
            }
            if (message.replacement != order && m_model.live(locate, message.replacement)) {
                return std::unexpected(BookError::DuplicateOrder);
            }
            const auto resting = m_model.orders().at({locate, order});
            m_model.take(locate, order, resting.remaining);
            m_model.add(locate, message.replacement, resting.side, message.price, message.shares);
            return {};
        }
        }
    }

    Input& m_input;
    order_book::ItchMarket m_market;
    MarketModel m_model;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Input input({data, size});
    Session session(input);
    while (!input.empty()) {
        session.step();
    }
    return 0;
}
