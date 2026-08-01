if(NOT DEFINED STRATA_BUILD_DIR OR NOT DEFINED STRATA_INSTALL_PREFIX OR
   NOT DEFINED STRATA_SMOKE_BUILD_DIR OR NOT DEFINED STRATA_GENERATOR OR
   NOT DEFINED STRATA_CTEST_COMMAND)
    message(FATAL_ERROR "installed-package test arguments are incomplete")
endif()
if(NOT DEFINED STRATA_CONFIG OR STRATA_CONFIG STREQUAL "")
    set(STRATA_CONFIG RelWithDebInfo)
endif()

file(REMOVE_RECURSE "${STRATA_INSTALL_PREFIX}" "${STRATA_SMOKE_BUILD_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${STRATA_BUILD_DIR}"
        --config "${STRATA_CONFIG}" --prefix "${STRATA_INSTALL_PREFIX}"
    RESULT_VARIABLE install_status
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_status EQUAL 0)
    message(FATAL_ERROR
        "SDK installation failed (${install_status})\n${install_output}\n${install_error}")
endif()
if(STRATA_EXPECT_VSCODE_EXTENSION)
    file(GLOB installed_vscode_packages
        "${STRATA_INSTALL_PREFIX}/share/strata/editor/strata-language-*.vsix"
    )
    list(LENGTH installed_vscode_packages installed_vscode_package_count)
    if(NOT installed_vscode_package_count EQUAL 1)
        message(FATAL_ERROR "SDK installation did not contain exactly one Strata VS Code extension")
    endif()
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${STRATA_INSTALL_PREFIX}/share/strata/samples"
    -B "${STRATA_SMOKE_BUILD_DIR}"
    -G "${STRATA_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${STRATA_INSTALL_PREFIX}"
    "-DSTRATA_REQUIRE_VSCODE_EXTENSION=${STRATA_EXPECT_VSCODE_EXTENSION}"
)
if(DEFINED STRATA_GENERATOR_PLATFORM AND NOT STRATA_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${STRATA_GENERATOR_PLATFORM}")
endif()
if(DEFINED STRATA_GENERATOR_TOOLSET AND NOT STRATA_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_command -T "${STRATA_GENERATOR_TOOLSET}")
endif()
if(DEFINED STRATA_BUILD_TYPE AND NOT STRATA_BUILD_TYPE STREQUAL "")
    list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${STRATA_BUILD_TYPE}")
endif()
execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_status
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_status EQUAL 0)
    message(FATAL_ERROR
        "installed consumer configuration failed (${configure_status})\n"
        "${configure_output}\n${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${STRATA_SMOKE_BUILD_DIR}"
        --config "${STRATA_CONFIG}" --parallel
    RESULT_VARIABLE build_status
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_status EQUAL 0)
    message(FATAL_ERROR
        "installed consumer build failed (${build_status})\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${STRATA_CTEST_COMMAND}" --test-dir "${STRATA_SMOKE_BUILD_DIR}"
        --build-config "${STRATA_CONFIG}" --output-on-failure
    RESULT_VARIABLE test_status
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error
)
if(NOT test_status EQUAL 0)
    message(FATAL_ERROR
        "installed consumer tests failed (${test_status})\n${test_output}\n${test_error}")
endif()
message(STATUS "Installed-package consumer acceptance passed")
