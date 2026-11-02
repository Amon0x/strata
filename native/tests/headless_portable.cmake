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
endforeach()

file(READ "${STRATA_OUTPUT}/after_click.json" frame)
string(JSON state_count LENGTH "${frame}" state)
set(clicked FALSE)
if(state_count GREATER 0)
    math(EXPR last_state "${state_count} - 1")
    foreach(index RANGE 0 ${last_state})
        string(JSON name GET "${frame}" state ${index} name)
        if(name STREQUAL "count")
            string(JSON value GET "${frame}" state ${index} value)
            if(value EQUAL 1)
                set(clicked TRUE)
            endif()
        endif()
    endforeach()
endif()
if(NOT clicked)
    message(FATAL_ERROR "portable pointer input did not mutate local application state")
endif()
file(READ "${STRATA_OUTPUT}/result.json" result)
string(JSON backend GET "${result}" backend)
if(NOT backend STREQUAL "reference")
    message(FATAL_ERROR "portable headless smoke did not use the reference backend")
endif()

# Keep the same clock and input, but wait for actual preparation before each reference capture.
file(READ "${STRATA_SCENARIO}" asynchronous)
string(JSON asynchronous SET "${asynchronous}" taskExecution "\"asynchronous\"")
string(JSON step_count LENGTH "${asynchronous}" steps)
math(EXPR last_step "${step_count} - 1")
set(steps "[]")
set(next_step 0)
set(captured_pending FALSE)
foreach(index RANGE 0 ${last_step})
    string(JSON step GET "${asynchronous}" steps ${index})
    string(JSON capture ERROR_VARIABLE not_capture GET "${step}" capture)
    if(NOT not_capture)
        if(NOT captured_pending)
            string(JSON steps SET "${steps}" ${next_step} "{\"capture\":\"pending\"}")
            math(EXPR next_step "${next_step} + 1")
            set(captured_pending TRUE)
        endif()
        string(JSON steps SET "${steps}" ${next_step} "{\"settle\":{}}")
        math(EXPR next_step "${next_step} + 1")
    endif()
    string(JSON steps SET "${steps}" ${next_step} "${step}")
    math(EXPR next_step "${next_step} + 1")
endforeach()
string(JSON asynchronous SET "${asynchronous}" steps "${steps}")
file(WRITE "${STRATA_OUTPUT}/asynchronous.json" "${asynchronous}")
execute_process(
    COMMAND "${STRATA_HEADLESS}"
        --resources "${STRATA_RESOURCES}"
        --scenario "${STRATA_OUTPUT}/asynchronous.json"
        --output "${STRATA_OUTPUT}/asynchronous"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "asynchronous preparation did not settle (${status})\n${output}\n${error}")
endif()
foreach(capture initial after_click)
    file(SHA256 "${STRATA_OUTPUT}/${capture}.png" expected)
    file(SHA256 "${STRATA_OUTPUT}/asynchronous/${capture}.png" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "settled asynchronous ${capture} differs from deterministic output")
    endif()
endforeach()
file(SHA256 "${STRATA_OUTPUT}/asynchronous/pending.png" pending)
file(SHA256 "${STRATA_OUTPUT}/asynchronous/initial.png" ready)
if(pending STREQUAL ready)
    message(FATAL_ERROR "asynchronous preparation did not expose its pending first frame")
endif()
file(READ "${STRATA_OUTPUT}/asynchronous/result.json" asynchronous_result)
string(JSON diagnostics LENGTH "${asynchronous_result}" diagnostics)
if(NOT diagnostics EQUAL 0)
    message(FATAL_ERROR "asynchronous preparation reported diagnostics")
endif()
