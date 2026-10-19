# Development workflows

Prerequisite: complete the [README setup](../README.md) for your platform first.

## Sanitizers

Outcome: first-party code and dependencies built with matching sanitizer instrumentation, followed
by the instrumented test suite.

> [!IMPORTANT]
> The first sanitizer build may add `compiler.sanitizer` from
> [`conan/settings_user.yml`](../conan/settings_user.yml) to the global Conan configuration. If the
> Conan home already has `settings_user.yml`, the build leaves it untouched and checks that it
> contains the required values. If the check fails, merge the reported values into the existing
> file, then rerun the same command.

Choose one mode:

- **ASan + UBSan:** `make sanitize` uses [`profiles/sanitize`](../profiles/sanitize) and
  `build/debug-addressundefinedbehavior`
- **ASan:** `make sanitize-asan` uses [`profiles/sanitize-asan`](../profiles/sanitize-asan) and
  `build/debug-address`
- **UBSan:** `make sanitize-ubsan` uses [`profiles/sanitize-ubsan`](../profiles/sanitize-ubsan) and
  `build/debug-undefinedbehavior`
- **TSan:** `make sanitize-tsan` uses [`profiles/sanitize-tsan`](../profiles/sanitize-tsan) and
  `build/debug-thread`. TSan cannot run alongside ASan, so it has no combined mode. `replay()`
  runs its workers on threads, so this is the mode that checks them for data races.

The sanitizer run is complete when the selected target exits successfully after running the tests.
If a sanitizer detects an error, it prints a stack trace, stops at the first finding, and returns a
failure through CTest to the selected `make` target. Fix the reported source error, then rerun the
same target.

CI runs `make sanitize` and `make sanitize-tsan`. On Linux it first sets `vm.mmap_rnd_bits=28`: the
runner kernel randomizes mmap with more bits than the TSan runtime's memory layout allows. To install
all four instrumented dependency graphs without running their workflows, use
`make bootstrap-sanitize`. The default `make bootstrap` does
not install them.

All four profiles inherit [`profiles/sanitize-common`](../profiles/sanitize-common), which includes
`profiles/default`, selects `Debug`, and defines the shared instrumentation flags and `[runenv]`.
Each mode appends its own `-fsanitize` flags and shares `conan.lock`.

The custom `compiler.sanitizer` setting gives each instrumented dependency graph a distinct Conan
`package_id`. Without it, `--build=missing` could reuse plain Debug dependencies. The setting
defaults to `null`, which is omitted from `package_id`, so non-sanitizer builds do not change.
First-party targets receive the same profile flags through the Conan toolchain.

Sanitizer runtime options such as `halt_on_error` and `print_stacktrace` live under `[runenv]` in
`profiles/sanitize-common`. Conan injects them into the generated test presets inherited by the
public `sanitize*` presets, keeping the runtime configuration out of `CMakePresets.json`.

## Fuzzing

Outcome: three libFuzzer targets, built with ASan and UBSan, run over their seeds by `make fuzz` and
without end by `make fuzz-run`.

- `csv_replay_fuzz` and `itch_replay_fuzz` replay the input as a feed of their format on one worker
  and on three. Both runs must count the same events, print the same lines, and fail with the same
  error. The seeds are the CSV feeds in `tests/data`, and `sample.csv` and `wide_references.csv` as
  `scripts/csv_to_itch.py` encodes them.
- `itch_market_fuzz` reads the input as adds, executions, deletes, and replacements on four locates,
  with reference numbers drawn from ten on both sides of a page, a chunk, and the end of the index,
  and checks every result, BBO, and order count against the model in
  [`tests/market_model.hpp`](../tests/market_model.hpp), the one the churn test uses.

`make fuzz` builds the `fuzz` preset in `build/fuzz` and runs its CTest tests: each replay fuzzer
over its seeds once, and the market fuzzer for 50,000 inputs from a fixed seed. `make fuzz-run` then
fuzzes each target for `FUZZ_SECONDS`, 60 by default, keeps the inputs that reach new code in
`build/fuzz/corpus/<target>`, and writes an input that fails to `build/fuzz/crashes/`;
`build/fuzz/<target> <file>` replays it. CI runs `make fuzz-run` on Linux with the default 60
seconds a target.

