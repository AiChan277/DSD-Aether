add_executable(aether_studio
    application.cpp
    application.hpp
    input_events.cpp
    input_events.hpp
    main.cpp
    pipeline_skeleton.cpp
    pipeline_skeleton.hpp
)

aether_apply_cxx_flags(aether_studio)

target_compile_definitions(aether_studio PRIVATE
    AETHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_include_directories(aether_studio PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

find_package(implot CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(aether_studio PRIVATE
    aether_ui
    aether_renderer
    aether_output
    aether_tracking
    aether_animation
    aether_native_plugin_manager
    implot::implot
    nlohmann_json::nlohmann_json
)

add_dependencies(aether_studio aether_vcam_ds_x64 aether-plugin-runner)

aether_stage_ort_runtime(aether_studio app)

if(WIN32)
    target_link_libraries(aether_studio PRIVATE
        dwmapi
        ole32
        shell32
    )
endif()
