if(NOT DEFINED STRATA_COMPILER OR NOT DEFINED STRATA_REGISTRY OR
   NOT DEFINED STRATA_VALID_SOURCE OR NOT DEFINED STRATA_VALID_SCHEMAS OR
   NOT DEFINED STRATA_TEST_DIRECTORY)
    message(FATAL_ERROR "compiler diagnostics JSON test arguments are incomplete")
endif()

execute_process(
    COMMAND "${STRATA_COMPILER}" --check-module-json
        "${STRATA_VALID_SOURCE}" "${STRATA_REGISTRY}" "${STRATA_VALID_SCHEMAS}"
    RESULT_VARIABLE valid_status
    OUTPUT_VARIABLE valid_output
    ERROR_VARIABLE valid_error
)
if(NOT valid_status EQUAL 0)
    message(FATAL_ERROR "valid JSON diagnostics check failed\n${valid_output}\n${valid_error}")
endif()
string(JSON valid_format GET "${valid_output}" format)
string(JSON valid_succeeded GET "${valid_output}" succeeded)
string(JSON valid_count LENGTH "${valid_output}" diagnostics)
if(NOT valid_format STREQUAL "strata.diagnostics" OR NOT valid_succeeded OR
   NOT valid_count EQUAL 0)
    message(FATAL_ERROR "valid compiler diagnostics document has the wrong shape: ${valid_output}")
endif()

file(MAKE_DIRECTORY "${STRATA_TEST_DIRECTORY}")
set(invalid_source "${STRATA_TEST_DIRECTORY}/invalid-editor-module.strata")
file(WRITE "${invalid_source}" "overlay Broken { root MissingWidget(label: 1) }\n")
execute_process(
    COMMAND "${STRATA_COMPILER}" --check-module-json
        "${invalid_source}" "${STRATA_REGISTRY}" "${STRATA_VALID_SCHEMAS}"
    RESULT_VARIABLE invalid_status
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error
)
if(NOT invalid_status EQUAL 2)
    message(FATAL_ERROR
        "invalid JSON diagnostics check returned ${invalid_status}\n${invalid_output}\n${invalid_error}")
endif()
string(JSON invalid_format GET "${invalid_output}" format)
string(JSON invalid_succeeded GET "${invalid_output}" succeeded)
string(JSON invalid_count LENGTH "${invalid_output}" diagnostics)
if(NOT invalid_format STREQUAL "strata.diagnostics" OR invalid_succeeded OR
   invalid_count LESS 1)
    message(FATAL_ERROR "invalid compiler diagnostics document has the wrong shape: ${invalid_output}")
endif()
string(JSON diagnostic_code GET "${invalid_output}" diagnostics 0 code)
string(JSON diagnostic_line GET "${invalid_output}" diagnostics 0 range start line)
string(JSON diagnostic_column GET "${invalid_output}" diagnostics 0 range start column)
if(diagnostic_code STREQUAL "" OR diagnostic_line LESS 1 OR diagnostic_column LESS 1)
    message(FATAL_ERROR "compiler diagnostic lost its stable code or source range: ${invalid_output}")
endif()

execute_process(
    COMMAND "${STRATA_COMPILER}" --check-module-json
        "${STRATA_TEST_DIRECTORY}/missing.strata" "${STRATA_REGISTRY}"
    RESULT_VARIABLE failure_status
    OUTPUT_VARIABLE failure_output
    ERROR_VARIABLE failure_error
)
if(NOT failure_status EQUAL 2)
    message(FATAL_ERROR
        "tool failure JSON diagnostics returned ${failure_status}\n${failure_output}\n${failure_error}")
endif()
string(JSON failure_format GET "${failure_output}" format)
string(JSON failure_code GET "${failure_output}" diagnostics 0 code)
if(NOT failure_format STREQUAL "strata.diagnostics" OR
   NOT failure_code STREQUAL "STRATA.TOOL.VALIDATION_FAILED")
    message(FATAL_ERROR "tool failure did not preserve the diagnostics protocol: ${failure_output}")
endif()
