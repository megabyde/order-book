#pragma once

// Builders for ITCH 5.0 messages, laid out as in the specification, for tests

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace itch {

// The builders take each message's fields in the specification's order and widths
// NOLINTBEGIN(bugprone-easily-swappable-parameters,readability-magic-numbers)

template <typename T> void put(std::string& out, T value)
{
    if constexpr (std::endian::native == std::endian::little) {
        value = std::byteswap(value);
    }
    std::array<char, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    out.append(bytes.data(), bytes.size());
}

inline std::string header(char type, std::uint16_t locate, std::uint64_t time)
{
    std::string out(1, type);
    put(out, locate);
    put(out, std::uint16_t{0});
    put(out, static_cast<std::uint16_t>(time >> 32U));
    put(out, static_cast<std::uint32_t>(time));
    return out;
}

inline std::string stock(std::string_view symbol)
{
    std::string out(symbol);
    out.resize(8, ' ');
    return out;
}

// A, or F with `attribution`
inline std::string add(std::uint16_t locate, std::uint64_t time, std::uint64_t order, char side,
                       std::uint32_t shares, std::string_view symbol, std::uint32_t price,
                       std::string_view attribution = {})
{
    auto out = header(attribution.empty() ? 'A' : 'F', locate, time);
    put(out, order);
    out += side;
    put(out, shares);
    out += stock(symbol);
    put(out, price);
    out += attribution;
    return out;
}

// E, or C with `price`
inline std::string executed(std::uint16_t locate, std::uint64_t time, std::uint64_t order,
                            std::uint32_t shares, std::uint32_t price = 0)
{
    auto out = header(price == 0 ? 'E' : 'C', locate, time);
    put(out, order);
    put(out, shares);
    put(out, std::uint64_t{1});
    if (price != 0) {
        out += 'Y';
        put(out, price);
    }
    return out;
}

inline std::string cancel(std::uint16_t locate, std::uint64_t time, std::uint64_t order,
                          std::uint32_t shares)
{
    auto out = header('X', locate, time);
    put(out, order);
    put(out, shares);
    return out;
}

inline std::string remove(std::uint16_t locate, std::uint64_t time, std::uint64_t order)
{
    auto out = header('D', locate, time);
    put(out, order);
    return out;
}

inline std::string replace(std::uint16_t locate, std::uint64_t time, std::uint64_t order,
                           std::uint64_t replacement, std::uint32_t shares, std::uint32_t price)
{
    auto out = header('U', locate, time);
    put(out, order);
    put(out, replacement);
    put(out, shares);
    put(out, price);
    return out;
}

// P
inline std::string trade(std::uint16_t locate, std::uint64_t time, std::uint32_t shares,
                         std::string_view symbol, std::uint32_t price)
{
    auto out = header('P', locate, time);
    put(out, std::uint64_t{0});
    out += 'B';
    put(out, shares);
    out += stock(symbol);
    put(out, price);
    put(out, std::uint64_t{2});
    return out;
}

// Q
inline std::string cross(std::uint16_t locate, std::uint64_t time, std::uint64_t shares,
                         std::string_view symbol, std::uint32_t price)
{
    auto out = header('Q', locate, time);
    put(out, shares);
    out += stock(symbol);
    put(out, price);
    put(out, std::uint64_t{3});
    out += 'C';
    return out;
}

// S, which the book skips
inline std::string system_event(std::uint64_t time, char code)
{
    return header('S', 0, time) + code;
}

// The message after its 2-byte big-endian length, as in an ITCH file
inline std::string framed(std::string_view message)
{
    std::string out;
    put(out, static_cast<std::uint16_t>(message.size()));
    out += message;
    return out;
}

// NOLINTEND(bugprone-easily-swappable-parameters,readability-magic-numbers)

} // namespace itch
