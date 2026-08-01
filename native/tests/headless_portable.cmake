if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "portable headless smoke arguments are incomplete")
endif()

file(REMOVE_RECURSE "${STRATA_OUTPUT}")
execute_process(
    COMMAND "${STRATA_HEADLESS}"
        --resources "${STRATA_RESOURCES}"
        --scenario "${STRATA_SCENARIO}"
        --output "${STRATA_OUTPUT}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "portable headless scenario failed (${status})\n${output}\n${error}")
endif()

foreach(artifact initial.png initial.json after_click.png after_click.json result.json)
    if(NOT EXISTS "${STRATA_OUTPUT}/${artifact}")
        message(FATAL_ERROR "portable headless scenario did not create ${artifact}")
    endif()
    file(SIZE "${STRATA_OUTPUT}/${artifact}" artifact_size)
    if(artifact_size EQUAL 0)
        message(FATAL_ERROR "portable headless scenario created empty ${artifact}")
    endif()
endforeach()

file(READ "${STRATA_OUTPUT}/after_click.json" frame)
if(NOT frame MATCHES "Clicked 1 times")
    message(FATAL_ERROR "portable pointer input did not mutate local application state")
endif()
file(READ "${STRATA_OUTPUT}/result.json" result)
if(NOT result MATCHES "\"backend\": \"reference\"")
    message(FATAL_ERROR "portable headless smoke did not use the reference backend")
endif()
