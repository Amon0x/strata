cmake_minimum_required(VERSION 3.25)

project(
    Strata
    VERSION 0.1.0
    DESCRIPTION "Portable Strata UI runtime"
    LANGUAGES C CXX
)

if(MSVC AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    list(
        APPEND CMAKE_VS_GLOBALS
        "UseMultiToolTask=true"
        "EnforceProcessCountAcrossBuilds=true"
    )
endif()

include(CMakePackageConfigHelpers)
include(CTest)
include(FetchContent)
include(cmake/StrataModule.cmake)

option(STRATA_BUILD_TESTS "Build native unit and ABI tests" ON)
option(STRATA_BUILD_TOOLS "Build native command-line tools" ON)
option(STRATA_BUILD_AUTHORING "Build only the schema authoring tool for source embeddings" OFF)
option(STRATA_BUILD_SAMPLES "Build C and C++ embedding smoke applications" ON)
option(STRATA_BUILD_DESKTOP "Build the native Win32 fidelity host" ON)
option(STRATA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(STRATA_ENABLE_ASAN "Enable AddressSanitizer where supported" OFF)
option(STRATA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer where supported" OFF)
option(STRATA_ENABLE_STATIC_ANALYSIS "Enable compiler static analysis" OFF)
option(STRATA_ENABLE_PCH "Precompile stable standard-library headers for large targets" ON)
option(STRATA_ENABLE_TRACY "Enable local Tracy instrumentation" OFF)
option(
    STRATA_ENABLE_THREAD_SAFE_STATICS
    "Use compiler-managed thread-safe initialization for function-local statics"
    ON
)

set(STRATA_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(STRATA_RESOURCE_ROOT "${STRATA_REPOSITORY_ROOT}/src/main/resources")

if(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR
        "Strata requires MSVC 19.40 or newer; use the repository's windows-x64 workflow."
    )
endif()

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
    URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
    URL_HASH SHA256=bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103
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
                # Let cl parallelize translation units. Visual Studio's Multi-Tool Task scheduler
                # above coordinates compiler processes across concurrent MSBuild projects.
                $<$<COMPILE_LANGUAGE:C,CXX>:/MP>
                $<$<COMPILE_LANGUAGE:C,CXX>:/FS>
                $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
                $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
                $<$<BOOL:${STRATA_WARNINGS_AS_ERRORS}>:/WX>
                $<$<BOOL:${STRATA_ENABLE_STATIC_ANALYSIS}>:/analyze>
        )
        if(NOT STRATA_ENABLE_THREAD_SAFE_STATICS)
            target_compile_options(${target} PRIVATE /Zc:threadSafeInit-)
        endif()
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

# Standalone, dependency-free static SVG parser and deterministic reference rasterizer. The UI
# runtime consumes the same parsed display list directly, while external tools can use Strata::svg
# without linking the runtime or a render backend.
add_library(
    strata_svg
    STATIC
        src/svg/svg.cpp
        src/svg/rasterizer.cpp
)
add_library(Strata::svg ALIAS strata_svg)
target_include_directories(
    strata_svg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(
    strata_svg
    PROPERTIES
        EXPORT_NAME svg
        POSITION_INDEPENDENT_CODE ON
)
strata_target_defaults(strata_svg)
target_compile_features(strata_svg PUBLIC cxx_std_23)

add_library(
    strata_render_host
    STATIC
        host/render_packet.cpp
        host/surface_stream.cpp
)
add_library(Strata::render_host ALIAS strata_render_host)
target_include_directories(
    strata_render_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(
    strata_render_host
    PROPERTIES
        EXPORT_NAME render_host
        POSITION_INDEPENDENT_CODE ON
)
strata_target_defaults(strata_render_host)

add_library(
    strata_host_support
    STATIC
        host/browser_model.cpp
        host/extensions.cpp
        host/module_path.cpp
)
target_include_directories(
    strata_host_support
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(
    strata_host_support
    PUBLIC strata_internal_data Strata::render_host
    PRIVATE ${CMAKE_DL_LIBS}
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
        src/compiler/builtin_catalog.cpp
        src/compiler/builtin_catalog_actions.cpp
        src/compiler/builtin_catalog_helpers.cpp
        src/compiler/builtin_catalog_properties.cpp
        src/compiler/builtin_catalog_types.cpp
        src/compiler/builtin_catalog_widgets_collections.cpp
        src/compiler/builtin_catalog_widgets_controls.cpp
        src/compiler/builtin_catalog_widgets_primitives.cpp
        src/compiler/builtin_catalog_widgets_shell.cpp
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
        src/resource/svg_image.cpp
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
        src/ui/svg_image.cpp
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
target_link_libraries(strata_core PRIVATE freetype strata_internal_data Strata::svg)
if(STRATA_ENABLE_TRACY)
    target_link_libraries(strata_core PRIVATE Tracy::TracyClient)
    target_compile_definitions(strata_core PRIVATE STRATA_ENABLE_TRACY)
endif()
strata_target_defaults(strata_core)
if(STRATA_ENABLE_PCH)
    target_precompile_headers(
        strata_core
        PRIVATE
            <algorithm>
            <array>
            <cmath>
            <cstddef>
            <cstdint>
            <limits>
            <memory>
            <optional>
            <ranges>
            <stdexcept>
            <string>
            <string_view>
            <utility>
            <vector>
    )
endif()

set(
    strata_c_sources
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
add_library(strata_c SHARED ${strata_c_sources})
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

# Pleasant typed C++ host bindings over the stable C ABI. This is static by design: STL model
# types never become part of strata_c's compiler-independent DLL boundary.
set(
    strata_host_cpp_sources
    src/host.cpp
    src/core/utf8.cpp
    src/data/json.cpp
    src/data/json_view.cpp
)
add_library(strata_host_cpp STATIC ${strata_host_cpp_sources})
add_library(Strata::host ALIAS strata_host_cpp)
target_link_libraries(strata_host_cpp PUBLIC Strata::c)
target_include_directories(
    strata_host_cpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(
    strata_host_cpp
    PROPERTIES
        EXPORT_NAME host
        POSITION_INDEPENDENT_CODE ON
)
target_compile_features(strata_host_cpp PUBLIC cxx_std_23)
strata_target_defaults(strata_host_cpp)

# Source-tree consumers that must produce one self-contained module can link the same stable C ABI
# and C++ facade statically. These targets intentionally remain build-tree-only because the
# installed shared ABI is the portable SDK boundary and a relocatable static package would also
# need to package Strata's private FreeType object graph.
add_library(strata_c_static STATIC ${strata_c_sources})
add_library(Strata::c_static ALIAS strata_c_static)
target_link_libraries(strata_c_static PRIVATE Strata::core)
target_include_directories(
    strata_c_static
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(strata_c_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
strata_target_defaults(strata_c_static)

add_library(strata_host_cpp_static STATIC ${strata_host_cpp_sources})
add_library(Strata::host_static ALIAS strata_host_cpp_static)
target_link_libraries(strata_host_cpp_static PUBLIC Strata::c_static)
target_include_directories(
    strata_host_cpp_static
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(strata_host_cpp_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(strata_host_cpp_static PUBLIC cxx_std_23)
strata_target_defaults(strata_host_cpp_static)

# Generated application contracts are repository-internal consumers of the public host API. The
# installed generator emits equivalent headers for external application schemas.
add_library(strata_application_contracts INTERFACE)
target_link_libraries(strata_application_contracts INTERFACE Strata::host)
target_include_directories(
    strata_application_contracts
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated>
)

# Static C++ authoring support is linked into each external package. Runtime hosts consume only the
# stable C descriptor exported by that package's shared library.
add_library(strata_extensions STATIC src/extensions/authoring.cpp)
add_library(Strata::extensions ALIAS strata_extensions)
target_link_libraries(strata_extensions PUBLIC Strata::c)
target_compile_features(strata_extensions PUBLIC cxx_std_23)
target_include_directories(
    strata_extensions
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(
    strata_extensions
    PROPERTIES
        EXPORT_NAME extensions
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
)
strata_target_defaults(strata_extensions)

strata_add_extension_package(
    TARGET strata_demo_extension
    PACKAGE strata.demo.v1
    SOURCES src/extensions/demo_package.cpp
    INSTALL
)
target_include_directories(strata_demo_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
strata_target_defaults(strata_demo_extension)
strata_add_extension_package(
    TARGET strata_control_deck_extension
    PACKAGE strata.control-deck.v1
    SOURCES src/extensions/control_deck_package.cpp
    INSTALL
)
strata_target_defaults(strata_control_deck_extension)


if(WIN32)
    add_library(
        strata_d3d11
        STATIC
            d3d11/blur_pass.cpp
            d3d11/effect_pass.cpp
            d3d11/presenter.cpp
            d3d11/presenter_abi.cpp
            d3d11/render_context.cpp
            d3d11/renderer.cpp
            d3d11/texture_store.cpp
    )
    add_library(Strata::d3d11 ALIAS strata_d3d11)
    target_link_libraries(
        strata_d3d11
        PRIVATE
            Strata::render_host
        PUBLIC
            d3d11
            d3dcompiler
            ole32
            windowscodecs
    )
    target_include_directories(
        strata_d3d11
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(
        strata_d3d11
        PROPERTIES
            EXPORT_NAME d3d11
            POSITION_INDEPENDENT_CODE ON
    )
    target_compile_features(strata_d3d11 PUBLIC cxx_std_23)
    strata_target_defaults(strata_d3d11)

    add_library(strata_win32 STATIC win32/input.cpp win32/input_abi.cpp)
    add_library(Strata::win32 ALIAS strata_win32)
    target_link_libraries(strata_win32 PUBLIC imm32 user32)
    target_include_directories(
        strata_win32
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    set_target_properties(
        strata_win32
        PROPERTIES
            EXPORT_NAME win32
            POSITION_INDEPENDENT_CODE ON
    )
    target_compile_features(strata_win32 PUBLIC cxx_std_23)
    strata_target_defaults(strata_win32)
endif()

if(WIN32 AND STRATA_BUILD_DESKTOP)
    # Complete consumer-facing Win32 host. Its backend implementation is deliberately contained in
    # this archive so the installed target does not depend on repository-private host libraries.
    add_library(
        strata_desktop_host
        STATIC
            desktop/application.cpp
            desktop/host_services.cpp
            desktop/ime.cpp
            desktop/renderer.cpp
            host/extensions.cpp
            host/module_path.cpp
    )
    add_library(Strata::desktop ALIAS strata_desktop_host)
    target_link_libraries(
        strata_desktop_host
        PUBLIC Strata::host
        PRIVATE
            Strata::d3d11
            Strata::render_host
            d3d11
            d3dcompiler
            imm32
            ole32
            user32
            windowscodecs
    )
    target_include_directories(
        strata_desktop_host
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set_target_properties(
        strata_desktop_host
        PROPERTIES
            EXPORT_NAME desktop
            POSITION_INDEPENDENT_CODE ON
    )
    strata_target_defaults(strata_desktop_host)
endif()

if(STRATA_BUILD_TOOLS OR STRATA_BUILD_AUTHORING)
    add_executable(
        strata_authoring
        tools/authoring_main.cpp
        tools/authoring.cpp
        tools/authoring.hpp
        tools/authoring_contract.cpp
        tools/authoring_contract.hpp
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
endif()

if(STRATA_BUILD_TOOLS)
    add_executable(strata_svg_render tools/svg_render.cpp)
    target_link_libraries(strata_svg_render PRIVATE Strata::svg)
    strata_target_defaults(strata_svg_render)

    add_executable(
        strata_compile
        tools/compiler_main.cpp
        tools/compiler_check.cpp
        tools/compiler_check.hpp
    )
    target_link_libraries(strata_compile PRIVATE Strata::core strata_host_support)
    add_dependencies(
        strata_compile
        strata_demo_extension
        strata_control_deck_extension
    )
    target_include_directories(
        strata_compile
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    strata_target_defaults(strata_compile)

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
    target_link_libraries(
        strata_cpp_smoke PRIVATE Strata::host Strata::render_host Strata::svg
    )
    strata_target_defaults(strata_cpp_smoke)

    add_executable(strata_cpp_static_smoke samples/cpp_smoke.cpp)
    target_link_libraries(
        strata_cpp_static_smoke
        PRIVATE Strata::host_static Strata::render_host Strata::svg
    )
    strata_target_defaults(strata_cpp_static_smoke)

    if(WIN32 AND STRATA_BUILD_DESKTOP)
        add_executable(strata_desktop_sample samples/desktop_app.cpp)
        target_link_libraries(strata_desktop_sample PRIVATE Strata::desktop)
        strata_target_defaults(strata_desktop_sample)
    endif()
    if(WIN32)
        add_executable(strata_adapter_c_smoke samples/adapter_c_smoke.c)
        target_link_libraries(
            strata_adapter_c_smoke
            PRIVATE Strata::c Strata::d3d11 Strata::win32
        )
        set_target_properties(strata_adapter_c_smoke PROPERTIES LINKER_LANGUAGE CXX)
        strata_target_defaults(strata_adapter_c_smoke)

        add_executable(strata_d3d11_target_smoke samples/d3d11_target_smoke.cpp)
        target_link_libraries(
            strata_d3d11_target_smoke
            PRIVATE Strata::d3d11 Strata::host Strata::win32
        )
        strata_target_defaults(strata_d3d11_target_smoke)
    endif()
endif()

if(WIN32 AND STRATA_BUILD_DESKTOP)
    add_executable(
        strata_desktop
        desktop/host.cpp
        desktop/main.cpp
        desktop/performance.cpp
        desktop/showcase.cpp
        headless/scenario.cpp
    )
    target_link_libraries(
        strata_desktop
        PRIVATE
            Strata::host
            Strata::desktop
            strata_application_contracts
            strata_host_support
            strata_d3d11
            dxgi
            ole32
            user32
            windowscodecs
    )
    strata_target_defaults(strata_desktop)
endif()

if(STRATA_BUILD_TOOLS)
    set(STRATA_REGISTRY "${CMAKE_BINARY_DIR}/generated/strata/registry-v1.json")
    add_custom_command(
        OUTPUT "${STRATA_REGISTRY}"
        COMMAND strata_authoring --write-registry "${STRATA_REGISTRY}"
        DEPENDS strata_authoring
        COMMENT "Generating the language-neutral Strata registry artifact"
        VERBATIM
    )
    add_custom_target(strata_registry ALL DEPENDS "${STRATA_REGISTRY}")

    add_custom_target(
        strata_generate_authoring
        COMMAND strata_authoring --write "${STRATA_REPOSITORY_ROOT}"
        DEPENDS strata_authoring
        COMMENT "Regenerating catalog-derived Strata authoring files"
        VERBATIM
    )
    add_custom_target(
        strata_check_authoring
        COMMAND strata_authoring --check "${STRATA_REPOSITORY_ROOT}"
        DEPENDS strata_authoring
        COMMENT "Checking catalog-derived Strata authoring files"
        VERBATIM
    )
    file(READ "${STRATA_REPOSITORY_ROOT}/editor/vscode/package.json" strata_vscode_manifest)
    string(JSON STRATA_VSCODE_VERSION GET "${strata_vscode_manifest}" version)
    set(
        STRATA_VSCODE_PACKAGE
        "${CMAKE_BINARY_DIR}/editor/strata-language-${STRATA_VSCODE_VERSION}.vsix"
    )
    add_custom_command(
        OUTPUT ${STRATA_VSCODE_PACKAGE}
        COMMAND ${CMAKE_COMMAND}
            -DSTRATA_EDITOR_SOURCE=${STRATA_REPOSITORY_ROOT}/editor/vscode
            -DSTRATA_EDITOR_PACKAGE=${STRATA_VSCODE_PACKAGE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_vscode_extension.cmake
        DEPENDS
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/package.json
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/language-configuration.json
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/extension.js
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/compiler_arguments.js
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/README.md
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/strata-completions.json
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/syntaxes/strata.tmLanguage.json
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/extension.vsixmanifest
            ${STRATA_REPOSITORY_ROOT}/editor/vscode/[Content_Types].xml
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_vscode_extension.cmake
        COMMENT "Packaging the schema-aware Strata VS Code extension"
        VERBATIM
    )
    add_custom_target(strata_vscode_extension ALL DEPENDS ${STRATA_VSCODE_PACKAGE})
    add_custom_target(strata_validate_modules COMMENT "Validating bundled Strata modules")
    add_custom_target(strata_generate_artifacts COMMENT "Regenerating bundled Strata artifacts")
    add_custom_target(strata_check_artifacts COMMENT "Checking bundled Strata artifacts")
    add_dependencies(strata_validate_modules strata_compile)
    add_dependencies(strata_generate_artifacts strata_compile)
    add_dependencies(strata_check_artifacts strata_compile)

    function(strata_register_bundled_module name source schemas artifact)
        add_custom_command(
            TARGET strata_validate_modules
            POST_BUILD
            COMMAND strata_compile
                --extension-path $<TARGET_FILE_DIR:strata_demo_extension>
                --check-module
                "${STRATA_REPOSITORY_ROOT}/${source}"
                "${STRATA_REPOSITORY_ROOT}/${schemas}"
            VERBATIM
        )
        if(NOT artifact STREQUAL "")
            add_custom_command(
                TARGET strata_generate_artifacts
                POST_BUILD
                COMMAND strata_compile
                    --extension-path $<TARGET_FILE_DIR:strata_demo_extension>
                    --emit-artifact
                    "${STRATA_REPOSITORY_ROOT}/${source}"
                    "${STRATA_REPOSITORY_ROOT}/${schemas}"
                    "${STRATA_RESOURCE_ROOT}"
                    "${STRATA_REPOSITORY_ROOT}/${artifact}"
                VERBATIM
            )
            add_custom_command(
                TARGET strata_check_artifacts
                POST_BUILD
                COMMAND strata_compile
                    --extension-path $<TARGET_FILE_DIR:strata_demo_extension>
                    --check-artifact
                    "${STRATA_REPOSITORY_ROOT}/${source}"
                    "${STRATA_REPOSITORY_ROOT}/${schemas}"
                    "${STRATA_RESOURCE_ROOT}"
                    "${STRATA_REPOSITORY_ROOT}/${artifact}"
                VERBATIM
            )
        endif()
        if(BUILD_TESTING AND STRATA_BUILD_TESTS)
            add_test(
                NAME "strata.module.${name}"
                COMMAND strata_compile
                    --extension-path $<TARGET_FILE_DIR:strata_demo_extension>
                    --check-module
                    "${STRATA_REPOSITORY_ROOT}/${source}"
                    "${STRATA_REPOSITORY_ROOT}/${schemas}"
            )
            if(NOT artifact STREQUAL "")
                add_test(
                    NAME "strata.artifact.${name}"
                    COMMAND strata_compile
                        --extension-path $<TARGET_FILE_DIR:strata_demo_extension>
                        --check-artifact
                        "${STRATA_REPOSITORY_ROOT}/${source}"
                        "${STRATA_REPOSITORY_ROOT}/${schemas}"
                        "${STRATA_RESOURCE_ROOT}"
                        "${STRATA_REPOSITORY_ROOT}/${artifact}"
                )
            endif()
        endif()
    endfunction()

    strata_register_bundled_module(
        showcase
        "src/main/resources/assets/strata/ui/demo_surface.strata"
        "src/main/resources/assets/strata/ui/demo_surface.schemas.json"
        "src/main/resources/assets/strata/ui/demo_surface.compiled.bin"
    )
    strata_register_bundled_module(
        settings
        "src/main/resources/assets/strata/ui/settings_app.strata"
        "src/main/resources/assets/strata/ui/settings_app.schemas.json"
        "src/main/resources/assets/strata/ui/settings_app.compiled.bin"
    )
    strata_register_bundled_module(
        debug
        "src/main/resources/assets/strata/ui/debug_overlay.strata"
        "src/main/resources/assets/strata/ui/debug_overlay.schemas.json"
        "src/main/resources/assets/strata/ui/debug_overlay.compiled.bin"
    )
    strata_register_bundled_module(
        performance_hud
        "src/main/resources/assets/strata/ui/performance_hud.strata"
        "src/main/resources/assets/strata/ui/performance_hud.schemas.json"
        "src/main/resources/assets/strata/ui/performance_hud.compiled.bin"
    )
    strata_register_bundled_module(
        hub
        "src/main/resources/assets/strata/ui/strata_hub.strata"
        "src/main/resources/assets/strata/ui/strata_hub.schemas.json"
        "src/main/resources/assets/strata/ui/strata_hub.compiled.bin"
    )
    strata_register_bundled_module(
        control_deck
        "src/main/resources/assets/strata/ui/control_deck.strata"
        "src/main/resources/assets/strata/ui/control_deck.schemas.json"
        "src/main/resources/assets/strata/ui/control_deck.compiled.bin"
    )
    strata_register_bundled_module(
        tuning_panel
        "src/main/resources/assets/strata/ui/tuning_panel.strata"
        "src/main/resources/assets/strata/ui/tuning_panel.schemas.json"
        "src/main/resources/assets/strata/ui/tuning_panel.compiled.bin"
    )
endif()

if(WIN32 AND STRATA_BUILD_DESKTOP)
    set(STRATA_PERFORMANCE_BASELINE "" CACHE FILEPATH "Optional desktop performance baseline")
    set(strata_performance_arguments
        --performance "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/performance/showcase.json"
        --output "${CMAKE_BINARY_DIR}/performance/showcase-desktop"
    )
    if(NOT STRATA_PERFORMANCE_BASELINE STREQUAL "")
        list(APPEND strata_performance_arguments --baseline "${STRATA_PERFORMANCE_BASELINE}")
    endif()
    list(APPEND strata_performance_arguments "${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources")
    add_custom_target(
        strata_benchmark_desktop
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/performance/showcase-desktop"
        COMMAND strata_desktop ${strata_performance_arguments}
        DEPENDS strata_desktop
        COMMENT "Running the visible uncapped desktop performance scenario"
        VERBATIM
    )
endif()

if(BUILD_TESTING AND STRATA_BUILD_TESTS)
    if(STRATA_BUILD_TOOLS)
        add_test(
            NAME strata.authoring.generated
            COMMAND strata_authoring --check "${CMAKE_CURRENT_SOURCE_DIR}/.."
        )
        add_test(
            NAME strata.compiler.diagnostics_json
            COMMAND ${CMAKE_COMMAND}
                -DSTRATA_COMPILER=$<TARGET_FILE:strata_compile>
                -DSTRATA_VALID_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/assets/strata/ui/settings_app.strata
                -DSTRATA_VALID_SCHEMAS=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources/assets/strata/ui/settings_app.schemas.json
                -DSTRATA_TEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/compiler-diagnostics-json
                -DSTRATA_EXTENSION_DIRECTORY=$<TARGET_FILE_DIR:strata_demo_extension>
                -DSTRATA_EXTENSION_SOURCE=${STRATA_REPOSITORY_ROOT}/src/main/resources/assets/strata/ui/demo_surface.strata
                -DSTRATA_EXTENSION_SCHEMAS=${STRATA_REPOSITORY_ROOT}/src/main/resources/assets/strata/ui/demo_surface.schemas.json
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_diagnostics_json.cmake
        )
        add_test(
            NAME strata.editor.vscode.package
            COMMAND ${CMAKE_COMMAND}
                -DSTRATA_EDITOR_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/../editor/vscode
                -DSTRATA_EDITOR_PACKAGE=${CMAKE_CURRENT_BINARY_DIR}/editor-test/strata-language.vsix
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_vscode_extension.cmake
        )
        find_program(strata_node_executable NAMES node nodejs)
        if(strata_node_executable)
            add_test(
                NAME strata.editor.vscode.javascript
                COMMAND ${strata_node_executable} --check
                    ${CMAKE_CURRENT_SOURCE_DIR}/../editor/vscode/extension.js
            )
            add_test(
                NAME strata.editor.vscode.compiler_arguments
                COMMAND ${strata_node_executable}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../editor/vscode/compiler_arguments.test.js
            )
        endif()
    endif()

    add_executable(
        strata_host_tests
        tests/host_test_runner.cpp
        tests/host_tests.cpp
        tests/browser_model_tests.cpp
        tests/showcase_model_tests.cpp
        headless/scenario.cpp
        desktop/showcase.cpp
    )
    target_link_libraries(
        strata_host_tests
        PRIVATE Strata::host strata_application_contracts strata_host_support
    )
    target_include_directories(strata_host_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/desktop)
    strata_target_defaults(strata_host_tests)
    add_test(NAME strata.host COMMAND strata_host_tests host)
    add_test(NAME strata.host.browser_model COMMAND strata_host_tests browser_model)
    add_test(NAME strata.showcase.model COMMAND strata_host_tests showcase_model)

    add_executable(
        strata_core_tests
        tests/core_test_runner.cpp
        tests/svg_tests.cpp
        tests/core_tests.cpp
        tests/data_tests.cpp
        tests/compiler_tests.cpp
        tests/runtime_tests.cpp
        tests/profiler_tests.cpp
        tests/resource_host_residual_tests.cpp
        tests/ui_tests.cpp
        tests/render_packet_tests.cpp
        tests/text_font_residual_tests.cpp
        tests/theme_tests.cpp
        tests/input_editor_tests.cpp
        tests/chip_input_tests.cpp
        tests/shell_lifecycle_tests.cpp
        tests/interaction_residual_tests.cpp
        tests/widget_binding_tests.cpp
        tests/form_validation_tests.cpp
        tests/command_binding_tests.cpp
    )
    target_link_libraries(
        strata_core_tests
        PRIVATE Strata::core Strata::render_host Strata::svg
    )
    target_include_directories(strata_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_core_tests)
    add_test(
        NAME strata.svg
        COMMAND strata_core_tests svg ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/svg
    )
    add_test(NAME strata.core COMMAND strata_core_tests core)
    add_test(NAME strata.data COMMAND strata_core_tests data)
    add_test(NAME strata.compiler COMMAND strata_core_tests compiler)
    add_test(
        NAME strata.runtime
        COMMAND strata_core_tests runtime
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic-session
    )
    add_test(NAME strata.profiler COMMAND strata_core_tests profiler)
    add_test(
        NAME strata.resource.host.residual
        COMMAND strata_core_tests resource_host_residual
    )
    add_test(
        NAME strata.ui
        COMMAND strata_core_tests ui
            ${STRATA_RESOURCE_ROOT}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic-session
    )
    add_test(
        NAME strata.render_packet
        COMMAND strata_core_tests render_packet ${STRATA_RESOURCE_ROOT}
    )

    if(TARGET strata_headless_host)
        add_executable(strata_effect_refresh_tests tests/effect_refresh_tests.cpp)
        target_link_libraries(strata_effect_refresh_tests PRIVATE strata_headless_host)
        target_include_directories(
            strata_effect_refresh_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/headless
        )
        strata_target_defaults(strata_effect_refresh_tests)
        add_test(NAME strata.effect_refresh COMMAND strata_effect_refresh_tests)
    endif()

    add_test(
        NAME strata.ui.text_font_residuals
        COMMAND strata_core_tests text_font_residual ${STRATA_RESOURCE_ROOT}
    )
    add_test(NAME strata.ui.theme COMMAND strata_core_tests theme)
    add_test(NAME strata.input.editor COMMAND strata_core_tests input_editor)
    add_test(
        NAME strata.widget.chip_input
        COMMAND strata_core_tests chip_input ${STRATA_RESOURCE_ROOT}
    )
    add_test(
        NAME strata.widget.shell_lifecycle
        COMMAND strata_core_tests shell_lifecycle ${STRATA_RESOURCE_ROOT}
    )
    add_test(
        NAME strata.ui.interaction_residuals
        COMMAND strata_core_tests interaction_residual ${STRATA_RESOURCE_ROOT}
    )
    add_test(
        NAME strata.widget.binding
        COMMAND strata_core_tests widget_binding ${STRATA_RESOURCE_ROOT}
    )
    add_test(
        NAME strata.form.validation
        COMMAND strata_core_tests form_validation ${STRATA_RESOURCE_ROOT}
    )
    add_test(
        NAME strata.command.binding
        COMMAND strata_core_tests command_binding ${STRATA_RESOURCE_ROOT}
    )

    add_executable(strata_extension_tests tests/extension_tests.cpp)
    target_link_libraries(strata_extension_tests PRIVATE strata_extensions strata_host_support)
    add_dependencies(
        strata_extension_tests
        strata_demo_extension
        strata_control_deck_extension
    )
    strata_target_defaults(strata_extension_tests)
    add_test(
        NAME strata.extension
        COMMAND strata_extension_tests
            ${STRATA_RESOURCE_ROOT}
    )

    add_executable(
        strata_abi_tests
        tests/abi_test_runner.cpp
        tests/abi_tests.cpp
        tests/scale_tests.cpp
    )
    target_link_libraries(strata_abi_tests PRIVATE Strata::c Strata::core)
    target_include_directories(strata_abi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    strata_target_defaults(strata_abi_tests)
    add_test(NAME strata.scale COMMAND strata_abi_tests scale)
    add_test(
        NAME strata.abi
        COMMAND strata_abi_tests abi
            ${STRATA_RESOURCE_ROOT}
    )

    if(STRATA_BUILD_SAMPLES)
        add_test(
            NAME strata.embed.c
            COMMAND strata_c_smoke
        )
        add_test(
            NAME strata.embed.cpp
            COMMAND strata_cpp_smoke
        )
        add_test(
            NAME strata.embed.cpp_static
            COMMAND strata_cpp_static_smoke
        )
        if(WIN32 AND STRATA_BUILD_DESKTOP)
            add_test(
                NAME strata.embed.desktop
                COMMAND strata_desktop_sample --smoke
                    ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
            )
        endif()
        if(WIN32)
            add_test(NAME strata.embed.adapter_c COMMAND strata_adapter_c_smoke)
            add_test(NAME strata.embed.d3d11_target COMMAND strata_d3d11_target_smoke)
        endif()
    endif()
    if(STRATA_BUILD_TOOLS)
        add_test(
            NAME strata.headless.portable
            COMMAND ${CMAKE_COMMAND}
                -DSTRATA_HEADLESS=$<TARGET_FILE:strata_headless>
                -DSTRATA_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                -DSTRATA_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/portable.json
                -DSTRATA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/headless-portable
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_portable.cmake
        )
        add_test(
            NAME strata.headless.portable.interactive
            COMMAND ${CMAKE_COMMAND}
                -DSTRATA_HEADLESS=$<TARGET_FILE:strata_headless>
                -DSTRATA_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                -DSTRATA_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/portable.json
                -DSTRATA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/headless-portable-interactive
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_portable_interactive.cmake
        )
        if(WIN32)
            add_test(
                NAME strata.headless.d3d11
                COMMAND ${CMAKE_COMMAND}
                    -DSTRATA_HEADLESS=$<TARGET_FILE:strata_headless>
                    -DSTRATA_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                    -DSTRATA_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/showcase.json
                    -DSTRATA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/headless-d3d11
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_smoke.cmake
            )
            add_test(
                NAME strata.headless.d3d11.interactive
                COMMAND ${CMAKE_COMMAND}
                    -DSTRATA_HEADLESS=$<TARGET_FILE:strata_headless>
                    -DSTRATA_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                    -DSTRATA_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/showcase.json
                    -DSTRATA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/headless-d3d11-interactive
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_interactive.cmake
            )
            add_test(
                NAME strata.headless.effects
                COMMAND ${CMAKE_COMMAND}
                    -DSTRATA_HEADLESS=$<TARGET_FILE:strata_headless>
                    -DSTRATA_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                    -DSTRATA_D3D11_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/effects.json
                    -DSTRATA_PORTABLE_SCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/headless/effects-portable.json
                    -DSTRATA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/headless-effects
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_effects.cmake
            )
        endif()
    endif()
    if(WIN32 AND STRATA_BUILD_DESKTOP)
        add_test(
            NAME strata.desktop.smoke
            COMMAND strata_desktop --smoke ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
        )
        add_test(
            NAME strata.desktop.application
            COMMAND strata_desktop
                --application ${CMAKE_CURRENT_SOURCE_DIR}/samples/desktop_app.json
                --resources ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
                --smoke
        )
        add_test(
            NAME strata.desktop.performance_smoke
            COMMAND strata_desktop
                --performance ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/performance/smoke.json
                --output ${CMAKE_CURRENT_BINARY_DIR}/performance-smoke
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/resources
        )
    endif()

    add_test(
        NAME strata.release.dependencies
        COMMAND ${CMAKE_COMMAND}
            -DSTRATA_C_LIBRARY=$<TARGET_FILE:strata_c>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_dependencies.cmake
    )
    add_test(
        NAME strata.install.consumer
        COMMAND ${CMAKE_COMMAND}
            -DSTRATA_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DSTRATA_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/installed-test/prefix
            -DSTRATA_SMOKE_BUILD_DIR=${CMAKE_BINARY_DIR}/installed-test/consumer
            -DSTRATA_GENERATOR=${CMAKE_GENERATOR}
            -DSTRATA_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
            -DSTRATA_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
            -DSTRATA_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DSTRATA_CONFIG=$<CONFIG>
            -DSTRATA_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}
            -DSTRATA_EXPECT_VSCODE_EXTENSION=${STRATA_BUILD_TOOLS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_package.cmake
    )
    set_tests_properties(strata.install.consumer PROPERTIES RUN_SERIAL TRUE)
endif()

install(
    TARGETS strata_c strata_host_cpp strata_extensions strata_render_host strata_svg
    EXPORT StrataTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
if(WIN32)
    install(
        TARGETS strata_d3d11 strata_win32
        EXPORT StrataTargets
        ARCHIVE DESTINATION lib
    )
endif()
if(WIN32 AND STRATA_BUILD_DESKTOP)
    install(
        TARGETS strata_desktop_host
        EXPORT StrataTargets
        ARCHIVE DESTINATION lib
    )
    install(TARGETS strata_desktop RUNTIME DESTINATION bin)
endif()
if(STRATA_BUILD_TOOLS)
    install(
        TARGETS strata_compile strata_authoring strata_headless strata_svg_render
        EXPORT StrataTargets
        RUNTIME DESTINATION bin
    )
    install(
        FILES ${STRATA_VSCODE_PACKAGE}
        DESTINATION share/strata/editor
        OPTIONAL
    )
    install(
        FILES ${STRATA_REGISTRY}
        DESTINATION share/strata
    )
endif()
install(DIRECTORY include/ DESTINATION include)
install(
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.txt
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
        samples/installed/contract.schemas.json
        samples/installed/contract_smoke.cpp
        samples/installed/validated_module.strata
        samples/installed/external_extension.cpp
        samples/installed/extension_loader_smoke.cpp
        samples/desktop_app.cpp
        samples/desktop_app.json
        samples/adapter_c_smoke.c
        samples/d3d11_target_smoke.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StrataModule.cmake
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
