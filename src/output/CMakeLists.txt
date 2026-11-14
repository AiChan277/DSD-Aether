add_library(aether_output STATIC
    dsp_sync_bridge.cpp
    dsp_sync_bridge.hpp
    ndi_output.cpp
    ndi_output.hpp
    osc_sender.cpp
    osc_sender.hpp
    output_config.hpp
    output_coordinator.cpp
    output_coordinator.hpp
    output_ring.hpp
    spout_output.cpp
    spout_output.hpp
    vcam_transport.cpp
    vcam_transport.hpp
    vmc_sender.cpp
    vmc_sender.hpp
)

aether_apply_cxx_flags(aether_output)

target_include_directories(aether_output PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(aether_output PUBLIC
    aether_core
    aether_rhi
    aether_animation
)

if(WIN32)
    target_link_libraries(aether_output PUBLIC
        d3d11
        dxgi
        ws2_32
        winmm
    )
endif()
