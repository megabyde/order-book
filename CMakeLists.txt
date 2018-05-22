cmake_minimum_required(VERSION 3.5)
project(order-book CXX)

option(BUILD_TESTS "Build unit tests." OFF)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(t)
  add_subdirectory(external/googletest)
  # Prevent overriding the parent project's compiler/linker settings on Windows
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

find_package(Threads REQUIRED)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 14)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /wd4100 /WX /EHsc)
else()
  add_compile_options(-g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror)
endif()

add_executable(order-book main.cpp)
add_library(order_book order_book.cpp)
target_link_libraries(order-book order_book ${CMAKE_THREAD_LIBS_INIT})
