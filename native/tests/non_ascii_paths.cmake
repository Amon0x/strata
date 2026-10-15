# Compiles a module from a folder whose name is outside every single-byte code page, the way a
# user with a non-ASCII Windows account name or project folder would.
if(NOT STRATA_COMPILER OR NOT STRATA_TEST_DIRECTORY)
    message(FATAL_ERROR "STRATA_COMPILER and STRATA_TEST_DIRECTORY are required")
endif()

set(folder "${STRATA_TEST_DIRECTORY}/şçğ-日本")
file(REMOVE_RECURSE "${STRATA_TEST_DIRECTORY}")
file(MAKE_DIRECTORY "${folder}")
file(WRITE "${folder}/main.strata" "overlay Main { root Text(text: \"ok\") }\n")

execute_process(
    COMMAND "${STRATA_COMPILER}" --check-module-json "${folder}/main.strata"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    # The compiler writes UTF-8; without this CMake decodes it with the console's code page.
    ENCODING UTF8
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "compiling from a non-ASCII folder failed (${status})\n${output}\n${error}")
endif()
string(FIND "${output}" "şçğ-日本/main.strata" entry)
if(entry EQUAL -1)
    message(FATAL_ERROR "the compiler did not report the non-ASCII entry path intact\n${output}")
endif()
file(REMOVE_RECURSE "${STRATA_TEST_DIRECTORY}")
