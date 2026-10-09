#include <order_book/replay.hpp>

#include <fstream>
#include <iostream>
#include <stdexcept>

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, const char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " FILE\n";
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "error: cannot open '" << argv[1] << "'\n";
        return 1;
    }

    try {
        order_book::replay(input, std::cout);
    }
    catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
