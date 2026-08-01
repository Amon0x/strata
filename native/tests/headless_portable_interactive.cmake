if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "portable interactive test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${STRATA_OUTPUT}")
file(MAKE_DIRECTORY "${STRATA_OUTPUT}")
set(commands "${STRATA_OUTPUT}/commands.jsonl")
file(WRITE "${commands}"
    "{\"id\":\"increment\",\"click\":{\"key\":\"example.increment\"}}\n"
    "{\"id\":\"settle\",\"advance\":{\"milliseconds\":32,\"frames\":2}}\n"
    "{\"id\":\"inspect\",\"inspect\":{}}\n"
    "{\"id\":\"close\",\"close\":{}}\n"
)
set(protocol "${STRATA_OUTPUT}/protocol.jsonl")
execute_process(
    COMMAND "${STRATA_HEADLESS}"
        --resources "${STRATA_RESOURCES}"
        --scenario "${STRATA_SCENARIO}"
        --output "${STRATA_OUTPUT}"
        --interactive
    INPUT_FILE "${commands}"
    OUTPUT_FILE "${protocol}"
    ERROR_VARIABLE error
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "portable interactive host failed (${result}): ${error}")
endif()
foreach(path IN ITEMS current.png current.json result.json protocol.jsonl)
    if(NOT EXISTS "${STRATA_OUTPUT}/${path}")
        message(FATAL_ERROR "portable interactive host did not write ${path}")
    endif()
endforeach()
file(READ "${protocol}" responses)
if(responses MATCHES "\"ok\":false" OR responses MATCHES "\"event\":\"error\"")
    message(FATAL_ERROR "portable interactive protocol returned an error: ${responses}")
endif()
string(REGEX MATCHALL "\"ok\":true" successful "${responses}")
list(LENGTH successful successful_count)
if(NOT successful_count EQUAL 5)
    message(FATAL_ERROR "portable interactive protocol response count is incomplete")
endif()
if(NOT responses MATCHES "\"event\":\"ready\"" OR
   NOT responses MATCHES "\"event\":\"closed\"" OR
   NOT responses MATCHES "\"id\":\"increment\"")
    message(FATAL_ERROR "portable interactive protocol lifecycle is incomplete")
endif()
file(READ "${STRATA_OUTPUT}/current.json" snapshot)
if(NOT snapshot MATCHES "Clicked 1 times")
    message(FATAL_ERROR "portable interactive click did not mutate the application")
endif()
