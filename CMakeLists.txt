# CMake 3.29 is required for CMAKE_LINKER_TYPE, which the Conan toolchain sets when mold or LLD is
# on PATH. The project also uses workflow presets and CMakePresets schema v6 (3.25),
# COMPILE_WARNING_AS_ERROR (3.24), and cxx_std_23 (3.20).
cmake_minimum_required(VERSION 3.29)

project(
    order_book
    VERSION 0.1.0
    DESCRIPTION "Exchange feed order book that prints the best bid and offer"
    LANGUAGES CXX
)

include(CTest)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS
    ON
    CACHE BOOL "Enable color diagnostics in generated build systems"
)

option(WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(ENABLE_HARDENING "Enable hardening compile and link flags" ON)
option(ENABLE_LTO "Enable link-time optimization for Release builds" ON)

include(CheckIPOSupported)
if(ENABLE_LTO)
    check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_UNSUPPORTED_REASON)
    if(NOT LTO_SUPPORTED)
        message(STATUS "LTO not supported by this toolchain: ${LTO_UNSUPPORTED_REASON}")
    endif()
endif()

include(CheckPIESupported)
if(ENABLE_HARDENING)
    # Without a successful check, POSITION_INDEPENDENT_CODE only adds the compile flag (-fPIE) and
    # CMake silently drops the -pie link flag, leaving the executable non-PIE.
    check_pie_supported(OUTPUT_VARIABLE PIE_UNSUPPORTED_REASON LANGUAGES CXX)
    if(NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
        message(STATUS "PIE link flags not supported: ${PIE_UNSUPPORTED_REASON}")
    endif()
endif()

if(CMAKE_EXPORT_COMPILE_COMMANDS
   AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles"
   AND NOT CMAKE_BINARY_DIR STREQUAL PROJECT_SOURCE_DIR
)
    file(REMOVE ${PROJECT_SOURCE_DIR}/compile_commands.json)
    file(CREATE_LINK ${CMAKE_BINARY_DIR}/compile_commands.json
         ${PROJECT_SOURCE_DIR}/compile_commands.json SYMBOLIC COPY_ON_ERROR
    )
endif()

find_package(Doxygen QUIET)

function(enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(WARNINGS_AS_ERRORS)
        # Adds /WX for MSVC and -Werror for GCC/Clang.
        set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    endif()
endfunction()

function(enable_coverage target)
    if(NOT ENABLE_COVERAGE)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE --coverage)
        target_link_options(${target} PRIVATE --coverage)
    else()
        message(FATAL_ERROR "Coverage requires GCC, Clang, or AppleClang")
    endif()
endfunction()

function(enable_hardening target)
    if(NOT ENABLE_HARDENING)
        return()
    endif()

    if(MSVC)
        # Control Flow Guard requires both compile and link flags; /GS is on by default.
        target_compile_options(${target} PRIVATE /guard:cf)
        target_link_options(${target} PRIVATE /guard:cf)
        # CET shadow stack, a link-time marker the OS honors on capable hardware and ignores
        # elsewhere. x64 only: the linker rejects the flag for ARM64 targets.
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
            target_link_options(${target} PRIVATE /CETCOMPAT)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        # Stack protector always; fortified libc calls only in optimized configs (fortify needs
        # optimization, so Debug skips it). -U first: Ubuntu's gcc predefines _FORTIFY_SOURCE when
        # optimizing, and redefining it is fatal under -Werror. Compile options, not definitions, to
        # keep the -U/-D order.
        target_compile_options(
            ${target} PRIVATE -fstack-protector-strong
                              $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>
        )
        # Control-flow protection per architecture: CET landing pads (endbr64) on x86-64, PAC
        # return-address signing plus BTI on AArch64. Both encode as NOPs on hardware without the
        # feature. AArch64 is Linux-only: on macOS arm64 the compact-unwind format cannot describe
        # pac-ret frames, so a thrown exception skips every catch and terminates (Apple reserves
        # pointer auth for its arm64e ABI, which signs OS-wide without this flag).
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            target_compile_options(${target} PRIVATE -fcf-protection=full)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND CMAKE_SYSTEM_NAME STREQUAL
                                                                      "Linux"
        )
            target_compile_options(${target} PRIVATE -mbranch-protection=standard)
        endif()
        # PIE makes ASLR cover the program image itself, not just libraries. Applied to the static
        # library too so its objects compile as -fPIE and can link into the PIE executable. The
        # link-side -pie needs the check_pie_supported() call above.
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        # ELF linker mitigations: full RELRO (GOT read-only after eager binding) and an explicitly
        # non-executable stack. Linux-gated because macOS's ld64 has no -z options; Mach-O images
        # get the equivalent protections by default.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_options(${target} PRIVATE -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
        endif()
    endif()
    # Other compilers fall through and build unhardened.
