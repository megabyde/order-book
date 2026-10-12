# Encode the CSV feed CSV as ITCH with scripts/csv_to_itch.py, then fail unless APP's output on it
# with --format=itch matches EXPECTED
set(INPUT ${WORK}.itch)
set(ACTUAL ${WORK}.actual)
execute_process(
    COMMAND ${PYTHON} ${SOURCE_DIR}/scripts/csv_to_itch.py ${CSV} -o ${INPUT}
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "csv_to_itch.py exited with ${result}")
endif()
set(OPTION --format=itch)
include(${CMAKE_CURRENT_LIST_DIR}/compare_output.cmake)
