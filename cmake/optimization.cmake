# Optimize every source-built application target, including helper libraries
# created inside components (for example LVGL port and mbedTLS sub-libraries).
# Imported/precompiled libraries and the separate bootloader build are excluded.
function(tab5_optimize_sources directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(target_type "${target}" TYPE)
        if(target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-O3>")
        endif()
    endforeach()

    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        tab5_optimize_sources("${child}")
    endforeach()
endfunction()

function(tab5_audit_optimization)
    if(TAB5_INFERENCE_AUDIT AND TAB5_AUDIT_DL_O2)
        idf_component_get_property(dl_target espressif__esp-dl COMPONENT_LIB)
        # SHELL keeps CMake option deduplication from dropping this final -O2
        # because the IDF global flags already contain an earlier -O2.
        target_compile_options(${dl_target} PRIVATE "SHELL:-O2")
        message(STATUS "Inference audit: final -O2 override on ${dl_target}")
    endif()
endfunction()
