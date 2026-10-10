#include <order_book/feed.hpp>
#include <order_book/order_book.hpp>
#include <order_book/replay.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <unordered_map>

namespace order_book {

namespace {

// Input is read, and output written, in blocks of about this size rather than a line at a time
constexpr std::size_t block_size = std::size_t{1} << 16U;

// Splits a stream into LF-terminated lines without copying each one out
class LineReader {
public:
    explicit LineReader(std::istream& in) : m_source(in.rdbuf()), m_done(!in) {}

    // The next line without its LF, or nothing at the end of the stream. The view dangles at the
    // next call.
    std::optional<std::string_view> next()
    {
        while (true) {
            const std::string_view rest(m_buffer.data() + m_begin, m_end - m_begin);
            if (const auto newline = rest.find('\n'); newline != std::string_view::npos) {
                m_begin += newline + 1;
                return rest.substr(0, newline);
            }
            if (m_done) {
                m_begin = m_end;
                return rest.empty() ? std::nullopt : std::optional(rest);
            }
            // Move the partial line to the front, and grow the buffer if the line fills it
            std::memmove(m_buffer.data(), rest.data(), rest.size());
            m_begin = 0;
            m_end = rest.size();
            if (m_end == m_buffer.size()) {
                m_buffer.resize(m_buffer.size() * 2);
            }
            // A stream buffer may return less than asked before its end, so only an empty read ends
            // the input
            const auto count = m_source->sgetn(
                m_buffer.data() + m_end, static_cast<std::streamsize>(m_buffer.size() - m_end));
            m_done = count == 0;
            m_end += static_cast<std::size_t>(count);
        }
    }

private:
    std::streambuf* m_source;
    bool m_done;
    std::string m_buffer = std::string(block_size, '\0');
    std::size_t m_begin = 0;
    std::size_t m_end = 0;
};

void append_uint(std::string& buffer, std::uint64_t value)
{
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> digits{};
    auto* const end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    buffer.append(digits.data(), end);
}

void append_level(std::string& buffer, const std::optional<Level>& level)
{
    if (level) {
        append_uint(buffer, level->price);
        buffer += ',';
        append_uint(buffer, level->quantity);
    }
    else {
        buffer += ',';
    }
}

std::string describe(BookError error, std::string_view order)
{
    return std::format("{} order '{}'",
                       error == BookError::DuplicateOrder ? "duplicate" : "unknown", order);
}

std::invalid_argument line_error(std::size_t number, std::string_view message)
{
    const auto what = std::format("line {}: {}", number, message);
    return std::invalid_argument(what);
}

} // namespace

std::size_t replay(std::istream& in, std::ostream& out)
{
    std::unordered_map<std::string, OrderBook, StringHash, std::equal_to<>> books;
    LineReader lines(in);
    std::string buffer;
    std::size_t number = 1;
    std::size_t events = 0;
    const auto flush = [&] {
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        buffer.clear();
    };
    // Output up to the failing line is written before the error propagates
    const auto fail = [&](std::string_view message) {
        flush();
        return line_error(number, message);
    };

    // Skip the header
    lines.next();
    while (auto line = lines.next()) {
        ++number;
        if (line->ends_with('\r')) {
            line->remove_suffix(1);
        }
        if (line->find_last_not_of(" \t\n\v\f\r") == std::string_view::npos) {
            continue;
        }

        const auto message = parse_line(*line);
        if (!message) {
            throw fail(message.error());
        }
        auto book = books.find(message->ticker);
        if (book == books.end()) {
            book = books.emplace(std::string(message->ticker), OrderBook{}).first;
        }

        const auto before = book->second.bbo();
        if (const auto applied = book->second.apply(message->order, message->event); !applied) {
            throw fail(describe(applied.error(), message->order));
        }
        if (const auto after = book->second.bbo(); after != before) {
            append_uint(buffer, message->time);
            buffer += ',';
            buffer += message->ticker;
            buffer += ',';
            append_level(buffer, after.bid);
            buffer += ',';
            append_level(buffer, after.ask);
            buffer += '\n';
            if (buffer.size() >= block_size) {
                flush();
            }
        }
        ++events;
    }
    flush();
    return events;
}

} // namespace order_book
