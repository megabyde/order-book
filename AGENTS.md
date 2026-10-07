# AGENTS

Contributor and coding-agent guide for this repository.

## Scope

This repository is a template. Every change lands in every project generated from it, so prefer the
narrower change and leave project-specific decisions to the generated repository.

Documentation is split by what it serves:

- `README.md`: the entry point. What the template provides, and how to instantiate it.
- `docs/development.md`: the workflow reference. Sanitizers, presets, the lock file, formatting and
  linting, coverage, documentation, build policy, and IDE setup.
- `SECURITY.md`: the vulnerability reporting process.
- `AGENTS.md`: the contributor process the user docs do not need.

Do not restate `docs/development.md` here. Link to the section instead.

## Commands

`make help` lists every target. The `Makefile` is the human entry point; `CMakePresets.json` is the
public build interface, and the workflow presets are what CI runs.

- `make debug` and `make release` build and test through the matching workflow preset.
- `make sanitize`, `make sanitize-asan`, and `make sanitize-ubsan` build instrumented trees.
- `make coverage` and `make coverage-report`, which enforces a line floor of `COVERAGE_FAIL_UNDER`,
  100 by default.
- `make lint` runs clang-tidy, cmake-lint, ruff, and markdownlint. `make format` rewrites in place
  and `make format-check` fails instead.
- `make lock` regenerates `conan.lock`; `make lock-check` is the CI guard.
- `make docs` generates the Doxygen HTML.

Run `make format`, then the workflow preset matching the change, then `make lint` before commit.
`make bootstrap` installs Conan dependencies for debug and release; sanitizer trees need
`make bootstrap-sanitize` first.

## Build Policy

The defaults differ from a stock CMake project, and the differences are deliberate. Read
[Build policy](docs/development.md#build-policy) before changing a flag.

- Warnings are errors in every preset. Fix the warning; do not configure with
  `-DWARNINGS_AS_ERRORS=OFF` to get a build through.
- Hardening and, for `release` only, link-time optimization are on by default. Both cover
  first-party targets alone, because dependency binaries come from the Conan cache and are not
  rebuilt. Do not describe either as a process-wide guarantee.
- Sanitizer and coverage builds omit fortification on purpose. Fortify conflicts with the ASan
  interceptors, and coverage builds run at `-O0` where glibc fortification warns.
- The template does not replace the system allocator, and the reasons are recorded under
  [Allocators](docs/development.md#allocators). Adding one is a measured decision for a generated
  project, not a template change.

## Dependencies

Update in this order: edit the pins in `conanfile.py`, run `make lock`, run the workflow that
covers the change, then commit `conanfile.py` and `conan.lock` together. A lock regenerated without
a pin change is a no-op commit; drop it rather than committing churn.

`scripts/check_settings_subset.py` verifies that an installed Conan `settings_user.yml` covers the
sanitizer values this project needs. It parses only Conan's documented two-level structure and uses
the standard library alone, because PyYAML is not available everywhere it runs. Unrecognized input
fails the check and expects a manual merge.

## Template Invariants

- The rename utility is the instantiation path, and the template workflow renames the repository in
  CI, then builds, tests, and runs the result. A change that adds a file carrying the project name,
  the namespace, an include path, or a GitHub link must teach the rename utility about it in the
  same change, or that workflow fails.
- The rename utility uses only the Python standard library. Keep it that way: it runs before any
  dependency is installed.
- C++23 is the language floor (`cxx_std_23`), and the CMake floor is stated at the top of
  `CMakeLists.txt` with the feature that sets it. Raise either only with the reason recorded there.

## Definition of Done

- `make format && make lint` is clean, and the workflow preset covering the change passes.
- Coverage stays at the floor. Exclude genuinely unreachable defensive code rather than lowering
  `COVERAGE_FAIL_UNDER`.
- Documentation changed in the same commit when the change touches presets, make targets, flags,
  the rename utility, or the dependency set.
- Commits follow Conventional Commits, and work reaches `main` through a pull request.
