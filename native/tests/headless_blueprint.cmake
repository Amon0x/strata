if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "blueprint headless test requires executable, resources, scenario, and output")
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
    message(FATAL_ERROR "blueprint headless scenario failed (${status})\n${output}\n${error}")
endif()

# The node graph editor is .strata alone: each step is checked by what the editor shows.
function(expect capture text why)
    file(READ "${STRATA_OUTPUT}/${capture}.json" content)
    string(FIND "${content}" "${text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${why}: '${capture}' lacks '${text}'")
    endif()
endfunction()

expect(ran "Step 4" "the starter graph did not run its loop")
expect(ran "High score!" "the starter graph did not branch on its variable")
expect(dragged "id n1 at 133, 351" "dragging a node's header did not move it")
expect(added "id n13 at 318, 66" "the palette did not add a node where it was opened")
expect(marquee "2 nodes selected" "the marquee did not select the nodes it covered")
expect(deleted "11 nodes, 11 wires" "Delete did not remove the selection and its wires")
expect(final "Keep trying" "the rewired graph did not run as wired")
expect(final "Hello" "the wire drawn to the new node did not connect it")

file(READ "${STRATA_OUTPUT}/result.json" result)
if(result MATCHES "\"severity\": 2")
    message(FATAL_ERROR "the blueprint editor reported errors:\n${result}")
endif()
