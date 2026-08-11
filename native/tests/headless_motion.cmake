if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "motion headless test requires executable, resources, scenario, and output")
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
    message(FATAL_ERROR "motion headless scenario failed (${status})\n${output}\n${error}")
endif()

foreach(artifact motion.png motion.json in-flight.png in-flight.json settled.png settled.json idle.png idle.json result.json)
    if(NOT EXISTS "${STRATA_OUTPUT}/${artifact}")
        message(FATAL_ERROR "motion scenario did not create ${artifact}")
    endif()
    file(SIZE "${STRATA_OUTPUT}/${artifact}" artifact_size)
    if(artifact_size EQUAL 0)
        message(FATAL_ERROR "motion scenario created empty ${artifact}")
    endif()
endforeach()

file(SHA256 "${STRATA_OUTPUT}/in-flight.png" moving_hash)
file(SHA256 "${STRATA_OUTPUT}/settled.png" settled_hash)
if(moving_hash STREQUAL settled_hash)
    message(FATAL_ERROR "inertial scrubber did not produce a distinct settled frame")
endif()

file(READ "${STRATA_OUTPUT}/idle.json" idle)
if(NOT idle MATCHES "\"nodesVisited\": 0" OR
   NOT idle MATCHES "\"layoutWork\": 0")
    message(FATAL_ERROR "idle inertial scrubber retained frame, render, or layout work")
endif()
file(READ "${STRATA_OUTPUT}/result.json" result)
if(NOT result MATCHES "control-deck.motion.commit" OR
   NOT result MATCHES "motion-committed")
    message(FATAL_ERROR "inertial settling did not emit the declared commit contract")
endif()
if(NOT result MATCHES "\"diagnostics\": \\[\\]" OR
   NOT result MATCHES "\"materialFallbacks\": \\[\\]")
    message(FATAL_ERROR "motion scenario reported diagnostics or a D3D11 material fallback")
endif()
