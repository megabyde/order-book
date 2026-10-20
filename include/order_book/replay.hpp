#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>

namespace order_book {

/// Encoding of the feed that replay() reads
enum class Format : std::uint8_t {
    Csv,  ///< A header line, then one event per line in the format parse_line() reads
    Itch, ///< Nasdaq TotalView-ITCH 5.0 messages, each after its 2-byte big-endian length
};

/// Replay an exchange feed and write the best bid and offer of a ticker whenever it changes
///
/// `in` holds a header line, then one event per line in the format Event parses; a trailing CR
/// and blank lines are skipped. Each change writes `<TIME>,<TICKER>,<BBP>,<BBQ>,<BAP>,<BAQ>` to
/// `out`, with empty fields for an empty side. Returns the number of events applied, trades
/// included. Throws std::invalid_argument prefixed with `line N: ` on the first line that cannot
/// be parsed or applied; output up to that line has already been written.
///
/// Throws std::runtime_error if `out` fails, including on the flush that ends a replay. If a read
/// of `in`'s stream buffer throws, the whole records before it are replayed, a record it cut short
/// is not, and the exception is rethrown, unless an earlier record failed. libstdc++'s
/// std::filebuf throws on a read error; libc++'s and MSVC's report it as the end of the file,
/// which ends the replay as the end would.
///
/// The input is read in batches of at least 1 MiB, several ahead of the output, so `in` should be a
/// file or a stream that reaches its end: a producer that waits for output before writing more
/// can stall. Uses the platform default worker count; see the overload taking `format`.
std::size_t replay(std::istream& in, std::ostream& out);

/// Replay as above with `threads` worker threads, at least 1
///
/// Each worker keeps the books of the tickers whose names hash to it, and the calling thread
/// reads the input and writes the output in line order, so the output and the exception do not
/// depend on `threads`. A feed with fewer tickers than workers leaves the rest idle.
std::size_t replay(std::istream& in, std::ostream& out, std::size_t threads);

/// Replay a feed in `format` with the platform default worker count
///
/// Uses one worker per hardware thread, at least 1 and at most 8.
/// The overload taking `threads` allows an explicit count.
std::size_t replay(std::istream& in, std::ostream& out, Format format);

/// Replay a feed in `format` with `threads` worker threads, at least 1
///
/// Format::Csv is the overload above. Format::Itch reads the messages decode_itch() describes:
/// workers keep the books of the stock locates assigned to them, `<TIME>` is in nanoseconds, the
/// ticker is the stock symbol of the locate's first add, and errors name the message by its
/// position in the feed, `message N: `, counting from 1. The count returned includes adds,
/// executions, cancels, deletes, replaces, and trades, but not the message types the book skips.
std::size_t replay(std::istream& in, std::ostream& out, Format format, std::size_t threads);

} // namespace order_book
