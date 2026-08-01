cmake_minimum_required(VERSION 3.25)

project(StrataInstalledSmoke LANGUAGES C CXX)

include(CTest)
find_package(Strata CONFIG REQUIRED)

function(strata_installed_smoke target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE Strata::c)
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Strata::c>
            $<TARGET_FILE_DIR:${target}>
        VERBATIM
    )
    add_test(NAME ${target} COMMAND ${target} "${Strata_REGISTRY}")
endfunction()

strata_installed_smoke(strata_installed_c c_smoke.c)
set_target_properties(strata_installed_c PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)

strata_installed_smoke(strata_installed_cpp cpp_smoke.cpp)
target_link_libraries(strata_installed_cpp PRIVATE Strata::host Strata::render_host)
set_target_properties(strata_installed_cpp PROPERTIES CXX_EXTENSIONS OFF)

if(WIN32 AND TARGET Strata::desktop)
    add_executable(strata_installed_desktop desktop_app.cpp)
    target_link_libraries(strata_installed_desktop PRIVATE Strata::desktop)
    set_target_properties(strata_installed_desktop PROPERTIES CXX_EXTENSIONS OFF)
    add_custom_command(
        TARGET strata_installed_desktop
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Strata::c>
            $<TARGET_FILE_DIR:strata_installed_desktop>
        VERBATIM
    )
    add_test(
        NAME strata_installed_desktop
        COMMAND strata_installed_desktop --smoke "${Strata_RESOURCES}"
    )
    if(EXISTS "${Strata_DESKTOP_RUNNER}")
        add_test(
            NAME strata_installed_desktop_runner
            COMMAND "${Strata_DESKTOP_RUNNER}"
                --application "${CMAKE_CURRENT_LIST_DIR}/desktop_app.json"
                --resources "${Strata_RESOURCES}"
                --smoke
        )
    endif()
endif()
