add_library(aether_tracking STATIC
    manifest.cpp
    camera_mf.cpp
    file_camera.cpp
    roi_preprocess.cpp
    ort_router.cpp
    blink_gaze_filter.cpp
    landmark_solver.cpp
    npu_postprocess.cpp
    npu_postprocess.hpp
    tracking_engine.cpp
    wasapi_audio_lipsync.cpp
)

target_link_libraries(aether_tracking
    PUBLIC
        aether_core
        aether_animation
        aether_ort_sdk
        bcrypt
        d3d12
        dxgi
        mf
        mfplat
        mfreadwrite
        mfuuid
        strmiids
        propsys
        ole32
        uuid
)

target_include_directories(aether_tracking
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
)

if (MSVC)
    set_source_files_properties(roi_preprocess.cpp npu_postprocess.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

aether_apply_cxx_flags(aether_tracking)
