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
