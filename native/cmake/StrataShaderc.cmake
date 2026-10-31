# Shaderc packages use several names across Linux distributions and the Vulkan SDK.
if(NOT TARGET StrataShaderc::shaderc)
    find_path(STRATA_SHADERC_INCLUDE_DIR shaderc/shaderc.hpp HINTS "$ENV{VULKAN_SDK}/include" REQUIRED)
    find_library(STRATA_SHADERC_LIBRARY NAMES shaderc_shared shaderc
        HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    add_library(StrataShaderc::shaderc UNKNOWN IMPORTED)
    set_target_properties(StrataShaderc::shaderc PROPERTIES
        IMPORTED_LOCATION "${STRATA_SHADERC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${STRATA_SHADERC_INCLUDE_DIR}")
    if(WIN32 AND STRATA_SHADERC_LIBRARY MATCHES "shaderc_shared\\.lib$")
        set_property(TARGET StrataShaderc::shaderc PROPERTY
            INTERFACE_COMPILE_DEFINITIONS SHADERC_SHAREDLIB)
    endif()
endif()

# Cached compiler output is untrusted binary input too. Validate it before giving it to a driver.
if(NOT TARGET StrataShaderc::spirv_tools)
    find_path(STRATA_SPIRV_TOOLS_INCLUDE_DIR spirv-tools/libspirv.h
        HINTS "${STRATA_SHADERC_INCLUDE_DIR}" "$ENV{VULKAN_SDK}/include" REQUIRED)
    find_library(STRATA_SPIRV_TOOLS_LIBRARY NAMES SPIRV-Tools-shared SPIRV-Tools
        HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    add_library(StrataShaderc::spirv_tools UNKNOWN IMPORTED)
    set_target_properties(StrataShaderc::spirv_tools PROPERTIES
        IMPORTED_LOCATION "${STRATA_SPIRV_TOOLS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${STRATA_SPIRV_TOOLS_INCLUDE_DIR}")
    if(STRATA_SPIRV_TOOLS_LIBRARY MATCHES "SPIRV-Tools-shared\\.")
        set_property(TARGET StrataShaderc::spirv_tools PROPERTY
            INTERFACE_COMPILE_DEFINITIONS SPIRV_TOOLS_SHAREDLIB)
    endif()
endif()
