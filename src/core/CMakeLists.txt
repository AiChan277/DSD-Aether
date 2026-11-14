add_library(aether_core STATIC
    arena.hpp
    clock.cpp
    clock.hpp
    cpu_topology.cpp
    cpu_topology.hpp
    crash_handler.cpp
    crash_handler.hpp
    expected.hpp
    garbage_collector.cpp
    garbage_collector.hpp
    hot_path_guard.cpp
    hot_path_guard.hpp
    hw_enum.cpp
    hw_enum.hpp
    log.cpp
    log.hpp
    paths.cpp
    paths.hpp
    spsc_ring.hpp
    telemetry.cpp
    telemetry.hpp
    triple_buffer.hpp
)

aether_apply_cxx_flags(aether_core)

target_include_directories(aether_core PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(aether_core PUBLIC
    aether_rust_host
)

if(AETHER_WITH_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(aether_core PUBLIC Tracy::TracyClient)
endif()

if(WIN32)
    target_link_libraries(aether_core PRIVATE
        dxgi
        mf
        mfplat
        mfreadwrite
        ole32
        setupapi
        shell32
        avrt
    )
endif()
