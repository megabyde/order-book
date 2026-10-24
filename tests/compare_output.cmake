# Run APP with an optional OPTION on INPUT and fail unless it exits with 0 and its stdout matches
# EXPECTED, line endings aside: Windows writes CRLF to a text-mode stdout. With STDIN set, APP reads
# INPUT from its standard input as `-`; with STDERR_PATTERN set, its stderr must match it.
if(STDIN)
    set(arguments - INPUT_FILE ${INPUT})
else()
    set(arguments ${INPUT})
endif()
execute_process(
    COMMAND ${APP} ${OPTION} ${arguments}
    OUTPUT_FILE ${ACTUAL}
    ERROR_VARIABLE stderr
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${APP} exited with ${result}:\n${stderr}")
endif()
if(DEFINED STDERR_PATTERN AND NOT stderr MATCHES "${STDERR_PATTERN}")
    message(FATAL_ERROR "stderr does not match '${STDERR_PATTERN}':\n${stderr}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol ${EXPECTED} ${ACTUAL} RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
    message(FATAL_ERROR "output differs: ${EXPECTED} vs ${ACTUAL}")
endif()
