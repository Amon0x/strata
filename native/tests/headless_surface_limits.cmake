# A surface or resize step that no backend can render is rejected with a message, before anything
# allocates a framebuffer for it: 70000 x 70000 would be a 19 GB capture, 0.3 x 0.3 rounds to none.
if(NOT DEFINED STRATA_HEADLESS OR NOT DEFINED STRATA_RESOURCES OR NOT DEFINED STRATA_SCENARIO OR
   NOT DEFINED STRATA_OUTPUT)
    message(FATAL_ERROR "headless surface limits test requires executable, resources, scenario, and output")
endif()

file(READ "${STRATA_SCENARIO}" base)
file(REMOVE_RECURSE "${STRATA_OUTPUT}")
file(MAKE_DIRECTORY "${STRATA_OUTPUT}")

function(expect_rejected name scenario)
    set(path "${STRATA_OUTPUT}/${name}.json")
    file(WRITE "${path}" "${scenario}")
    execute_process(
        COMMAND "${STRATA_HEADLESS}"
            --resources "${STRATA_RESOURCES}"
            --scenario "${path}"
            --output "${STRATA_OUTPUT}/${name}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(status EQUAL 0 OR NOT stderr MATCHES "1 to 16384 device pixels")
        message(FATAL_ERROR "${name} was not rejected for its size (${status})\n${stdout}\n${stderr}")
    endif()
endfunction()

string(JSON huge_resize SET "${base}" steps
    [=[[{"advance":{"milliseconds":0}},{"resize":{"width":70000,"height":70000}},{"capture":"h"}]]=])
expect_rejected(huge-resize "${huge_resize}")

string(JSON tiny_surface SET "${base}" surface width 0.3)
string(JSON tiny_surface SET "${tiny_surface}" steps [=[[{"capture":"h"}]]=])
expect_rejected(tiny-surface "${tiny_surface}")

string(JSON huge_scale SET "${base}" surface scale 40)
string(JSON huge_scale SET "${huge_scale}" steps [=[[{"capture":"h"}]]=])
expect_rejected(huge-scale "${huge_scale}")
