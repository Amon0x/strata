include_guard(GLOBAL)

function(strata_validate_module)
    set(options)
    set(one_value TARGET SOURCE SCHEMAS)
    set(multi_value EXTENSION_PATHS)
    cmake_parse_arguments(
        STRATA_VALIDATE
        "${options}"
        "${one_value}"
        "${multi_value}"
        ${ARGN}
    )
    if(STRATA_VALIDATE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "strata_validate_module received unknown arguments: "
            "${STRATA_VALIDATE_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT STRATA_VALIDATE_TARGET OR NOT TARGET "${STRATA_VALIDATE_TARGET}")
        message(FATAL_ERROR "strata_validate_module requires an existing TARGET")
    endif()
    if(NOT STRATA_VALIDATE_SOURCE)
        message(FATAL_ERROR "strata_validate_module requires SOURCE")
    endif()

    set(tool_dependency)
    if(Strata_COMPILER)
        set(compiler "${Strata_COMPILER}")
    elseif(TARGET strata_compile)
        set(compiler "$<TARGET_FILE:strata_compile>")
        set(tool_dependency strata_compile)
    else()
        message(FATAL_ERROR
            "strata_validate_module requires strata_compile; install the compiler tool "
            "or enable STRATA_BUILD_TOOLS for a source embedding")
    endif()

    cmake_path(ABSOLUTE_PATH STRATA_VALIDATE_SOURCE
        BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        NORMALIZE
        OUTPUT_VARIABLE source)
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Strata module source does not exist: ${source}")
    endif()
    cmake_path(GET source PARENT_PATH source_directory)
    # The compiler intentionally confines imports to the entry module's directory. Depending on
    # every possible module in that root keeps the validation stamp correct when an imported file
    # changes or a new import target is added, without maintaining a second CMake-side parser.
    file(GLOB_RECURSE module_sources
        CONFIGURE_DEPENDS
        "${source_directory}/*.strata"
    )

    set(command "${compiler}")
    set(dependencies ${module_sources} ${tool_dependency})
    foreach(extension_path IN LISTS STRATA_VALIDATE_EXTENSION_PATHS)
        cmake_path(ABSOLUTE_PATH extension_path
            BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            NORMALIZE
            OUTPUT_VARIABLE absolute_extension_path)
        list(APPEND command --extension-path "${absolute_extension_path}")
    endforeach()
    list(APPEND command --check-module "${source}")
    if(STRATA_VALIDATE_SCHEMAS)
        cmake_path(ABSOLUTE_PATH STRATA_VALIDATE_SCHEMAS
            BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            NORMALIZE
            OUTPUT_VARIABLE schemas)
        if(NOT EXISTS "${schemas}")
            message(FATAL_ERROR "Strata application schemas do not exist: ${schemas}")
        endif()
        list(APPEND command "${schemas}")
        list(APPEND dependencies "${schemas}")
    endif()

    string(SHA256 validation_id
        "${STRATA_VALIDATE_TARGET}|${source}|${STRATA_VALIDATE_SCHEMAS}|${STRATA_VALIDATE_EXTENSION_PATHS}")
    string(SUBSTRING "${validation_id}" 0 12 validation_id)
    set(validation_target
        "${STRATA_VALIDATE_TARGET}_strata_validate_${validation_id}")
    set(stamp
        "${CMAKE_CURRENT_BINARY_DIR}/strata-validation/${validation_id}.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/strata-validation"
        COMMAND ${command}
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${dependencies}
        COMMENT "Validating Strata module ${source}"
        VERBATIM
    )
    add_custom_target("${validation_target}" DEPENDS "${stamp}")
    add_dependencies("${STRATA_VALIDATE_TARGET}" "${validation_target}")
endfunction()
