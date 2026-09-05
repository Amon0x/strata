# Shaderc packages use several names across Linux distributions and the Vulkan SDK.
if(NOT TARGET StrataShaderc::shaderc)
    find_path(STRATA_SHADERC_INCLUDE_DIR shaderc/shaderc.hpp HINTS "$ENV{VULKAN_SDK}/include" REQUIRED)
    find_library(STRATA_SHADERC_LIBRARY NAMES shaderc_shared shaderc
        HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    add_library(StrataShaderc::shaderc UNKNOWN IMPORTED)
    set_target_properties(StrataShaderc::shaderc PROPERTIES
        IMPORTED_LOCATION "${STRATA_SHADERC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${STRATA_SHADERC_INCLUDE_DIR}")
endif()
