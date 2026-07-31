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
target_compile_features(strata_installed_cpp PRIVATE cxx_std_23)
set_target_properties(strata_installed_cpp PROPERTIES CXX_EXTENSIONS OFF)
