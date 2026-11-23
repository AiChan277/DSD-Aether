find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

function(aether_add_cpp_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    aether_apply_cxx_flags(${target_name})
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/cpp"
    )
    target_link_libraries(${target_name} PRIVATE
        Catch2::Catch2WithMain
        aether_core
    )
    add_dependencies(${target_name} ffi_header_check)
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

aether_add_cpp_test(ffi_layout_test cpp/ffi_layout_test.cpp)
target_link_libraries(ffi_layout_test PRIVATE nlohmann_json::nlohmann_json)

aether_add_cpp_test(zero_copy_borrow_test cpp/zero_copy_borrow_test.cpp)

aether_add_cpp_test(triple_buffer_test cpp/triple_buffer_test.cpp)
set_tests_properties(triple_buffer_test PROPERTIES TIMEOUT 120)

aether_add_cpp_test(spsc_mailbox_test cpp/spsc_mailbox_test.cpp)
aether_add_cpp_test(hot_path_guard_test cpp/hot_path_guard_test.cpp)
aether_add_cpp_test(topology_clock_test cpp/topology_clock_test.cpp)

aether_add_cpp_test(rhi_validation_smoke_test cpp/rhi_validation_smoke_test.cpp)
target_link_libraries(rhi_validation_smoke_test PRIVATE aether_rhi)

