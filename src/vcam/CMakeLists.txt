add_library(aether_vcam_ds_x64 SHARED
    aether_vcam_ds.cpp
    aether_vcam_ds.def
    guids.h
    vcam_shm.h
)

aether_apply_cxx_flags(aether_vcam_ds_x64)

if(WIN32)
    set(AETHER_VCAM_ARCH "x64")
    set(AETHER_VCAM_VER_COMMA "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    set(AETHER_VCAM_VER_STR "${PROJECT_VERSION}.0")
    set(AETHER_RELEASE_TAG "a${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}B")
    configure_file(aether_vcam_ds.rc.in "${CMAKE_CURRENT_BINARY_DIR}/aether_vcam_ds_x64.rc" @ONLY)
    target_sources(aether_vcam_ds_x64 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/aether_vcam_ds_x64.rc")
endif()

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