libFuzzer comes with LLVM's Clang, so the toolchain depends on the platform:

| Platform | Toolchain                                                                             |
| -------- | ------------------------------------------------------------------------------------- |
| Linux    | Clang and its compiler-rt; CI uses Ubuntu's `clang-19` and `libclang-rt-19-dev`       |
| macOS    | Homebrew LLVM (`brew install llvm`), the default; AppleClang has no libFuzzer runtime |
| GCC      | Not supported: GCC has no libFuzzer                                                   |
| Windows  | Not supported: MSVC has `/fsanitize=fuzzer`, but the preset passes Clang flags        |

`FUZZ_CC` and `FUZZ_CXX` name the compiler, `clang` and `clang++` by default on Linux:
`make fuzz FUZZ_CC=clang-19 FUZZ_CXX=clang++-19`. The preset can use a different compiler from the
rest of the build because it turns off the tests and benchmarks, whose GoogleTest and Google
Benchmark binaries the default compiler built, and takes from the debug Conan toolchain only
`unordered_dense`, which is header-only. It compiles at `-O1`.

The replay fuzzers' inputs stay within libFuzzer's length limit of a few KiB, so each fits in one
1 MiB batch; records that straddle batches are left to the unit tests.

## Tests

Tests are controlled by CMake's built-in `BUILD_TESTING` option from `include(CTest)`. This project
leaves it at the default `ON`, so `make debug`, `make release`, `make sanitize*`, and
`make coverage` all run the suite. It has four layers:

- Unit tests in `tests/*_test.cpp` cover the CSV parser, the ITCH decoder, the CSV book, the ITCH
  market, and `replay()` in both formats. Each book is also checked against a `std::map` model
  under randomized churn. [`tests/itch_messages.hpp`](../tests/itch_messages.hpp) builds ITCH messages
  field by field for them.
- `AppTest.*` runs the application on each error path through
  [`tests/expect_error.cmake`](../tests/expect_error.cmake), which checks the exit status and the
  stderr message separately.
- `AppTest.ReplaysSample` replays the hand-checked [`tests/data/sample.csv`](../tests/data/sample.csv)
  and compares the output with `tests/data/sample.expected`. `AppTest.ReplaysSampleItch` replays
  the same feed encoded as ITCH and expects the same output.
- `Differential.Default` and `Differential.Large` generate a seeded feed with
  [`scripts/gen_feed.py`](../scripts/gen_feed.py), 50k events over 8 tickers and 200k over 20, and
  compare the application's output with [`scripts/reference_bbo.py`](../scripts/reference_bbo.py),
  an independent Python model of the feed semantics. `Differential.DefaultItch` and
  `Differential.LargeItch` replay the same feeds encoded as ITCH by
  [`scripts/csv_to_itch.py`](../scripts/csv_to_itch.py) and expect the model's output too. The
  encoder varies the encoding so that every ITCH type the replay decodes appears, `U` replacements
  included. The scripts use the standard library only, and these tests are skipped when CMake finds
  no Python 3.

`reference_bbo.py --stats FILE` also prints feed-consistency counters, such as adds that cross the
book and `C` messages that do not decrease the order, for checking that a real capture fits the
replay semantics.

## Benchmarks

Prerequisite: Python 3 on `PATH` to generate the synthetic feed.

Build the release benchmarks and run them:

```bash
make bench
```

The first run writes two seeded feeds with [`scripts/gen_feed.py`](../scripts/gen_feed.py), and
`BM_Replay/<path>/threads:N` replays each through the same `order_book::replay()` the application
uses, with 1, 2, 4, and 8 worker threads, timed by the wall clock:

- `build/bench/feed.csv`: 1M events over 8 tickers. Few live orders, so the book stays in cache and
  input, parsing, and output dominate.
- `build/bench/deep.csv`: 4M events over 200 tickers with up to 4000 live orders each. The books
  outgrow the caches, so the order index and the levels dominate.

