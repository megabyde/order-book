# Development workflows

Prerequisite: complete the [README setup](../README.md) for your platform first.

## Sanitizers (ASan + UBSan)

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

The sanitizer run is complete when the selected target exits successfully after running the tests.
If a sanitizer detects an error, it prints a stack trace, stops at the first finding, and returns a
failure through CTest to the selected `make` target. Fix the reported source error, then rerun the
same target.

`make sanitize` is the combined mode used by CI. To install all three instrumented dependency graphs
without running their workflows, use `make bootstrap-sanitize`. The default `make bootstrap` does
not install them.

All three profiles inherit [`profiles/sanitize-common`](../profiles/sanitize-common), which includes
`profiles/default`, selects `Debug`, and defines the shared instrumentation flags and `[runenv]`.
Each mode appends its own `-fsanitize` flags and shares `conan.lock`.

The custom `compiler.sanitizer` setting gives each instrumented dependency graph a distinct Conan
`package_id`. Without it, `--build=missing` could reuse plain Debug dependencies. The setting
defaults to `null`, which is omitted from `package_id`, so non-sanitizer builds do not change.
First-party targets receive the same profile flags through the Conan toolchain.

ASan/UBSan runtime options such as `halt_on_error` and `print_stacktrace` live under `[runenv]` in
`profiles/sanitize-common`. Conan injects them into the generated test presets inherited by the
public `sanitize*` presets, keeping the runtime configuration out of `CMakePresets.json`.

## Tests

Tests are controlled by CMake's built-in `BUILD_TESTING` option from `include(CTest)`. This project
leaves it at the default `ON`, so `make debug`, `make release`, `make sanitize*`, and
`make coverage` all run the GTest suite.

## Public presets

The main workflow presets are `debug`, `release`, `sanitize`, `sanitize-asan`, `sanitize-ubsan`, and
`coverage`. Configure, build, and test presets use the same names. `docs` is configure/build only
because it generates Doxygen HTML instead of compiling and testing the application.

The Conan-generated `conan-*` presets are internal implementation details and are not the public
interface for developers or CI.

## Dependency lock file

Outcome: updated dependency pins and a matching `conan.lock` for reproducible builds.

Update dependencies in this order:

1. Edit version pins in `conanfile.py`.
2. Regenerate the lock file with `make lock`.
3. Run the appropriate `make` target to verify.
4. Commit both `conanfile.py` and `conan.lock`.

The update is complete when the selected build and test workflow passes and both files contain the
intended dependency change.

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
interface.

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

### Allocators

This template does not replace the system allocator, and no preset or CMake option selects
[mimalloc](https://github.com/microsoft/mimalloc), [jemalloc](https://github.com/jemalloc/jemalloc),
or [tcmalloc](https://github.com/google/tcmalloc). An allocator is chosen against a measured
allocation profile, which a boilerplate does not have. Three constraints make it more than a
link-line change here:

- ASan installs its own `malloc`/`free` interceptors. An override linked on top of them either fails
  to link or leaves the heap diagnostics silently disabled, so the `sanitize` and `sanitize-asan`
  presets would have to gate the allocator off. UBSan does not replace the allocator, so
  `sanitize-ubsan` can keep it.
- The override mechanism is per platform. mimalloc's static override works on Linux; macOS needs
  runtime interposition through `DYLD_INSERT_LIBRARIES`; Windows needs `mimalloc-redirect.dll`
  beside the executable, which the `install(TARGETS)` rule does not ship.
- The override is process-wide, but dependency binaries come from the Conan cache and are not
  rebuilt. Any measurement has to cover the whole process, not the first-party targets alone.

To add one, follow the path the recipe already uses for optional tools: declare a Conan option in
[`conanfile.py`](../conanfile.py), add the requirement under `requirements()`, forward the choice to
CMake through `tc.cache_variables` next to the `ccache` and `mold`/LLD probes in `generate()`, and
fail configuration when `compiler.sanitizer` is `Address` or `AddressUndefinedBehavior`. Rerun
`make lock` afterwards.

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
