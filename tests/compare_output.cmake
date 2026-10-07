# Run APP on INPUT and fail unless its stdout matches EXPECTED byte for byte.
execute_process(
    COMMAND ${APP} ${INPUT}
    OUTPUT_FILE ${ACTUAL}
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${APP} exited with ${result}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${EXPECTED} ${ACTUAL} RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
    message(FATAL_ERROR "output differs: ${EXPECTED} vs ${ACTUAL}")
endif()
