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
offer (BBO) of a ticker each time it changes. It keeps one book per ticker: each side is a sorted set
of price levels, and each level holds its orders in arrival order.

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

### Output

One line per BBO change, on stdout:

```text
<TIME>,<TICKER>,<BBP>,<BBQ>,<BAP>,<BAQ>
```

`<TIME>` and `<TICKER>` come from the event that caused the change. `<BBP>` and `<BBQ>` are the price
and total quantity of the best bid level, empty if there are no bids, and `<BAP>` and `<BAQ>` are the
same for the ask. An event that leaves the BBO as it was prints nothing: an add or delete below the
best level, a trade, or a `C` that does not decrease the order.

### Errors

The replay stops at the first line it cannot apply and prints `error: line N: <reason>` to stderr
with exit status 1; the lines before it have already been printed. A line fails on a malformed or
out-of-range field, an unknown message type, an add that reuses a live order ID, or a `C`, `D`, `E`,
or `F` that names no live order. A file that cannot be opened also exits with 1, and a wrong argument
count prints the usage to stderr and exits with 2.

### Design

The library in `include/order_book/` has three parts:

- [`feed.hpp`](include/order_book/feed.hpp): `parse_line()` turns a line into a `Message` whose
  event is a `std::variant`, or into a `std::expected` error naming the field. It is the only code
  that knows the line format.
- [`order_book.hpp`](include/order_book/order_book.hpp): one ticker's book. Each side is a
  `std::map` from price to a level holding a running total and a `std::list` of orders, and an index
  from order ID to stable iterators finds any order in one hash lookup. An event costs that lookup,
  plus a level lookup for an add, whatever the depth of the level.
- [`replay.hpp`](include/order_book/replay.hpp): `replay(std::istream&, std::ostream&)` reads the
  feed, keeps a book per ticker, and writes a line whenever a BBO changes. The application opens the
  file and calls it, and so does the replay benchmark.

This repository uses:

- [CMake](https://cmake.org) configure, build, and test presets as the public build interface
- [Conan 2](https://conan.io) for dependency management
- [GoogleTest](https://github.com/google/googletest) and
  [Google Benchmark](https://github.com/google/benchmark) via Conan
- [Doxygen](https://www.doxygen.nl) for generated API documentation

## Operating model

The build is layered, and each layer owns one thing:

- **Conan** owns the dependency graph, the toolchain, the CMake generator, and the ABI-relevant
  settings. It resolves them from [`conanfile.py`](conanfile.py) and the [`profiles/`](profiles)
  files; [`conan.lock`](conan.lock) pins the exact graph for reproducible builds.
- **CMake presets** ([`CMakePresets.json`](CMakePresets.json)) are the public build interface: the
  `debug`, `release`, `sanitize*`, and `coverage` names you configure, build, and test. They are
  checked in and are the source of truth for how the project is built.
- Conan writes its toolchain details into a generated `ConanPresets.json` that `CMakePresets.json`
  includes. That file is an implementation detail, not an interface; `make bootstrap` is the
  one-time per-clone step that materializes it.
- The [`Makefile`](Makefile) is a thin convenience wrapper: each target runs `conan install` for the
  right profile, then `cmake --workflow --preset <name>`. On Windows you run those two commands
  directly (see [Windows](#windows)).

When you change the Conan configuration (versions, options, or profile), rerun `make bootstrap` or
the matching `make` target and keep using the same public preset names. The preset interface is
stable across toolchain changes.

## Layout

```text
.
├── include/order_book/      public headers
├── src/                     library and application sources
├── tests/                   unit, application, and differential tests; sample feed in tests/data/
├── bench/                   Google Benchmark suite
├── scripts/                 feed generator, reference model, Conan settings check
├── docs/                    development workflows
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

The `Makefile` is a Unix convenience wrapper. On Windows, run Conan and the CMake preset directly
from any shell. The Visual Studio generator locates MSVC, so no Developer PowerShell or `vcvarsall`
setup is required.

```bash
conan install . -pr=profiles/default -s="build_type=Release" --build=missing --lockfile=conan.lock
cmake --workflow --preset release
```

The setup is complete when the workflow exits successfully after running the tests.
`cmake --workflow --preset <name>` runs configure, build, and test in one step; the Unix `make`
targets call the same workflows. The `sanitize`, `sanitize-asan`, `sanitize-ubsan`, and `coverage`
presets are Unix-only.

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
