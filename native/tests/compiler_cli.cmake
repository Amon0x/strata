# The compiler's exit codes are what build scripts branch on: 1 for a usage error and 3 for an
# artifact that needs --emit-artifact, whether it is stale or was never written.
if(NOT STRATA_COMPILER OR NOT STRATA_SOURCE OR NOT STRATA_SCHEMAS OR NOT STRATA_RESOURCE_ROOT OR
   NOT STRATA_TEST_DIRECTORY)
    message(FATAL_ERROR "the compiler CLI test requires the compiler, a module and a work folder")
endif()

execute_process(
    COMMAND "${STRATA_COMPILER}"
    RESULT_VARIABLE status
    ERROR_VARIABLE error
)
if(NOT status EQUAL 1 OR NOT error MATCHES "usage:")
    message(FATAL_ERROR "running without a command did not print usage and exit 1 (${status})\n${error}")
endif()

file(REMOVE_RECURSE "${STRATA_TEST_DIRECTORY}")
file(MAKE_DIRECTORY "${STRATA_TEST_DIRECTORY}")
set(artifact "${STRATA_TEST_DIRECTORY}/module.bin")
execute_process(
    COMMAND "${STRATA_COMPILER}" --check-artifact
        "${STRATA_SOURCE}" "${STRATA_SCHEMAS}" "${STRATA_RESOURCE_ROOT}" "${artifact}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 3 OR NOT error MATCHES "artifact is missing")
    message(FATAL_ERROR "checking a missing artifact did not exit 3 (${status})\n${output}\n${error}")
endif()

execute_process(
    COMMAND "${STRATA_COMPILER}" --emit-artifact
        "${STRATA_SOURCE}" "${STRATA_SCHEMAS}" "${STRATA_RESOURCE_ROOT}" "${artifact}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "emitting the artifact failed (${status})\n${output}\n${error}")
endif()
execute_process(
    COMMAND "${STRATA_COMPILER}" --check-artifact
        "${STRATA_SOURCE}" "${STRATA_SCHEMAS}" "${STRATA_RESOURCE_ROOT}" "${artifact}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "a fresh artifact did not check clean (${status})\n${output}\n${error}")
endif()

file(APPEND "${artifact}" "x")
execute_process(
    COMMAND "${STRATA_COMPILER}" --check-artifact
        "${STRATA_SOURCE}" "${STRATA_SCHEMAS}" "${STRATA_RESOURCE_ROOT}" "${artifact}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 3 OR NOT error MATCHES "artifact is stale")
    message(FATAL_ERROR "a modified artifact did not check stale (${status})\n${output}\n${error}")
endif()
file(REMOVE_RECURSE "${STRATA_TEST_DIRECTORY}")

# Imports resolve within the resource root, as the hosts load them: an app may share components
# from a sibling folder once the root is given, and never reaches outside it.
set(root "${STRATA_TEST_DIRECTORY}/imports")
file(REMOVE_RECURSE "${STRATA_TEST_DIRECTORY}")
file(MAKE_DIRECTORY "${root}/app" "${root}/shared")
file(WRITE "${root}/shared/label.strata" "component Label() { Text(text: \"shared\") }\n")
file(WRITE "${root}/app/main.strata"
    "import \"../shared/label.strata\";\noverlay Main { root Label() }\n")
file(WRITE "${STRATA_TEST_DIRECTORY}/outside.strata" "component Outside() { Text(text: \"x\") }\n")
file(WRITE "${root}/app/escape.strata"
    "import \"../../outside.strata\";\noverlay Main { root Outside() }\n")

function(check_imports expected_status name)
    execute_process(
        COMMAND "${STRATA_COMPILER}" ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT status EQUAL expected_status)
        message(FATAL_ERROR "${name}: expected ${expected_status}, got ${status}\n${output}\n${error}")
    endif()
endfunction()

check_imports(0 "a sibling import inside the resource root"
    --resource-root "${root}" --check-module "${root}/app/main.strata")
check_imports(2 "a sibling import without a resource root"
    --check-module "${root}/app/main.strata")
check_imports(2 "an import that leaves the resource root"
    --resource-root "${root}" --check-module "${root}/app/escape.strata")
check_imports(4 "an entry module outside the resource root"
    --resource-root "${root}/shared" --check-module "${root}/app/main.strata")
file(REMOVE_RECURSE "${STRATA_TEST_DIRECTORY}")