Workers split the tickers between them, so a feed scales with threads only up to its number of
tickers, and a one-ticker capture not at all.

`ORDER_BOOK_REPLAY_CSV` lists the feeds to replay, separated like `PATH`. To replay a capture
instead of the synthetic feeds, point it at the capture:

```bash
ORDER_BOOK_REPLAY_CSV=/path/to/feed.csv make bench
```

`ORDER_BOOK_REPLAY_ITCH` lists uncompressed ITCH files the same way and adds their replays to the
CSV ones:

```bash
ORDER_BOOK_REPLAY_ITCH=/path/to/12302019.NASDAQ_ITCH50 make bench
```

Each ITCH file also gets `BM_ItchEngine/<path>`, which applies every message to one `ItchMarket` on
one thread, from memory, the way the replay's workers do: the same prefetching, `visit_itch()`
calling the market's method for the message's kind, and the BBO read after each change. It leaves
out what the replay adds around that: reading the file, picking out a worker's messages, formatting
lines, tracking stock symbols, and merging the output. That is the region other ITCH books time in
their own benchmarks, so its `per_message` counter is the per-core figure to compare with them, an
average over the day rather than a latency. Each iteration starts from an empty market, made and
destroyed outside the timing, and a full day takes about 15 seconds, so give it a few repetitions
rather than the default 10:

```bash
ORDER_BOOK_REPLAY_ITCH=/path/to/12302019.NASDAQ_ITCH50 \
  make bench BENCH_ARGS='--benchmark_filter=BM_ItchEngine --benchmark_repetitions=3'
```

The Bench workflow runs the suite on every push to `main` on four GitHub runners: x86-64 Linux,
Arm64 Linux, macOS on Apple silicon, and x86-64 Windows. Each replays the two seeded feeds above and
the deep one encoded as ITCH by `scripts/csv_to_itch.py`, then posts a table to the run's summary
with [`scripts/bench_summary.py`](../scripts/bench_summary.py) and uploads the Google Benchmark JSON
as an artifact. Shared runners vary from run to run, so the workflow reports and never fails, and a
comparison between runners or commits needs several runs. Run it on a branch from the Actions tab.

`BENCH_ARGS` replaces the default Google Benchmark flags, 10 repetitions reporting aggregates only.
For example, `make bench BENCH_ARGS=--benchmark_filter=BM_Replay` runs the replays alone.

The suite is a Conan `test_requires` behind the `with_benchmarks` option, which sets CMake's
`BUILD_BENCHMARKS`; both default to on. Every workflow preset builds it and runs one iteration of
each benchmark as the `Bench.Smoke` test, so the suite cannot rot between measurements. Smoke
timings are meaningless; take numbers from `make bench` on an idle machine.

## Paired hillclimb measurements

`scripts/hillclimb.py` compares the application CLI on Linux with Python 3.11+ and no extra Python
packages. Generate synthetic CSV inputs with `scripts/gen_feed.py` and encode matching ITCH inputs
with `scripts/csv_to_itch.py`. Check baseline outputs against `scripts/reference_bbo.py` before
freezing an experiment. Keep release hardening, LTO, and warnings-as-errors enabled.

```bash
python3 scripts/hillclimb.py freeze --baseline build/release/order_book \
  --manifest build/hillclimb/manifest.json \
  --case target:long_csv:csv:build/hillclimb/long.csv \
  --case target:long_itch:itch:build/hillclimb/long.itch \
  --case guard:broad_csv:csv:build/hillclimb/broad.csv \
  --case guard:broad_itch:itch:build/hillclimb/broad.itch \
  --case guard:single_csv:csv:build/hillclimb/single.csv \
  --case guard:single_itch:itch:build/hillclimb/single.itch
python3 scripts/hillclimb.py run --manifest build/hillclimb/manifest.json \
  --candidate build/release/order_book --out .audit/hillclimb/attempt-01
python3 scripts/hillclimb.py profile --manifest build/hillclimb/manifest.json \
  --candidate build/release/order_book --out .audit/hillclimb/profile-01
```

