if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "gradient headless test requires executable, resources, scenario, and output")
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
    message(FATAL_ERROR "gradient headless scenario failed (${status})\n${output}\n${error}")
endif()

foreach(artifact gradient.png gradient.json adjusted.png adjusted.json result.json)
    if(NOT EXISTS "${STRATA_OUTPUT}/${artifact}")
        message(FATAL_ERROR "gradient scenario did not create ${artifact}")
    endif()
    file(SIZE "${STRATA_OUTPUT}/${artifact}" artifact_size)
    if(artifact_size EQUAL 0)
        message(FATAL_ERROR "gradient scenario created empty ${artifact}")
    endif()
endforeach()

file(READ "${STRATA_OUTPUT}/adjusted.json" frame)
if(NOT frame MATCHES "Gradient stop 5")
    message(FATAL_ERROR "gradient keyboard insertion did not reach semantic output")
endif()
file(READ "${STRATA_OUTPUT}/result.json" result)
if(NOT result MATCHES "control-deck.gradient.commit" OR
   NOT result MATCHES "gradient-committed")
    message(FATAL_ERROR "gradient changes did not emit the declared commit contract")
endif()
if(NOT result MATCHES "\"diagnostics\": \\[\\]" OR
   NOT result MATCHES "\"materialFallbacks\": \\[\\]")
    message(FATAL_ERROR "gradient scenario reported diagnostics or a D3D11 material fallback")
endif()
