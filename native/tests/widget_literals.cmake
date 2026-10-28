# Generic UI code holds no built-in widget names: widgets reach the router, hit testing, focus,
# layout, motion, rendering and the surface only through their WidgetLifecycle. A name check here
# usually means a lifecycle hook is missing.
#
# Inputs: -DSTRATA_NATIVE_ROOT=<repository>/native
#
# Widget names come from the compiler's built-in catalog, so a new widget is covered as soon as it
# is declared. Widget-owned modules (each widget's description, input, inspection, semantics,
# presentation and subtarget projections, their models, and the registration table) may name the
# widgets they register.

if(NOT DEFINED STRATA_NATIVE_ROOT)
    message(FATAL_ERROR "STRATA_NATIVE_ROOT is required")
endif()

file(GLOB catalog_sources "${STRATA_NATIVE_ROOT}/src/compiler/builtin_catalog_widgets_*.cpp")
set(widget_names "")
foreach(catalog IN LISTS catalog_sources)
    file(STRINGS "${catalog}" declarations REGEX "\\.name = \"[A-Z][A-Za-z]+\"")
    foreach(declaration IN LISTS declarations)
        string(REGEX MATCH "\\.name = \"([A-Z][A-Za-z]+)\"" _ "${declaration}")
        list(APPEND widget_names "${CMAKE_MATCH_1}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES widget_names)
list(LENGTH widget_names widget_count)
if(widget_count LESS 20)
    message(FATAL_ERROR "found only ${widget_count} built-in widget names in the catalog")
endif()
list(JOIN widget_names "|" alternatives)
set(literal_pattern "\"(${alternatives})\"")

set(ui "${STRATA_NATIVE_ROOT}/src/ui")
file(GLOB_RECURSE generic_sources
    "${ui}/*.cpp" "${ui}/*.hpp" "${ui}/*.inc"
)
# Widget-owned modules.
list(FILTER generic_sources EXCLUDE REGEX "/ui/widget/(description|input|inspection|semantics|presentation)_[a-z_]+\\.cpp$")
list(FILTER generic_sources EXCLUDE REGEX "/ui/widget/[a-z_]+_model\\.(cpp|hpp)$")
list(FILTER generic_sources EXCLUDE REGEX "/ui/widget/subtarget_projections\\.cpp$")
list(FILTER generic_sources EXCLUDE REGEX "/ui/widget/registry\\.cpp$")
# The description builder seam: slot and repeater expansion are being replaced by the
# ComponentInstance scheduler, which owns these files.
list(FILTER generic_sources EXCLUDE REGEX "/ui/description\\.cpp$")
list(FILTER generic_sources EXCLUDE REGEX "/ui/tree\\.cpp$")
# Theme defaults still classify widgets by name until widgets declare their style schema.
list(FILTER generic_sources EXCLUDE REGEX "/ui/theme\\.cpp$")

# Single lines that a concurrent change removes; each names its owner.
set(exempt_lines
    # The native Slider $value exemption in Surface's retained invalidation, deleted with the
    # push-based ComponentInstance scheduler.
    "surface\\.cpp:.*\"Slider\" && name == \"\\$value\""
)

set(violations "")
foreach(source IN LISTS generic_sources)
    file(READ "${source}" content)
    if(NOT content MATCHES "${literal_pattern}")
        continue()
    endif()
    file(RELATIVE_PATH relative "${STRATA_NATIVE_ROOT}" "${source}")
    # One list element per line: neutralise the characters CMake lists treat specially.
    string(REPLACE ";" "," content "${content}")
    string(REPLACE "[" "(" content "${content}")
    string(REPLACE "]" ")" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    foreach(line IN LISTS lines)
        if(NOT line MATCHES "${literal_pattern}")
            continue()
        endif()
        set(exempt FALSE)
        foreach(pattern IN LISTS exempt_lines)
            if("${relative}:${line}" MATCHES "${pattern}")
                set(exempt TRUE)
            endif()
        endforeach()
        if(NOT exempt)
            string(STRIP "${line}" stripped)
            list(APPEND violations "${relative}: ${stripped}")
        endif()
    endforeach()
endforeach()

if(violations)
    list(LENGTH violations count)
    list(JOIN violations "\n  " report)
    message(FATAL_ERROR
        "${count} built-in widget name(s) in generic UI code; move the behavior onto a "
        "WidgetLifecycle hook:\n  ${report}")
endif()
message(STATUS "no built-in widget names in generic UI code (${widget_count} names checked)")
