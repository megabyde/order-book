# Run APP with an optional OPTION and INPUT and fail unless it exits with EXIT and its stderr matches
# PATTERN, and its stdout STDOUT_PATTERN when that is set. RESULT_VARIABLE holds a description
# instead of a number when APP dies on a signal, so a crash fails the exit check. With CLOSED_STDOUT
# set, APP runs through a POSIX shell with its standard output closed, and with STDIN_FROM set, with
# its standard input redirected from that path.
set(command ${APP})
if(CLOSED_STDOUT)
    set(command sh -c [[exec "$0" "$@" >&-]] ${APP})
elseif(DEFINED STDIN_FROM)
    set(command sh -c [[exec "$0" "$@" < "$STDIN_FROM"]] ${APP})
    set(ENV{STDIN_FROM} ${STDIN_FROM})
endif()
execute_process(
    COMMAND ${command} ${OPTION} ${INPUT}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE result
)
if(NOT result STREQUAL EXIT)
    message(FATAL_ERROR "${APP} exited with ${result}, expected ${EXIT}")
endif()
if(NOT stderr MATCHES "${PATTERN}")
    message(FATAL_ERROR "stderr does not match '${PATTERN}':\n${stderr}")
endif()
if(DEFINED STDOUT_PATTERN AND NOT stdout MATCHES "${STDOUT_PATTERN}")
    message(FATAL_ERROR "stdout does not match '${STDOUT_PATTERN}':\n${stdout}")
endif()
