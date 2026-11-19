add_executable(aether-plugin-runner
    runner_main.cpp
    native_plugin_abi.h
)
aether_apply_cxx_flags(aether-plugin-runner)
if(WIN32)
    set(AETHER_RELEASE_TAG "a${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}B")
    configure_file(aether_plugin_runner.rc.in "${CMAKE_CURRENT_BINARY_DIR}/aether_plugin_runner.rc" @ONLY)
    target_sources(aether-plugin-runner PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/aether_plugin_runner.rc"
        "${CMAKE_CURRENT_SOURCE_DIR}/aether_plugin_runner.manifest"
    )
endif()
set_target_properties(aether-plugin-runner PROPERTIES
    OUTPUT_NAME "aether-plugin-runner"
)
target_include_directories(aether-plugin-runner PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)
if(WIN32)
    target_link_libraries(aether-plugin-runner PRIVATE kernel32)
endif()

add_library(aether_native_plugin_manager STATIC
    native_plugin_manager.hpp
    native_plugin_manager.cpp
    native_plugin_abi.h
)
aether_apply_cxx_flags(aether_native_plugin_manager)
target_include_directories(aether_native_plugin_manager PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)
if(WIN32)
    target_link_libraries(aether_native_plugin_manager PUBLIC advapi32 kernel32)
endif()
add_dependencies(aether_native_plugin_manager aether-plugin-runner)
