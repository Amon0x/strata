if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR
   NOT DEFINED STRATA_SCENARIO OR NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "color popup headless test requires executable, resources, scenario, and output")
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
    message(FATAL_ERROR "color popup headless scenario failed (${status})\n${output}\n${error}")
endif()

foreach(name closed open plane selected-dismissed reopened hue alpha closing dismissed idle outside-baseline outside-dismissed switched gradient)
    foreach(extension png json)
        set(artifact "${name}.${extension}")
        if(NOT EXISTS "${STRATA_OUTPUT}/${artifact}")
            message(FATAL_ERROR "color popup scenario did not create ${artifact}")
        endif()
        file(SIZE "${STRATA_OUTPUT}/${artifact}" artifact_size)
        if(artifact_size EQUAL 0)
            message(FATAL_ERROR "color popup scenario created empty ${artifact}")
        endif()
    endforeach()
endforeach()
if(NOT EXISTS "${STRATA_OUTPUT}/result.json")
    message(FATAL_ERROR "color popup scenario did not create result.json")
endif()

file(SHA256 "${STRATA_OUTPUT}/open.png" open_hash)
file(SHA256 "${STRATA_OUTPUT}/plane.png" plane_hash)
file(SHA256 "${STRATA_OUTPUT}/hue.png" hue_hash)
file(SHA256 "${STRATA_OUTPUT}/alpha.png" alpha_hash)
file(SHA256 "${STRATA_OUTPUT}/dismissed.png" dismissed_hash)
if(open_hash STREQUAL plane_hash OR plane_hash STREQUAL hue_hash OR
   hue_hash STREQUAL alpha_hash OR alpha_hash STREQUAL dismissed_hash)
    message(FATAL_ERROR "popup opening, picker drags, or dismissal did not produce distinct frames")
endif()
function(read_swatch_fill document output)
    string(REGEX MATCH
        "\"fill\"[ \t\r\n]*:[ \t\r\n]*\"[0-9A-Fa-f]+\"[ \t\r\n]*,[ \t\r\n]*\"hitTest\"[ \t\r\n]*:[ \t\r\n]*null[ \t\r\n]*,[ \t\r\n]*\"kind\"[ \t\r\n]*:[ \t\r\n]*\"rounded_rect\"[ \t\r\n]*,[ \t\r\n]*\"radii\"[ \t\r\n]*:[ \t\r\n]*\\{[ \t\r\n]*\"bottomLeft\"[ \t\r\n]*:[ \t\r\n]*7"
        swatch_command "${document}")
    string(REGEX MATCH "\"fill\"[ \t\r\n]*:[ \t\r\n]*\"([0-9A-Fa-f]+)\"" unused "${swatch_command}")
    set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

file(READ "${STRATA_OUTPUT}/open.json" open)
file(READ "${STRATA_OUTPUT}/plane.json" live_plane)
read_swatch_fill("${open}" open_swatch)
read_swatch_fill("${live_plane}" live_swatch)
if(open_swatch STREQUAL "" OR live_swatch STREQUAL "" OR open_swatch STREQUAL live_swatch)
    message(FATAL_ERROR
        "picker drag did not repaint the external swatch: '${open_swatch}' vs '${live_swatch}'")
endif()

