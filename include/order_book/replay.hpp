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
std::size_t replay(std::istream& in, std::ostream& out);

} // namespace order_book
