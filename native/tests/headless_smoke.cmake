if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "headless smoke requires executable, resources, scenario, and output")
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
    message(FATAL_ERROR "headless scenario failed (${status})\n${output}\n${error}")
endif()

foreach(artifact initial.png initial.json after_pulse.png after_pulse.json result.json)
    if(NOT EXISTS "${STRATA_OUTPUT}/${artifact}")
        message(FATAL_ERROR "headless scenario did not create ${artifact}")
    endif()
    file(SIZE "${STRATA_OUTPUT}/${artifact}" artifact_size)
    if(artifact_size EQUAL 0)
        message(FATAL_ERROR "headless scenario created empty ${artifact}")
    endif()
endforeach()

file(READ "${STRATA_OUTPUT}/after_pulse.json" frame)
if(NOT frame MATCHES "extend.custom.pulse")
    message(FATAL_ERROR "headless frame snapshot lost the selected retained node")
endif()
file(READ "${STRATA_OUTPUT}/result.json" result)
if(NOT result MATCHES "demo.custom.pulse")
    message(FATAL_ERROR "headless pointer control did not traverse ordinary action dispatch")
endif()
if(NOT result MATCHES "\"backend\": \"d3d11\"")
    message(FATAL_ERROR "headless smoke did not use the offscreen D3D11 backend")
endif()
if(result MATCHES "demo:aurora")
    message(FATAL_ERROR "headless D3D11 backend fell back instead of compiling the authored shader")
endif()