Freeze copies the baseline beside the manifest and seals input hashes, observed baseline output
hashes/counts, harness identity, compiler identity/settings, release cache flags, generated Conan
toolchain, and `conan.lock`. `--build-dir` defaults to `build/release`; `--timeout` defaults to 300
seconds per process. Harness changes require a new experiment. Source and documentation edits do
not change the frozen identity.

Run validates complete output before eight alternating AB/BA pairs with timed stdout sent to
`/dev/null`. Each result preserves commands, order, elapsed time, child CPU/context switches, and
exit/timeout status. Input counts describe CSV rows excluding the header or ITCH frames, including
metadata frames. Validation observes output lines/bytes; neither count claims applied events.
Any failed process or changed identity fails the run and leaves raw observations without a score.
Use a fresh output directory for every attempt; an experiment lock prevents overlapping runs and
profiles. Freeze diagnostics are written beside the manifest as `*.freeze-result.json`.

The target score is the geometric mean of target cases' candidate/reference median elapsed ratios.
Guards are excluded. Paired target geometric ratios supply an exact one-sided sign test against 1;
a metric win requires score below 1 and p at most 0.05. Every case above 1.05 requires investigation,
and a paired regression p at most 0.05 confirms a regression. `eligible` additionally requires no
case above 1.05. `--against PATH` compares an incumbent while preserving the original baseline.
Final verification uses the original baseline; a score at most 0.75 alone does not establish the
campaign requirement of at least ten real source attempts.

Profile uses separate `perf record -e cpu-clock -g` and hardware-counter `perf stat` invocations;
commands, perf version, failures, and artifacts remain separate from timing samples. Kernel perf
permissions and available counters determine whether profiling succeeds.

## Public presets

The main workflow presets are `debug`, `release`, `sanitize`, `sanitize-asan`, `sanitize-ubsan`,
`sanitize-tsan`, `coverage`, and `fuzz`. Configure, build, and test presets use the same names.
`docs` is configure/build only because it generates Doxygen HTML instead of compiling and testing
the application.

The Conan-generated `conan-*` presets are internal implementation details and are not the public
interface for developers or CI.

## Dependency lock file

Outcome: a `conan.lock` with the newest versions `conanfile.py` allows, for reproducible builds.

`conanfile.py` gives each dependency a version range, and `conan.lock` pins the version and recipe
revision every build installs. Conan reads the lock beside the recipe on its own, so a build or a
`conan lock create` keeps each locked version its range still allows and resolves only the rest.

Update dependencies in this order:

1. If the code needs a newer version than a range allows, raise its lower bound in `conanfile.py`.
2. Regenerate the lock file with `make lock`. It ignores the current lock and resolves every range
   to the newest version on the remote.
3. Run the appropriate `make` target to verify.
4. Commit `conan.lock`, and `conanfile.py` if a range changed.

The update is complete when the selected build and test workflow passes and `conan.lock` holds the
intended versions. `make lock-check`, which CI runs, fails when the lock no longer satisfies
`conanfile.py` or keeps entries the graph does not use; a new upstream release does not fail it.

## Formatting and linting

Use `make format` to rewrite supported files in place:

```bash
make format
```

Then verify formatting and lint findings without modifying files:

```bash
make format-check
make lint
```

The check is complete when both verification targets exit successfully. If `make format-check`
fails, rerun `make format` and inspect its changes before checking again. Reported lint findings
require a source fix.

