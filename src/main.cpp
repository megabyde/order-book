#include <order_book/replay.hpp>
#include <order_book/version.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// Standard input as a stream buffer that fread()s each request whole, since std::cin's buffer may
// read a character at a time and replay() asks for a megabyte per call. replay() reads only through
// sgetn(), so there is no underflow(). A read error throws, on every standard library, so that
// replay() keeps the whole records before it and drops the one it cut short; returning a short
// count alone would look like the end of the input, where a last line without its LF is applied.
class StdinBuffer : public std::streambuf {
protected:
    std::streamsize xsgetn(char* data, std::streamsize count) override
    {
        if (!m_failed) {
            const auto read = std::fread(data, 1, static_cast<std::size_t>(count), stdin);
            m_failed = std::ferror(stdin) != 0;
            // fread() returns the bytes it read before an error, and replay() keeps nothing from a
            // call that throws, so the error waits for the next call unless this one read nothing
            if (read > 0 || !m_failed) {
                return static_cast<std::streamsize>(read);
            }
        }
        throw std::runtime_error("cannot read the input");
        // The brace holds only the cleanup for a throw whose exception cannot be constructed
    } // GCOVR_EXCL_LINE

private:
    bool m_failed = false;
};

// Passes output through to `out`, counting the lines. replay() writes only through sputn() and
// flushes, so there is no overflow(), and a single put fails the stream rather than going
// uncounted.
class LineCounter : public std::streambuf {
public:
    explicit LineCounter(std::streambuf* out) : m_out(out) {}
    [[nodiscard]] std::size_t lines() const { return m_lines; }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        m_lines += static_cast<std::size_t>(std::count(data, data + count, '\n'));
        return m_out->sputn(data, count);
    }
    int sync() override { return m_out->pubsync(); }

private:
    std::streambuf* m_out;
    std::size_t m_lines = 0;
};

int run(int argc, const char* const* argv)
{
    CLI::App app("Replay an exchange feed into per-ticker order books and print the best bid and "
                 "offer whenever it changes",
                 "order_book");
    app.set_version_flag("--version", std::string(order_book::version));
    app.failure_message([](const CLI::App* /*app*/, const CLI::Error& error) {
        return std::format("error: {}\nRun with --help for more information.\n", error.what());
    });
    std::string format_name = "csv";
    app.add_option("--format", format_name, "Feed encoding: csv or itch")
        ->check(
            [](const std::string& name) {
                return name == "csv" || name == "itch"
                           ? std::string()
                           : std::format("unknown format '{}'; expected csv or itch", name);
            },
            "csv|itch")
        ->capture_default_str();
    // A bound well above any core count, so that a typo cannot ask for millions of threads
    constexpr std::size_t max_threads = 1024;
    std::size_t threads = 0;
    app.add_option("-t,--threads", threads,
                   std::format("Worker threads; {} by default, one per hardware thread up to 8",
                               order_book::default_threads()))
        ->check(CLI::Range(std::size_t{1}, max_threads));
    bool stats = false;
    app.add_flag("--stats", stats, "Print event and line counts and the elapsed time to stderr");
    std::string path;
    app.add_option("FILE", path, "Feed to replay, or - for standard input")->required();
    try {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& error) {
        // Help and version exit with 0, and a usage error with 2
        return app.exit(error) == 0 ? 0 : 2;
    }
    const auto format = format_name == "itch" ? order_book::Format::Itch : order_book::Format::Csv;

    StdinBuffer stdin_buffer;
    // A file stream reads a few KiB at a time by default; the buffer must be set before open()
    constexpr std::size_t buffer_size = std::size_t{1} << 20U;
    std::vector<char> buffer;
    std::ifstream file;
    std::istream input(&stdin_buffer);
    if (path == "-") {
#ifdef _WIN32
        // ITCH is binary, and text mode would translate CR LF in its bytes
        if (format == order_book::Format::Itch) {
            _setmode(_fileno(stdin), _O_BINARY);
        }
#endif
    }
    else {
        buffer.resize(buffer_size);
        file.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        // ITCH is binary, and a text-mode stream would translate its bytes on some platforms
        file.open(path, format == order_book::Format::Itch ? std::ios::binary : std::ios::in);
        if (!file) {
            std::cerr << "error: cannot open '" << path << "'\n";
            return 1;
        }
        input.rdbuf(file.rdbuf());
    }

    if (threads == 0) {
        threads = order_book::default_threads();
    }
    LineCounter counter(std::cout.rdbuf());
    std::ostream out(stats ? &counter : std::cout.rdbuf());
    const auto start = std::chrono::steady_clock::now();
    // Flushes the output and throws if that fails
    const auto events = order_book::replay(input, out, format, threads);
    if (stats) {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        std::cerr << std::format("{} events, {} lines, {} threads, {:.3f} s\n", events,
                                 counter.lines(), threads, elapsed.count());
    }
    return 0;
}

} // namespace

int main(int argc, const char* argv[])
{
    try {
        return run(argc, argv);
    }
    catch (const std::exception& error) {
        // The lines before a failing record are part of the result
        std::cout.flush();
        std::cerr << "error: " << error.what() << '\n';
    }
    // Nothing in the program throws anything else
    // GCOVR_EXCL_START
    catch (...) {
        std::cerr << "error: unknown exception\n";
    }
    // GCOVR_EXCL_STOP
    return 1;
}
