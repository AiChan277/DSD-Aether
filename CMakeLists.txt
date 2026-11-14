cmake_minimum_required(VERSION 3.28)

# Auto-discover vcpkg toolchain if caller did not pass CMAKE_TOOLCHAIN_FILE explicitly.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain" FORCE)
    else()
        foreach(_vcpkg_cand
            "D:/VS2022/BuildTools/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
        )
            if(EXISTS "${_vcpkg_cand}")
                set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_cand}" CACHE FILEPATH "vcpkg toolchain" FORCE)
                break()
            endif()
        endforeach()
    endif()
endif()

project(AetherStudio VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# Serialize final link + vcpkg applocal steps and disable incremental linking to prevent Windows LNK1168 file-lock races.
set_property(GLOBAL PROPERTY JOB_POOLS link_pool=1)
set(CMAKE_JOB_POOL_LINK link_pool)
if(MSVC)
    foreach(_cfg_flag IN ITEMS
        CMAKE_EXE_LINKER_FLAGS_DEBUG
        CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
        CMAKE_SHARED_LINKER_FLAGS_DEBUG
        CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO
    )
        string(REGEX REPLACE "/INCREMENTAL(:[A-Za-z]+)?" "" ${_cfg_flag} "${${_cfg_flag}}")
        set(${_cfg_flag} "${${_cfg_flag}} /INCREMENTAL:NO")
    endforeach()
endif()

option(AETHER_WITH_OPTIX "NVIDIA OptiX denoiser (needs CUDA 12, Phase 4)" OFF)
option(AETHER_WITH_TENSORRT "Enable TensorRT EP probing (Phase 3)" ON)
option(AETHER_WITH_NDI "NDI 6 output (Phase 5)" ON)
option(AETHER_WITH_TRACY "Tracy profiler zones" ON)
option(AETHER_ENABLE_ASAN "AddressSanitizer (ci-validation)" OFF)
option(AETHER_ENABLE_HOT_PATH_GUARD "Enable per-thread hot-path heap allocation guard" OFF)
option(AETHER_ENABLE_TEST_HOOKS "Compile Rust test-only FFI hooks (never set in shipping release)" OFF)
option(AETHER_ENABLE_GPU_VALIDATION "Enable Vulkan GPU-assisted validation in ci-validation" OFF)
option(AETHER_ENABLE_LTO "Enable whole-program optimization (/GL /LTCG) for release" OFF)

include(CTest)
include(FetchContent)

# Reuse already-populated Corrosion source tree if present under build/_deps to avoid redundant git clones.
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/corrosion-src/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_CORROSION "${CMAKE_BINARY_DIR}/_deps/corrosion-src" CACHE PATH "" FORCE)
endif()

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.1
)
FetchContent_MakeAvailable(Corrosion)

if(AETHER_ENABLE_TEST_HOOKS)
    corrosion_import_crate(
        MANIFEST_PATH "${CMAKE_SOURCE_DIR}/Cargo.toml"
        CRATES aether-rust-host
        CRATE_TYPES staticlib
        FEATURES test-hooks
    )
else()
    corrosion_import_crate(
        MANIFEST_PATH "${CMAKE_SOURCE_DIR}/Cargo.toml"
        CRATES aether-rust-host
        CRATE_TYPES staticlib
    )
endif()

if(MSVC)
    # Rust staticlibs pull msvcrt.lib; suppress LNK4098 when linking against /MDd in Debug.
    target_link_options(aether_rust_host INTERFACE $<$<CONFIG:Debug>:/NODEFAULTLIB:msvcrt.lib>)
endif()

# Verify checked-in include/aether_ffi_contract.h matches cbindgen output on every build.
find_program(CBINDGEN_EXECUTABLE cbindgen REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ffi_check")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/ffi_check/aether_ffi_contract.h"
    COMMAND "${CBINDGEN_EXECUTABLE}"
        --config "${CMAKE_SOURCE_DIR}/cbindgen.toml"
        --crate aether-ffi
        --output "${CMAKE_BINARY_DIR}/ffi_check/aether_ffi_contract.h"
        "${CMAKE_SOURCE_DIR}/crates/aether-ffi"
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${CMAKE_BINARY_DIR}/ffi_check/aether_ffi_contract.h"
        "${CMAKE_SOURCE_DIR}/include/aether_ffi_contract.h"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/cbindgen.toml"
        "${CMAKE_SOURCE_DIR}/crates/aether-ffi/Cargo.toml"
        "${CMAKE_SOURCE_DIR}/crates/aether-ffi/src/lib.rs"
        "${CMAKE_SOURCE_DIR}/crates/aether-ffi/src/types.rs"
        "${CMAKE_SOURCE_DIR}/crates/aether-ffi/src/status.rs"
        "${CMAKE_SOURCE_DIR}/include/aether_ffi_contract.h"
    COMMENT "Verifying include/aether_ffi_contract.h against cbindgen output"
    VERBATIM
)
add_custom_target(ffi_header_check ALL
    DEPENDS "${CMAKE_BINARY_DIR}/ffi_check/aether_ffi_contract.h"
)

