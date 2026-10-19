#include <order_book/replay.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <fstream>
#include <ios>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int run(std::span<const char* const> args)
{
    constexpr std::string_view format_flag = "--format=";
    const auto flagged = args.size() == 3 && std::string_view(args[1]).starts_with(format_flag);
    if (args.size() != 2 && !flagged) {
        std::cerr << "Usage: " << args[0] << " [--format=csv|itch] FILE\n";
        return 2;
    }

    auto format = order_book::Format::Csv;
    if (flagged) {
        const auto name = std::string_view(args[1]).substr(format_flag.size());
        if (name == "itch") {
            format = order_book::Format::Itch;
        }
        else if (name != "csv") {
            std::cerr << "error: unknown format '" << name << "'; expected csv or itch\n";
            return 2;
        }
    }
    const auto* const path = args.back();

    // A file stream reads a few KiB at a time by default; the buffer must be set before open()
    constexpr std::size_t buffer_size = std::size_t{1} << 20U;
    std::vector<char> buffer(buffer_size);
    std::ifstream input;
    input.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    // ITCH is binary, and a text-mode stream would translate its bytes on some platforms
    input.open(path, format == order_book::Format::Itch ? std::ios::binary : std::ios::in);
    if (!input) {
        std::cerr << "error: cannot open '" << path << "'\n";
        return 1;
    }

    auto workers = std::thread::hardware_concurrency();
#if defined(__linux__) && defined(__x86_64__)
    constexpr auto max_workers = 8U;
    workers = std::min(workers, max_workers);
#endif
    // Flushes std::cout and throws if that fails
    order_book::replay(input, std::cout, format, workers);
    return 0;
}

} // namespace

int main(int argc, const char* argv[])
{
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
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
