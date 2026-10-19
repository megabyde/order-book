// Applies the operations the input encodes to an ItchMarket and to MarketModel, which must agree
// on every result, every BBO, and the number of live orders

#include <order_book/feed.hpp>
#include <order_book/itch.hpp>
#include <order_book/itch_market.hpp>
#include <order_book/order_book.hpp>

#include "market_model.hpp"

#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>

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

void check(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

class Session {
public:
    explicit Session(FuzzedDataProvider& input) : m_input(input) {}

    void step()
    {
        ItchMessage message{};
        message.locate = m_input.ConsumeIntegralInRange<std::uint16_t>(0, 3);
        message.order = reference();
        message.shares = m_input.ConsumeIntegralInRange<std::uint32_t>(0, 100);
        message.price = m_input.ConsumeIntegralInRange<order_book::Price>(1, 8) * 10;
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
    }

private:
    std::uint64_t reference() { return m_input.PickValueInArray(references); }

    // Applies `message` to the model, filling in what the kind needs; the error the market must
    // report, if any
    std::expected<void, BookError> expect(ItchMessage& message)
    {
        const auto locate = message.locate;
        const auto order = message.order;
        switch (m_input.ConsumeIntegralInRange(0, 3)) {
        case 0:
            message.kind = ItchKind::Add;
            message.side = m_input.ConsumeBool() ? order_book::Side::Buy : order_book::Side::Sell;
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
                         std::min(message.shares, m_model.orders().at({locate, order}).remaining));
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

    FuzzedDataProvider& m_input;
    order_book::ItchMarket m_market;
    MarketModel m_model;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    FuzzedDataProvider input(data, size);
    Session session(input);
    while (input.remaining_bytes() > 0) {
        session.step();
    }
    return 0;
}
