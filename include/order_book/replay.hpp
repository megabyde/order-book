#pragma once

#include <cstddef>
#include <istream>
#include <ostream>

namespace order_book {

/// Replay an exchange feed and write the best bid and offer of a ticker whenever it changes
///
/// `in` holds a header line, then one event per line in the format Event parses; a trailing CR
/// and blank lines are skipped. Each change writes `<TIME>,<TICKER>,<BBP>,<BBQ>,<BAP>,<BAQ>` to
/// `out`, with empty fields for an empty side. Returns the number of events applied, trades
/// included. Throws std::invalid_argument prefixed with `line N: ` on the first line that cannot
/// be parsed or applied; output up to that line has already been written.
///
/// Uses one worker thread per hardware thread; see the overload taking `threads`.
std::size_t replay(std::istream& in, std::ostream& out);

/// Replay as above with `threads` worker threads, at least 1
///
/// Each worker keeps the books of the tickers whose names hash to it, and the calling thread
/// reads the input and writes the output in line order, so the output and the exception do not
/// depend on `threads`. A feed with fewer tickers than workers leaves the rest idle.
std::size_t replay(std::istream& in, std::ostream& out, std::size_t threads);

} // namespace order_book
