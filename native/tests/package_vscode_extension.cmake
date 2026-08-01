if(NOT DEFINED STRATA_EDITOR_SOURCE OR NOT DEFINED STRATA_EDITOR_PACKAGE)
    message(FATAL_ERROR "VS Code package arguments are incomplete")
endif()

file(READ "${STRATA_EDITOR_SOURCE}/package.json" package_manifest)
string(JSON package_version GET "${package_manifest}" version)
file(READ "${STRATA_EDITOR_SOURCE}/extension.vsixmanifest" vsix_manifest)
if(NOT vsix_manifest MATCHES "Identity[^>]*Version=\"${package_version}\"")
    message(FATAL_ERROR "VS Code package.json and VSIX manifest versions differ")
endif()

get_filename_component(package_directory "${STRATA_EDITOR_PACKAGE}" DIRECTORY)
set(staging "${package_directory}/vscode-extension-staging")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}/extension/syntaxes")

foreach(file IN ITEMS package.json language-configuration.json extension.js README.md strata-completions.json)
    file(COPY_FILE
        "${STRATA_EDITOR_SOURCE}/${file}"
        "${staging}/extension/${file}"
        ONLY_IF_DIFFERENT
    )
endforeach()
file(COPY_FILE
    "${STRATA_EDITOR_SOURCE}/syntaxes/strata.tmLanguage.json"
    "${staging}/extension/syntaxes/strata.tmLanguage.json"
    ONLY_IF_DIFFERENT
)
file(COPY_FILE
    "${STRATA_EDITOR_SOURCE}/extension.vsixmanifest"
    "${staging}/extension.vsixmanifest"
    ONLY_IF_DIFFERENT
)
file(COPY_FILE
    "${STRATA_EDITOR_SOURCE}/[Content_Types].xml"
    "${staging}/[Content_Types].xml"
    ONLY_IF_DIFFERENT
)
file(REMOVE "${STRATA_EDITOR_PACKAGE}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${STRATA_EDITOR_PACKAGE}" --format=zip
        extension.vsixmanifest [Content_Types].xml extension
    WORKING_DIRECTORY "${staging}"
    RESULT_VARIABLE package_status
    OUTPUT_VARIABLE package_output
    ERROR_VARIABLE package_error
)
if(NOT package_status EQUAL 0)
    message(FATAL_ERROR
        "VS Code extension packaging failed (${package_status})\n${package_output}\n${package_error}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${STRATA_EDITOR_PACKAGE}"
    RESULT_VARIABLE list_status
    OUTPUT_VARIABLE package_contents
    ERROR_VARIABLE list_error
)
if(NOT list_status EQUAL 0 OR
   NOT package_contents MATCHES "extension/extension.js" OR
   NOT package_contents MATCHES "extension/strata-completions.json" OR
   NOT package_contents MATCHES "extension/syntaxes/strata.tmLanguage.json")
    message(FATAL_ERROR
        "VS Code extension package is incomplete\n${package_contents}\n${list_error}")
endif()
file(REMOVE_RECURSE "${staging}")
message(STATUS "Wrote ${STRATA_EDITOR_PACKAGE}")
