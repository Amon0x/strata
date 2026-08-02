if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_D3D11_SCENARIO OR NOT DEFINED STRATA_PORTABLE_SCENARIO OR
   NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "headless effects test requires executable, resources, scenarios, and output")
endif()

function(run_effect_scenario scenario output)
    file(REMOVE_RECURSE "${output}")
    execute_process(
        COMMAND "${STRATA_HEADLESS}"
            --resources "${STRATA_RESOURCES}"
            --scenario "${scenario}"
            --output "${output}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "headless effect scenario failed (${status})\n${stdout}\n${stderr}")
    endif()
    foreach(artifact liquid_glass.png liquid_glass.json result.json)
        if(NOT EXISTS "${output}/${artifact}")
            message(FATAL_ERROR "headless effect scenario did not create ${artifact}")
        endif()
        file(SIZE "${output}/${artifact}" artifact_size)
        if(artifact_size EQUAL 0)
            message(FATAL_ERROR "headless effect scenario created empty ${artifact}")
        endif()
    endforeach()
endfunction()

set(d3d_output "${STRATA_OUTPUT}/d3d11")
set(portable_output "${STRATA_OUTPUT}/portable")
set(portable_repeat_output "${STRATA_OUTPUT}/portable-repeat")
run_effect_scenario("${STRATA_D3D11_SCENARIO}" "${d3d_output}")
run_effect_scenario("${STRATA_PORTABLE_SCENARIO}" "${portable_output}")
run_effect_scenario("${STRATA_PORTABLE_SCENARIO}" "${portable_repeat_output}")

file(READ "${d3d_output}/result.json" d3d_result)
if(NOT d3d_result MATCHES "\\\"backend\\\": \\\"d3d11\\\"")
    message(FATAL_ERROR "authored effect scenario did not use D3D11")
endif()
file(READ "${d3d_output}/liquid_glass.json" d3d_frame)
if(NOT d3d_frame MATCHES "effect.fixture.select.option.glass" OR
   NOT d3d_frame MATCHES "effect.fixture.select.popup")
    message(FATAL_ERROR "authored Select templates were not materialized into the portal tree")
endif()
foreach(effect_id
        "demo:liquid-glass"
        "demo:frosted-backdrop"
        "demo:content-prism"
        "demo:soft-content")
    if(NOT d3d_frame MATCHES "${effect_id}")
        message(FATAL_ERROR "effect gallery did not render ${effect_id}")
    endif()
endforeach()
file(READ "${portable_output}/result.json" portable_result)
if(NOT portable_result MATCHES "\\\"backend\\\": \\\"reference\\\"")
    message(FATAL_ERROR "authored effect fallback did not use the software renderer")
endif()

file(SHA256 "${d3d_output}/liquid_glass.png" d3d_hash)
file(SHA256 "${portable_output}/liquid_glass.png" portable_hash)
file(SHA256 "${portable_repeat_output}/liquid_glass.png" portable_repeat_hash)
if(d3d_hash STREQUAL portable_hash)
    message(FATAL_ERROR "D3D11 authored shader output unexpectedly matches the software approximation")
endif()
if(NOT portable_hash STREQUAL portable_repeat_hash)
    message(FATAL_ERROR "software authored-effect approximation is not deterministic")
endif()
