find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(AETHER_SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/generated/shaders/$<CONFIG>")
set(AETHER_SHADER_HEADER "${CMAKE_BINARY_DIR}/generated/include/generated/preview_shaders_spv.hpp")
set(AETHER_PHASE4_SHADER_HEADER "${CMAKE_BINARY_DIR}/generated/include/generated/phase4_shaders_spv.hpp")
set(AETHER_PHASE4_DXIL_HEADER "${CMAKE_BINARY_DIR}/generated/include/generated/phase4_shaders_dxil.hpp")

file(GLOB AETHER_SLANG_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*.slang")

add_custom_command(
    OUTPUT
        "${AETHER_SHADER_HEADER}"
        "${AETHER_PHASE4_SHADER_HEADER}"
        "${AETHER_PHASE4_DXIL_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/shader_build.py"
        --repo-root "${CMAKE_SOURCE_DIR}"
        --out-dir "${AETHER_SHADER_OUT_DIR}"
        --header "${AETHER_SHADER_HEADER}"
        --phase4-header "${AETHER_PHASE4_SHADER_HEADER}"
        --phase4-dxil-header "${AETHER_PHASE4_DXIL_HEADER}"
        --manifest "${CMAKE_SOURCE_DIR}/shaders/manifest.toml"
        --config "$<CONFIG>"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/shader_build.py"
        "${CMAKE_SOURCE_DIR}/tools/embed_spv.py"
        "${CMAKE_SOURCE_DIR}/shaders/manifest.toml"
        "${CMAKE_SOURCE_DIR}/shaders/preview_mesh.vert.hlsl"
        "${CMAKE_SOURCE_DIR}/shaders/preview_mesh.frag.hlsl"
        "${CMAKE_SOURCE_DIR}/shaders/preview_bones.vert.hlsl"
        "${CMAKE_SOURCE_DIR}/shaders/preview_bones.frag.hlsl"
        ${AETHER_SLANG_SOURCES}
    COMMENT "Compiling AETHER-2.5D shaders to SPIR-V (slangc + reflection + DXIL check)"
    VERBATIM
)

add_custom_target(aether_preview_shaders DEPENDS
    "${AETHER_SHADER_HEADER}"
    "${AETHER_PHASE4_SHADER_HEADER}"
    "${AETHER_PHASE4_DXIL_HEADER}"
)

add_library(aether_renderer STATIC
    accel_struct.cpp
    accel_struct.hpp
    asset_upload/gpu_avatar.cpp
    asset_upload/asset_job.cpp
    circadian_lighting.cpp
    circadian_lighting.hpp
    denoise/denoiser_backend.hpp
    denoise/denoiser_manager.cpp
    denoise/denoiser_manager.hpp
    denoise/svgf_lite_denoiser.cpp
    denoise/svgf_lite_denoiser.hpp
    denoise/vendor_denoisers.cpp
    denoise/vendor_denoisers.hpp
    dx11_bridge.cpp
    dx11_bridge.hpp
    dx11_renderer.cpp
    dx11_renderer.hpp
    dx12_bridge.cpp
    dx12_bridge.hpp
    dx12_frame_graph.cpp
    dx12_frame_graph.hpp
    frame_globals_builder.hpp
    frame_graph.cpp
    frame_graph.hpp
    gpu_structs.hpp
    lighting_rig.hpp
    npr_renderer.cpp
    npr_renderer.hpp
    preview_pass.cpp
    sdf_generator.cpp
    sdf_generator.hpp
)

aether_apply_cxx_flags(aether_renderer)

target_include_directories(aether_renderer PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated/include"
)

target_link_libraries(aether_renderer PUBLIC
    aether_rhi
    aether_core
    aether_rust_host
)

if(WIN32)
    target_link_libraries(aether_renderer PUBLIC
        d3d11
        dxgi
        d3dcompiler
    )
endif()

add_dependencies(aether_renderer
    ffi_header_check
    aether_preview_shaders
)
