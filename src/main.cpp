#include <order_book/split.hpp>
#include <order_book/version.hpp>

#include <cstddef>
#include <cstdio>
#include <print>
#include <span>
#include <string_view>

namespace {

void run()
{
    std::println("order-book {} starting", order_book::version);

    constexpr std::string_view record = "alpha,beta,gamma";
    std::size_t index = 0;
    for (const auto field : order_book::split_views(record)) {
        std::println("field {}: {}", index, field);
        ++index;
    }

    std::println("done");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[])
{
    const std::span args(argv, static_cast<std::size_t>(argc));
    if (args.size() == 1) {
        run();
        return 0;
    }
    const std::string_view arg = args[1];
    if (args.size() == 2 && arg == "--version") {
        std::println("{}", order_book::version);
        return 0;
    }
    if (args.size() == 2 && arg == "--help") {
        std::println("Usage: order_book [--help] [--version]");
        return 0;
    }
    std::println(stderr, "unexpected argument: {}", arg);
    return 2;
}
