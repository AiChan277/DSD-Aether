add_library(aether_rhi STATIC
    bindless.cpp
    bindless.hpp
    d3d12_device.cpp
    d3d12_device.hpp
    gpu_timing.cpp
    gpu_timing.hpp
    rebar_ring.cpp
    rebar_ring.hpp
    rhi_device.cpp
    rhi_device.hpp
    rhi_swapchain.cpp
    rhi_swapchain.hpp
)

aether_apply_cxx_flags(aether_rhi)

find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

target_include_directories(aether_rhi PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(aether_rhi PUBLIC
    aether_core
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
)

if(WIN32)
    target_link_libraries(aether_rhi PUBLIC d3d12 dxgi d3d11 dxguid)
endif()
