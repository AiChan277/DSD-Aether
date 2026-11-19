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

# Icon + VERSIONINFO (shown in Explorer, Task Manager, Apps & Features and read by AV reputation services).
if(WIN32)
    set(AETHER_APP_ICON "${CMAKE_SOURCE_DIR}/installer/assets/aether.ico")
    set(AETHER_RELEASE_TAG "a${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}B")
    configure_file(aether_studio.rc.in "${CMAKE_CURRENT_BINARY_DIR}/aether_studio.rc" @ONLY)
    target_sources(aether_studio PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/aether_studio.rc"
        "${CMAKE_CURRENT_SOURCE_DIR}/aether_app.manifest"
    )
endif()

if(MSVC)
    target_link_options(aether_studio PRIVATE
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
endif()

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