aether_add_cpp_test(asset_loader_test cpp/asset_loader_test.cpp)
target_compile_definitions(asset_loader_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")

aether_add_cpp_test(asset_upload_vk_test cpp/asset_upload_vk_test.cpp)
target_compile_definitions(asset_upload_vk_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(asset_upload_vk_test PRIVATE aether_renderer)

add_executable(input_t0_t3_test
    cpp/input_t0_t3_test.cpp
    "${CMAKE_SOURCE_DIR}/src/app/input_events.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/pipeline_skeleton.cpp"
)
aether_apply_cxx_flags(input_t0_t3_test)
target_compile_definitions(input_t0_t3_test PRIVATE
    AETHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_include_directories(input_t0_t3_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(input_t0_t3_test PRIVATE
    Catch2::Catch2WithMain
    aether_core
    aether_output
    aether_tracking
    aether_animation
)
aether_stage_ort_runtime(input_t0_t3_test tests)
add_test(NAME input_t0_t3_test COMMAND input_t0_t3_test)

# Phase 3: Tracking & Animation/IK exit-gate tests
aether_add_cpp_test(animation_filters_ik_test cpp/animation_filters_ik_test.cpp)
target_link_libraries(animation_filters_ik_test PRIVATE aether_animation)

aether_add_cpp_test(vmc_protocol_test cpp/vmc_protocol_test.cpp)
target_link_libraries(vmc_protocol_test PRIVATE aether_output aether_animation)

aether_add_cpp_test(spring_bone_simd_test cpp/spring_bone_simd_test.cpp)
target_link_libraries(spring_bone_simd_test PRIVATE aether_animation)

aether_add_cpp_test(tracking_preprocess_test cpp/tracking_preprocess_test.cpp)
target_compile_definitions(tracking_preprocess_test PRIVATE AETHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tracking_preprocess_test PRIVATE aether_tracking)
aether_stage_ort_runtime(tracking_preprocess_test tests)

aether_add_cpp_test(tracking_router_test cpp/tracking_router_test.cpp)
target_compile_definitions(tracking_router_test PRIVATE AETHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tracking_router_test PRIVATE aether_tracking)
aether_stage_ort_runtime(tracking_router_test tests)
set_tests_properties(tracking_router_test PROPERTIES TIMEOUT 240)

# Phase 4: Vulkan 2.5D NPR Renderer & Hardware Ray Tracer exit-gate tests
aether_add_cpp_test(rhi_phase4_test cpp/rhi_phase4_test.cpp)
target_compile_definitions(rhi_phase4_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(rhi_phase4_test PRIVATE aether_renderer aether_rhi windowscodecs ole32)
set_tests_properties(rhi_phase4_test PROPERTIES TIMEOUT 180)

aether_add_cpp_test(renderer_npr_effects_test cpp/renderer_npr_effects_test.cpp)
target_compile_definitions(renderer_npr_effects_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(renderer_npr_effects_test PRIVATE aether_renderer aether_rhi windowscodecs ole32)
set_tests_properties(renderer_npr_effects_test PROPERTIES TIMEOUT 180)

aether_add_cpp_test(denoiser_and_rt_test cpp/denoiser_and_rt_test.cpp)
target_compile_definitions(denoiser_and_rt_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(denoiser_and_rt_test PRIVATE aether_renderer aether_rhi windowscodecs ole32)
set_tests_properties(denoiser_and_rt_test PROPERTIES TIMEOUT 240)

aether_add_cpp_test(golden_render_test cpp/golden_render_test.cpp)
target_compile_definitions(golden_render_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(golden_render_test PRIVATE aether_renderer aether_rhi windowscodecs ole32)
set_tests_properties(golden_render_test PROPERTIES TIMEOUT 300)

# Phase 5: Alpha Video Output & Audio-DSP IPC exit-gate tests
add_executable(dsp_torture_worker cpp/dsp_torture_worker.c)
target_compile_features(dsp_torture_worker PRIVATE c_std_11)
if(MSVC)
    target_compile_options(dsp_torture_worker PRIVATE /W4 /WX /utf-8)
endif()
target_include_directories(dsp_torture_worker PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_dependencies(dsp_torture_worker ffi_header_check)

aether_add_cpp_test(dsp_sync_osc_test cpp/dsp_sync_osc_test.cpp)
target_compile_definitions(dsp_sync_osc_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(dsp_sync_osc_test PRIVATE aether_output aether_renderer aether_rhi ws2_32 windowscodecs ole32)
add_dependencies(dsp_sync_osc_test dsp_torture_worker)
set_tests_properties(dsp_sync_osc_test PROPERTIES TIMEOUT 120)

aether_add_cpp_test(output_spout_vcam_test cpp/output_spout_vcam_test.cpp)
target_compile_definitions(output_spout_vcam_test PRIVATE
    AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    AETHER_VCAM_DLL_PATH="$<TARGET_FILE:aether_vcam_ds_x64>"
)
target_link_libraries(output_spout_vcam_test PRIVATE aether_output aether_renderer aether_rhi d3d11 dxgi strmiids ole32 oleaut32 windowscodecs)
add_dependencies(output_spout_vcam_test aether_vcam_ds_x64)
set_tests_properties(output_spout_vcam_test PROPERTIES TIMEOUT 240)

add_executable(phase5_pipeline_m2p_test
    cpp/phase5_pipeline_m2p_test.cpp
    "${CMAKE_SOURCE_DIR}/src/app/pipeline_skeleton.cpp"
)
aether_apply_cxx_flags(phase5_pipeline_m2p_test)
target_compile_definitions(phase5_pipeline_m2p_test PRIVATE
    AETHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)
target_include_directories(phase5_pipeline_m2p_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp"
)
target_link_libraries(phase5_pipeline_m2p_test PRIVATE
    Catch2::Catch2WithMain
    aether_core
    aether_output
    aether_renderer
    aether_rhi
    aether_tracking
    aether_animation
    ws2_32
    windowscodecs
    ole32
)
aether_stage_ort_runtime(phase5_pipeline_m2p_test tests)
add_dependencies(phase5_pipeline_m2p_test ffi_header_check)
add_test(NAME phase5_pipeline_m2p_test COMMAND phase5_pipeline_m2p_test)
set_tests_properties(phase5_pipeline_m2p_test PROPERTIES TIMEOUT 240)

add_library(test_native_plugin SHARED
    cpp/test_native_plugin.cpp
)
aether_apply_cxx_flags(test_native_plugin)
target_include_directories(test_native_plugin PRIVATE
    "${CMAKE_SOURCE_DIR}/src/plugin_runner"
)

add_executable(native_plugin_runner_test
    cpp/native_plugin_runner_test.cpp
)
aether_apply_cxx_flags(native_plugin_runner_test)
target_link_libraries(native_plugin_runner_test PRIVATE
    Catch2::Catch2WithMain
    aether_native_plugin_manager
)
add_dependencies(native_plugin_runner_test test_native_plugin aether-plugin-runner)
add_test(NAME native_plugin_runner_test COMMAND native_plugin_runner_test)
set_tests_properties(native_plugin_runner_test PROPERTIES TIMEOUT 60)

# Phase 6: Studio UI & Budget Acceptance Tests
aether_add_cpp_test(studio_ui_test cpp/studio_ui_test.cpp)
target_link_libraries(studio_ui_test PRIVATE
    aether_ui
    aether_rhi
)
set_tests_properties(studio_ui_test PROPERTIES TIMEOUT 120)

# UE5 Blueprint node-graph wire editing: connect / disconnect / re-route / undo / JSON + real ImGui input
aether_add_cpp_test(node_graph_interaction_test cpp/node_graph_interaction_test.cpp)
target_link_libraries(node_graph_interaction_test PRIVATE
    aether_ui
)
set_tests_properties(node_graph_interaction_test PROPERTIES TIMEOUT 60)

# Headless Studio UI snapshots (CPU-rasterized ImGui draw data -> <build>/tests/ui_snapshots/*.png)
aether_add_cpp_test(studio_ui_snapshot_test cpp/studio_ui_snapshot_test.cpp)
target_compile_definitions(studio_ui_snapshot_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(studio_ui_snapshot_test PRIVATE aether_ui aether_rhi aether_renderer windowscodecs ole32)
set_tests_properties(studio_ui_snapshot_test PROPERTIES TIMEOUT 300)

# Epoch/Mark-Sweep Garbage Collector & DirectX 11 2.5D NPR Renderer Tests
aether_add_cpp_test(gc_and_dx11_renderer_test cpp/gc_and_dx11_renderer_test.cpp)
target_link_libraries(gc_and_dx11_renderer_test PRIVATE
    aether_renderer
    aether_rhi
)
set_tests_properties(gc_and_dx11_renderer_test PROPERTIES TIMEOUT 60)

# DirectX 11 render backend wired through the Vulkan frame graph + GC resource retirement (ADR-0016)
aether_add_cpp_test(dx11_backend_test cpp/dx11_backend_test.cpp)
target_compile_definitions(dx11_backend_test PRIVATE AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(dx11_backend_test PRIVATE aether_renderer aether_rhi windowscodecs ole32)
set_tests_properties(dx11_backend_test PROPERTIES TIMEOUT 180)

# Task 4: Real user VRM snapshot + tracking/spring steady-state verification
aether_add_cpp_test(real_vrm_verification_test cpp/real_vrm_verification_test.cpp)
target_compile_definitions(real_vrm_verification_test PRIVATE
    AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    AETHER_VRM_SNAPSHOT_DIR="${CMAKE_BINARY_DIR}/tests/ui_snapshots"
)
target_link_libraries(real_vrm_verification_test PRIVATE
    aether_renderer
    aether_rhi
    aether_animation
    aether_tracking
    windowscodecs
    ole32
)
set_tests_properties(real_vrm_verification_test PROPERTIES TIMEOUT 180)

# Tasks 1-8: DirectX 12 parity, Intel iGPU UMA, 3-Tier Lighting, Circadian & NPU Bounds Tests
aether_add_cpp_test(dx12_and_capabilities_test cpp/dx12_and_capabilities_test.cpp)
target_compile_definitions(dx12_and_capabilities_test PRIVATE
    AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    AETHER_VRM_SNAPSHOT_DIR="${CMAKE_BINARY_DIR}/tests/ui_snapshots"
)
target_link_libraries(dx12_and_capabilities_test PRIVATE
    aether_renderer
    aether_rhi
    aether_output
    aether_tracking
    windowscodecs
    ole32
)
set_tests_properties(dx12_and_capabilities_test PROPERTIES TIMEOUT 360)

# Lighting workspace tab: Vulkan + DX12 Stage/PiP pixel checks, single-row UI bounds, lighting_tab.png snapshot
aether_add_cpp_test(lighting_tab_verification_test cpp/lighting_tab_verification_test.cpp)
target_compile_definitions(lighting_tab_verification_test PRIVATE
    AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    AETHER_VRM_SNAPSHOT_DIR="${CMAKE_BINARY_DIR}/tests/ui_snapshots"
)
target_link_libraries(lighting_tab_verification_test PRIVATE
    aether_ui
    aether_renderer
    aether_rhi
    windowscodecs
    ole32
)
set_tests_properties(lighting_tab_verification_test PROPERTIES TIMEOUT 240)

# Phase 3: Non-destructive Node Material/Texture Override & Native PMX 2.0/2.1 Pipeline Tests
aether_add_cpp_test(phase3_material_and_pmx_test cpp/phase3_material_and_pmx_test.cpp)
target_compile_definitions(phase3_material_and_pmx_test PRIVATE
    AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(phase3_material_and_pmx_test PRIVATE
    aether_renderer
    aether_rhi
    aether_animation
    aether_ui
    windowscodecs
    ole32
)
set_tests_properties(phase3_material_and_pmx_test PROPERTIES TIMEOUT 180)

# Rev 3.1 (.alfx v1.0.1, .apr v1.0.0, ZIP, SHA-256, WIC 480x270 PNG, Legacy .aether migrator)
aether_add_cpp_test(project_archive_test cpp/project_archive_test.cpp)
target_compile_definitions(project_archive_test PRIVATE
    AETHER_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(project_archive_test PRIVATE
    aether_renderer
    aether_rhi
    aether_animation
    windowscodecs
    ole32
)
set_tests_properties(project_archive_test PROPERTIES TIMEOUT 120)

# Ensure Vulkan tests discover vcpkg's Release Khronos validation layer (avoids Debug-CRT HotPathGuard noise
# and enables CTest without requiring a separate system VULKAN_SDK installation).
set(_vcpkg_vk_layer_dir "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
if(EXISTS "${_vcpkg_vk_layer_dir}/VkLayer_khronos_validation.json")
    foreach(_vk_test IN ITEMS
        rhi_validation_smoke_test
        asset_upload_vk_test
        rhi_phase4_test
        renderer_npr_effects_test
        denoiser_and_rt_test
        golden_render_test
        dsp_sync_osc_test
        output_spout_vcam_test
        phase5_pipeline_m2p_test
        studio_ui_test
        studio_ui_snapshot_test
        dx11_backend_test
        real_vrm_verification_test
        dx12_and_capabilities_test
        lighting_tab_verification_test
    )
        if(TEST ${_vk_test})
            set_tests_properties(${_vk_test} PROPERTIES
                ENVIRONMENT_MODIFICATION "VK_LAYER_PATH=path_list_prepend:${_vcpkg_vk_layer_dir}"
            )
        endif()
    endforeach()
endif()