endfunction()

function(enable_lto target)
    # The _RELEASE suffix scopes this to the Release config under any generator, single- or
    # multi-config alike.
    if(ENABLE_LTO AND LTO_SUPPORTED)
        set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endfunction()

function(enable_project_options target)
    enable_project_warnings(${target})
    enable_coverage(${target})
    enable_hardening(${target})
    enable_lto(${target})
endfunction()

# --- library ---------------------------------------------------------------
# Non-main code lives in a library so the application and the tests link the same artifact instead
# of recompiling sources. Include dirs and the language standard are PUBLIC so both consumers
# inherit them.
add_library(order_book_lib STATIC src/order_book.cpp src/split.cpp)
target_include_directories(order_book_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(order_book_lib PUBLIC cxx_std_23)
enable_project_options(order_book_lib)

# --- application -----------------------------------------------------------
# Generate a version header from the project() version (the single source of truth; the Conan recipe
# reads the same value) and expose it to the application only.
configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/version.hpp.in
    ${PROJECT_BINARY_DIR}/generated/order_book/version.hpp @ONLY
)

add_executable(order_book src/main.cpp)
target_include_directories(order_book PRIVATE ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(order_book PRIVATE order_book_lib)
enable_project_options(order_book)

# Install only the application binary. The static library is an internal build artifact, not a
# public deliverable, so it is deliberately not installed or exported.
include(GNUInstallDirs)
install(TARGETS order_book RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- documentation ---------------------------------------------------------
if(Doxygen_FOUND)
    set(DOXYGEN_EXTRACT_ALL NO)
    set(DOXYGEN_FULL_PATH_NAMES NO)
    # Copies LICENSE next to index.html so README's license badge (a relative ./LICENSE link)
    # resolves in the generated site too; Doxygen does not rewrite that link on its own, since a
    # bare LICENSE has no extension it treats as a documentable page type.
    set(DOXYGEN_HTML_EXTRA_FILES ${PROJECT_SOURCE_DIR}/LICENSE)
    set(DOXYGEN_HTML_OUTPUT html)
    set(DOXYGEN_MARKDOWN_ID_STYLE GITHUB)
    set(DOXYGEN_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
    set(DOXYGEN_SOURCE_BROWSER YES)
    set(DOXYGEN_STRIP_FROM_PATH ${PROJECT_SOURCE_DIR})
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${PROJECT_SOURCE_DIR}/README.md)
    set(DOXYGEN_WARN_AS_ERROR YES)

    # LICENSE is also an input, not just an HTML_EXTRA_FILES entry. Editing it then invalidates the
    # docs target, and Doxygen includes it in the source browser.
    doxygen_add_docs(
        docs ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/docs/development.md
        ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/include
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Generate API documentation"
    )
else()
    add_custom_target(
        docs
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found. Install Doxygen to build docs."
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "Generate API documentation"
    )
endif()

# --- tests -----------------------------------------------------------------
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
    add_executable(
        order_book_test tests/order_book_test.cpp tests/split_test.cpp tests/utils_test.cpp
    )
    target_link_libraries(order_book_test PRIVATE order_book_lib GTest::gtest_main)
    enable_project_options(order_book_test)

    gtest_discover_tests(order_book_test DISCOVERY_MODE PRE_TEST)

    # End-to-end tests: together they cover all of src/main.cpp, including under the sanitizer and
    # coverage presets.
    add_test(NAME AppTest.PrintsUsage COMMAND order_book)
    set_tests_properties(AppTest.PrintsUsage PROPERTIES PASS_REGULAR_EXPRESSION "Usage: .* FILE")

    add_test(
        NAME AppTest.ReplaysSample
        COMMAND
            ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:order_book>
            -DINPUT=${PROJECT_SOURCE_DIR}/tests/data/sample.csv
            -DEXPECTED=${PROJECT_SOURCE_DIR}/tests/data/sample.expected
            -DACTUAL=${PROJECT_BINARY_DIR}/sample.actual -P
            ${PROJECT_SOURCE_DIR}/tests/compare_output.cmake
    )
endif()
