# Generate a synthetic feed with scripts/gen_feed.py, take the expected output from the independent
# model in scripts/reference_bbo.py, then compare APP against it.
foreach(script gen_feed reference_bbo)
    set(${script} ${SOURCE_DIR}/scripts/${script}.py)
endforeach()
set(INPUT ${WORK}.csv)
set(EXPECTED ${WORK}.expected)
set(ACTUAL ${WORK}.actual)

execute_process(
    COMMAND ${PYTHON} ${gen_feed} --events ${EVENTS} --tickers ${TICKERS} --seed ${SEED} -o
            ${INPUT} RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "gen_feed.py exited with ${result}")
endif()
execute_process(
    COMMAND ${PYTHON} ${reference_bbo} ${INPUT}
    OUTPUT_FILE ${EXPECTED}
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "reference_bbo.py exited with ${result}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/compare_output.cmake)