function(read_accent document output)
    string(JSON state_count LENGTH "${document}" state)
    math(EXPR state_last "${state_count} - 1")
    foreach(index RANGE 0 ${state_last})
        string(JSON state_name GET "${document}" state ${index} name)
        if(state_name STREQUAL "accent")
            string(JSON state_value GET "${document}" state ${index} value)
            set(${output} "${state_value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "color popup capture omitted the Color page accent state")
endfunction()

file(READ "${STRATA_OUTPUT}/closed.json" closed)
file(READ "${STRATA_OUTPUT}/selected-dismissed.json" selected_dismissed)
read_accent("${closed}" initial_accent)
read_accent("${selected_dismissed}" selected_accent)
if(initial_accent STREQUAL selected_accent)
    message(FATAL_ERROR "picker commit did not update the closed Color page swatch state")
endif()

file(READ "${STRATA_OUTPUT}/plane.json" plane)
file(READ "${STRATA_OUTPUT}/reopened.json" reopened)
string(JSON plane_semantics GET "${plane}" semantics)
string(JSON reopened_semantics GET "${reopened}" semantics)
string(REGEX MATCH "#[0-9A-Fa-f]+, hue [0-9]+ degrees" selected_value "${plane_semantics}")
string(REGEX MATCH "#[0-9A-Fa-f]+, hue [0-9]+ degrees" reopened_value "${reopened_semantics}")
if(selected_value STREQUAL "" OR NOT selected_value STREQUAL reopened_value)
    message(FATAL_ERROR
        "reopened picker did not restore its committed selection: '${selected_value}' vs '${reopened_value}'")
endif()


file(READ "${STRATA_OUTPUT}/dismissed.json" dismissed)
string(JSON dismissed_semantics GET "${dismissed}" semantics)
string(FIND "${dismissed_semantics}" "Workspace accent color picker" dismissed_popup)
if(NOT dismissed_popup EQUAL -1)
    message(FATAL_ERROR "dismissed picker remained in the active semantic tree")
endif()
file(READ "${STRATA_OUTPUT}/outside-dismissed.json" outside_dismissed)
string(JSON outside_root GET "${outside_dismissed}" inspection root)
string(FIND "${outside_root}" "deck.color.popup" outside_popup)
if(NOT outside_popup EQUAL -1)
    message(FATAL_ERROR "outside dismissal retained the picker portal in the active tree")
endif()


file(READ "${STRATA_OUTPUT}/switched.json" switched)
string(JSON switched_semantics GET "${switched}" semantics)
string(FIND "${switched_semantics}" "Workspace accent" stale_color_page)
string(FIND "${switched_semantics}" "Gradient editor" gradient_editor)
if(NOT stale_color_page EQUAL -1 OR gradient_editor EQUAL -1)
    message(FATAL_ERROR "Color-to-Gradient switch retained both pages during popup exit")
endif()


foreach(name plane hue alpha)
    file(READ "${STRATA_OUTPUT}/${name}.json" frame)
    string(JSON layout_work GET "${frame}" operationCounters layoutWork)
    string(JSON rebuilds GET "${frame}" operationCounters rebuilds)
    string(JSON measured GET "${frame}" operationCounters measuredNodes)
    string(JSON arranged GET "${frame}" operationCounters arrangedNodes)
    string(JSON described GET "${frame}" operationCounters describedNodes)
    string(JSON evaluated GET "${frame}" operationCounters evaluatedExpressions)
    if(layout_work GREATER 1 OR rebuilds GREATER 1 OR measured GREATER 16 OR
       arranged GREATER 16 OR described GREATER 80 OR evaluated GREATER 500)
        message(FATAL_ERROR
            "${name} commit escaped one bounded declarative projection: layout=${layout_work}, rebuilds=${rebuilds}, measured=${measured}, arranged=${arranged}, described=${described}, evaluated=${evaluated}")
    endif()
    string(JSON fragments_built GET "${frame}" operationCounters render fragmentsBuilt)
    string(JSON fragments_reused GET "${frame}" operationCounters render fragmentsReused)
    string(JSON nodes_visited GET "${frame}" operationCounters render nodesVisited)
    if(fragments_built GREATER 2 OR fragments_reused LESS 32 OR nodes_visited GREATER 48)
        message(FATAL_ERROR
            "${name} release escaped bounded retained rendering: built=${fragments_built}, reused=${fragments_reused}, visited=${nodes_visited}")
    endif()
endforeach()

file(READ "${STRATA_OUTPUT}/idle.json" idle)
string(JSON idle_layout GET "${idle}" operationCounters layoutWork)
string(JSON idle_rebuilds GET "${idle}" operationCounters rebuilds)
string(JSON idle_nodes GET "${idle}" operationCounters render nodesVisited)
if(NOT idle_layout EQUAL 0 OR NOT idle_rebuilds EQUAL 0 OR NOT idle_nodes EQUAL 0)
    message(FATAL_ERROR "dismissed picker retained idle layout, rebuild, or render work")
endif()

file(READ "${STRATA_OUTPUT}/result.json" result)
string(JSON action_count LENGTH "${result}" actions)
if(NOT action_count EQUAL 3)
    message(FATAL_ERROR "three picker gestures emitted ${action_count} actions instead of one commit per release")
endif()
math(EXPR action_last "${action_count} - 1")
foreach(index RANGE 0 ${action_last})
    string(JSON action_id GET "${result}" actions ${index} id)
    string(JSON source_key GET "${result}" actions ${index} sourceKey)
    if(NOT action_id STREQUAL "control-deck.color.commit" OR
       NOT source_key STREQUAL "deck.color.picker")
        message(FATAL_ERROR "picker emitted an unexpected action '${action_id}' from '${source_key}'")
    endif()
endforeach()
string(JSON diagnostic_count LENGTH "${result}" diagnostics)
string(JSON fallback_count LENGTH "${result}" materialFallbacks)
if(NOT diagnostic_count EQUAL 0 OR NOT fallback_count EQUAL 0)
    message(FATAL_ERROR "color popup scenario reported diagnostics or a D3D11 material fallback")
endif()
