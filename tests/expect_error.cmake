# Run APP with an optional INPUT and fail unless it exits with EXIT and its stderr matches PATTERN.
# RESULT_VARIABLE holds a description instead of a number when APP dies on a signal, so a crash
# fails the exit check
execute_process(
    COMMAND ${APP} ${INPUT}
    OUTPUT_QUIET
    ERROR_VARIABLE stderr
    RESULT_VARIABLE result
)
if(NOT result STREQUAL EXIT)
    message(FATAL_ERROR "${APP} exited with ${result}, expected ${EXIT}")
endif()
if(NOT stderr MATCHES "${PATTERN}")
    message(FATAL_ERROR "stderr does not match '${PATTERN}':\n${stderr}")
endif()
