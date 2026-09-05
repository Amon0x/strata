foreach(required STRATA_HEADLESS STRATA_SCENARIO STRATA_RESOURCES STRATA_OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Vulkan replay is missing ${required}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${STRATA_OUTPUT}")
file(READ "${STRATA_SCENARIO}" scenario)
string(JSON scenario SET "${scenario}" surface backend "\"vulkan\"")
file(WRITE "${STRATA_OUTPUT}/scenario.json" "${scenario}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env STRATA_VULKAN_VALIDATION=1
        "${STRATA_HEADLESS}" --resources "${STRATA_RESOURCES}"
        --scenario "${STRATA_OUTPUT}/scenario.json" --output "${STRATA_OUTPUT}/capture"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 120
)
if(NOT status EQUAL 0 OR error MATCHES "Vulkan validation:")
    message(FATAL_ERROR "Vulkan replay failed (${status})\n${output}\n${error}")
endif()
file(READ "${STRATA_OUTPUT}/capture/result.json" result)
string(JSON backend GET "${result}" backend)
string(JSON fallback_count LENGTH "${result}" materialFallbacks)
string(JSON capture_count LENGTH "${result}" captures)
if(NOT backend STREQUAL "vulkan" OR NOT fallback_count EQUAL 0 OR capture_count LESS 1)
    message(FATAL_ERROR "Vulkan replay did not produce captures without shader fallbacks")
endif()
math(EXPR last "${capture_count} - 1")
foreach(index RANGE ${last})
    string(JSON name GET "${result}" captures ${index})
    file(SIZE "${STRATA_OUTPUT}/capture/${name}.png" bytes)
    if(bytes LESS 100)
        message(FATAL_ERROR "Vulkan replay produced an empty PNG: ${name}")
    endif()
endforeach()
