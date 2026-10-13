#include <order_book/replay.hpp>

#include <cstddef>
#include <fstream>
#include <ios>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, const char* argv[])
{
    constexpr std::string_view format_flag = "--format=";
    const std::span args(argv, static_cast<std::size_t>(argc));
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

    // ITCH is binary, and a text-mode stream would translate its bytes on some platforms
    std::ifstream input(path, format == order_book::Format::Itch ? std::ios::binary : std::ios::in);
    if (!input) {
        std::cerr << "error: cannot open '" << path << "'\n";
        return 1;
    }

    try {
        order_book::replay(input, std::cout, format, std::thread::hardware_concurrency());
    }
    catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
