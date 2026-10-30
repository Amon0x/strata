if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "scripting headless test requires executable, resources, scenario, and output")
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
    message(FATAL_ERROR "scripting headless scenario failed (${status})\n${output}\n${error}")
endif()

foreach(name scripted recovered moved undone picked)
    if(NOT EXISTS "${STRATA_OUTPUT}/${name}.json")
        message(FATAL_ERROR "scripting scenario did not capture ${name}")
    endif()
endforeach()

# Functions, closures, loops, try/catch, types, template strings and the library, all run by
# handlers: the text each wrote is what the interface shows.
file(READ "${STRATA_OUTPUT}/scripted.json" scripted)
foreach(expected
        "count=2 twice=4 items=2 node=b/3"
        "caught boom; odd [10, 32]; square 49; fib 55; counter 5; Hello, Ada! Hi, Bo!"
        "[exec Run, data Value (number)]; sorted [9, 5, 3, 1]; sum 18"
        "box at 420,200 stamped false")
    string(FIND "${scripted}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "scripted capture lacks '${expected}'")
    endif()
endforeach()

# A handler that failed wrote nothing; one that awaited an answer finished with it.
file(READ "${STRATA_OUTPUT}/recovered.json" recovered)
foreach(expected "count=2 twice=4" "box at 420,200 stamped true")
    string(FIND "${recovered}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "recovered capture lacks '${expected}'")
    endif()
endforeach()
file(READ "${STRATA_OUTPUT}/result.json" result)
if(NOT result MATCHES "STRATA.DSL.RUNTIME_MISSING_PROPERTY")
    message(FATAL_ERROR "the failing handler's error was not reported")
endif()

# A controlled drag rests where its handler put it, and undoes as one step.
file(READ "${STRATA_OUTPUT}/moved.json" moved)
string(FIND "${moved}" "box at 570,280" found)
if(found EQUAL -1)
    message(FATAL_ERROR "the dragged handle did not rest where its handler put it")
endif()
file(READ "${STRATA_OUTPUT}/undone.json" undone)
string(FIND "${undone}" "box at 420,200" found)
if(found EQUAL -1)
    message(FATAL_ERROR "undo did not return the handle in one step")
endif()

# A handler handed to a component as an action runs when the component calls it.
file(READ "${STRATA_OUTPUT}/picked.json" picked)
string(FIND "${picked}" "picked first" found)
if(found EQUAL -1)
    message(FATAL_ERROR "a handler passed as a component parameter did not run when called")
endif()
