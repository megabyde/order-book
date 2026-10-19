#pragma once

// A reference model of ItchMarket, for tests and fuzzing

#include <order_book/feed.hpp>
#include <order_book/levels.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

// The BBO of each locate from a model that keeps every level's total in a std::map
class MarketModel {
public:
    void add(std::uint16_t locate, std::uint64_t order, order_book::Side side,
             order_book::Price price, order_book::Quantity shares)
    {
        m_orders[{locate, order}] = Resting{.side = side, .price = price, .remaining = shares};
        auto& level = levels(locate, side)[price];
        level.total += shares;
        ++level.orders;
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void take(std::uint16_t locate, std::uint64_t order, order_book::Quantity shares)
    {
        auto& resting = m_orders.at({locate, order});
        auto& side = levels(locate, resting.side);
        auto& level = side.at(resting.price);
        level.total -= shares;
        resting.remaining -= shares;
        if (resting.remaining == 0) {
            if (--level.orders == 0) {
                side.erase(resting.price);
            }
            m_orders.erase({locate, order});
        }
    }
    [[nodiscard]] order_book::Bbo bbo(std::uint16_t locate)
    {
        order_book::Bbo result;
        if (const auto& bids = levels(locate, order_book::Side::Buy); !bids.empty()) {
            result.bid = order_book::Level{
                .price = bids.rbegin()->first,
                .quantity = bids.rbegin()->second.total,
            };
        }
        if (const auto& asks = levels(locate, order_book::Side::Sell); !asks.empty()) {
            result.ask = order_book::Level{
                .price = asks.begin()->first,
                .quantity = asks.begin()->second.total,
            };
        }
        return result;
    }
    [[nodiscard]] const auto& orders() const { return m_orders; }
    [[nodiscard]] bool live(std::uint16_t locate, std::uint64_t order) const
    {
        return m_orders.contains({locate, order});
    }

private:
    struct Resting {
        order_book::Side side;
        order_book::Price price;
        order_book::Quantity remaining;
    };
    struct Total {
        order_book::Quantity total = 0;
        std::size_t orders = 0;
    };
    std::map<order_book::Price, Total>& levels(std::uint16_t locate, order_book::Side side)
    {
        return m_levels[{locate, side}];
    }
    std::map<std::pair<std::uint16_t, std::uint64_t>, Resting> m_orders;
    std::map<std::pair<std::uint16_t, order_book::Side>, std::map<order_book::Price, Total>>
        m_levels;
};
