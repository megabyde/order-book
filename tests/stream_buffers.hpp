#pragma once

// Stream buffers that fail the way files, pipes, and disks can, for tests

#include <order_book/replay.hpp>

#include <algorithm>
#include <cstddef>
#include <ios>
#include <istream>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string>
#include <utility>

namespace streams {

// Serves the first `limit` characters of its string, then throws, as libstdc++'s std::filebuf does
// on a read error
class ReadFailure : public std::stringbuf {
public:
    ReadFailure(std::string text, std::size_t limit)
        : std::stringbuf(std::move(text), std::ios::in), m_left(static_cast<std::streamsize>(limit))
    {
    }

protected:
    std::streamsize xsgetn(char* s, std::streamsize count) override
    {
        if (m_left == 0) {
            throw std::ios::failure("read failed");
        }
        const auto read = std::stringbuf::xsgetn(s, std::min(count, m_left));
        m_left -= read;
        return read;
    }

private:
    std::streamsize m_left;
};

// Accepts no output, as a closed descriptor or a full disk
class Refusing : public std::streambuf {};

// Accepts output but fails to flush it
class Unflushable : public std::stringbuf {
protected:
    int sync() override { return -1; }
};

// What replay() wrote before it rethrew the read failure of `buffer`; nullopt if it did not
inline std::optional<std::string>
written_before_read_failure(std::streambuf& buffer, order_book::Format format, std::size_t threads)
{
    std::istream in(&buffer);
    std::ostringstream out;
    try {
        order_book::replay(in, out, format, threads);
    }
    catch (const std::ios::failure&) {
        return out.str();
    }
    return std::nullopt;
}

} // namespace streams
