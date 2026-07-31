if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "headless interactive test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${STRATA_OUTPUT}")
file(MAKE_DIRECTORY "${STRATA_OUTPUT}")
set(commands "${STRATA_OUTPUT}/commands.jsonl")
file(WRITE "${commands}"
    "{\"id\":\"data\",\"click\":{\"role\":\"tab\",\"name\":\"DATA\"}}\n"
    "{\"id\":\"folder\",\"click\":{\"key\":\"data.tree.folder.0\"}}\n"
    "{\"id\":\"add\",\"click\":{\"role\":\"button\",\"name\":\"Add edit\"}}\n"
    "{\"id\":\"settle\",\"advance\":{\"milliseconds\":300,\"frames\":1}}\n"
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
    message(FATAL_ERROR "interactive headless host failed (${result}): ${error}")
endif()
foreach(path IN ITEMS current.png current.json result.json protocol.jsonl)
    if(NOT EXISTS "${STRATA_OUTPUT}/${path}")
        message(FATAL_ERROR "interactive headless host did not write ${path}")
    endif()
endforeach()
file(READ "${protocol}" responses)
if(responses MATCHES "\"ok\":false" OR responses MATCHES "\"event\":\"error\"")
    message(FATAL_ERROR "interactive protocol returned an error: ${responses}")
endif()
string(REGEX MATCHALL "\"ok\":true" successful "${responses}")
list(LENGTH successful successful_count)
if(NOT successful_count EQUAL 7)
    message(FATAL_ERROR "interactive protocol did not return ready plus six successful responses")
endif()
if(NOT responses MATCHES "\"event\":\"ready\"" OR
   NOT responses MATCHES "\"event\":\"closed\"" OR
   NOT responses MATCHES "\"id\":\"data\"")
    message(FATAL_ERROR "interactive protocol lifecycle or request correlation is incomplete")
endif()
file(READ "${STRATA_OUTPUT}/current.json" snapshot)
if(NOT snapshot MATCHES "Undoable edits: alpha, beta, edit-2")
    message(FATAL_ERROR "interactive selector clicks did not preserve and mutate the live application")
endif()
if(NOT responses MATCHES "\"name\":\"DATA\"" OR
   NOT responses MATCHES "\"selected\":true")
    message(FATAL_ERROR "interactive semantic browser did not expose the selected DATA tab")
endif()
if(NOT responses MATCHES "\"key\":\"data.tree.folder.0\"[^\n]*\"selected\":true")
    message(FATAL_ERROR "interactive semantic browser did not expose or select the folder row")
endif()
