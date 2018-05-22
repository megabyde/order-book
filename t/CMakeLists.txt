file(GLOB TESTS RELATIVE ${CMAKE_CURRENT_LIST_DIR} *_test.cpp)

foreach(test ${TESTS})
  string(REPLACE _test.cpp _test target ${test})
  add_executable(${target} ${test})
  add_test(NAME ${target} COMMAND ${target})
  target_link_libraries(${target} order_book gtest gtest_main gmock)
endforeach()
