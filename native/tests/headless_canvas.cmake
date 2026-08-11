if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "canvas headless test requires executable, resources, scenario, and output")
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
    message(FATAL_ERROR "canvas headless scenario failed (${status})\n${output}\n${error}")
endif()

foreach(name canvas edited selected zoomed panned keyboard idle)
    foreach(extension png json)
        set(artifact "${name}.${extension}")
        if(NOT EXISTS "${STRATA_OUTPUT}/${artifact}")
            message(FATAL_ERROR "canvas scenario did not create ${artifact}")
        endif()
        file(SIZE "${STRATA_OUTPUT}/${artifact}" artifact_size)
        if(artifact_size EQUAL 0)
            message(FATAL_ERROR "canvas scenario created empty ${artifact}")
        endif()
    endforeach()
endforeach()
if(NOT EXISTS "${STRATA_OUTPUT}/result.json")
    message(FATAL_ERROR "canvas scenario did not create result.json")
endif()

file(SHA256 "${STRATA_OUTPUT}/canvas.png" canvas_hash)
file(SHA256 "${STRATA_OUTPUT}/edited.png" edited_hash)
file(SHA256 "${STRATA_OUTPUT}/selected.png" selected_hash)
file(SHA256 "${STRATA_OUTPUT}/zoomed.png" zoomed_hash)
file(SHA256 "${STRATA_OUTPUT}/panned.png" panned_hash)
if(canvas_hash STREQUAL edited_hash OR edited_hash STREQUAL selected_hash OR
   selected_hash STREQUAL zoomed_hash OR zoomed_hash STREQUAL panned_hash)
    message(FATAL_ERROR "curve editing, lasso, zoom, or pan did not produce distinct frames")
endif()

file(READ "${STRATA_OUTPUT}/selected.json" selected)
file(READ "${STRATA_OUTPUT}/panned.json" panned)
if(NOT selected MATCHES "\"virtualIndex\": 4073" OR
   NOT panned MATCHES "\"virtualIndex\": 4073" OR
   NOT selected MATCHES "\"name\": \"Curve point 4074\"")
    message(FATAL_ERROR "the selected curve point lost its stable accessible identity")
endif()

file(READ "${STRATA_OUTPUT}/idle.json" idle)
if(NOT idle MATCHES "\"nodesVisited\": 0" OR
   NOT idle MATCHES "\"layoutWork\": 0")
    message(FATAL_ERROR "idle curve editor retained render or layout work")
endif()

file(READ "${STRATA_OUTPUT}/result.json" result)
if(NOT result MATCHES "control-deck.canvas.commit" OR
   NOT result MATCHES "canvas-committed" OR
   NOT result MATCHES "selected[^0-9]+32" OR
   NOT result MATCHES "zoom[^0-9]+1.08" OR
   NOT result MATCHES "centerX[^0-9]+4110.85")
    message(FATAL_ERROR "canvas scenario lost editing, lasso, zoom, pan, or commit state")
endif()
if(NOT result MATCHES "\"diagnostics\": \\[\\]" OR
   NOT result MATCHES "\"materialFallbacks\": \\[\\]")
    message(FATAL_ERROR "canvas scenario reported diagnostics or a D3D11 material fallback")
endif()