find_package(Vulkan 1.3 REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(imgui CONFIG REQUIRED)

# Phase 3: ONNX Runtime + OpenVINO + DirectML merged SDK (ADR-0007)
if(NOT DEFINED AETHER_ORT_SDK_DIR OR "${AETHER_ORT_SDK_DIR}" STREQUAL "")
    if(DEFINED ENV{AETHER_ORT_SDK_DIR} AND EXISTS "$ENV{AETHER_ORT_SDK_DIR}/include/onnxruntime_cxx_api.h")
        file(TO_CMAKE_PATH "$ENV{AETHER_ORT_SDK_DIR}" AETHER_ORT_SDK_DIR)
    elseif(DEFINED ENV{LOCALAPPDATA} AND EXISTS "$ENV{LOCALAPPDATA}/Aether/toolchains/ort-1.24.1/include/onnxruntime_cxx_api.h")
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Aether/toolchains/ort-1.24.1" AETHER_ORT_SDK_DIR)
    endif()
endif()
if(NOT DEFINED AETHER_ORT_SDK_DIR OR NOT EXISTS "${AETHER_ORT_SDK_DIR}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "ONNX Runtime SDK not found at '${AETHER_ORT_SDK_DIR}'. Run tools/fetch_phase3_deps.ps1 first.")
endif()
set(AETHER_ORT_SDK_DIR "${AETHER_ORT_SDK_DIR}" CACHE PATH "Merged ORT 1.24.1 + OpenVINO + DirectML SDK root")

add_library(aether_ort_sdk INTERFACE)
target_include_directories(aether_ort_sdk SYSTEM INTERFACE
    "${AETHER_ORT_SDK_DIR}/include"
)
target_link_libraries(aether_ort_sdk INTERFACE
    "${AETHER_ORT_SDK_DIR}/lib/onnxruntime.lib"
)

# Copy ORT/OpenVINO/TBB runtime DLLs once per binary output folder without parallel build races.
function(aether_stage_ort_runtime target_name dir_key)
    set(stage_target "aether_stage_ort_${dir_key}")
    set(stamp_file "${CMAKE_CURRENT_BINARY_DIR}/.ort_runtime_${dir_key}.stamp")
    if(NOT TARGET "${stage_target}")
        file(GLOB ORT_STAGE_INPUTS CONFIGURE_DEPENDS "${AETHER_ORT_SDK_DIR}/bin/*")
        add_custom_command(
            OUTPUT "${stamp_file}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${AETHER_ORT_SDK_DIR}/bin" "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp_file}"
            DEPENDS ${ORT_STAGE_INPUTS}
            COMMENT "Staging ORT/OpenVINO runtime DLLs into $<TARGET_FILE_DIR:${target_name}>"
            VERBATIM
        )
        add_custom_target("${stage_target}" DEPENDS "${stamp_file}")
    endif()
    add_dependencies(${target_name} "${stage_target}")
endfunction()

if(AETHER_WITH_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()

if(BUILD_TESTING)
    find_package(Catch2 CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

# Shared compiler/linker policy helper for all first-party Aether C++ targets.
function(aether_apply_cxx_flags target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            /MP
            /external:anglebrackets
            /external:W0
        )
        if(AETHER_ENABLE_ASAN)
            target_compile_options(${target_name} PRIVATE /fsanitize=address /Zi)
            target_compile_definitions(${target_name} PUBLIC
                _DISABLE_STRING_ANNOTATION
                _DISABLE_VECTOR_ANNOTATION
            )
        endif()
        if(AETHER_ENABLE_LTO)
            target_compile_options(${target_name} PRIVATE $<$<CONFIG:Release>:/GL>)
            target_link_options(${target_name} PRIVATE $<$<CONFIG:Release>:/LTCG /DEBUG>)
        endif()
    endif()
    target_compile_definitions(${target_name} PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        $<$<BOOL:${AETHER_WITH_TRACY}>:TRACY_ENABLE>
        $<$<BOOL:${AETHER_WITH_TRACY}>:AETHER_WITH_TRACY=1>
        $<$<BOOL:${AETHER_ENABLE_HOT_PATH_GUARD}>:AETHER_ENABLE_HOT_PATH_GUARD=1>
        $<$<BOOL:${AETHER_ENABLE_TEST_HOOKS}>:AETHER_ENABLE_TEST_HOOKS=1>
        $<$<BOOL:${AETHER_ENABLE_GPU_VALIDATION}>:AETHER_ENABLE_GPU_VALIDATION=1>
        $<$<OR:$<CONFIG:Debug>,$<BOOL:${AETHER_ENABLE_ASAN}>>:AETHER_ENABLE_VULKAN_VALIDATION=1>
    )
endfunction()

add_subdirectory(src)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
