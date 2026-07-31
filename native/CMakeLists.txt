cmake_minimum_required(VERSION 3.25)

project(
    Strata
    VERSION 0.1.0
    DESCRIPTION "Portable Strata UI runtime"
    LANGUAGES C CXX
)

include(CMakePackageConfigHelpers)
include(CTest)
include(FetchContent)

option(STRATA_BUILD_TESTS "Build native unit and ABI tests" ON)
option(STRATA_BUILD_TOOLS "Build native command-line tools" ON)
option(STRATA_BUILD_SAMPLES "Build C and C++ embedding smoke applications" ON)
option(STRATA_BUILD_DESKTOP "Build the native Win32 fidelity host" ON)
option(STRATA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(STRATA_ENABLE_ASAN "Enable AddressSanitizer where supported" OFF)
option(STRATA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer where supported" OFF)
option(STRATA_ENABLE_STATIC_ANALYSIS "Enable compiler static analysis" OFF)
option(STRATA_ENABLE_TRACY "Enable local Tracy instrumentation" OFF)

if(STRATA_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
    set(TRACY_NO_BROADCAST ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    # Windows sampling needs the same elevated system-profile privilege as WPR. The built-in
    # Strata zones remain available without elevation and are the intended local capture path.
    set(TRACY_NO_SAMPLING ON CACHE BOOL "" FORCE)
    set(TRACY_NO_SYSTEM_TRACING ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG 05cceee0df3b8d7c6fa87e9638af311dbabc63cb
    )
    FetchContent_MakeAvailable(tracy)
endif()

# Strata keeps shaping/layout portable and owned by the core. FreeType is the replaceable,
# size-specific grayscale raster backend and is linked statically into the native runtime.
set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL TRUE CACHE BOOL "" FORCE)
FetchContent_Declare(
    freetype
    URL https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
    URL_HASH SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(freetype)
set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(strata_target_defaults target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_EXTENSIONS OFF
            C_EXTENSIONS OFF
    )

    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE
                /W4
                /utf-8
                # The core intentionally has many ownership-sized translation units; let one
                # MSBuild project use all host threads instead of relying only on project-level
                # `cmake --build --parallel` scheduling.
                $<$<COMPILE_LANGUAGE:C,CXX>:/MP16>
                $<$<COMPILE_LANGUAGE:C,CXX>:/FS>
                $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
                $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
                $<$<BOOL:${STRATA_WARNINGS_AS_ERRORS}>:/WX>
                $<$<BOOL:${STRATA_ENABLE_STATIC_ANALYSIS}>:/analyze>
        )
        if(STRATA_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            # With the Visual Studio generator MSVC's compiler embeds the ASAN linker
            # directives in each instrumented object. Passing the compiler flag through to
            # link.exe itself only produces LNK4044 and has no effect.
            get_target_property(strata_target_type ${target} TYPE)
            if(NOT strata_target_type STREQUAL "STATIC_LIBRARY")
                get_filename_component(strata_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
                set(strata_asan_runtime "${strata_msvc_bin_dir}/clang_rt.asan_dynamic-x86_64.dll")
                if(EXISTS "${strata_asan_runtime}")
                    add_custom_command(
                        TARGET ${target}
                        POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${strata_asan_runtime}"
                            "$<TARGET_FILE_DIR:${target}>"
                        VERBATIM
                    )
                else()
                    message(FATAL_ERROR "MSVC ASAN runtime was not found beside ${CMAKE_CXX_COMPILER}")
                endif()
            endif()
        endif()
        if(STRATA_ENABLE_UBSAN)
            message(FATAL_ERROR "MSVC does not provide the UBSan configuration required by Strata")
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion
                $<$<BOOL:${STRATA_WARNINGS_AS_ERRORS}>:-Werror>
        )
        if(STRATA_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
        if(STRATA_ENABLE_UBSAN)
            target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=undefined)
        endif()
    endif()
endfunction()

add_library(
    strata_internal_data
    STATIC
        src/core/utf8.cpp
        src/data/json.cpp
        src/data/json_view.cpp
)
target_include_directories(
    strata_internal_data
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
set_target_properties(strata_internal_data PROPERTIES POSITION_INDEPENDENT_CODE ON)
strata_target_defaults(strata_internal_data)

add_library(
    strata_host_support
    STATIC
        host/extensions.cpp
        host/module_path.cpp
        host/render_packet.cpp
)
target_include_directories(
    strata_host_support
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(strata_host_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
strata_target_defaults(strata_host_support)

add_library(
    strata_core
    STATIC
        src/core/allocator.cpp
        src/core/diagnostics.cpp
        src/core/runtime.cpp
        src/compiler/artifact.cpp
        src/compiler/ast.cpp
        src/compiler/compile.cpp
        src/compiler/lexer.cpp
        src/compiler/module.cpp
        src/compiler/parser.cpp
        src/compiler/portable_ir.cpp
        src/compiler/portable_ir_validation.cpp
        src/compiler/security.cpp
        src/compiler/schema.cpp
        src/compiler/semantic.cpp
        src/compiler/source.cpp
        src/compiler/source_map.cpp
        src/font/opentype.cpp
        src/font/atlas.cpp
        src/font/msdf.cpp
        src/font/raster.cpp
        src/font/registry.cpp
        src/font/truetype_outline.cpp
        src/resource/assets.cpp
        src/resource/image.cpp
        src/resource/resource.cpp
        src/runtime/action.cpp
        src/runtime/application.cpp
        src/runtime/async.cpp
        src/runtime/diagnostic.cpp
        src/runtime/durability.cpp
        src/runtime/diagnostic_json.cpp
        src/runtime/expression.cpp
        src/runtime/host.cpp
        src/runtime/layer.cpp
        src/runtime/profiler.cpp
        src/runtime/registry.cpp
        src/runtime/services.cpp
        src/runtime/startup.cpp
        src/runtime/state.cpp
        src/runtime/undo.cpp
        src/runtime/unit.cpp
        src/runtime/value.cpp
        src/runtime/value_schema.cpp
        src/ui/description.cpp
        src/ui/behavior/input.cpp
        src/ui/behavior/manipulation.cpp
        src/ui/behavior/collection_marquee.cpp
        src/ui/behavior/registry.cpp
        src/ui/behavior/presentation.cpp
        src/ui/command.cpp
        src/ui/quick_pick.cpp
        src/ui/collection/model.cpp
        src/ui/collection/table_model.cpp
        src/ui/collection/virtualization.cpp
        src/ui/inspection.cpp
        src/ui/frame_snapshot.cpp
        src/ui/input/router.cpp
        src/ui/input/action_encoding.cpp
        src/ui/input/action_dispatch.cpp
        src/ui/input/framework_actions.cpp
        src/ui/input/diagnostics.cpp
        src/ui/input/forms.cpp
        src/ui/input/focus_navigation.cpp
        src/ui/input/cancellation.cpp
        src/ui/input/drag.cpp
        src/ui/input/drag_session.cpp
        src/ui/input/hit_test.cpp
        src/ui/input/interaction.cpp
        src/ui/input/queue.cpp
        src/ui/input/scroll.cpp
        src/ui/input/editor.cpp
        src/ui/input/text_editing.cpp
        src/ui/layout/geometry.cpp
        src/ui/layout/style.cpp
        src/ui/layout/motion.cpp
        src/ui/layout/snap.cpp
        src/ui/layout/engine.cpp
        src/ui/layout/measure.cpp
        src/ui/layout/arrange.cpp
        src/ui/motion.cpp
        src/ui/motion/catalog.cpp
        src/ui/motion/config.cpp
        src/ui/motion/disclosure.cpp
        src/ui/motion/lifecycle.cpp
        src/ui/motion/model.cpp
        src/ui/motion/move.cpp
        src/ui/motion/players.cpp
        src/ui/notification.cpp
        src/ui/paint.cpp
        src/ui/path.cpp
        src/ui/presentation_geometry.cpp
        src/ui/render.cpp
        src/ui/render/paint_geometry.cpp
        src/ui/render/path_geometry.cpp
        src/ui/render/serialization.cpp
        src/ui/render/packet.cpp
        src/ui/render/packet_native.cpp
        src/ui/render/submission.cpp
        src/ui/render/submission_cache.cpp
        src/ui/render/submission_planner.cpp
        src/ui/render/submission_mesh.cpp
        src/ui/render/material_registry.cpp
        src/ui/scale.cpp
        src/ui/scheduler.cpp
        src/ui/scroll_geometry.cpp
        src/ui/semantics.cpp
        src/ui/status.cpp
        src/ui/surface.cpp
        src/ui/text.cpp
        src/ui/text_geometry.cpp
        src/ui/theme.cpp
        src/ui/tree.cpp
        src/ui/tree_reconcile.cpp
        src/ui/widget/registry.cpp
        src/ui/widget/description.cpp
        src/ui/widget/description_primitives.cpp
        src/ui/widget/description_controls.cpp
        src/ui/widget/description_shell.cpp
        src/ui/widget/description_collection_common.cpp
        src/ui/widget/description_tree.cpp
        src/ui/widget/description_table.cpp
        src/ui/widget/description_item_grid.cpp
        src/ui/widget/description_collections.cpp
        src/ui/widget/input.cpp
        src/ui/widget/menu_model.cpp
        src/ui/widget/subtarget.cpp
        src/ui/widget/editor_geometry.cpp
        src/ui/widget/input_collection_common.cpp
        src/ui/widget/input_tree.cpp
        src/ui/widget/input_table.cpp
        src/ui/widget/input_item_grid.cpp
        src/ui/widget/input_primitives.cpp
        src/ui/widget/input_controls.cpp
        src/ui/widget/input_shell.cpp
        src/ui/widget/input_collections.cpp
        src/ui/widget/inspection.cpp
        src/ui/widget/inspection_primitives.cpp
        src/ui/widget/inspection_shell.cpp
        src/ui/widget/inspection_collections.cpp
        src/ui/widget/semantics.cpp
        src/ui/widget/semantics_primitives.cpp
        src/ui/widget/semantics_controls.cpp
        src/ui/widget/semantics_shell.cpp
        src/ui/widget/semantics_collections.cpp
        src/ui/widget/shell_model.cpp
        src/ui/widget/presentation.cpp
        src/ui/widget/presentation_editor.cpp
        src/ui/widget/presentation_primitives.cpp
        src/ui/widget/presentation_controls.cpp
        src/ui/widget/presentation_shell.cpp
        src/ui/widget/presentation_collections.cpp
)
add_library(Strata::core ALIAS strata_core)
target_include_directories(
    strata_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(strata_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(strata_core PRIVATE freetype strata_internal_data)
if(STRATA_ENABLE_TRACY)
    target_link_libraries(strata_core PRIVATE Tracy::TracyClient)
    target_compile_definitions(strata_core PRIVATE STRATA_ENABLE_TRACY)
endif()
strata_target_defaults(strata_core)

add_library(strata_c SHARED
    src/abi.cpp
    src/abi_extension.cpp
    src/abi_application.cpp
    src/abi_services.cpp
    src/profiler_abi.cpp
    src/scale_abi.cpp
    src/state_abi.cpp
    src/surface_abi.cpp
    src/surface_focus_abi.cpp
    src/surface_inspector_abi.cpp
    src/surface_theme_abi.cpp
)
add_library(Strata::c ALIAS strata_c)
target_link_libraries(strata_c PRIVATE Strata::core)
target_include_directories(
    strata_c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(
    strata_c
    PRIVATE STRATA_C_BUILD
    PUBLIC STRATA_C_SHARED
)
set_target_properties(
    strata_c
    PROPERTIES
        EXPORT_NAME c
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
)
strata_target_defaults(strata_c)

add_library(
    strata_extensions
    STATIC
        src/extensions/authoring.cpp
        src/extensions/demo_package.cpp
        src/extensions/packages.cpp
)
add_library(Strata::extensions ALIAS strata_extensions)
target_link_libraries(strata_extensions PUBLIC Strata::c)
target_include_directories(
    strata_extensions
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
strata_target_defaults(strata_extensions)
# Extension selection is host plumbing, but its descriptors are projected by the package library.
target_link_libraries(strata_host_support PUBLIC strata_extensions)

if(WIN32 AND (STRATA_BUILD_DESKTOP OR STRATA_BUILD_TOOLS))
    add_library(
        strata_d3d11
        STATIC
            d3d11/blur_pass.cpp
            d3d11/render_context.cpp
            d3d11/texture_store.cpp
    )
    target_link_libraries(
        strata_d3d11
        PUBLIC
            d3d11
            d3dcompiler
            ole32
            windowscodecs
    )
    target_include_directories(
        strata_d3d11 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )
    strata_target_defaults(strata_d3d11)
endif()

if(STRATA_BUILD_TOOLS)
    add_executable(
        strata_compile
        tools/compiler_main.cpp
        tools/compiler_check.cpp
        tools/compiler_check.hpp
    )
    target_link_libraries(strata_compile PRIVATE Strata::core strata_extensions)
    target_include_directories(
        strata_compile
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    strata_target_defaults(strata_compile)

    add_executable(
        strata_authoring
        tools/authoring_main.cpp
        tools/authoring.cpp
        tools/authoring.hpp
        tools/authoring_grammar.cpp
        tools/authoring_grammar.hpp
        tools/authoring_registry.cpp
        tools/authoring_registry.hpp
        tools/diagnostic_catalog.cpp
        tools/diagnostic_catalog.hpp
    )
    target_link_libraries(strata_authoring PRIVATE Strata::core)
    target_include_directories(
        strata_authoring
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    strata_target_defaults(strata_authoring)

    set(strata_headless_platform_sources headless/unsupported_image_codec.cpp)
    if(WIN32)
        set(
            strata_headless_platform_sources
            headless/d3d11_renderer.cpp
            headless/wic_image_codec.cpp
        )
    endif()
    add_library(
        strata_headless_host
        STATIC
            headless/application_host.cpp
            headless/browser_model.cpp
            headless/capture_renderer.cpp
            headless/interactive.cpp
            headless/png.cpp
            headless/runner.cpp
            headless/scenario.cpp
            headless/session.cpp
            headless/software_renderer.cpp
            ${strata_headless_platform_sources}
    )
    target_link_libraries(
        strata_headless_host
        PUBLIC
            Strata::c
            strata_host_support
            strata_internal_data
    )
    target_include_directories(
        strata_headless_host
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/headless>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}
    )
    if(WIN32)
        target_link_libraries(strata_headless_host PRIVATE strata_d3d11)
    endif()
    strata_target_defaults(strata_headless_host)

    add_executable(strata_headless headless/main.cpp)
    target_link_libraries(strata_headless PRIVATE strata_headless_host)
    strata_target_defaults(strata_headless)
endif()

if(STRATA_BUILD_SAMPLES)
    add_executable(strata_c_smoke samples/c_smoke.c)
    target_link_libraries(strata_c_smoke PRIVATE Strata::c)
    strata_target_defaults(strata_c_smoke)

    add_executable(strata_cpp_smoke samples/cpp_smoke.cpp)
    target_link_libraries(strata_cpp_smoke PRIVATE Strata::c)
    strata_target_defaults(strata_cpp_smoke)
endif()

if(WIN32 AND STRATA_BUILD_DESKTOP)
    add_executable(
        strata_desktop
        desktop/host.cpp
        desktop/host_services.cpp
        desktop/main.cpp
        desktop/renderer.cpp
    )
    target_link_libraries(
        strata_desktop
        PRIVATE
            Strata::c
            strata_extensions
            strata_host_support
            strata_d3d11
            dxgi
            ole32
            user32
            windowscodecs
    )
    strata_target_defaults(strata_desktop)
endif()

if(BUILD_TESTING AND STRATA_BUILD_TESTS)
    add_executable(strata_core_tests tests/core_tests.cpp)
    target_link_libraries(strata_core_tests PRIVATE Strata::core)
    target_include_directories(strata_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_core_tests)
    add_test(NAME strata.core COMMAND strata_core_tests)

    add_executable(strata_scale_tests tests/scale_tests.cpp)
    target_link_libraries(strata_scale_tests PRIVATE Strata::c Strata::core)
    target_include_directories(strata_scale_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_scale_tests)
    add_test(NAME strata.scale COMMAND strata_scale_tests)

    add_executable(strata_data_tests tests/data_tests.cpp)
    target_link_libraries(strata_data_tests PRIVATE Strata::core)
    target_include_directories(strata_data_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_data_tests)
    add_test(NAME strata.data COMMAND strata_data_tests)

    add_executable(strata_compiler_tests tests/compiler_tests.cpp)
    target_link_libraries(strata_compiler_tests PRIVATE Strata::core)
    target_include_directories(strata_compiler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_compiler_tests)
    add_test(NAME strata.compiler COMMAND strata_compiler_tests)

    add_executable(strata_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(strata_runtime_tests PRIVATE Strata::core)
    target_include_directories(strata_runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_runtime_tests)
    add_test(
        NAME strata.runtime
        COMMAND strata_runtime_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic-session
    )

    add_executable(strata_profiler_tests tests/profiler_tests.cpp)
    target_link_libraries(strata_profiler_tests PRIVATE Strata::core)
    target_include_directories(strata_profiler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_profiler_tests)
    add_test(NAME strata.profiler COMMAND strata_profiler_tests)

    add_executable(strata_resource_host_residual_tests tests/resource_host_residual_tests.cpp)
    target_link_libraries(strata_resource_host_residual_tests PRIVATE Strata::core)
    target_include_directories(strata_resource_host_residual_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_resource_host_residual_tests)
    add_test(NAME strata.resource.host.residual COMMAND strata_resource_host_residual_tests)

    add_executable(strata_ui_tests tests/ui_tests.cpp)
    target_link_libraries(strata_ui_tests PRIVATE Strata::core)
    target_include_directories(strata_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_ui_tests)
    add_test(
        NAME strata.ui
        COMMAND strata_ui_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic-session
    )

    add_executable(strata_text_font_residual_tests tests/text_font_residual_tests.cpp)
    target_link_libraries(strata_text_font_residual_tests PRIVATE Strata::core)
    target_include_directories(
        strata_text_font_residual_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    strata_target_defaults(strata_text_font_residual_tests)
    add_test(
        NAME strata.ui.text_font_residuals
        COMMAND strata_text_font_residual_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_theme_tests tests/theme_tests.cpp)
    target_link_libraries(strata_theme_tests PRIVATE Strata::core)
    target_include_directories(strata_theme_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_theme_tests)
    add_test(
        NAME strata.ui.theme
        COMMAND strata_theme_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_input_editor_tests tests/input_editor_tests.cpp)
    target_link_libraries(strata_input_editor_tests PRIVATE Strata::core)
    target_include_directories(strata_input_editor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_input_editor_tests)
    add_test(NAME strata.input.editor COMMAND strata_input_editor_tests)

    add_executable(strata_chip_input_tests tests/chip_input_tests.cpp)
    target_link_libraries(strata_chip_input_tests PRIVATE Strata::core)
    target_include_directories(strata_chip_input_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_chip_input_tests)
    add_test(
        NAME strata.widget.chip_input
        COMMAND strata_chip_input_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_shell_lifecycle_tests tests/shell_lifecycle_tests.cpp)
    target_link_libraries(strata_shell_lifecycle_tests PRIVATE Strata::core)
    target_include_directories(strata_shell_lifecycle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_shell_lifecycle_tests)
    add_test(
        NAME strata.widget.shell_lifecycle
        COMMAND strata_shell_lifecycle_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_interaction_residual_tests tests/interaction_residual_tests.cpp)
    target_link_libraries(strata_interaction_residual_tests PRIVATE Strata::core)
    target_include_directories(strata_interaction_residual_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_interaction_residual_tests)
    add_test(
        NAME strata.ui.interaction_residuals
        COMMAND strata_interaction_residual_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_widget_binding_tests tests/widget_binding_tests.cpp)
    target_link_libraries(strata_widget_binding_tests PRIVATE Strata::core)
    target_include_directories(strata_widget_binding_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_widget_binding_tests)
    add_test(
        NAME strata.widget.binding
        COMMAND strata_widget_binding_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_form_validation_tests tests/form_validation_tests.cpp)
    target_link_libraries(strata_form_validation_tests PRIVATE Strata::core)
    target_include_directories(strata_form_validation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_form_validation_tests)
    add_test(
        NAME strata.form.validation
        COMMAND strata_form_validation_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_command_binding_tests tests/command_binding_tests.cpp)
    target_link_libraries(strata_command_binding_tests PRIVATE Strata::core)
    target_include_directories(strata_command_binding_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_command_binding_tests)
    add_test(
        NAME strata.command.binding
        COMMAND strata_command_binding_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_extension_tests tests/extension_tests.cpp)
    target_link_libraries(strata_extension_tests PRIVATE strata_extensions)
    strata_target_defaults(strata_extension_tests)
    add_test(
        NAME strata.extension
        COMMAND strata_extension_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    add_executable(strata_abi_tests tests/abi_tests.cpp)
    target_link_libraries(strata_abi_tests PRIVATE Strata::c)
    strata_target_defaults(strata_abi_tests)
    add_test(
        NAME strata.abi
        COMMAND strata_abi_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    )

    if(STRATA_BUILD_SAMPLES)
        add_test(
            NAME strata.embed.c
            COMMAND strata_c_smoke
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
        )
        add_test(
            NAME strata.embed.cpp
            COMMAND strata_cpp_smoke
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
        )
    endif()
    if(STRATA_BUILD_TOOLS)
        add_test(
            NAME strata.headless.smoke
            COMMAND ${CMAKE_COMMAND}
                -DSTRATA_HEADLESS=$<TARGET_FILE:strata_headless>
                -DSTRATA_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                -DSTRATA_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/showcase.json
                -DSTRATA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/headless-smoke
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_smoke.cmake
        )
        add_test(
            NAME strata.headless.interactive
            COMMAND ${CMAKE_COMMAND}
                -DSTRATA_HEADLESS=$<TARGET_FILE:strata_headless>
                -DSTRATA_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                -DSTRATA_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/showcase.json
                -DSTRATA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/headless-interactive
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_interactive.cmake
        )
    endif()
    if(WIN32 AND STRATA_BUILD_DESKTOP)
        add_test(
            NAME strata.desktop.smoke
            COMMAND strata_desktop --smoke ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
        )
    endif()
endif()

install(
    TARGETS strata_c strata_extensions
    EXPORT StrataTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
if(WIN32 AND STRATA_BUILD_DESKTOP)
    install(TARGETS strata_desktop RUNTIME DESTINATION bin)
endif()
if(STRATA_BUILD_TOOLS)
    install(
        TARGETS strata_compile strata_authoring strata_headless
        EXPORT StrataTargets
        RUNTIME DESTINATION bin
    )
endif()
install(DIRECTORY include/ DESTINATION include)
install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/strata/registry-v1.json
    DESTINATION share/strata
)
install(
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/META-INF/licenses/FreeType-FTL.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/META-INF/licenses/Lucide-ISC.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/META-INF/licenses/Roboto-OFL.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/assets/strata/fonts/LICENSE-JetBrainsMono.txt
    DESTINATION share/licenses/strata
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/assets/strata/
    DESTINATION share/assets/strata
)
install(
    FILES
        samples/installed/CMakeLists.txt
        samples/c_smoke.c
        samples/cpp_smoke.cpp
    DESTINATION share/strata/samples
)
install(
    EXPORT StrataTargets
    FILE StrataTargets.cmake
    NAMESPACE Strata::
    DESTINATION lib/cmake/Strata
)
configure_package_config_file(
    cmake/StrataConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/StrataConfig.cmake
    INSTALL_DESTINATION lib/cmake/Strata
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/StrataConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/StrataConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/StrataConfigVersion.cmake
    DESTINATION lib/cmake/Strata
)

find_program(STRATA_CLANG_FORMAT NAMES clang-format clang-format.exe)
if(STRATA_CLANG_FORMAT)
    file(
        GLOB_RECURSE STRATA_FORMAT_SOURCES
        CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/desktop/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/desktop/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/d3d11/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/d3d11/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/headless/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/headless/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/host/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/host/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/*.cpp
    )
    add_custom_target(
        strata_format_check
        COMMAND ${STRATA_CLANG_FORMAT} --dry-run --Werror ${STRATA_FORMAT_SOURCES}
        COMMENT "Checking native source formatting"
        VERBATIM
    )
endif()
