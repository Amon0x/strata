if(NOT DEFINED STRATA_C_LIBRARY)
    message(FATAL_ERROR "release dependency test requires the Strata C library")
endif()
file(STRINGS "${STRATA_C_LIBRARY}" sanitizer_imports
    REGEX "clang_rt[.]asan_dynamic" LIMIT_COUNT 1)
if(sanitizer_imports)
    message(FATAL_ERROR "ordinary Strata C library imports the ASan runtime")
endif()
