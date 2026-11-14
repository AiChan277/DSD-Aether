add_library(aether_vcam_ds_x64 SHARED
    aether_vcam_ds.cpp
    aether_vcam_ds.def
    guids.h
    vcam_shm.h
)

aether_apply_cxx_flags(aether_vcam_ds_x64)

set_target_properties(aether_vcam_ds_x64 PROPERTIES
    OUTPUT_NAME "aether_vcam_ds_x64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(aether_vcam_ds_x64 PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

if(WIN32)
    target_link_libraries(aether_vcam_ds_x64 PRIVATE
        ole32
        oleaut32
        uuid
        strmiids
        advapi32
        gdi32
        winmm
    )
endif()
