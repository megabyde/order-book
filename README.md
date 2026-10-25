# Order Book

[![Build](https://github.com/megabyde/order-book/actions/workflows/build.yml/badge.svg)](https://github.com/megabyde/order-book/actions/workflows/build.yml)
[![CodeQL](https://github.com/megabyde/order-book/actions/workflows/codeql.yml/badge.svg)](https://github.com/megabyde/order-book/actions/workflows/codeql.yml)
[![Pages](https://github.com/megabyde/order-book/actions/workflows/pages.yml/badge.svg)](https://github.com/megabyde/order-book/actions/workflows/pages.yml)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-3.29%2B-064F8C.svg)](https://cmake.org)
[![Conan](https://img.shields.io/badge/Conan-2.x-6699CB.svg)](https://conan.io)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](./LICENSE)

## Overview

`order_book` replays one trading day of an equity exchange's event feed and prints the best bid and
offer (BBO) of a ticker each time it changes. It reads the CSV format below or Nasdaq TotalView-ITCH
5.0. It keeps one book per ticker: each side is a sorted set of price levels, and each level holds
the total quantity of its orders.

### Usage

```text
order_book [--format=csv|itch] [--threads=N] [--stats] FILE
```

`FILE` is the feed, or `-` for standard input, so a compressed capture replays without being
decompressed to disk:

```bash
gzip -dc 12302019.NASDAQ_ITCH50.gz | order_book --format=itch - > bbo.csv
```

`--threads` sets the number of worker threads, from 1 to 1024; the default is one per hardware
thread, at most 8. `--stats` prints the events applied, the BBO lines written, the worker count, and
the elapsed seconds to stderr once a replay succeeds. Counting the lines takes a pass over the
output, about a second of CPU on the ITCH day. `--help` and `--version` print to stdout and exit
with 0.

### Input

A CSV file with a header line, then one event per line:

```text
Time,Ticker,Order,T,Shares,Price[,...]
```

`Time` is milliseconds since midnight, and `Price` is in 100ths of a penny. Columns after `Price`,
such as the `MPID` and `X` of an exchange capture, are ignored. CRLF line endings and blank lines are
accepted.

| `T`      | Effect on the order named by `Order`                                        |
| -------- | --------------------------------------------------------------------------- |
| `B`, `S` | Add a buy or sell order of `Shares` at `Price`                              |
| `C`      | Decrease the order _to_ `Shares`; no effect unless smaller, removes it at 0 |
| `D`      | Delete the order                                                            |
| `E`      | Execute `Shares` of the order, clamped to what remains; removes it at 0     |
| `F`      | Fill the order, removing it                                                 |
| `T`, `X` | A trade or cross trade; the book does not change                            |

The program treats the feed as the exchange's own record, in which the matching has already
happened. `E` and `F` name the resting order that traded, and each counterparty's executions arrive
as their own messages, so the book never matches orders itself. Matching an `E` against the
opposite side again would count every fill twice.

### ITCH input

`order_book --format=itch FILE` reads Nasdaq TotalView-ITCH 5.0 instead: the uncompressed binary
file Nasdaq publishes, in which each message follows its 2-byte big-endian length. A `.gz` capture
has to be decompressed, to a file or through a pipe into `-`. The default is `--format=csv`.

| ITCH message   | Effect on the order it names                                            |
| -------------- | ----------------------------------------------------------------------- |
| `A`, `F`       | Add a buy or sell order                                                 |
| `E`, `C`, `X`  | Take its shares off the order, clamped to what remains; removes it at 0 |
| `D`            | Delete the order                                                        |
| `U`            | Replace the order by one on the same side under a new reference number  |
| `P`, `Q`       | A trade or cross trade; the book does not change                        |
| Any other type | Skipped: system events, the stock directory, trading status, imbalances |

Books are kept per stock locate. `<TIME>` is the message's timestamp in nanoseconds since midnight,
and `<TICKER>` is the stock symbol of the locate's first add, without its padding. Prices keep the
feed's unit, 1/10000 of a dollar, the same as the CSV format's 100ths of a penny.

### Output

One line per BBO change, on stdout:

```text
<TIME>,<TICKER>,<BBP>,<BBQ>,<BAP>,<BAQ>
```

`<TIME>` and `<TICKER>` come from the event that caused the change; `<TIME>` is in milliseconds for
CSV input and nanoseconds for ITCH. `<BBP>` and `<BBQ>` are the price
and total quantity of the best bid level, empty if there are no bids, and `<BAP>` and `<BAQ>` are the
same for the ask. An event that leaves the BBO as it was prints nothing: an add or delete below the
best level, a trade, or a `C` that does not decrease the order.

### Errors

The replay stops at the first line it cannot apply and prints `error: line N: <reason>` to stderr
with exit status 1; the lines before it have already been printed. A line fails on a malformed or
out-of-range field, an unknown message type, an add that reuses a live order ID, or a `C`, `D`, `E`,
or `F` that names no live order. A file that cannot be opened also exits with 1. A usage error, such
as a missing or extra argument, an unknown `--format`, or a `--threads` outside 1 to 1024, prints
`error: <reason>` to stderr and exits with 2.

Any other failure also prints `error: <reason>` and exits with 1, after the output before it:
output that cannot be written, down to the final flush, memory or threads that run out, and a read
error. Status 0 therefore means every line reached stdout. Standard input reports a read error on
every platform, but a file reports one only with libstdc++; libc++'s and MSVC's file streams take a
read error for the end of the file, and the replay ends there. On POSIX, output into a pipe whose
reader has exited ends the program with `SIGPIPE`, as it ends `cat` or `grep`.

ITCH errors name the message instead, `error: message N: <reason>`, counting messages from 1. A
message fails on an unknown type, a size other than the specification's for a type the book uses, a
side other than `B` or `S`, a truncated last message, an add or a replacement that reuses a live
order reference of its stock, or an execution, cancel, delete, or replace that names no live order of
its stock.

### Design

[Architecture](docs/architecture.md) follows a batch through the threads and the books and gives
the measurements behind each choice. The library in `include/order_book/` has six parts:

- [`feed.hpp`](include/order_book/feed.hpp): `parse_line()` turns a CSV line into a `Message`, whose
  event is a `std::variant`, or into an error naming the field.
- [`itch.hpp`](include/order_book/itch.hpp): `visit_itch()` decodes one ITCH message and calls the
  handler method for its kind, so the replay dispatches on the type byte once; `decode_itch()`
  returns the same fields as an `ItchMessage`.
- [`levels.hpp`](include/order_book/levels.hpp): the price levels both books keep. A level is a
  pooled record of its total and order count, and each order holds its level's handle. Each side of
  a book is a vector of prices and handles, worst first, so the best level is the last element.
- [`order_book.hpp`](include/order_book/order_book.hpp): one ticker's book for the CSV feed, with its
  orders in an [unordered_dense](https://github.com/martinus/unordered_dense) map keyed by order ID.
- [`itch_market.hpp`](include/order_book/itch_market.hpp): the books of many stock locates for ITCH.
  Reference numbers are unique for the day and assigned in sequence, so orders live in pages of
  consecutive reference numbers instead of a hash table.
- [`replay.hpp`](include/order_book/replay.hpp): `replay()` reads the feed, applies it on worker
  threads that each own a share of the books, and writes a line whenever a BBO changes, in feed
  order whatever the thread count. The default is one worker per hardware thread, at most 8.

The application in `src/main.cpp` parses its options with CLI11, opens the file or standard input,
and calls `replay()`.

The project uses:

- [CMake](https://cmake.org) presets as the build interface
- [Conan 2](https://conan.io) for dependencies
- [unordered_dense](https://github.com/martinus/unordered_dense) for the CSV book's maps
- [CLI11](https://github.com/CLIUtils/CLI11) for the application's options
- [GoogleTest](https://github.com/google/googletest) and
  [Google Benchmark](https://github.com/google/benchmark) for the tests and benchmarks
- [Doxygen](https://www.doxygen.nl) for the API documentation

## Operating model

- **Conan** resolves the dependencies, the toolchain, and the CMake generator from
  [`conanfile.py`](conanfile.py) and [`profiles/`](profiles), and [`conan.lock`](conan.lock) pins the
  exact versions.
- **CMake presets** in [`CMakePresets.json`](CMakePresets.json) are the public build interface: the
  `debug`, `release`, `sanitize*`, and `coverage` names you configure, build, and test. They include
  the `ConanPresets.json` that Conan generates, which `make bootstrap` creates once per clone.
- The [`Makefile`](Makefile) runs `conan install` with the right profile, then
  `cmake --workflow --preset <name>`. On Windows you run those two commands directly (see
  [Windows](#windows)).

After changing the Conan configuration, rerun `make bootstrap` or the matching `make` target; the
preset names stay the same.

## Layout

```text
.
├── include/order_book/      public headers
├── src/                     library and application sources
├── tests/                   unit, application, and differential tests; sample feed in tests/data/
├── bench/                   Google Benchmark suite
├── scripts/                 feed tools, reference model, benchmark summary, Conan settings check
├── docs/                    architecture and development workflows
├── conanfile.py             Conan dependency definition
├── conan/settings_user.yml  custom sanitizer setting
├── profiles/                default and sanitizer Conan profiles
└── CMakePresets.json        project-owned public presets
```

## Prerequisites

Required:

- [CMake](https://cmake.org/download/) 3.29+ (for workflow presets and `CMAKE_LINKER_TYPE`)
- [Conan](https://docs.conan.io/2/installation.html) 2.25+ (for the `CMakeConfigDeps` generator)
- A compiler and standard library with C++23 `std::expected` support
  - [GCC](https://gcc.gnu.org/) 14+
  - [LLVM Clang](https://llvm.org/) 19+ with libstdc++ 14+, or 17+ with libc++ 17+
  - [Apple Clang](https://developer.apple.com/xcode/) 17+ on macOS
  - [MSVC](https://visualstudio.microsoft.com/) 2022 (17.10+) on Windows

Optional:

- [Ninja](https://ninja-build.org/) for parallel, incremental Unix builds; GNU Make is used when
  Ninja is absent
- [ccache](https://ccache.dev/) to reuse compiler output across rebuilds; used automatically for
  first-party targets when on PATH, except with the Visual Studio generator, which ignores compiler
  launchers
- [mold](https://github.com/rui314/mold) or [LLD](https://lld.llvm.org/) to reduce Linux link times;
  used automatically for first-party targets when on PATH, with mold preferred
- [Doxygen](https://www.doxygen.nl) to generate local API documentation with `make docs`

The Conan recipe selects the CMake generator:

- `Ninja` on Unix-like systems when it is available
- `Unix Makefiles` on Unix-like systems when `ninja` is not installed
- The Visual Studio generator matching the detected MSVC on Windows (multi-config; locates MSVC
  itself, so no extra tool or `vcvars` environment is needed)

The project builds on Linux, macOS, and Windows.

## Configure, build, and test

### Quick start

Outcome: dependencies installed, the debug preset configured and built, and the test suite passed.

```bash
git clone https://github.com/megabyde/order-book.git
cd order-book
make bootstrap  # generate ConanPresets.json
make debug
```

The setup is complete when `make debug` exits successfully after running the tests.

> [!TIP]
> Run `make help` to list the other local targets.

### Windows

Outcome: dependencies installed, the release preset configured and built, and the test suite passed.

The `Makefile` is Unix-only. On Windows, run Conan and the CMake preset directly from any shell. The
Visual Studio generator locates MSVC, so the build needs no Developer PowerShell or `vcvarsall`;
only the `sanitize-asan` tests do, for the ASan runtime on `PATH`.

```bash
conan install . -pr=profiles/default -s="build_type=Release" --build=missing --lockfile=conan.lock
cmake --workflow --preset release
```

The setup is complete when the workflow exits successfully after running the tests. `debug` works
the same way with `build_type=Debug`. MSVC has AddressSanitizer alone, so `sanitize-asan` is the one
sanitizer preset on Windows, from its own profile (see [Sanitizers](docs/development.md#sanitizers)).
The `sanitize`, `sanitize-ubsan`, `sanitize-tsan`, and `coverage` presets are Unix-only.

## Development workflows

For sanitizer modes, the test layers, benchmarks, preset behavior, dependency updates, formatting
and linting, coverage, documentation generation, build policy, and IDE setup, see
[Development workflows](docs/development.md).

## Install

Outcome: the release application installed at `<prefix>/bin/order_book`. Only the executable is
installed; the static library remains an internal build artifact.

1. Build and test the release preset:

```bash
cmake --workflow --preset release
```

1. Install to the selected prefix:

```bash
cmake --install build/release --prefix /path/to/prefix
```

1. From the repository root, run the installed binary on the sample feed:

```console
$ /path/to/prefix/bin/order_book tests/data/sample.csv
1,AAA,1000,100,,
2,AAA,1000,100,1010,50
3,BBB,,,2000,10
...
```

The install is complete when the output matches `tests/data/sample.expected`.
