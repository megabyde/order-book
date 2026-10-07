UNAME_S := $(shell uname -s)

COLOR_CYAN := \033[36m
COLOR_RED := \033[31m
COLOR_RESET := \033[0m

CLICOLOR ?= 1
GTEST_COLOR ?= 1
export CLICOLOR GTEST_COLOR

# Fail the recipe with a red ERROR message on stderr: $(DIE) "message". %b expands
# \n for multi-line messages. A sh -c one-liner (message as shell arg $0) rather than
# a $(call ...) macro because call splits its arguments on the commas messages contain.
# The subprocess cannot exit the recipe shell itself, so a call site must either end
# its recipe line (make checks each line's status) or run under `set -e` so the shell
# stops at the failed status; every site here does one or the other.
DIE := sh -c 'printf "$(COLOR_RED)ERROR:$(COLOR_RESET) %b\n" "$$0" >&2; exit 1'

# Both compilers emit gcov-format data (--coverage), reported by gcovr. The gcov
# tool must match the compiler: gcc's data needs gcov, clang's needs llvm-cov gcov.
# Darwin implies AppleClang (xcrun resolves the matching Xcode tool); clang-on-Linux
# users override with GCOV_EXECUTABLE="llvm-cov gcov".
ifeq ($(UNAME_S),Darwin)
HOMEBREW_LLVM_PREFIX := $(shell brew --prefix llvm 2>/dev/null)
ifneq ($(HOMEBREW_LLVM_PREFIX),)
export PATH := $(HOMEBREW_LLVM_PREFIX)/bin:$(PATH)
endif
GCOV_EXECUTABLE ?= xcrun llvm-cov gcov
else
GCOV_EXECUTABLE ?= gcov
endif

.DEFAULT_GOAL := help

ifneq ($(VERBOSE),1)
.SILENT:
endif

# ---------------------------------------------------------------------------
# Conan configuration: release gets its own profile state, coverage reuses the
# debug toolchain (its dependency graph is identical to Debug), and sanitize uses
# a dedicated profile plus a custom compiler.sanitizer setting (conan/settings_user.yml)
# so instrumented dependency binaries get a distinct package_id and are rebuilt
# rather than reused from cache.
# ---------------------------------------------------------------------------
CONAN_PROFILE ?= profiles/default

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
STAMP_DIR := build/.stamps
COVERAGE_DIR := build/coverage
COVERAGE_FAIL_UNDER ?= 100
SANITIZE_STAMPS := \
	$(STAMP_DIR)/sanitize.stamp \
	$(STAMP_DIR)/sanitize-asan.stamp \
	$(STAMP_DIR)/sanitize-ubsan.stamp
FORMAT_SOURCES = $(shell find include src tests -type f \( -name '*.hpp' -o -name '*.cpp' \))
CMAKE_FORMAT_SOURCES = CMakeLists.txt
TIDY_SOURCES = $(shell find src tests -type f -name '*.cpp')
# Tracked Markdown/JSON/YAML only; conan.lock is Conan-generated, so its formatting is
# Conan's, not prettier's.
PRETTIER_SOURCES = $(shell git ls-files '*.md' '*.json' '*.yml' '*.yaml' ':!conan.lock')
MARKDOWN_SOURCES = $(shell git ls-files '*.md')
PYTHON_SOURCES = scripts/ conanfile.py

define require-tool
	command -v $(1) >/dev/null || $(DIE) "$(1) not found"
endef

# ---------------------------------------------------------------------------
# Conan lock file (lazy: regenerated when conanfile.py changes)
# ---------------------------------------------------------------------------
# --lockfile-clean drops entries the current graph no longer uses; without it,
# `conan lock create` merges into the existing lock and stale pins accumulate after
# a version bump or a removed dependency.
conan.lock: conanfile.py $(CONAN_PROFILE)
	echo "Regenerating conan.lock..."
	conan lock create . -pr=$(CONAN_PROFILE) --lockfile-clean --lockfile-out=conan.lock

# ---------------------------------------------------------------------------
# Conan install
# ---------------------------------------------------------------------------
$(STAMP_DIR)/debug.stamp: BUILD_TYPE = Debug
$(STAMP_DIR)/release.stamp: BUILD_TYPE = Release
$(STAMP_DIR)/debug.stamp $(STAMP_DIR)/release.stamp: conan.lock
	echo "Installing Conan dependencies ($(BUILD_TYPE))..."
	mkdir -p $(STAMP_DIR)
	conan install . -pr=$(CONAN_PROFILE) -s="build_type=$(BUILD_TYPE)" --build=missing --lockfile=conan.lock
	touch $@

$(SANITIZE_STAMPS): $(STAMP_DIR)/%.stamp: conan.lock conan/settings_user.yml profiles/sanitize-common profiles/%
	echo "Installing Conan dependencies ($*)..."
	mkdir -p $(STAMP_DIR)
	# conan config install would overwrite a settings_user.yml another project (or the
	# user's dotfiles) put in the Conan home. Install only when none exists; otherwise
	# leave the file untouched and require it to cover this project's sanitizer values.
	# A superset (extra compilers, values, or subsettings) is fine.
	$(call require-tool,python3)
	installed="$$(conan config home)/settings_user.yml"; \
	if [ ! -f "$$installed" ]; then \
		conan config install conan/; \
	elif ! python3 scripts/check_settings_subset.py conan/settings_user.yml "$$installed"; then \
		$(DIE) "$$installed lacks sanitizer values this project needs (see above).\nMerge conan/settings_user.yml into it by hand, then rerun."; \
	fi
	conan install . -pr=profiles/$* --build=missing --lockfile=conan.lock
	touch $@

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
.PHONY: help
help: ## Show this help message
	@awk 'BEGIN {FS = ":.*##"; printf "Usage: make [options] $(COLOR_CYAN)[target] ...$(COLOR_RESET)\n\n"} \
	/^[a-zA-Z_-]+:.*##/ {printf "  $(COLOR_CYAN)%-20s$(COLOR_RESET) %s\n", $$1, $$2}' \
		$(MAKEFILE_LIST)

.PHONY: bootstrap
bootstrap: $(STAMP_DIR)/debug.stamp $(STAMP_DIR)/release.stamp ## Install Conan dependencies for debug+release
.PHONY: bootstrap-sanitize
bootstrap-sanitize: $(SANITIZE_STAMPS) ## Install sanitizer-instrumented Conan dependencies

# ---------------------------------------------------------------------------
# Build + test via workflow presets
# ---------------------------------------------------------------------------
.PHONY: debug
debug: ## Build and test via the debug workflow preset

.PHONY: release
release: ## Build and test via the release workflow preset

.PHONY: sanitize
sanitize: ## Build and test via the sanitize workflow preset

.PHONY: sanitize-asan
sanitize-asan: ## Build and test via the ASan workflow preset

.PHONY: sanitize-ubsan
sanitize-ubsan: ## Build and test via the UBSan workflow preset

.PHONY: coverage
coverage: coverage-data-clean ## Build and test via the coverage workflow preset

.PHONY: docs
docs: $(STAMP_DIR)/debug.stamp ## Generate Doxygen HTML documentation
	cmake --preset docs
	cmake --build --preset docs

debug release sanitize sanitize-asan sanitize-ubsan: %: $(STAMP_DIR)/%.stamp
	cmake --workflow --preset $@

# coverage reuses the debug Conan dependency graph (see the Conan configuration note above).
coverage: $(STAMP_DIR)/debug.stamp
	cmake --workflow --preset $@

# ---------------------------------------------------------------------------
# Coverage report generation
# ---------------------------------------------------------------------------
.PHONY: coverage-data-clean
coverage-data-clean:
	-find $(COVERAGE_DIR) -name '*.gcda' -delete 2>/dev/null

.PHONY: coverage-report
coverage-report: coverage ## Generate an HTML coverage report and enforce the line floor
	$(call require-tool,python3)
	mkdir -p $(COVERAGE_DIR)/coverage-report
	python3 -m gcovr --root . --gcov-executable "$(GCOV_EXECUTABLE)" \
		--filter 'include/' --filter 'src/' --exclude 'tests/' \
		--html-details $(COVERAGE_DIR)/coverage-report/index.html \
		--cobertura $(COVERAGE_DIR)/coverage.xml \
		--txt-summary --fail-under-line $(COVERAGE_FAIL_UNDER) \
		$(COVERAGE_DIR)

# ---------------------------------------------------------------------------
# Lint and format
# ---------------------------------------------------------------------------
.PHONY: lint
lint: $(STAMP_DIR)/debug.stamp ## Run clang-tidy, cmake-lint, ruff, and markdownlint
	$(call require-tool,clang-tidy)
	cmake --preset debug
	PATH="$(PATH)" clang-tidy -p build/debug $(TIDY_SOURCES)
	echo "Linting CMake sources..."
	$(call require-tool,cmake-lint)
	cmake-lint $(CMAKE_FORMAT_SOURCES)
	echo "Linting Python sources..."
	$(call require-tool,ruff)
	ruff check $(PYTHON_SOURCES)
	echo "Linting Markdown sources..."
	$(call require-tool,markdownlint-cli2)
	markdownlint-cli2 $(MARKDOWN_SOURCES)

.PHONY: format
format: ## Format C++, CMake, Python, and Markdown/JSON/YAML sources in place
	echo "Formatting C++ sources..."
	$(call require-tool,clang-format)
	PATH="$(PATH)" clang-format -i $(FORMAT_SOURCES)
	echo "Formatting CMake sources..."
	$(call require-tool,cmake-format)
	cmake-format -i $(CMAKE_FORMAT_SOURCES)
	echo "Formatting Python sources..."
	$(call require-tool,ruff)
	ruff format $(PYTHON_SOURCES)
	echo "Formatting Markdown/JSON/YAML sources..."
	$(call require-tool,prettier)
	prettier --log-level warn --write $(PRETTIER_SOURCES)

.PHONY: format-check
format-check: ## Fail if any formatted source type is not format-clean
	echo "Checking C++ formatting..."
	$(call require-tool,clang-format)
	PATH="$(PATH)" clang-format --dry-run --Werror $(FORMAT_SOURCES)
	echo "Checking CMake formatting..."
	$(call require-tool,cmake-format)
	cmake-format --check $(CMAKE_FORMAT_SOURCES)
	echo "Checking Python formatting..."
	$(call require-tool,ruff)
	ruff format --check $(PYTHON_SOURCES)
	echo "Checking Markdown/JSON/YAML formatting..."
	$(call require-tool,prettier)
	prettier --check $(PRETTIER_SOURCES)

# ---------------------------------------------------------------------------
# Lock and clean
# ---------------------------------------------------------------------------
.PHONY: lock
lock: ## Force-regenerate conan.lock from conanfile.py
	echo "Regenerating conan.lock..."
	conan lock create . -pr=$(CONAN_PROFILE) --lockfile-clean --lockfile-out=conan.lock

# Scope: validates the graph resolved by the default profile only. If requirements()
# ever gains platform-conditional requires (the pattern its comment advertises), this
# check needs to run once per platform profile to cover the full graph.
.PHONY: lock-check
lock-check: ## Fail if conan.lock is out of date with conanfile.py (used by CI)
	set -e; tmp=$$(mktemp); trap 'rm -f "$$tmp"' EXIT; \
	conan lock create . -pr=$(CONAN_PROFILE) --lockfile-clean --lockfile-out=$$tmp >/dev/null 2>&1 \
		|| $(DIE) "conan lock create failed"; \
	if diff conan.lock $$tmp >/dev/null; then \
		echo "conan.lock is up to date"; \
	else \
		diff conan.lock $$tmp || true; \
		$(DIE) "conan.lock is stale; run 'make lock' and commit the result"; \
	fi

.PHONY: clean
clean: ## Remove generated build artifacts and Conan preset files
	echo "Removing generated artifacts..."
	rm -rf build/ compile_commands.json CMakeUserPresets.json ConanPresets.json
