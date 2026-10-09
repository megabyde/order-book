# Run APP on INPUT and fail unless its stdout matches EXPECTED, line endings aside: Windows writes
# CRLF to a text-mode stdout
execute_process(
    COMMAND ${APP} ${INPUT}
    OUTPUT_FILE ${ACTUAL}
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${APP} exited with ${result}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol ${EXPECTED} ${ACTUAL} RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
    message(FATAL_ERROR "output differs: ${EXPECTED} vs ${ACTUAL}")
endif()