`make format` and `make format-check` cover C++ sources (clang-format),
[`CMakeLists.txt`](../CMakeLists.txt) (cmake-format, from the
[cmakelang](https://cmake-format.readthedocs.io) package), `scripts/` and `conanfile.py`
([ruff](https://docs.astral.sh/ruff/) format), and tracked Markdown/JSON/YAML files
([prettier](https://prettier.io); `conan.lock` is excluded because Conan owns its formatting).
`make lint` runs clang-tidy against the debug compilation database, `cmake-lint` on
`CMakeLists.txt`, `ruff check` on `scripts/` and `conanfile.py`, and
[markdownlint](https://github.com/DavidAnson/markdownlint-cli2) on Markdown files. Any reported
finding fails the target. CI pins all lint and format tool versions in
[`.github/ci.env`](../.github/ci.env).

## Coverage

Prerequisite: Python 3.10+ with [gcovr](https://gcovr.com) installed (e.g., `pip install gcovr`).

Build, test, and generate the coverage report:

```bash
make coverage-report
```

The run is complete when the tests pass, gcovr reports a line percentage at or above the configured
floor, and these files exist:

- `build/coverage/coverage-report/index.html`
- `build/coverage/coverage.xml`

Both supported compilers emit GCov-format data (`--coverage`), which gcovr reports through one
interface. `-fprofile-update=atomic` keeps the counts exact while worker threads run the same code.

The report fails if line coverage falls below `COVERAGE_FAIL_UNDER` (default 100; override with
`make coverage-report COVERAGE_FAIL_UNDER=80`).

## Documentation

Prerequisite: Doxygen installed and available on `PATH`.

Generate the API documentation:

```bash
make docs
```

The build is complete when `build/docs/html/index.html` exists. GitHub Pages runs the same target
and publishes the result from the `main` branch.

## Build policy

Warnings are errors by default (`WARNINGS_AS_ERRORS`, default `ON`) in every preset, locally and in
CI. Relax it for a single build tree with `cmake --preset <name> -DWARNINGS_AS_ERRORS=OFF`.

First-party targets also build with hardening flags by default (`ENABLE_HARDENING`, default `ON`;
disable with `-DENABLE_HARDENING=OFF` on any configure preset):

- GCC/Clang/AppleClang: `-fstack-protector-strong` and architecture-matched control-flow protection
  (`-fcf-protection=full` on x86-64; `-mbranch-protection=standard` on AArch64, Linux only, because
  pac-ret frames break exception unwinding under macOS's compact-unwind format) in every
  configuration; optimized configurations add `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2` (Debug skips
  fortification because it requires optimization). Executables link as PIE so ASLR covers the
  program image, and on Linux the linker adds full RELRO (`-z relro -z now`) and an explicitly
  non-executable stack.
- MSVC: `/guard:cf` (Control Flow Guard) at compile and link; x64 also links with `/CETCOMPAT` (CET
  shadow stack).
- Other compilers build unhardened rather than failing to configure.

First-party targets build the `release` preset with link-time optimization (LTO) by default
(`ENABLE_LTO`, default `ON`; disable with `-DENABLE_LTO=OFF`). It applies only to the `Release`
configuration (CMake's `INTERPROCEDURAL_OPTIMIZATION_RELEASE` property), so `debug`, `sanitize*`,
and `coverage` builds are unaffected; when the toolchain reports no LTO support, configure logs the
reason and continues without it rather than failing.

Sanitizer and coverage builds omit fortification (they build as Debug, which never defines
`_FORTIFY_SOURCE`): fortify conflicts with the ASan interceptors, and coverage builds run at `-O0`
where glibc fortification warns. The other hardening flags stay on. Like the warning options,
hardening covers first-party code only; dependency binaries from the Conan cache are not rebuilt
with these flags.

## IDE setup

### VS Code

Prerequisite: run `make bootstrap` so Conan generates `ConanPresets.json`.

1. Open the project folder in VS Code.
2. Accept the recommended extensions.
3. Select the matching public preset in CMake Tools.
4. Select a target in the CMake Tools sidebar.
5. Start `Debug: CMake Target` or press F5.

Setup is complete when F5 builds the selected debug target and launches it from `build/debug`.

### CLion

Prerequisite: run `make bootstrap` so Conan generates `ConanPresets.json`.

In CLion:

1. Open the project root
2. Select the `debug`, `release`, `sanitize*`, or `coverage` preset as the active CMake profile
3. Reload CMake

Setup is complete when CLion finishes reloading the selected public preset without a configure
error.

> [!NOTE]
> No IDE-specific task files are required for the build. The presets are the source of truth.
> `debug`, `sanitize`, and `coverage` each use their own build tree, so switching between them
> does not require forcing a fresh reconfigure.
