#include "app/application.hpp"

#include "aether_ffi_contract.h"
#include "app/input_events.hpp"
#include "app/pipeline_skeleton.hpp"
#include "core/clock.hpp"
#include "core/cpu_topology.hpp"
#include "core/crash_handler.hpp"
#include "core/garbage_collector.hpp"
#include "core/hot_path_guard.hpp"
#include "core/hw_enum.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/spsc_ring.hpp"
#include "output/dsp_sync_bridge.hpp"
#include "output/output_config.hpp"
#include "output/output_ring.hpp"
#include "plugin_runner/native_plugin_manager.hpp"
#include "renderer/asset_upload/asset_job.hpp"
#include "renderer/dx11_bridge.hpp"
#include "renderer/npr_renderer.hpp"
#include "rhi/rhi_device.hpp"
#include "rhi/rhi_swapchain.hpp"
#include "ui/dev_shell.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/theme/precision_dark.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/workspace/studio_workspace.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
#include <implot.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <Windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>

namespace aether::app {

namespace {

#if defined(_WIN32)
ImGuiKey vk_to_imgui_key(WPARAM wparam) noexcept {
    switch (wparam) {
    case VK_TAB:
        return ImGuiKey_Tab;
    case VK_LEFT:
        return ImGuiKey_LeftArrow;
    case VK_RIGHT:
        return ImGuiKey_RightArrow;
    case VK_UP:
        return ImGuiKey_UpArrow;
    case VK_DOWN:
        return ImGuiKey_DownArrow;
    case VK_PRIOR:
        return ImGuiKey_PageUp;
    case VK_NEXT:
        return ImGuiKey_PageDown;
    case VK_HOME:
        return ImGuiKey_Home;
    case VK_END:
        return ImGuiKey_End;
    case VK_INSERT:
        return ImGuiKey_Insert;
    case VK_DELETE:
        return ImGuiKey_Delete;
    case VK_BACK:
        return ImGuiKey_Backspace;
    case VK_SPACE:
        return ImGuiKey_Space;
    case VK_RETURN:
        return ImGuiKey_Enter;
    case VK_ESCAPE:
        return ImGuiKey_Escape;
    case VK_CONTROL:
    case VK_LCONTROL:
        return ImGuiKey_LeftCtrl;
    case VK_RCONTROL:
        return ImGuiKey_RightCtrl;
    case VK_SHIFT:
    case VK_LSHIFT:
        return ImGuiKey_LeftShift;
    case VK_RSHIFT:
        return ImGuiKey_RightShift;
    case VK_MENU:
    case VK_LMENU:
        return ImGuiKey_LeftAlt;
    case VK_RMENU:
        return ImGuiKey_RightAlt;
    case VK_OEM_2:
        return ImGuiKey_Slash;
    case VK_OEM_MINUS:
        return ImGuiKey_Minus;
    case VK_OEM_PLUS:
        return ImGuiKey_Equal;
    case VK_OEM_PERIOD:
        return ImGuiKey_Period;
    case VK_OEM_COMMA:
        return ImGuiKey_Comma;
    default:
        if (wparam >= VK_F1 && wparam <= VK_F12) {
            return static_cast<ImGuiKey>(ImGuiKey_F1 + (wparam - VK_F1));
        }
        if (wparam >= '0' && wparam <= '9') {
            return static_cast<ImGuiKey>(ImGuiKey_0 + (wparam - '0'));
        }
        if (wparam >= 'A' && wparam <= 'Z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (wparam - 'A'));
        }
        return ImGuiKey_None;
    }
}

struct WindowSharedState {
    InputEventRing input_ring{};
    SpscRing<std::array<char, 512>, 8> dropped_files{};
    std::atomic<bool> quit_requested{false};
    std::atomic<bool> resize_pending{false};
    std::atomic<bool> dpi_changed_pending{false};
    std::atomic<uint32_t> pending_w{1440};
    std::atomic<uint32_t> pending_h{860};
    std::atomic<uint32_t> dpi{96};
};

uint32_t pack_win32_mods() noexcept {
    uint32_t mods = 0u;
    if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        mods |= 1u;
    }
    if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        mods |= 2u;
    }
    if ((::GetKeyState(VK_MENU) & 0x8000) != 0) {
        mods |= 4u;
    }
    return mods;
}

LRESULT CALLBACK aether_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    // NOLINTNEXTLINE(performance-no-int-to-ptr) -- Win32 GWLP_USERDATA ABI
    auto* shared = reinterpret_cast<WindowSharedState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (shared == nullptr) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_CLOSE:
        shared->quit_requested.store(true, std::memory_order_release);
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        shared->quit_requested.store(true, std::memory_order_release);
        ::PostQuitMessage(0);
        return 0;
    case WM_SIZE: {
        const uint32_t w = LOWORD(lp);
        const uint32_t h = HIWORD(lp);
        shared->pending_w.store(w, std::memory_order_release);
        shared->pending_h.store(h, std::memory_order_release);
        shared->resize_pending.store(true, std::memory_order_release);
        return 0;
    }
    case WM_DPICHANGED: {
        const uint32_t new_dpi = HIWORD(wp);
        shared->dpi.store(new_dpi, std::memory_order_release);
        shared->dpi_changed_pending.store(true, std::memory_order_release);
        // NOLINTNEXTLINE(performance-no-int-to-ptr) -- Win32 WM_DPICHANGED LPARAM ABI
        const auto* rect = reinterpret_cast<const RECT*>(lp);
        if (rect != nullptr) {
            ::SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT cr{};
        if (::GetClientRect(hwnd, &cr) != FALSE) {
            const uint32_t cw = static_cast<uint32_t>(std::max<LONG>(0L, cr.right - cr.left));
            const uint32_t ch = static_cast<uint32_t>(std::max<LONG>(0L, cr.bottom - cr.top));
            if (cw > 0U && ch > 0U) {
                shared->pending_w.store(cw, std::memory_order_release);
                shared->pending_h.store(ch, std::memory_order_release);
                shared->resize_pending.store(true, std::memory_order_release);
            }
        }
        return 0;
    }
    case WM_DROPFILES: {
        // NOLINTNEXTLINE(performance-no-int-to-ptr) -- Win32 WM_DROPFILES WPARAM ABI
        HDROP hdrop = reinterpret_cast<HDROP>(wp);
        wchar_t wpath[512]{};
        if (::DragQueryFileW(hdrop, 0u, wpath, 512u) > 0u) {
            std::array<char, 512> utf8_path{};
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8_path.data(),
                                                static_cast<int>(utf8_path.size()), nullptr, nullptr);
            if (n > 0) {
                (void)shared->dropped_files.try_push(utf8_path);
            }
        }
        ::DragFinish(hdrop);
        return 0;
    }
    case WM_MOUSEMOVE: {
        InputEvent ev{};
        ev.kind = InputEventKind::MouseMove;
        ev.x = static_cast<float>(static_cast<int16_t>(LOWORD(lp)));
        ev.y = static_cast<float>(static_cast<int16_t>(HIWORD(lp)));
        ev.timestamp_us = now_us();
        (void)shared->input_ring.push(ev);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        InputEvent ev{};
        ev.kind = InputEventKind::MouseButton;
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
            ev.code = 0;
        } else if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) {
            ev.code = 1;
        } else {
            ev.code = 2;
        }
        ev.button_or_state = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) ? 1U : 0U;
        ev.x = static_cast<float>(static_cast<int16_t>(LOWORD(lp)));
        ev.y = static_cast<float>(static_cast<int16_t>(HIWORD(lp)));
        ev.timestamp_us = now_us();
        (void)shared->input_ring.push(ev);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        InputEvent ev{};
        ev.kind = InputEventKind::MouseWheel;
        ev.y = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA);
        ev.timestamp_us = now_us();
        (void)shared->input_ring.push(ev);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        InputEvent ev{};
        ev.kind = InputEventKind::KeyDown;
        ev.code = static_cast<uint32_t>(wp);
        ev.button_or_state = static_cast<uint8_t>(pack_win32_mods());
        ev.timestamp_us = now_us();
        (void)shared->input_ring.push(ev);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        InputEvent ev{};
        ev.kind = InputEventKind::KeyUp;
        ev.code = static_cast<uint32_t>(wp);
        ev.button_or_state = static_cast<uint8_t>(pack_win32_mods());
        ev.timestamp_us = now_us();
        (void)shared->input_ring.push(ev);
        return 0;
    }
    case WM_CHAR: {
        InputEvent ev{};
        ev.kind = InputEventKind::CharInput;
        ev.code = static_cast<uint32_t>(wp);
        ev.timestamp_us = now_us();
        (void)shared->input_ring.push(ev);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        InputEvent ev{};
        ev.kind = InputEventKind::Focus;
        ev.button_or_state = (msg == WM_SETFOCUS) ? 1U : 0U;
        ev.timestamp_us = now_us();
        (void)shared->input_ring.push(ev);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
#endif

bool verify_ffi_contract_startup() noexcept {
    const uint32_t ver = aether_ffi_version();
    if (((ver >> 16) & 0xFFFFU) != AETHER_FFI_VERSION_MAJOR) {
        return false;
    }
    uintptr_t json_len = 0;
    const uint8_t* json_ptr = aether_ffi_layout_table_json(&json_len);
    return (json_ptr != nullptr && json_len > 64);
}

const char* source_format_name(uint32_t fmt) noexcept {
    switch (fmt) {
    case AETHER_SOURCE_FORMAT_VRM0:
        return "VRM 0.x";
    case AETHER_SOURCE_FORMAT_VRM1:
        return "VRM 1.0";
    case AETHER_SOURCE_FORMAT_GLTF:
        return "glTF/GLB";
    case AETHER_SOURCE_FORMAT_FBX:
        return "FBX";
    case AETHER_SOURCE_FORMAT_PMX:
        return "PMX";
    case AETHER_SOURCE_FORMAT_PMD:
        return "PMD";
    default:
        return "Unknown";
    }
}

std::string find_default_interactive_avatar() {
    const char* candidates[3] = {
        "tests/data/reference_avatar.vrm",
        "../tests/data/reference_avatar.vrm",
        "../../tests/data/reference_avatar.vrm",
    };
    for (const char* p : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            return std::string(p);
        }
    }
    return {};
}

animation::RestSkeletonMetrics build_rest_metrics_from_avatar(const renderer::GpuAvatar& avatar) noexcept {
    animation::RestSkeletonMetrics rm = animation::build_default_humanoid_rest_metrics();
    if (avatar.joint_rest_world_pos.size() != avatar.joints.size()) {
        return rm;
    }

    std::vector<animation::Quat> joint_world_rot(avatar.joints.size(), animation::Quat::identity());
    std::vector<uint8_t> rot_resolved(avatar.joints.size(), 0U);
    auto resolve_joint_world_rot = [&](auto& self, size_t idx, int depth) -> animation::Quat {
        if (idx >= avatar.joints.size() || depth > 128) {
            return animation::Quat::identity();
        }
        if (rot_resolved[idx] != 0U) {
            return joint_world_rot[idx];
        }
        const auto& j = avatar.joints[idx];
        const float* br = j.bind_rotation_quat;
        const float q2 = br[0] * br[0] + br[1] * br[1] + br[2] * br[2] + br[3] * br[3];
        const animation::Quat q_local =
            (q2 > 1e-6f) ? animation::normalize(animation::Quat{br[0], br[1], br[2], br[3]})
                         : animation::Quat::identity();
        animation::Quat q_parent = animation::Quat::identity();
        if (j.parent_index >= 0 && static_cast<size_t>(j.parent_index) < avatar.joints.size()) {
            q_parent = self(self, static_cast<size_t>(j.parent_index), depth + 1);
        }
        joint_world_rot[idx] = animation::normalize(q_parent * q_local);
        rot_resolved[idx] = 1U;
        return joint_world_rot[idx];
    };
    for (size_t i = 0; i < avatar.joints.size(); ++i) {
        (void)resolve_joint_world_rot(resolve_joint_world_rot, i, 0);
    }

    std::array<animation::Vec3, AETHER_HUMANOID_BONE_COUNT> world{};
    const uint16_t hips_j = avatar.desc.humanoid_to_joint[AETHER_BONE_HIPS];
    if (hips_j < avatar.joint_rest_world_pos.size()) {
        const auto& hp = avatar.joint_rest_world_pos[hips_j];
        world[AETHER_BONE_HIPS] = {hp[0], hp[1], hp[2]};
        rm.rest_world_rotations[AETHER_BONE_HIPS] =
            (hips_j < joint_world_rot.size()) ? joint_world_rot[hips_j] : animation::Quat::identity();
    } else {
        world[AETHER_BONE_HIPS] = rm.rest_local_positions[AETHER_BONE_HIPS];
        rm.rest_world_rotations[AETHER_BONE_HIPS] = animation::Quat::identity();
    }
    rm.rest_local_rotations[AETHER_BONE_HIPS] = rm.rest_world_rotations[AETHER_BONE_HIPS];

    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        const uint16_t j = avatar.desc.humanoid_to_joint[b];
        if (j < avatar.joint_rest_world_pos.size()) {
            const auto& jp = avatar.joint_rest_world_pos[j];
            world[b] = {jp[0], jp[1], jp[2]};
            rm.rest_world_rotations[b] =
                (j < joint_world_rot.size()) ? joint_world_rot[j] : rm.rest_world_rotations[pi];
        } else {
            world[b] = world[pi] + rm.rest_local_positions[b];
            rm.rest_world_rotations[b] = rm.rest_world_rotations[pi];
        }
        rm.rest_local_rotations[b] =
            animation::normalize(animation::conjugate(rm.rest_world_rotations[pi]) * rm.rest_world_rotations[b]);
    }

    rm.rest_local_positions[AETHER_BONE_HIPS] = world[AETHER_BONE_HIPS];
    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        rm.rest_local_positions[b] = world[b] - world[pi];
        rm.bone_lengths[b] = std::max(animation::length(rm.rest_local_positions[b]), 0.02f);
    }

    uint32_t custom_cnt = 0;
    for (const auto& col : avatar.colliders) {
        if (custom_cnt >= rm.custom_colliders.size()) {
            break;
        }
        uint16_t hb = AETHER_BONE_HIPS;
        std::array<float, 3> jr{0.0f, 0.0f, 0.0f};
        animation::Quat col_j_rot = animation::Quat::identity();
        if (col.joint_index < avatar.joints.size()) {
            jr = avatar.joint_rest_world_pos[col.joint_index];
            col_j_rot = joint_world_rot[col.joint_index];
            uint32_t j = col.joint_index;
            for (size_t steps = 0; steps < avatar.joints.size() && j < avatar.joints.size(); ++steps) {
                const uint16_t candidate = avatar.joints[j].humanoid_bone;
                if (candidate < AETHER_HUMANOID_BONE_COUNT) {
                    hb = candidate;
                    break;
                }
                const int32_t p_idx = avatar.joints[j].parent_index;
                if (p_idx < 0 || static_cast<size_t>(p_idx) >= avatar.joints.size()) {
                    break;
                }
                j = static_cast<uint32_t>(p_idx);
            }
        }
        auto& dst = rm.custom_colliders[custom_cnt++];
        dst.attached_bone = static_cast<int32_t>(hb);
        dst.shape = (col.shape_kind == AETHER_COLLIDER_SHAPE_CAPSULE) ? 1U : 0U;
        dst.radius = std::clamp(col.radius, 0.01f, 0.50f);
        const animation::Vec3 j_pos{jr[0], jr[1], jr[2]};
        const animation::Vec3 a_ws =
            j_pos + animation::rotate_vec(col_j_rot, {col.offset[0], col.offset[1], col.offset[2]});
        const animation::Vec3 b_ws =
            j_pos + animation::rotate_vec(col_j_rot, {col.tail[0], col.tail[1], col.tail[2]});
        const animation::Quat inv_hb_rot = animation::conjugate(rm.rest_world_rotations[hb]);
        dst.local_a = animation::rotate_vec(inv_hb_rot, a_ws - world[hb]);
        dst.local_b = animation::rotate_vec(inv_hb_rot, b_ws - world[hb]);
    }
    rm.custom_collider_count = custom_cnt;
    rm.valid = true;
    return rm;
}

void pump_t3_inputs(WindowSharedState& shared, ui::DevShellInputEcho& echo, ImGuiIO& io) noexcept {
    InputEvent ev{};
    while (shared.input_ring.pop(ev)) {
        ++echo.total_events;
        switch (ev.kind) {
        case InputEventKind::MouseMove:
            echo.cursor_x = ev.x;
            echo.cursor_y = ev.y;
            io.AddMousePosEvent(ev.x, ev.y);
            break;
        case InputEventKind::MouseButton:
            io.AddMousePosEvent(ev.x, ev.y);
            io.AddMouseButtonEvent(static_cast<int>(ev.code), ev.button_or_state != 0);
            break;
        case InputEventKind::MouseWheel:
            io.AddMouseWheelEvent(0.0f, ev.y);
            break;
        case InputEventKind::KeyDown:
            echo.last_key_vk = ev.code;
            io.AddKeyEvent(ImGuiMod_Ctrl, (ev.button_or_state & 1u) != 0u);
            io.AddKeyEvent(ImGuiMod_Shift, (ev.button_or_state & 2u) != 0u);
            io.AddKeyEvent(ImGuiMod_Alt, (ev.button_or_state & 4u) != 0u);
            if (const ImGuiKey key = vk_to_imgui_key(ev.code); key != ImGuiKey_None) {
                io.AddKeyEvent(key, true);
            }
            break;
        case InputEventKind::KeyUp:
            io.AddKeyEvent(ImGuiMod_Ctrl, (ev.button_or_state & 1u) != 0u);
            io.AddKeyEvent(ImGuiMod_Shift, (ev.button_or_state & 2u) != 0u);
            io.AddKeyEvent(ImGuiMod_Alt, (ev.button_or_state & 4u) != 0u);
            if (const ImGuiKey key = vk_to_imgui_key(ev.code); key != ImGuiKey_None) {
                io.AddKeyEvent(key, false);
            }
            break;
        case InputEventKind::CharInput:
            if (ev.code > 0 && ev.code < 0x10000U) {
                io.AddInputCharacter(ev.code);
            }
            break;
        case InputEventKind::Focus:
            io.AddFocusEvent(ev.button_or_state != 0);
            break;
        default:
            break;
        }
    }
    echo.dropped_moves = shared.input_ring.dropped_moves();
}

enum class FileDialogKind : uint8_t {
    None = 0,
    OpenAvatar = 1,
    OpenProject = 2,
    SaveProject = 3,
};

struct AsyncFileDialogJob {
    std::atomic<bool> busy{false};
    std::atomic<bool> ready{false};
    FileDialogKind active_kind{FileDialogKind::None};
    std::array<char, 512> result_utf8{};
    std::thread worker{};

    ~AsyncFileDialogJob() { join(); }

    void join() {
        if (worker.joinable()) {
            worker.join();
        }
    }

    void launch(HWND owner, FileDialogKind kind) {
        if (busy.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        join();
        ready.store(false, std::memory_order_release);
        active_kind = kind;
        result_utf8[0] = '\0';

        worker = std::thread([this, owner, kind]() {
            const HRESULT hr_co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            IFileDialog* dlg = nullptr;
            const CLSID clsid = (kind == FileDialogKind::SaveProject) ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
            if (SUCCEEDED(::CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) && dlg != nullptr) {
                if (kind == FileDialogKind::OpenAvatar) {
                    static constexpr COMDLG_FILTERSPEC kFilters[2] = {
                        {L"3D Avatar Models (*.vrm;*.glb;*.gltf;*.fbx;*.pmx;*.pmd)",
                         L"*.vrm;*.glb;*.gltf;*.fbx;*.pmx;*.pmd"},
                        {L"All Files (*.*)", L"*.*"},
                    };
                    (void)dlg->SetFileTypes(2, kFilters);
                    (void)dlg->SetTitle(L"Load Avatar Model");
                } else {
                    static constexpr COMDLG_FILTERSPEC kFilters[2] = {
                        {L"AETHER Project (*.aether;*.json)", L"*.aether;*.json"},
                        {L"All Files (*.*)", L"*.*"},
                    };
                    (void)dlg->SetFileTypes(2, kFilters);
                    if (kind == FileDialogKind::SaveProject) {
                        (void)dlg->SetDefaultExtension(L"aether");
                        (void)dlg->SetFileName(L"studio_project.aether");
                        (void)dlg->SetTitle(L"Save AETHER Studio Project");
                    } else {
                        (void)dlg->SetTitle(L"Open AETHER Studio Project");
                    }
                }
                if (SUCCEEDED(dlg->Show(owner))) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dlg->GetResult(&item)) && item != nullptr) {
                        PWSTR psz = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz != nullptr) {
                            (void)::WideCharToMultiByte(CP_UTF8, 0, psz, -1, result_utf8.data(),
                                                        static_cast<int>(result_utf8.size()), nullptr, nullptr);
                            ::CoTaskMemFree(psz);
                        }
                        item->Release();
                    }
                }
                dlg->Release();
            }
            if (SUCCEEDED(hr_co)) {
                ::CoUninitialize();
            }
            ready.store(result_utf8[0] != '\0', std::memory_order_release);
            busy.store(false, std::memory_order_release);
        });
    }

    [[nodiscard]] bool take_result(FileDialogKind& out_kind, std::array<char, 512>& out_path) noexcept {
        if (!ready.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        out_kind = active_kind;
        out_path = result_utf8;
        return out_path[0] != '\0';
    }
};

void post_host_cmd(AetherHostHandle host, uint32_t kind, uint64_t target_id = 0, float param_f32 = 0.0f,
                   uint32_t param_u32 = 0, const char* payload = nullptr) noexcept {
    if (host == nullptr) {
        return;
    }
    AetherHostCommand cmd{};
    cmd.struct_size = static_cast<uint32_t>(sizeof(cmd));
    cmd.kind = kind;
    cmd.target_id = target_id;
    cmd.param_f32 = param_f32;
    cmd.param_u32 = param_u32;
    cmd.payload_utf8 = payload;
    cmd.payload_len = (payload != nullptr) ? std::strlen(payload) : 0ULL;
    (void)aether_host_post_command(host, &cmd);
}

void push_avatar_scene_items_to_ui(const renderer::GpuAvatar& avatar, ui::StudioWorkspace& ws) {
    std::vector<ui::OutlinerBoneInfo> bones;
    bones.reserve(avatar.joints.size());
    for (size_t i = 0; i < avatar.joints.size(); ++i) {
        const auto& j = avatar.joints[i];
        ui::OutlinerBoneInfo info{};
        info.bone_id = static_cast<uint32_t>(i);
        const std::string name = avatar.string_slice(j.name_offset, j.name_len);
        std::snprintf(info.name.data(), info.name.size(), "%s", name.empty() ? "Joint" : name.c_str());
        info.humanoid_mapped = (j.humanoid_bone < 55U);
        info.confidence = info.humanoid_mapped ? avatar.desc.bone_match_score[j.humanoid_bone] : 1.0f;
        bones.push_back(info);
    }

    std::vector<ui::OutlinerMaterialInfo> mats;
    mats.reserve(avatar.materials.size());
    for (size_t i = 0; i < avatar.materials.size(); ++i) {
        ui::OutlinerMaterialInfo m{};
        m.index = static_cast<uint32_t>(i);
        const uint32_t off = avatar.materials[i].name_offset;
        if (off < avatar.strings.size()) {
            std::snprintf(m.name.data(), m.name.size(), "%s", avatar.strings.c_str() + off);
        }
        if (m.name[0] == '\0') {
            std::snprintf(m.name.data(), m.name.size(), "Material_%zu", i);
        }
        mats.push_back(m);
    }

    ws.sync_scene_items(bones, mats, {});
}

void sync_plugins_snapshot(AetherHostHandle plugin_host, const native_plugin::NativePluginManager* native_mgr,
                           std::array<char, 65536>& json_scratch, std::vector<ui::LoadedPluginVm>& out_plugins) {
    out_plugins.clear();
    if (plugin_host != nullptr) {
        uintptr_t out_len = 0;
        if (aether_host_copy_project_json(plugin_host, reinterpret_cast<uint8_t*>(json_scratch.data()),
                                          json_scratch.size() - 1U, &out_len) == AETHER_OK &&
            out_len > 0U) {
            json_scratch[std::min(out_len, json_scratch.size() - 1U)] = '\0';
            try {
                const auto root = nlohmann::json::parse(std::string_view(json_scratch.data(), out_len));
                if (root.contains("plugins") && root["plugins"].is_array()) {
                    for (const auto& p : root["plugins"]) {
                        ui::LoadedPluginVm vm{};
                        const std::string pid = p.value("id", "");
                        const std::string pname = p.value("name", pid);
                        const std::string pstatus = p.value("status", "Active");
                        const std::string perr = p.contains("last_error") && p["last_error"].is_string()
                                                     ? p["last_error"].get<std::string>()
                                                     : "";
                        std::snprintf(vm.id.data(), vm.id.size(), "%s", pid.c_str());
                        std::snprintf(vm.name.data(), vm.name.size(), "%s", pname.c_str());
                        std::snprintf(vm.kind.data(), vm.kind.size(), "WASM");
                        std::snprintf(vm.status_text.data(), vm.status_text.size(), "%s%s%s", pstatus.c_str(),
                                      perr.empty() ? "" : ": ", perr.c_str());
                        vm.enabled = (pstatus == "Active");
                        vm.faulted = (pstatus == "Isolated");
                        vm.permanently_disabled = false;
                        vm.native_slot = -1;
                        if (p.contains("capabilities") && p["capabilities"].is_array()) {
                            for (const auto& c : p["capabilities"]) {
                                const std::string cs = c.is_string() ? c.get<std::string>() : "";
                                if (cs == "ReadTracking") {
                                    vm.capability_mask |= 1u << 0;
                                } else if (cs == "WriteParams") {
                                    vm.capability_mask |= 1u << 1;
                                } else if (cs == "EmitEvents") {
                                    vm.capability_mask |= 1u << 2;
                                } else if (cs == "UiPanel") {
                                    vm.capability_mask |= 1u << 3;
                                }
                            }
                        }
                        out_plugins.push_back(vm);
                    }
                }
            } catch (...) {
                AETHER_LOG_WARN("plugin", "Failed to parse plugin snapshot JSON");
            }
        }
    }
    if (native_mgr != nullptr) {
        for (uint32_t slot = 0; slot < native_plugin::NativePluginManager::kMaxSlots; ++slot) {
            const auto h = native_mgr->health(slot);
            if (h.state == native_plugin::PluginState::Stopped && h.crash_count == 0 && !h.permanently_disabled &&
                h.desc.name[0] == '\0') {
                continue;
            }
            ui::LoadedPluginVm vm{};
            const char* pname = (h.desc.name[0] != '\0') ? h.desc.name : "NativePlugin";
            std::snprintf(vm.id.data(), vm.id.size(), "native_slot_%u", slot);
            std::snprintf(vm.name.data(), vm.name.size(), "%s", pname);
            std::snprintf(vm.kind.data(), vm.kind.size(), "Native");
            std::snprintf(vm.status_text.data(), vm.status_text.size(), "Restarts %u | Crashes %u%s%s", h.restart_count,
                          h.crash_count, h.last_error.empty() ? "" : " | ", h.last_error.c_str());
            vm.enabled = (h.state == native_plugin::PluginState::Running);
            vm.faulted = (h.state == native_plugin::PluginState::Restarting || h.permanently_disabled);
            vm.permanently_disabled = h.permanently_disabled;
            vm.restart_count = h.restart_count;
            vm.trap_count = h.crash_count + h.hang_kill_count;
            vm.native_slot = static_cast<int32_t>(slot);
            out_plugins.push_back(vm);
        }
    }
}

void update_host_consent_details(AetherHostHandle plugin_host, uint64_t source_id_hash,
                                 std::array<char, 65536>& json_scratch, ui::StudioWorkspace& ws) {
    if (plugin_host == nullptr) {
        return;
    }
    uintptr_t out_len = 0;
    if (aether_host_copy_project_json(plugin_host, reinterpret_cast<uint8_t*>(json_scratch.data()),
                                      json_scratch.size() - 1U, &out_len) != AETHER_OK ||
        out_len == 0U) {
        return;
    }
    json_scratch[std::min(out_len, json_scratch.size() - 1U)] = '\0';
    try {
        const auto root = nlohmann::json::parse(std::string_view(json_scratch.data(), out_len));
        if (!root.contains("plugins") || !root["plugins"].is_array()) {
            return;
        }
        for (const auto& p : root["plugins"]) {
            if (p.value("id_hash", uint64_t{0}) == source_id_hash) {
                const std::string pid = p.value("id", "");
                const std::string pname = p.value("name", pid);
                uint32_t mask = 0;
                if (p.contains("capabilities") && p["capabilities"].is_array()) {
                    for (const auto& c : p["capabilities"]) {
                        const std::string cs = c.is_string() ? c.get<std::string>() : "";
                        if (cs == "ReadTracking") {
                            mask |= 1u << 0;
                        } else if (cs == "WriteParams") {
                            mask |= 1u << 1;
                        } else if (cs == "EmitEvents") {
                            mask |= 1u << 2;
                        } else if (cs == "UiPanel") {
                            mask |= 1u << 3;
                        }
                    }
                }
                ws.request_plugin_consent(pid.c_str(), pname.c_str(), "", mask);
                return;
            }
        }
    } catch (...) {
        AETHER_LOG_WARN("plugin", "Failed to parse plugin consent JSON");
    }
}

void apply_project_json_to_ui(const std::string& json_str, ui::StudioWorkspace& ws) {
    try {
        const auto root = nlohmann::json::parse(json_str);
        if (root.contains("graph") && root["graph"].is_object()) {
            ws.set_graph_json_from_host(root["graph"].dump());
        } else if (root.contains("nodes") && root["nodes"].is_array()) {
            ws.set_graph_json_from_host(json_str);
        }
        if (root.contains("project") && root["project"].is_object()) {
            const auto& proj = root["project"];
            for (const char* subkey : {"params", "render"}) {
                if (proj.contains(subkey) && proj[subkey].is_object()) {
                    for (const auto& [k, v] : proj[subkey].items()) {
                        if (v.is_number()) {
                            (void)ws.params().set_by_id(k, v.get<double>());
                        } else if (v.is_boolean()) {
                            (void)ws.params().set_by_id(k, v.get<bool>() ? 1.0 : 0.0);
                        }
                    }
                }
            }
        }
    } catch (...) {
        AETHER_LOG_WARN("plugin", "Failed to apply migrated project JSON");
    }
}

/** GC finalizer for avatars retired on swap: runs once the frame timeline passes the retire epoch. */
void finalize_retired_avatar(void* handle, uint64_t /*user_tag*/, uint64_t /*byte_size*/) noexcept {
    delete static_cast<renderer::GpuAvatar*>(handle);
}

uint64_t completed_frame_timeline(const rhi::RhiDevice& rhi) noexcept {
    uint64_t v = 0;
    if (rhi.device() != VK_NULL_HANDLE && rhi.frame_timeline() != VK_NULL_HANDLE) {
        (void)vkGetSemaphoreCounterValue(rhi.device(), rhi.frame_timeline(), &v);
    }
    return v;
}

bool ends_with_icase(std::string_view hay, std::string_view suffix) noexcept {
    if (hay.size() < suffix.size()) {
        return false;
    }
    const size_t off = hay.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[off + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool parse_luid_hex(std::string_view hex, std::array<uint8_t, 8>& out_luid) noexcept {
    if (hex.size() != 16U) {
        return false;
    }
    auto hex_nibble = [](char ch, uint8_t& v) noexcept -> bool {
        if (ch >= '0' && ch <= '9') {
            v = static_cast<uint8_t>(ch - '0');
            return true;
        }
        if (ch >= 'a' && ch <= 'f') {
            v = static_cast<uint8_t>(10 + (ch - 'a'));
            return true;
        }
        if (ch >= 'A' && ch <= 'F') {
            v = static_cast<uint8_t>(10 + (ch - 'A'));
            return true;
        }
        return false;
    };
    for (size_t i = 0; i < 8U; ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!hex_nibble(hex[i * 2U], hi) || !hex_nibble(hex[i * 2U + 1U], lo)) {
            return false;
        }
        out_luid[i] = static_cast<uint8_t>((hi << 4U) | lo);
    }
    return true;
}

} // namespace

int run_application(const AppLaunchOptions& opts) {
#if defined(_WIN32)
    (void)::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    (void)::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ImGui_ImplWin32_EnableDpiAwareness();
#endif

    HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);

    const RuntimePaths paths = discover_runtime_paths();
    CrashHandler::install(paths.crash_dir);

    AetherRuntimeConfig rt_cfg{};
    rt_cfg.struct_size = static_cast<uint32_t>(sizeof(rt_cfg));
    rt_cfg.log_level = 1;
    rt_cfg.worker_threads = 0;
    rt_cfg.flags = 0;
    rt_cfg.app_data_dir_utf8 = paths.app_data_dir.c_str();
    rt_cfg.cache_dir_utf8 = paths.cache_dir.c_str();

    if (aether_runtime_init(&rt_cfg) != AETHER_OK) {
        CrashHandler::shutdown();
        return 1;
    }

    const bool ffi_ok = verify_ffi_contract_startup();
    if (!ffi_ok) {
        AETHER_LOG_FATAL("app", "FFI ABI version/layout verification failed");
        aether_runtime_shutdown();
        CrashHandler::shutdown();
        return 2;
    }

    const HardwareInventory hw = enumerate_hardware();
    (void)apply_thread_placement(ThreadRole::MainT0, hw.cpu);

#if defined(_WIN32)
    ImGui_ImplWin32_EnableDpiAwareness();

    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    WindowSharedState shared{};

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = &aether_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW wide resource ID
    wc.lpszClassName = L"AetherStudioMainWindow";
    if (::RegisterClassExW(&wc) == 0) {
        aether_runtime_shutdown();
        CrashHandler::shutdown();
        return 3;
    }

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"AETHER-2.5D Studio", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 1440, 860, nullptr, nullptr, hinst, nullptr);
    if (hwnd == nullptr) {
        ::UnregisterClassW(wc.lpszClassName, hinst);
        aether_runtime_shutdown();
        CrashHandler::shutdown();
        return 4;
    }

    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&shared));

    // Dark window chrome title bar on Windows 10/11 (DWMWA_USE_IMMUTABLE_DARK_MODE = 20).
    const BOOL dark_title = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20, &dark_title, sizeof(dark_title));

    shared.dpi.store(::GetDpiForWindow(hwnd), std::memory_order_release);
    ::DragAcceptFiles(hwnd, TRUE);
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    ui::AppSettings boot_settings{};
    ui::ParamStore boot_params{};
    if (opts.smoke_frames == 0) {
        const std::filesystem::path cfg_dir =
            paths.app_data_dir.empty() ? ui::default_config_dir() : std::filesystem::path(paths.app_data_dir);
        (void)ui::load_app_settings_toml(cfg_dir / "settings.toml", boot_settings, boot_params);
        if (boot_params.get_int(ui::ParamId::TrackingEpMode) == 4) {
            boot_params.set(ui::ParamId::TrackingEpMode, 0.0);
        }
    }

    rhi::RhiDeviceConfig rhi_cfg{};
#if defined(AETHER_ENABLE_VULKAN_VALIDATION) && AETHER_ENABLE_VULKAN_VALIDATION
    rhi_cfg.enable_validation = true;
    rhi_cfg.require_validation = opts.require_validation;
#else
    rhi_cfg.enable_validation = opts.require_validation;
    rhi_cfg.require_validation = opts.require_validation;
#endif
#if defined(AETHER_ENABLE_GPU_VALIDATION) && AETHER_ENABLE_GPU_VALIDATION
    rhi_cfg.enable_gpu_assisted = true;
#endif
    rhi_cfg.headless = false;
    rhi_cfg.inventory = &hw;
    rhi_cfg.pipeline_cache_dir = std::filesystem::path(paths.cache_dir) / "vk_pipeline_cache";

    std::vector<rhi::GpuAdapterDescriptor> gpu_adapters{};
    if (auto enum_res = rhi::RhiDevice::enumerate_vulkan_devices(rhi_cfg); enum_res) {
        gpu_adapters = std::move(enum_res.value());
    }
    if (opts.smoke_frames == 0) {
        std::array<uint8_t, 8> parsed_luid{};
        if (parse_luid_hex(boot_settings.preferred_gpu_luid, parsed_luid)) {
            rhi_cfg.preferred_luid = parsed_luid;
            rhi_cfg.use_preferred_luid = true;
        } else if (!boot_settings.preferred_gpu_name.empty()) {
            for (const auto& ad : gpu_adapters) {
                if (ad.device_name == boot_settings.preferred_gpu_name) {
                    if (ad.luid_valid) {
                        rhi_cfg.preferred_luid = ad.luid;
                        rhi_cfg.use_preferred_luid = true;
                    } else {
                        rhi_cfg.preferred_vendor_id = ad.vendor_id;
                        rhi_cfg.preferred_device_id = ad.device_id;
                    }
                    break;
                }
            }
        }
    }

    auto rhi_res = rhi::RhiDevice::create(rhi_cfg);
    if (!rhi_res) {
        AETHER_LOG_FATAL("rhi", "Failed to create RhiDevice: %s", rhi_res.error().c_str());
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, hinst);
        aether_runtime_shutdown();
        CrashHandler::shutdown();
        return 5;
    }
    rhi::RhiDevice* rhi = rhi_res.value();

    output::OutputConfig init_out_cfg{};
    init_out_cfg.resolution = output::OutputResolutionPreset::Fhd1080;
    init_out_cfg.width = output::output_preset_width(init_out_cfg.resolution);
    init_out_cfg.height = output::output_preset_height(init_out_cfg.resolution);
    init_out_cfg.fps = 60u;
    init_out_cfg.premul_alpha = true;
    init_out_cfg.spout_enabled = true;
    init_out_cfg.vcam_enabled = true;
    init_out_cfg.vcam_skip_when_no_reader = true;
    init_out_cfg.ndi_enabled = false;
    init_out_cfg.osc_enabled = true;

    auto pipeline = std::make_unique<PipelineSkeleton>();
    PipelineConfig pipe_cfg{};
    pipe_cfg.enable_live_tracking = true;
    pipe_cfg.use_synthetic_camera =
        (opts.smoke_frames > 0) ? true : boot_params.get_bool(ui::ParamId::TrackingSyntheticCamera);
    pipe_cfg.camera_active = (opts.smoke_frames == 0);
    pipe_cfg.camera_fps = static_cast<uint32_t>(std::max(30, boot_params.get_int(ui::ParamId::TrackingCameraFps)));
    pipe_cfg.preferred_camera_link = boot_settings.preferred_camera_link;
    pipe_cfg.preferred_ep =
        ui::tracking_ep_mode_to_router_override(boot_params.get_int(ui::ParamId::TrackingEpMode));
    pipe_cfg.ov_cache_dir = std::filesystem::path(paths.cache_dir) / "ov_cache";
    pipe_cfg.output_device = rhi;
    pipe_cfg.output_cfg = init_out_cfg;

    const std::string plugins_dir_utf8 = (std::filesystem::path(paths.app_data_dir) / "plugins").string();
    (void)ensure_directory_exists(plugins_dir_utf8);
    AetherHostHandle plugin_host = nullptr;
    {
        AetherHostConfig hcfg{};
        hcfg.struct_size = static_cast<uint32_t>(sizeof(hcfg));
        hcfg.worker_threads = 2u;
        hcfg.osc_in_port = 9002u;
        hcfg.osc_out_port = 9001u;
        hcfg.ws_port = 19190u;
        hcfg.flags = 0u;
        hcfg.plugins_dir_utf8 = plugins_dir_utf8.c_str();
        if (aether_host_create(&hcfg, &plugin_host) != AETHER_OK) {
            AETHER_LOG_WARN("plugin", "aether_host_create returned error; running without Rust plugin host");
            plugin_host = nullptr;
        }
    }
    pipeline->set_plugin_host(plugin_host);

    if (!pipeline->start(hw, pipe_cfg)) {
        if (plugin_host != nullptr) {
            aether_host_destroy(plugin_host);
            plugin_host = nullptr;
        }
        rhi->destroy();
        delete rhi;
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, hinst);
        aether_runtime_shutdown();
        CrashHandler::shutdown();
        return 6;
    }

    auto native_mgr = std::make_unique<native_plugin::NativePluginManager>();
    AsyncFileDialogJob dialog_job{};
    std::atomic<int> render_exit_code{0};

    // Dedicated T3 (Render + UI) thread owning Swapchain, ImGui, and Vulkan presentation.
    std::thread t3_thread([&]() {
#if defined(_WIN32)
        (void)::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
        aether_set_thread_role(AETHER_THREAD_ROLE_UI);
        (void)apply_thread_placement(ThreadRole::RenderT3, hw.cpu);

        RECT client_rect{};
        ::GetClientRect(hwnd, &client_rect);
        const uint32_t init_w =
            static_cast<uint32_t>(client_rect.right > client_rect.left ? (client_rect.right - client_rect.left) : 1440);
        const uint32_t init_h =
            static_cast<uint32_t>(client_rect.bottom > client_rect.top ? (client_rect.bottom - client_rect.top) : 860);

        rhi::RhiSwapchain swapchain;
        if (auto sw_res = swapchain.init(*rhi, hinst, hwnd, init_w, init_h); !sw_res) {
            AETHER_LOG_FATAL("rhi", "Swapchain init failed: %s", sw_res.error().c_str());
            shared.quit_requested.store(true, std::memory_order_release);
            render_exit_code.store(7, std::memory_order_release);
            return;
        }
        if (opts.smoke_frames == 0 && boot_params.get_bool(ui::ParamId::RenderVsync)) {
            (void)swapchain.set_vsync(true);
        }

        ui::ImGuiArenaAllocator imgui_arena;
        ui::install_imgui_arena(imgui_arena);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        // §12.14: Multi-viewport disabled in v1.0 cleanly.

        ui::apply_precision_dark_style(ImGui::GetStyle());

        ui::DevShell shell;
        const float dpi_scale = static_cast<float>(shared.dpi.load(std::memory_order_acquire)) / 96.0f;
        shell.init(paths, hw, dpi_scale, opts.avatar_path.empty() ? nullptr : opts.avatar_path.c_str(),
                   opts.smoke_frames == 0);
        if (plugin_host != nullptr) {
            const std::string init_graph = shell.workspace().node_canvas().serialize_graph_json();
            post_host_cmd(plugin_host, AETHER_HOST_CMD_SET_GRAPH_JSON, 0, 0.0f, 0, init_graph.c_str());
        }

        ImGui_ImplWin32_Init(hwnd);

        auto init_imgui_vulkan = [&]() {
            VkFormat sw_format = swapchain.format();
            VkPipelineRenderingCreateInfoKHR dyn_ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
            dyn_ri.colorAttachmentCount = 1;
            dyn_ri.pColorAttachmentFormats = &sw_format;

            ImGui_ImplVulkan_InitInfo vk_info{};
            vk_info.ApiVersion = VK_API_VERSION_1_3;
            vk_info.Instance = rhi->instance();
            vk_info.PhysicalDevice = rhi->physical_device();
            vk_info.Device = rhi->device();
            vk_info.QueueFamily = rhi->graphics_family();
            vk_info.Queue = rhi->graphics_queue();
            vk_info.DescriptorPoolSize = 16;
            vk_info.MinImageCount = 2;
            vk_info.ImageCount = std::max(2U, swapchain.image_count());
            vk_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            vk_info.UseDynamicRendering = true;
            vk_info.PipelineRenderingCreateInfo = dyn_ri;
            vk_info.MinAllocationSize = 1024ULL * 1024ULL;
            ImGui_ImplVulkan_Init(&vk_info);
            ImGui_ImplVulkan_CreateFontsTexture();
        };
        init_imgui_vulkan();

        VkSampler vp_sampler = VK_NULL_HANDLE;
        auto create_vp_sampler = [&]() -> bool {
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = 1.0f;
            return vkCreateSampler(rhi->device(), &sci, nullptr, &vp_sampler) == VK_SUCCESS;
        };
        auto destroy_vp_sampler = [&]() {
            if (vp_sampler != VK_NULL_HANDLE && rhi->device() != VK_NULL_HANDLE) {
                vkDestroySampler(rhi->device(), vp_sampler, nullptr);
            }
            vp_sampler = VK_NULL_HANDLE;
        };
        if (!create_vp_sampler()) {
            AETHER_LOG_FATAL("renderer", "Failed to create viewport VkSampler");
            shared.quit_requested.store(true, std::memory_order_release);
            render_exit_code.store(11, std::memory_order_release);
            shell.shutdown();
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImPlot::DestroyContext();
            ImGui::DestroyContext();
            swapchain.destroy();
            imgui_arena.shutdown();
            aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
            return;
        }

        output::OutputConfig active_out_cfg = init_out_cfg;
        renderer::RendererConfig render_cfg{};
        render_cfg.width = 960;
        render_cfg.height = 640;
        render_cfg.output_width = active_out_cfg.width;
        render_cfg.output_height = active_out_cfg.height;
        render_cfg.enable_rt = rhi->rt_supported();
#if defined(AETHER_ENABLE_VULKAN_VALIDATION) && AETHER_ENABLE_VULKAN_VALIDATION
        render_cfg.allow_disk_shader_reload = true;
#else
        render_cfg.allow_disk_shader_reload = false;
#endif
        render_cfg.denoiser = renderer::DenoiserKind::Auto;
        render_cfg.debug_view = renderer::ShadingDebugView::Final;
        render_cfg.premul_alpha = active_out_cfg.premul_alpha;

        renderer::NprRenderer npr_renderer;
        if (auto nr_rc = npr_renderer.init(*rhi, render_cfg); !nr_rc) {
            AETHER_LOG_FATAL("renderer", "NprRenderer init failed: %s", nr_rc.error().c_str());
            shared.quit_requested.store(true, std::memory_order_release);
            render_exit_code.store(11, std::memory_order_release);
            destroy_vp_sampler();
            shell.shutdown();
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImPlot::DestroyContext();
            ImGui::DestroyContext();
            swapchain.destroy();
            imgui_arena.shutdown();
            aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
            return;
        }

        VkDescriptorSet vp_tex_set = VK_NULL_HANDLE;
        VkDescriptorSet orb_tex_set = VK_NULL_HANDLE;
        VkDescriptorSet preview_tex_set = VK_NULL_HANDLE;
        rhi::RhiImage preview_img{};
        std::array<rhi::RhiBuffer, rhi::RhiSwapchain::kFramesInFlight> preview_staging{};
        bool preview_img_initialized = false;
        uint64_t preview_uploaded_seq = 0ULL;
        auto preview_cache = std::make_unique<tracking::LiveCameraPreviewFrame>();

        auto create_preview_resources = [&]() {
            preview_img_initialized = false;
            preview_uploaded_seq = 0ULL;
            if (vp_sampler == VK_NULL_HANDLE || rhi->device() == VK_NULL_HANDLE) {
                return;
            }
            constexpr uint32_t pw = tracking::LiveCameraPreviewFrame::kPreviewWidth;
            constexpr uint32_t ph = tracking::LiveCameraPreviewFrame::kPreviewHeight;
            constexpr VkDeviceSize pbytes = static_cast<VkDeviceSize>(pw) * ph * 4ULL;
            if (auto img_res = rhi->create_image_2d(pw, ph, VK_FORMAT_R8G8B8A8_UNORM,
                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                    VK_IMAGE_ASPECT_COLOR_BIT, false);
                img_res) {
                preview_img = img_res.value();
                preview_tex_set = ImGui_ImplVulkan_AddTexture(vp_sampler, preview_img.view,
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            } else {
                AETHER_LOG_WARN("renderer", "Failed to create live camera preview image: %s", img_res.error().c_str());
            }
            for (auto& sb : preview_staging) {
                if (auto buf_res = rhi->create_buffer(pbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                          VMA_ALLOCATION_CREATE_MAPPED_BIT);
                    buf_res) {
                    sb = buf_res.value();
                } else {
                    AETHER_LOG_WARN("renderer", "Failed to create live preview staging buffer: %s",
                                    buf_res.error().c_str());
                }
            }
        };
        auto destroy_preview_resources = [&](bool remove_imgui_descriptor) {
            if (remove_imgui_descriptor && preview_tex_set != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(preview_tex_set);
            }
            preview_tex_set = VK_NULL_HANDLE;
            rhi->destroy_image(preview_img);
            for (auto& sb : preview_staging) {
                rhi->destroy_buffer(sb);
            }
            preview_img_initialized = false;
            preview_uploaded_seq = 0ULL;
        };

        auto refresh_vp_descriptors = [&]() {
            rhi->wait_idle();
            if (vp_tex_set != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(vp_tex_set);
                vp_tex_set = VK_NULL_HANDLE;
            }
            if (orb_tex_set != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(orb_tex_set);
                orb_tex_set = VK_NULL_HANDLE;
            }
            const auto out = npr_renderer.graph().outputs();
            if (vp_sampler != VK_NULL_HANDLE) {
                if (out.viewport_view != VK_NULL_HANDLE) {
                    vp_tex_set = ImGui_ImplVulkan_AddTexture(vp_sampler, out.viewport_view,
                                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                if (out.orbit_orb_view != VK_NULL_HANDLE) {
                    orb_tex_set = ImGui_ImplVulkan_AddTexture(vp_sampler, out.orbit_orb_view,
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
        };
        refresh_vp_descriptors();
        create_preview_resources();

        const auto& tok = ui::tokens();
        const float clear_rgba[4] = {tok.bg_app.x, tok.bg_app.y, tok.bg_app.z, 1.0f};

        renderer::AssetJob asset_job;
        std::unique_ptr<renderer::GpuAvatar> current_avatar;
        std::string last_loaded_avatar_path = opts.avatar_path;
        renderer::PreviewCamera camera{};

        // Engine garbage collector (ADR-0015/0016): GPU objects replaced while frames are in flight are retired
        // against the frame timeline and destroyed only after the GPU has passed that epoch.
        GarbageCollector gc;
        renderer::Dx11Bridge dx11_bridge;
        dx11_bridge.attach(*rhi, gc);
        renderer::RenderBackend active_backend = renderer::RenderBackend::Vulkan;
        uint8_t requested_backend_ui = 0xFFU; // last UI value seen; 0xFF = none yet
        std::array<char, 96> backend_detail{};
        backend_detail[0] = '\0';
        if (opts.render_backend == "dx11" || opts.render_backend == "directx11") {
            active_backend = renderer::RenderBackend::DirectX11;
        }
        auto retire_avatar = [&](std::unique_ptr<renderer::GpuAvatar> old) {
            if (old == nullptr) {
                return;
            }
            renderer::GpuAvatar* raw = old.release();
            const uint64_t bytes = static_cast<uint64_t>(raw->requirements.vertex_count) * 64ULL;
            if (!gc.retire(raw, bytes, npr_renderer.frame_timeline_value(), &finalize_retired_avatar, 0x41564154ULL)) {
                rhi->wait_idle();
                delete raw;
            }
        };
        auto configure_dx11_for_avatar = [&]() -> bool {
            if (active_backend != renderer::RenderBackend::DirectX11 || current_avatar == nullptr) {
                return false;
            }
            const auto outs = npr_renderer.graph().outputs();
            const uint32_t ow = outs.output_width != 0U ? outs.output_width : active_out_cfg.width;
            const uint32_t oh = outs.output_height != 0U ? outs.output_height : active_out_cfg.height;
            const auto v = static_cast<uint32_t>(current_avatar->rest_vertices.size());
            const auto i = static_cast<uint32_t>(current_avatar->rest_indices.size());
            if (auto rc = dx11_bridge.configure(ow, oh, v, i); !rc) {
                AETHER_LOG_ERROR("renderer", "DirectX 11 backend unavailable: %s", rc.error().c_str());
                return false;
            }
            return true;
        };

        ui::DevShellViewportActions vp_actions{};
        vp_actions.rt_enabled = render_cfg.enable_rt;
        vp_actions.key_azimuth_deg = npr_renderer.lighting().lights[0].orbit_azimuth_deg;
        vp_actions.key_elevation_deg = npr_renderer.lighting().lights[0].orbit_elevation_deg;
        vp_actions.key_intensity = npr_renderer.lighting().lights[0].intensity;
        vp_actions.fill_intensity = npr_renderer.lighting().lights[1].intensity;
        vp_actions.rim_intensity = npr_renderer.lighting().lights[2].intensity;
        vp_actions.live2d_flatness = npr_renderer.lighting().global_live2d_flatness;
        vp_actions.bloom_intensity = npr_renderer.lighting().bloom_intensity;
        vp_actions.outline_scale = npr_renderer.lighting().outline_width_scale;
        vp_actions.face_normal_flatten = npr_renderer.lighting().face_normal_flatten;
        vp_actions.cel_band_sharpness = npr_renderer.lighting().cel_band_sharpness;
        vp_actions.shadow_warm_cool_shift = npr_renderer.lighting().shadow_warm_cool_shift;
        vp_actions.iro_tsuke_strength = npr_renderer.lighting().iro_tsuke_strength;
        vp_actions.crease_strength = npr_renderer.lighting().crease_strength;
        vp_actions.hair_highlight_intensity = npr_renderer.lighting().hair_highlight_intensity;
        vp_actions.rim_step_strength = npr_renderer.lighting().rim_step_strength;
        if (opts.smoke_frames == 0) {
            vp_actions.target_fps_mode = static_cast<uint8_t>(boot_params.get_int(ui::ParamId::RenderTargetFps));
            vp_actions.vsync_enabled = boot_params.get_bool(ui::ParamId::RenderVsync);
            vp_actions.background_eco_mode = boot_params.get_bool(ui::ParamId::RenderBackgroundEco);
            vp_actions.render_resolution_scale =
                std::clamp(static_cast<float>(boot_params.get_int(ui::ParamId::RenderResolutionScalePct)) * 0.01f,
                           0.50f, 2.00f);
            vp_actions.aa_mode = static_cast<uint8_t>(boot_params.get_int(ui::ParamId::RenderAaMode));
        } else {
            vp_actions.target_fps_mode = 3U; // Uncapped during smoke tests
        }

        ui::DevShellInputEcho input_echo{};
        uint64_t rendered_frames = 0;
        uint64_t last_late_latch_us = 0;
        uint64_t graph_frame_idx = 1ULL;
        uint64_t last_out_submit_us = 0ULL;
        uint64_t last_present_stamp_us = 0ULL;
        uint64_t next_pace_us = 0ULL;
        float present_fps_ema = 0.0f;
        uint64_t vram_usage_mb_cached = 0ULL;
        uint64_t vram_budget_mb_cached = 0ULL;
        std::array<bool, rhi::RhiSwapchain::kFramesInFlight> slot_submitted{};
        std::array<uint64_t, rhi::RhiSwapchain::kFramesInFlight> slot_last_graph_idx{};
        bool baked_face_sdf_applied = false;
        uint32_t device_recovery_count = 0;
        std::array<char, 128> last_recovery_msg{};
        HighResTimer phase_timer;
        TelemetrySnapshot telem_cache{};
        TrackingHudSnapshot track_hud{};
        ui::StudioActions studio_actions{};
        ui::StudioHostStatus host_status{};
        host_status.host_alive = (plugin_host != nullptr);
        std::string project_path{};
        std::vector<ui::LoadedPluginVm> plugin_vms{};
        plugin_vms.reserve(32);
        std::array<AetherHostEvent, 32> host_events{};
        std::array<char, 65536> project_json_buf{};
        bool on_air = false;
        bool ws_enabled = false;
        bool tracking_paused = false;
        bool need_plugin_sync = true;

        auto push_project_snapshot_to_host = [&]() {
            if (plugin_host == nullptr) {
                return;
            }
            try {
                nlohmann::json upd;
                upd["avatar_path"] = last_loaded_avatar_path;
                nlohmann::json render_obj = nlohmann::json::object();
                for (const auto& spec : ui::all_param_specs()) {
                    if (spec.type == ui::ParamType::Bool) {
                        render_obj[spec.id] = shell.workspace().params().get_bool(spec.pid);
                    } else {
                        render_obj[spec.id] = shell.workspace().params().get(spec.pid);
                    }
                }
                upd["render"] = std::move(render_obj);
                upd["graph"] = nlohmann::json::parse(shell.workspace().node_canvas().serialize_graph_json());
                const std::string upd_str = upd.dump();
                post_host_cmd(plugin_host, AETHER_HOST_CMD_PROJECT_UPDATE_JSON, 0, 0.0f, 0, upd_str.c_str());
            } catch (...) {
                AETHER_LOG_WARN("plugin", "Failed to serialize project update JSON");
            }
        };

        auto recreate_vulkan_stack = [&](bool is_device_lost, const char* toast_title, const char* toast_msg) -> bool {
            if (!is_device_lost) {
                pipe_cfg.use_synthetic_camera = track_hud.using_synthetic_camera;
                pipe_cfg.camera_active = track_hud.camera_active;
                if (track_hud.camera_fps > 0U) {
                    pipe_cfg.camera_fps = track_hud.camera_fps;
                }
                if (track_hud.camera_symlink[0] != '\0') {
                    pipe_cfg.preferred_camera_link = track_hud.camera_symlink;
                }
                pipe_cfg.preferred_ep = track_hud.selected_ep_override;
                pipe_cfg.output_cfg = active_out_cfg;
                (void)pipeline->wait_output_slots_drained(250u);
                rhi->wait_idle();
            } else {
                AETHER_LOG_WARN("rhi", "Attempting automatic VK_ERROR_DEVICE_LOST recovery...");
            }
            pipeline->stop();
            vp_tex_set = VK_NULL_HANDLE;
            orb_tex_set = VK_NULL_HANDLE;
            destroy_preview_resources(false);
            asset_job.reset();
            dx11_bridge.release();
            current_avatar.reset();
            (void)gc.flush_all_epochs();
            destroy_vp_sampler();
            ImGui_ImplVulkan_Shutdown();
            swapchain.destroy();

            double recovery_ms = 0.0;
            if (is_device_lost) {
                const auto rec = npr_renderer.recover_from_device_lost(*rhi, rhi_cfg, render_cfg, nullptr, paths.crash_dir);
                if (!rec.recovered) {
                    AETHER_LOG_FATAL("rhi", "Device recovery failed: %s", rec.error_message.c_str());
                    return false;
                }
                recovery_ms = rec.recovery_ms;
            } else {
                const uint64_t t0_us = now_us();
                npr_renderer.destroy();
                if (auto dev_res = rhi->recreate_after_device_lost(rhi_cfg); !dev_res) {
                    AETHER_LOG_ERROR("rhi", "GPU switch failed during RhiDevice recreate: %s",
                                     dev_res.error().c_str());
                    return false;
                }
                render_cfg.enable_rt = rhi->rt_supported();
                if (auto init_res = npr_renderer.init(*rhi, render_cfg); !init_res) {
                    AETHER_LOG_FATAL("renderer", "NprRenderer re-init after GPU switch failed: %s",
                                     init_res.error().c_str());
                    return false;
                }
                recovery_ms = static_cast<double>(now_us() - t0_us) * 0.001;
            }
            if (auto enum_res = rhi::RhiDevice::enumerate_vulkan_devices(rhi_cfg); enum_res) {
                gpu_adapters = std::move(enum_res.value());
            }

            pipeline->set_plugin_host(plugin_host);
            pipeline->set_tracking_paused(tracking_paused);
            if (!pipeline->start(hw, pipe_cfg)) {
                AETHER_LOG_FATAL("rhi", "Pipeline restart after GPU recreate failed");
                return false;
            }
            RECT cr{};
            ::GetClientRect(hwnd, &cr);
            const uint32_t sw_w = static_cast<uint32_t>(std::max<LONG>(cr.right - cr.left, 64));
            const uint32_t sw_h = static_cast<uint32_t>(std::max<LONG>(cr.bottom - cr.top, 64));
            if (auto sw_r = swapchain.init(*rhi, hinst, hwnd, sw_w, sw_h); !sw_r) {
                AETHER_LOG_FATAL("rhi", "Swapchain recreate failed: %s", sw_r.error().c_str());
                return false;
            }
            if (vp_actions.vsync_enabled) {
                (void)swapchain.set_vsync(true);
            }
            init_imgui_vulkan();
            if (!create_vp_sampler()) {
                return false;
            }
            refresh_vp_descriptors();
            create_preview_resources();
            slot_submitted.fill(false);
            slot_last_graph_idx.fill(0ULL);
            baked_face_sdf_applied = false;
            vp_actions.lighting_dirty = true;
            vp_actions.output_config_dirty = true;
            requested_backend_ui = 0xFFU;
            next_pace_us = 0ULL;
            vram_usage_mb_cached = 0ULL;
            vram_budget_mb_cached = 0ULL;
            if (is_device_lost) {
                ++device_recovery_count;
                std::snprintf(last_recovery_msg.data(), last_recovery_msg.size(), "Recovered in %.1f ms", recovery_ms);
            } else {
                AETHER_LOG_INFO("rhi", "Switched Vulkan adapter to %s in %.1f ms", rhi->gpu_info().device_name.c_str(),
                                recovery_ms);
            }
            shell.workspace().push_toast(
                ui::ToastSeverity::Success, toast_title,
                toast_msg != nullptr ? toast_msg : last_recovery_msg.data(), static_cast<double>(now_us()) * 1e-6);
            if (!last_loaded_avatar_path.empty()) {
                asset_job.start_load(*rhi, last_loaded_avatar_path);
            }
            return true;
        };

        auto attempt_device_recovery = [&]() -> bool {
            return recreate_vulkan_stack(true, "GPU Device Recovered", nullptr);
        };

        const bool wait_smoke_avatar = (opts.smoke_frames > 0 && !opts.avatar_path.empty());
        if (!opts.avatar_path.empty()) {
            asset_job.start_load(*rhi, opts.avatar_path);
        } else if (opts.smoke_frames == 0) {
            if (std::string default_vrm = find_default_interactive_avatar(); !default_vrm.empty()) {
                last_loaded_avatar_path = default_vrm;
                asset_job.start_load(*rhi, std::move(default_vrm));
            }
        }

        while (!shared.quit_requested.load(std::memory_order_acquire)) {
            const uint64_t loop_begin_us = now_us();
            const double frame_now_s = static_cast<double>(loop_begin_us) * 1e-6;

            // Rebuild fonts/style cleanly BETWEEN frames when DPI or UI Scale changed (`WM_DPICHANGED` / Preferences).
            if (shared.dpi_changed_pending.exchange(false, std::memory_order_acq_rel) ||
                studio_actions.request_reapply_theme_or_dpi) {
                studio_actions.request_reapply_theme_or_dpi = false;
                rhi->wait_idle();
                const float new_dpi_scale = static_cast<float>(shared.dpi.load(std::memory_order_acquire)) / 96.0f;
                shell.workspace().on_dpi_or_theme_changed(new_dpi_scale);
                ImGui_ImplVulkan_DestroyFontsTexture();
                ImGui_ImplVulkan_CreateFontsTexture();
                io.FontGlobalScale = 1.0f;
            }

            // Drain Win32 WM_DROPFILES paths (.aether project or avatar mesh).
            {
                std::array<char, 512> drop_buf{};
                while (shared.dropped_files.try_pop(drop_buf)) {
                    const std::string_view drop_sv(drop_buf.data());
                    if (ends_with_icase(drop_sv, ".aether") || ends_with_icase(drop_sv, ".json")) {
                        if (on_air) {
                            shell.workspace().push_toast(ui::ToastSeverity::Warning, "Blocked While ON AIR",
                                                         "Cannot load project while live.", frame_now_s);
                        } else if (plugin_host != nullptr) {
                            project_path = std::string(drop_sv);
                            (void)aether_host_load_project(plugin_host, project_path.c_str());
                        }
                    } else if (ends_with_icase(drop_sv, ".vrm") || ends_with_icase(drop_sv, ".glb") ||
                               ends_with_icase(drop_sv, ".gltf") || ends_with_icase(drop_sv, ".fbx") ||
                               ends_with_icase(drop_sv, ".pmx") || ends_with_icase(drop_sv, ".pmd")) {
                        last_loaded_avatar_path = std::string(drop_sv);
                        asset_job.start_load(*rhi, last_loaded_avatar_path);
                    } else {
                        shell.workspace().push_toast(ui::ToastSeverity::Warning, "Unsupported Drop File",
                                                     drop_buf.data(), frame_now_s);
                    }
                }
            }

            // Handle async native COM file dialog results cleanly without blocking T3.
            {
                FileDialogKind dlg_kind = FileDialogKind::None;
                std::array<char, 512> dlg_path{};
                if (dialog_job.take_result(dlg_kind, dlg_path)) {
                    if (dlg_kind == FileDialogKind::OpenAvatar) {
                        last_loaded_avatar_path = dlg_path.data();
                        asset_job.start_load(*rhi, last_loaded_avatar_path);
                    } else if (dlg_kind == FileDialogKind::OpenProject && plugin_host != nullptr) {
                        project_path = dlg_path.data();
                        (void)aether_host_load_project(plugin_host, project_path.c_str());
                    } else if (dlg_kind == FileDialogKind::SaveProject && plugin_host != nullptr) {
                        project_path = dlg_path.data();
                        push_project_snapshot_to_host();
                        (void)aether_host_save_project(plugin_host, project_path.c_str());
                    }
                }
            }

            // Poll Rust plugin host events, stats, and refresh plugin VM list outside AETHER_HOT_PATH_SCOPE.
            if (plugin_host != nullptr) {
                uint32_t evt_count = 0;
                if (aether_host_poll_events(plugin_host, host_events.data(), static_cast<uint32_t>(host_events.size()),
                                            &evt_count) == AETHER_OK) {
                    for (uint32_t ei = 0; ei < evt_count; ++ei) {
                        const auto& ev = host_events[ei];
                        const char* msg = reinterpret_cast<const char*>(ev.message_utf8);
                        switch (ev.kind) {
                        case AETHER_HOST_EVT_GRAPH_COMPILED:
                            host_status.error_node_id = 0;
                            host_status.graph_error_msg[0] = '\0';
                            break;
                        case AETHER_HOST_EVT_GRAPH_COMPILE_ERR:
                            host_status.error_node_id = ev.source_id;
                            std::snprintf(host_status.graph_error_msg.data(), host_status.graph_error_msg.size(), "%s",
                                          msg);
                            shell.workspace().push_toast(ui::ToastSeverity::Error, "Node Graph Error", msg,
                                                         frame_now_s);
                            break;
                        case AETHER_HOST_EVT_PROJECT_LOADED:
                            if (ev.status == AETHER_OK) {
                                uintptr_t plen = 0;
                                if (aether_host_copy_project_json(plugin_host,
                                                                  reinterpret_cast<uint8_t*>(project_json_buf.data()),
                                                                  project_json_buf.size() - 1U, &plen) == AETHER_OK &&
                                    plen > 0U) {
                                    project_json_buf[std::min(plen, project_json_buf.size() - 1U)] = '\0';
                                    apply_project_json_to_ui(std::string(project_json_buf.data(), plen),
                                                             shell.workspace());
                                }
                                shell.workspace().undo_stack().mark_saved();
                                shell.workspace().push_toast(ui::ToastSeverity::Success, "Project Loaded", msg,
                                                             frame_now_s);
                            } else {
                                shell.workspace().push_toast(ui::ToastSeverity::Error, "Project Load Failed", msg,
                                                             frame_now_s);
                            }
                            need_plugin_sync = true;
                            break;
                        case AETHER_HOST_EVT_PROJECT_SAVED:
                            if (ev.status == AETHER_OK) {
                                shell.workspace().undo_stack().mark_saved();
                                shell.workspace().push_toast(ui::ToastSeverity::Success, "Project Saved", msg,
                                                             frame_now_s);
                            } else {
                                shell.workspace().push_toast(ui::ToastSeverity::Error, "Project Save Failed", msg,
                                                             frame_now_s);
                            }
                            break;
                        case AETHER_HOST_EVT_PLUGIN_LOADED:
                            need_plugin_sync = true;
                            shell.workspace().push_toast(ui::ToastSeverity::Success, "Plugin Loaded", msg, frame_now_s);
                            break;
                        case AETHER_HOST_EVT_PLUGIN_CONSENT_REQ:
                            need_plugin_sync = true;
                            update_host_consent_details(plugin_host, ev.source_id, project_json_buf, shell.workspace());
                            break;
                        case AETHER_HOST_EVT_PLUGIN_TRAP:
                            need_plugin_sync = true;
                            shell.workspace().push_toast(ui::ToastSeverity::Error, "Plugin Isolated", msg, frame_now_s);
                            break;
                        case AETHER_HOST_EVT_GRAPH_BUDGET_WARN:
                            shell.workspace().push_toast(ui::ToastSeverity::Warning, "Graph Eval > 100 us", msg,
                                                         frame_now_s);
                            break;
                        default:
                            break;
                        }
                    }
                }

                AetherGraphPlanStats pstats{};
                pstats.struct_size = static_cast<uint32_t>(sizeof(pstats));
                if (aether_host_get_plan_stats(plugin_host, &pstats) == AETHER_OK) {
                    host_status.plan_stats = pstats;
                    host_status.compiled_node_count = pstats.active_node_count;
                    host_status.last_compile_ms = pstats.last_compile_ms;
                    host_status.last_eval_us = pstats.last_eval_us;
                    host_status.eval_budget_warn = (pstats.budget_warn != 0u);
                    host_status.ws_enabled = (pstats.ws_enabled != 0u);
                    if (pstats.error_node_id != AETHER_INVALID_INDEX) {
                        host_status.error_node_id = pstats.error_node_id;
                    }
                }
                host_status.telemetry.struct_size = static_cast<uint32_t>(sizeof(AetherGraphTelemetryReadout));
                (void)aether_host_get_telemetry_readout(plugin_host, &host_status.telemetry);
            }
            if (need_plugin_sync || (rendered_frames % 60ULL) == 0ULL) {
                need_plugin_sync = false;
                sync_plugins_snapshot(plugin_host, native_mgr.get(), project_json_buf, plugin_vms);
            }

            // Execute one-shot StudioActions emitted by the previous UI frame cleanly outside hot path.
            if (studio_actions.request_set_on_air) {
                studio_actions.request_set_on_air = false;
                on_air = studio_actions.target_on_air;
                host_status.on_air = on_air;
                post_host_cmd(plugin_host, AETHER_HOST_CMD_SET_ON_AIR, 0, 0.0f, on_air ? 1u : 0u);
                shell.workspace().push_toast(on_air ? ui::ToastSeverity::Warning : ui::ToastSeverity::Info,
                                             on_air ? "ON AIR Active" : "ON AIR Disengaged",
                                             on_air ? "Destructive structural edits locked." : "All controls unlocked.",
                                             frame_now_s);
            }
            if (studio_actions.request_toggle_ws) {
                studio_actions.request_toggle_ws = false;
                ws_enabled = studio_actions.target_ws_enabled;
                host_status.ws_enabled = ws_enabled;
                post_host_cmd(plugin_host, AETHER_HOST_CMD_SET_WS_ENABLED, 0, 0.0f, ws_enabled ? 1u : 0u);
            }
            if (studio_actions.request_pause_tracking) {
                studio_actions.request_pause_tracking = false;
                tracking_paused = !tracking_paused;
                host_status.tracking_paused = tracking_paused;
                pipeline->set_tracking_paused(tracking_paused);
                shell.workspace().push_toast(ui::ToastSeverity::Info,
                                             tracking_paused ? "Tracking Paused" : "Tracking Resumed", "", frame_now_s);
            }
            if (studio_actions.request_apply_graph_json || studio_actions.request_recompile_graph_now) {
                studio_actions.request_apply_graph_json = false;
                studio_actions.request_recompile_graph_now = false;
                if (studio_actions.pending_graph_json.empty()) {
                    studio_actions.pending_graph_json = shell.workspace().node_canvas().serialize_graph_json();
                }
                post_host_cmd(plugin_host, AETHER_HOST_CMD_SET_GRAPH_JSON, 0, 0.0f, 0,
                              studio_actions.pending_graph_json.c_str());
                studio_actions.pending_graph_json.clear();
            }
            if (studio_actions.expression_dirty_mask != 0) {
                // Hermite-ramped quick-expression weights (UI interpolates; host applies as overrides).
                for (size_t slot = 0; slot < ui::StudioActions::kQuickExpressionSlots; ++slot) {
                    const int32_t graph_slot = ui::StudioActions::kQuickExpressionGraphSlot[slot];
                    if ((studio_actions.expression_dirty_mask & (1u << slot)) != 0 && graph_slot >= 0) {
                        post_host_cmd(plugin_host, AETHER_HOST_CMD_TRIGGER_EXPRESSION,
                                      static_cast<uint64_t>(graph_slot), studio_actions.expression_weights[slot]);
                    }
                }
                studio_actions.expression_dirty_mask = 0;
            }
            if (studio_actions.request_plugin_consent_reply) {
                studio_actions.request_plugin_consent_reply = false;
                post_host_cmd(plugin_host, AETHER_HOST_CMD_GRANT_PLUGIN_CONSENT, 0, 0.0f,
                              studio_actions.plugin_consent_granted ? 1u : 0u, studio_actions.plugin_consent_id.data());
                need_plugin_sync = true;
            }
            if (studio_actions.request_reload_plugins) {
                studio_actions.request_reload_plugins = false;
                post_host_cmd(plugin_host, AETHER_HOST_CMD_RELOAD_PLUGINS);
                need_plugin_sync = true;
            }
            if (studio_actions.request_reset_native_plugin) {
                studio_actions.request_reset_native_plugin = false;
                if (studio_actions.reset_native_slot >= 0 && native_mgr != nullptr) {
                    (void)native_mgr->reset_and_restart(static_cast<uint32_t>(studio_actions.reset_native_slot));
                    need_plugin_sync = true;
                }
            }
            if (studio_actions.request_force_device_lost) {
                studio_actions.request_force_device_lost = false;
                rhi->mark_device_lost();
                if (!attempt_device_recovery()) {
                    render_exit_code.store(9, std::memory_order_release);
                    break;
                }
                continue;
            }
            if (studio_actions.request_switch_gpu) {
                studio_actions.request_switch_gpu = false;
                if (on_air) {
                    shell.workspace().push_toast(ui::ToastSeverity::Warning, "Blocked While ON AIR",
                                                 "Cannot switch rendering GPU while live.", frame_now_s);
                } else {
                    const rhi::RhiDeviceConfig prev_rhi_cfg = rhi_cfg;
                    rhi_cfg.preferred_luid = studio_actions.target_gpu_luid;
                    rhi_cfg.use_preferred_luid = studio_actions.target_gpu_luid_valid;
                    rhi_cfg.preferred_vendor_id = studio_actions.target_gpu_vendor_id;
                    rhi_cfg.preferred_device_id = studio_actions.target_gpu_device_id;
                    rhi_cfg.prefer_integrated = false;
                    const char* target_label = studio_actions.target_gpu_name[0] != '\0'
                                                   ? studio_actions.target_gpu_name.data()
                                                   : "Selected GPU";
                    if (!recreate_vulkan_stack(false, "Active GPU Switched", target_label)) {
                        rhi_cfg = prev_rhi_cfg;
                        if (!recreate_vulkan_stack(false, "GPU Switch Reverted",
                                                   "Reverted to previous Vulkan adapter")) {
                            render_exit_code.store(9, std::memory_order_release);
                            break;
                        }
                    }
                    continue;
                }
            }
            if (studio_actions.request_new_project) {
                studio_actions.request_new_project = false;
                project_path.clear();
                shell.workspace().params().reset_project_to_defaults();
                shell.workspace().undo_stack().clear();
                if (plugin_host != nullptr) {
                    const std::string g = shell.workspace().node_canvas().serialize_graph_json();
                    post_host_cmd(plugin_host, AETHER_HOST_CMD_SET_GRAPH_JSON, 0, 0.0f, 0, g.c_str());
                }
                shell.workspace().push_toast(ui::ToastSeverity::Info, "New Project", "Reset to Studio defaults.",
                                             frame_now_s);
            }
            if (studio_actions.request_open_project_dialog) {
                studio_actions.request_open_project_dialog = false;
                dialog_job.launch(hwnd, FileDialogKind::OpenProject);
            }
            if (studio_actions.request_open_project_path) {
                studio_actions.request_open_project_path = false;
                if (studio_actions.project_path_buf[0] != '\0' && plugin_host != nullptr) {
                    project_path = studio_actions.project_path_buf.data();
                    (void)aether_host_load_project(plugin_host, project_path.c_str());
                }
            }
            if (studio_actions.request_save_project) {
                studio_actions.request_save_project = false;
                if (project_path.empty()) {
                    dialog_job.launch(hwnd, FileDialogKind::SaveProject);
                } else if (plugin_host != nullptr) {
                    push_project_snapshot_to_host();
                    (void)aether_host_save_project(plugin_host, project_path.c_str());
                }
            }
            if (studio_actions.request_save_project_as) {
                studio_actions.request_save_project_as = false;
                dialog_job.launch(hwnd, FileDialogKind::SaveProject);
            }
            if (studio_actions.request_open_avatar_dialog) {
                studio_actions.request_open_avatar_dialog = false;
                dialog_job.launch(hwnd, FileDialogKind::OpenAvatar);
            }
            if (studio_actions.request_wizard_apply) {
                studio_actions.request_wizard_apply = false;
                pipeline->send_tracking_command({TrackingCmdKind::SetEpOverride, studio_actions.wizard_ep_override});
            }

            if (project_path.empty()) {
                host_status.project_path[0] = '\0';
                std::snprintf(host_status.project_title.data(), host_status.project_title.size(), "Untitled");
            } else {
                std::snprintf(host_status.project_path.data(), host_status.project_path.size(), "%s",
                              project_path.c_str());
                const std::string fname = std::filesystem::path(project_path).filename().string();
                std::snprintf(host_status.project_title.data(), host_status.project_title.size(), "%s",
                              fname.empty() ? "Project" : fname.c_str());
            }

            if (wait_smoke_avatar && current_avatar == nullptr) {
                while (asset_job.state() == renderer::AssetJobState::Loading &&
                       !shared.quit_requested.load(std::memory_order_acquire)) {
                    phase_timer.sleep_us(200);
                }
                if (asset_job.state() == renderer::AssetJobState::Failed ||
                    asset_job.state() == renderer::AssetJobState::Cancelled) {
                    AETHER_LOG_ERROR("renderer", "Startup --avatar load failed: %s", asset_job.last_error());
                    render_exit_code.store(12, std::memory_order_release);
                    break;
                }
            }

            if (render_cfg.allow_disk_shader_reload && (rendered_frames % 15ULL) == 0ULL) {
                (void)npr_renderer.graph().poll_shader_hot_reload();
            }

            if (opts.smoke_reload_avatar_at > 0U && rendered_frames == opts.smoke_reload_avatar_at &&
                !opts.avatar_path.empty() && current_avatar != nullptr) {
                asset_job.start_load(*rhi, opts.avatar_path);
                while (asset_job.state() == renderer::AssetJobState::Loading &&
                       !shared.quit_requested.load(std::memory_order_acquire)) {
                    phase_timer.sleep_us(200);
                }
            }

            if (asset_job.state() == renderer::AssetJobState::ReadyForGpu) {
                auto up_res = asset_job.finalize_gpu_upload(*rhi);
                if (!up_res) {
                    AETHER_LOG_ERROR("renderer", "GPU avatar upload failed: %s", up_res.error().c_str());
                    if (wait_smoke_avatar) {
                        render_exit_code.store(13, std::memory_order_release);
                        break;
                    }
                } else {
                    (void)pipeline->wait_output_slots_drained(250u);
                    // The previous avatar is retired to the GC against the frame timeline instead of being
                    // destroyed here; NprRenderer::bind_avatar fences its own frame-graph resources.
                    retire_avatar(std::move(current_avatar));
                    current_avatar = std::move(up_res.value());
                    last_loaded_avatar_path = current_avatar->source_path;
                    if (auto b_rc = npr_renderer.bind_avatar(current_avatar.get()); !b_rc) {
                        AETHER_LOG_ERROR("renderer", "NprRenderer bind_avatar failed: %s", b_rc.error().c_str());
                    }
                    dx11_bridge.bind_avatar(current_avatar.get());
                    (void)configure_dx11_for_avatar();
                    baked_face_sdf_applied = false;
                    camera = renderer::fit_camera_to_avatar(*current_avatar);
                    pipeline->push_rest_skeleton(build_rest_metrics_from_avatar(*current_avatar));
                    push_avatar_scene_items_to_ui(*current_avatar, shell.workspace());
                    shell.workspace().push_toast(ui::ToastSeverity::Success, "Avatar Loaded",
                                                 current_avatar->source_path.c_str(), frame_now_s);
                }
            }

            if (vp_actions.request_cancel_load) {
                asset_job.cancel();
                vp_actions.request_cancel_load = false;
            }
            if (vp_actions.request_load_path != nullptr) {
                last_loaded_avatar_path = vp_actions.request_load_path;
                asset_job.start_load(*rhi, vp_actions.request_load_path);
                vp_actions.request_load_path = nullptr;
            }
            if (vp_actions.request_calibrate_neutral) {
                pipeline->send_tracking_command({TrackingCmdKind::CalibrateNeutral, 0});
                vp_actions.request_calibrate_neutral = false;
            }
            if (vp_actions.request_toggle_camera_source) {
                pipeline->send_tracking_command({TrackingCmdKind::ToggleCameraSource, 0});
                vp_actions.request_toggle_camera_source = false;
            }
            if (vp_actions.request_set_camera_active) {
                pipeline->send_tracking_command(
                    {TrackingCmdKind::SetCameraActive, vp_actions.target_camera_active ? 1 : 0});
                vp_actions.request_set_camera_active = false;
            }
            if (vp_actions.request_set_synthetic_camera) {
                pipeline->send_tracking_command(
                    {TrackingCmdKind::SetSyntheticCamera, vp_actions.target_synthetic_camera ? 1 : 0});
                vp_actions.request_set_synthetic_camera = false;
            }
            if (vp_actions.request_select_camera) {
                TrackingControlCommand cmd{};
                cmd.kind = TrackingCmdKind::SelectCameraDevice;
                cmd.int_arg = vp_actions.select_camera_fps;
                std::snprintf(cmd.str_arg, sizeof(cmd.str_arg), "%s", vp_actions.select_camera_link.data());
                pipeline->send_tracking_command(cmd);
                vp_actions.request_select_camera = false;
            }
            if (vp_actions.request_ep_override != -99) {
                pipeline->send_tracking_command({TrackingCmdKind::SetEpOverride, vp_actions.request_ep_override});
                vp_actions.request_ep_override = -99;
            }
            if (vp_actions.request_denoiser_apply) {
                // Index order matches `render.denoiser` enum labels in ui/params/param_registry.hpp.
                static constexpr renderer::DenoiserKind kUiToDenoiserKind[6] = {
                    renderer::DenoiserKind::Auto, renderer::DenoiserKind::SvgfLite,   renderer::DenoiserKind::NrdSigma,
                    renderer::DenoiserKind::Oidn, renderer::DenoiserKind::FfxCompute, renderer::DenoiserKind::Optix,
                };
                const auto kind = kUiToDenoiserKind[std::min<uint8_t>(vp_actions.denoiser_kind, 5)];
                (void)npr_renderer.graph().denoiser().select_backend(kind);
                vp_actions.request_denoiser_apply = false;
            }
            if (vp_actions.lighting_dirty) {
                auto& rig = npr_renderer.lighting();
                rig.lights[0].orbit_azimuth_deg = vp_actions.key_azimuth_deg;
                rig.lights[0].orbit_elevation_deg = vp_actions.key_elevation_deg;
                rig.lights[1].orbit_azimuth_deg = vp_actions.fill_azimuth_deg;
                rig.lights[1].orbit_elevation_deg = vp_actions.fill_elevation_deg;
                rig.lights[2].orbit_azimuth_deg = vp_actions.rim_azimuth_deg;
                rig.lights[2].orbit_elevation_deg = vp_actions.rim_elevation_deg;
                rig.lights[0].intensity = vp_actions.key_intensity;
                rig.lights[1].intensity = vp_actions.fill_intensity;
                rig.lights[2].intensity = vp_actions.rim_intensity;
                rig.global_live2d_flatness = vp_actions.live2d_flatness;
                rig.bloom_intensity = vp_actions.bloom_intensity;
                rig.outline_width_scale = vp_actions.outline_scale;
                rig.face_normal_flatten = vp_actions.face_normal_flatten;
                rig.cel_band_sharpness = vp_actions.cel_band_sharpness;
                rig.shadow_warm_cool_shift = vp_actions.shadow_warm_cool_shift;
                rig.iro_tsuke_strength = vp_actions.iro_tsuke_strength;
                rig.crease_strength = vp_actions.crease_strength;
                rig.hair_highlight_intensity = vp_actions.hair_highlight_intensity;
                rig.rim_step_strength = vp_actions.rim_step_strength;
                vp_actions.lighting_dirty = false;
            }
            if (vp_actions.request_bake_face_sdf) {
                vp_actions.request_bake_face_sdf = false;
                if (current_avatar != nullptr && !current_avatar->rest_vertices.empty()) {
                    renderer::SdfBakeInput sdf_in{};
                    sdf_in.resolution = 256;
                    sdf_in.azimuth_steps = 16;
                    sdf_in.vertices = current_avatar->rest_vertices.data();
                    sdf_in.vertex_count = static_cast<uint32_t>(current_avatar->rest_vertices.size());
                    if (!current_avatar->rest_indices.empty()) {
                        sdf_in.indices = current_avatar->rest_indices.data();
                        sdf_in.index_count = static_cast<uint32_t>(current_avatar->rest_indices.size());
                    }
                    auto sdf_res = npr_renderer.bake_and_apply_face_sdf(sdf_in);
                    if (sdf_res) {
                        baked_face_sdf_applied = true;
                    } else {
                        AETHER_LOG_ERROR("renderer", "Generate Face SDF failed: %s", sdf_res.error().c_str());
                    }
                }
            }
            if (vp_actions.reset_camera && current_avatar != nullptr) {
                camera = renderer::fit_camera_to_avatar(*current_avatar);
                vp_actions.reset_camera = false;
            }
            if (vp_actions.request_focal_mm > 0.0f) {
                // Focal length on a 24 mm-high sensor; dolly to keep the subject's framed size constant.
                const float new_fovy = 2.0f * std::atan(12.0f / std::max(8.0f, vp_actions.request_focal_mm));
                camera.distance = std::clamp(
                    camera.distance * std::tan(camera.fovy_rad * 0.5f) / std::tan(new_fovy * 0.5f), 0.25f, 60.0f);
                camera.fovy_rad = new_fovy;
                vp_actions.request_focal_mm = 0.0f;
            }
            camera.yaw_rad += vp_actions.orbit_yaw_delta;
            camera.pitch_rad = std::clamp(camera.pitch_rad + vp_actions.orbit_pitch_delta, -1.45f, 1.45f);
            camera.distance = std::clamp(camera.distance * std::exp(-vp_actions.zoom_delta), 0.25f, 60.0f);
            camera.target_x += vp_actions.pan_dx * camera.distance * std::cos(camera.yaw_rad);
            camera.target_z -= vp_actions.pan_dx * camera.distance * std::sin(camera.yaw_rad);
            camera.target_y += vp_actions.pan_dy * camera.distance;
            vp_actions.orbit_yaw_delta = 0.0f;
            vp_actions.orbit_pitch_delta = 0.0f;
            vp_actions.pan_dx = 0.0f;
            vp_actions.pan_dy = 0.0f;
            vp_actions.zoom_delta = 0.0f;

            RECT live_cr{};
            ::GetClientRect(hwnd, &live_cr);
            const uint32_t live_w =
                static_cast<uint32_t>(live_cr.right > live_cr.left ? (live_cr.right - live_cr.left) : 0);
            const uint32_t live_h =
                static_cast<uint32_t>(live_cr.bottom > live_cr.top ? (live_cr.bottom - live_cr.top) : 0);
            const bool pending_resize = shared.resize_pending.exchange(false, std::memory_order_acq_rel);
            if (pending_resize ||
                (live_w > 0U && live_h > 0U &&
                 (live_w != swapchain.extent().width || live_h != swapchain.extent().height))) {
                const uint32_t nw = live_w > 0U ? live_w : shared.pending_w.load(std::memory_order_acquire);
                const uint32_t nh = live_h > 0U ? live_h : shared.pending_h.load(std::memory_order_acquire);
                if (nw > 0U && nh > 0U) {
                    (void)swapchain.resize(nw, nh);
                }
            }

            if (vp_actions.vsync_enabled != swapchain.vsync_enabled()) {
                (void)swapchain.set_vsync(vp_actions.vsync_enabled);
                next_pace_us = 0ULL;
            }

            if (vp_actions.output_config_dirty) {
                vp_actions.output_config_dirty = false;
                active_out_cfg.resolution =
                    static_cast<output::OutputResolutionPreset>(std::min<uint8_t>(vp_actions.output_res_preset, 3));
                active_out_cfg.width = output::output_preset_width(active_out_cfg.resolution);
                active_out_cfg.height = output::output_preset_height(active_out_cfg.resolution);
                active_out_cfg.fps = output::clamp_output_fps_for_resolution(
                    active_out_cfg.width, active_out_cfg.height, vp_actions.output_fps);
                vp_actions.output_fps = active_out_cfg.fps;
                active_out_cfg.premul_alpha = vp_actions.output_premul_alpha;
                active_out_cfg.non_alpha_bg =
                    static_cast<output::NonAlphaBgPreset>(std::min<uint8_t>(vp_actions.output_non_alpha_bg, 2));
                active_out_cfg.custom_bg_rgb = vp_actions.output_custom_bg;
                active_out_cfg.spout_enabled = vp_actions.spout_enabled;
                active_out_cfg.spout_mode = vp_actions.spout_cpu_fallback ? output::SpoutSharingMode::CpuCompatibility
                                                                          : output::SpoutSharingMode::GpuDefault;
                active_out_cfg.vcam_enabled = vp_actions.vcam_enabled;
                active_out_cfg.vcam_skip_when_no_reader = true;
                active_out_cfg.ndi_enabled = vp_actions.ndi_enabled;
                active_out_cfg.ndi_format = (vp_actions.ndi_format == 1) ? output::NdiVideoFormat::UyvaStraight
                                                                         : output::NdiVideoFormat::BgraStraight;
                active_out_cfg.osc_enabled = vp_actions.osc_enabled;
                active_out_cfg.dsp_user_offset_ms = std::clamp(vp_actions.dsp_user_offset_ms, -150.0f, 150.0f);
                pipeline->update_output_config(active_out_cfg);
            }

            if (vp_actions.desired_vp_w >= 64U && vp_actions.desired_vp_h >= 64U) {
                const float res_scale = std::clamp(vp_actions.render_resolution_scale, 0.50f, 2.00f);
                const uint32_t clamped_w = std::clamp(
                    static_cast<uint32_t>(std::lround(static_cast<double>(vp_actions.desired_vp_w) * res_scale)),
                    64U, 4096U);
                const uint32_t clamped_h = std::clamp(
                    static_cast<uint32_t>(std::lround(static_cast<double>(vp_actions.desired_vp_h) * res_scale)),
                    64U, 4096U);
                const uint32_t target_out_w = active_out_cfg.width;
                const uint32_t target_out_h = active_out_cfg.height;
                const auto cur_out = npr_renderer.graph().outputs();
                if (clamped_w != cur_out.width || clamped_h != cur_out.height || target_out_w != cur_out.output_width ||
                    target_out_h != cur_out.output_height) {
                    (void)pipeline->wait_output_slots_drained(250u);
                    rhi->wait_idle();
                    if (auto rz_rc = npr_renderer.resize(clamped_w, clamped_h, target_out_w, target_out_h); rz_rc) {
                        refresh_vp_descriptors();
                    }
                }
            }

            // Apply a backend change requested from the Inspector (render.backend), then reclaim retired objects.
            if (vp_actions.render_backend != requested_backend_ui) {
                const bool first_sync = (requested_backend_ui == 0xFFU);
                requested_backend_ui = vp_actions.render_backend;
                const auto wanted = (vp_actions.render_backend == 1U) ? renderer::RenderBackend::DirectX11
                                                                      : renderer::RenderBackend::Vulkan;
                // A CLI --render-backend wins over the persisted setting on the first sync.
                if (!(first_sync && !opts.render_backend.empty()) && wanted != active_backend) {
                    active_backend = wanted;
                    if (active_backend == renderer::RenderBackend::Vulkan) {
                        dx11_bridge.release();
                    } else {
                        dx11_bridge.bind_avatar(current_avatar.get());
                    }
                    shell.workspace().push_toast(ui::ToastSeverity::Info, "Render Backend",
                                                 renderer::render_backend_name(active_backend), frame_now_s);
                }
            }
            if (active_backend == renderer::RenderBackend::DirectX11 && current_avatar != nullptr) {
                if (!dx11_bridge.ready()) {
                    dx11_bridge.bind_avatar(current_avatar.get());
                }
                if (!configure_dx11_for_avatar()) {
                    active_backend = renderer::RenderBackend::Vulkan;
                    dx11_bridge.release();
                    shell.workspace().push_toast(ui::ToastSeverity::Warning, "DirectX 11 Unavailable",
                                                 "Falling back to the Vulkan renderer", frame_now_s);
                }
            }
            (void)gc.collect_epoch(completed_frame_timeline(*rhi), 16U);

            if (rendered_frames == 5) {
                imgui_arena.mark_warmup_complete();
            }

            auto acq = swapchain.acquire_and_begin_commands();
            if (!acq) {
                if (rhi->is_device_lost() && attempt_device_recovery()) {
                    continue;
                }
                AETHER_LOG_ERROR("rhi", "acquire_and_begin_commands failed: %s", acq.error().c_str());
                render_exit_code.store(8, std::memory_order_release);
                break;
            }
            const rhi::AcquiredFrame& frame = acq.value();
            if (!frame.valid) {
                phase_timer.sleep_us(4000);
                continue;
            }

            if (slot_submitted[frame.frame_slot]) {
                const auto& gp_snap = npr_renderer.graph().timing().resolve_frame(frame.frame_slot);
                if (gp_snap.valid) {
                    npr_renderer.graph().denoiser().observe_gpu_time_ms(gp_snap.denoiser_ms);
                }
            }

            if (preview_cache != nullptr && preview_tex_set != VK_NULL_HANDLE &&
                preview_img.handle != VK_NULL_HANDLE) {
                (void)pipeline->acquire_live_preview(*preview_cache);
                const auto& sb = preview_staging[frame.frame_slot];
                if (preview_cache->valid && preview_cache->sequence != preview_uploaded_seq &&
                    sb.handle != VK_NULL_HANDLE && sb.info.pMappedData != nullptr) {
                    std::memcpy(sb.info.pMappedData, preview_cache->rgba8.data(), preview_cache->rgba8.size());
                    (void)vmaFlushAllocation(rhi->allocator(), sb.allocation, 0, preview_cache->rgba8.size());
                    rhi::RhiDevice::cmd_transition_image(
                        frame.cmd, preview_img.handle,
                        preview_img_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        preview_img_initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                        preview_img_initialized ? VK_ACCESS_2_SHADER_READ_BIT : 0ULL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
                    VkBufferImageCopy reg{};
                    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    reg.imageExtent = {tracking::LiveCameraPreviewFrame::kPreviewWidth,
                                       tracking::LiveCameraPreviewFrame::kPreviewHeight, 1};
                    vkCmdCopyBufferToImage(frame.cmd, sb.handle, preview_img.handle,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
                    rhi::RhiDevice::cmd_transition_image(
                        frame.cmd, preview_img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_READ_BIT);
                    preview_img_initialized = true;
                    preview_uploaded_seq = preview_cache->sequence;
                }
            }

            const auto out_status = pipeline->latest_output_status();
            const SkinningSnapshot& skin_snap = pipeline->acquire_skinning_ref();
            last_late_latch_us = 0;
            if (current_avatar != nullptr && skin_snap.frame_sequence != 0ULL) {
                AETHER_HOT_PATH_SCOPE;
                const uint64_t ll_start = now_us();
                AetherDspSyncPacket t3_dsp{};
                const bool has_fresh_dsp = pipeline->try_read_latest_dsp(t3_dsp);
                std::array<float, animation::kMaxMorphs> latched_morphs{};
                const uint32_t morph_count = std::min<uint32_t>(skin_snap.frame.morph_count, animation::kMaxMorphs);
                if (morph_count > 0) {
                    std::memcpy(latched_morphs.data(), skin_snap.frame.morph_weights, morph_count * sizeof(float));
                }
                const bool dsp_voice_on = has_fresh_dsp && ((t3_dsp.dsp_flags & 0x05u) == 0x05u);
                if (dsp_voice_on) {
                    const float owner = std::clamp(skin_snap.frame.mouth_owner_blend, 0.0f, 1.0f);
                    if (!skin_snap.dsp_attenuated_on_t2) {
                        output::DspSyncBridge::apply_late_latch_visemes(t3_dsp, owner, latched_morphs.data(),
                                                                        morph_count);
                    } else {
                        for (uint32_t v = 0; v < animation::kVisemeCount; ++v) {
                            const uint32_t dst = animation::kVisemeSlotOffset + v;
                            if (dst >= morph_count) {
                                break;
                            }
                            latched_morphs[dst] = std::clamp(t3_dsp.dsp_viseme_weights[v], 0.0f, 1.0f) * owner;
                        }
                    }
                }
                current_avatar->update_pose(frame.frame_slot, skin_snap.frame_sequence, skin_snap.frame.bone_matrices,
                                            skin_snap.frame.bone_count, latched_morphs.data(), morph_count, true);
                if (dsp_voice_on) {
                    last_late_latch_us = now_us() - ll_start;
                }
            }

            ++graph_frame_idx;
            if ((graph_frame_idx & 1ULL) != (frame.frame_slot & 1U)) {
                ++graph_frame_idx;
            }
            const uint64_t completed_idx = slot_last_graph_idx[frame.frame_slot];
            slot_last_graph_idx[frame.frame_slot] = graph_frame_idx;

            std::array<float, 128> joint_stress_scratch{};
            uint32_t joint_stress_cnt = 0;
            if (vp_actions.show_skeleton_heatmap && current_avatar != nullptr) {
                joint_stress_cnt =
                    static_cast<uint32_t>(std::min(current_avatar->joints.size(), joint_stress_scratch.size()));
                for (uint32_t ji = 0; ji < joint_stress_cnt; ++ji) {
                    float d = 0.20f;
                    if (ji < current_avatar->joint_anim_world_pos.size() &&
                        ji < current_avatar->joint_rest_world_pos.size()) {
                        const float dx =
                            current_avatar->joint_anim_world_pos[ji][0] - current_avatar->joint_rest_world_pos[ji][0];
                        const float dy =
                            current_avatar->joint_anim_world_pos[ji][1] - current_avatar->joint_rest_world_pos[ji][1];
                        const float dz =
                            current_avatar->joint_anim_world_pos[ji][2] - current_avatar->joint_rest_world_pos[ji][2];
                        d = std::clamp(0.20f + std::sqrt(dx * dx + dy * dy + dz * dz) * 3.5f, 0.0f, 1.0f);
                    }
                    joint_stress_scratch[ji] = d;
                }
            }

            const uint64_t out_now_us = now_us();
            const uint32_t out_target_fps = std::clamp(active_out_cfg.fps, 1U, 60U);
            const uint64_t out_period_us = 1000000ULL / static_cast<uint64_t>(out_target_fps);
            const bool out_due = (last_out_submit_us == 0ULL) || ((out_now_us - last_out_submit_us) >= out_period_us);
            const bool vcam_consumer_active =
                active_out_cfg.vcam_enabled && (out_status.vcam_filter_active || out_status.vcam_rgb_pin_connected);
            const bool spout_needed = active_out_cfg.spout_enabled;
            const bool ndi_needed = active_out_cfg.ndi_enabled && out_status.ndi_runtime_available;
            const bool any_gpu_output_needed = spout_needed || ndi_needed || vcam_consumer_active;

            renderer::FrameRenderRequest render_req{};
            render_req.avatar = current_avatar.get();
            render_req.rig = &npr_renderer.lighting();
            render_req.camera = camera;
            render_req.bone_matrices = (skin_snap.frame_sequence != 0ULL) ? skin_snap.frame.bone_matrices : nullptr;
            render_req.bone_count = (skin_snap.frame_sequence != 0ULL) ? skin_snap.frame.bone_count : 0U;
            render_req.joint_stress = (joint_stress_cnt > 0U) ? joint_stress_scratch.data() : nullptr;
            render_req.joint_stress_count = joint_stress_cnt;
            render_req.frame_index = graph_frame_idx;
            render_req.completed_frame_index = completed_idx;
            render_req.frame_slot = frame.frame_slot;
            render_req.debug_view =
                static_cast<renderer::ShadingDebugView>(std::min<uint8_t>(vp_actions.debug_view, 13));
            render_req.rt_enabled = vp_actions.rt_enabled && rhi->rt_supported();
            render_req.premul_alpha = active_out_cfg.premul_alpha;
            render_req.need_straight_alpha = ndi_needed || !active_out_cfg.premul_alpha;
            render_req.need_bottom_up = vcam_consumer_active && out_status.vcam_rgb_pin_connected;
            render_req.encode_output_bgra = any_gpu_output_needed && out_due;
            render_req.encode_output_nv12 = vcam_consumer_active && out_due;
            render_req.throttle_rt_accel_struct = true;
            render_req.aa_mode = vp_actions.aa_mode;
            if (active_out_cfg.non_alpha_bg == output::NonAlphaBgPreset::Black) {
                render_req.non_alpha_bg_rgb = {0.0f, 0.0f, 0.0f};
            } else if (active_out_cfg.non_alpha_bg == output::NonAlphaBgPreset::Custom) {
                render_req.non_alpha_bg_rgb = active_out_cfg.custom_bg_rgb;
            } else {
                render_req.non_alpha_bg_rgb = {0.0f, 177.0f / 255.0f, 64.0f / 255.0f};
            }
            render_req.show_checkerboard = vp_actions.show_checkerboard;
            render_req.show_skeleton_heatmap = vp_actions.show_skeleton_heatmap;
            render_req.wireframe = vp_actions.wireframe;
            render_req.show_rig_colliders = vp_actions.show_rig_colliders;
            render_req.show_orbit_orb = vp_actions.show_orbit_orb;
            render_req.render_csm_shadow = true;
            render_req.outline_width_scale = npr_renderer.lighting().outline_width_scale;
            render_req.avatar_pos = vp_actions.avatar_pos;
            render_req.avatar_rot_y_deg = vp_actions.avatar_rot_y_deg;
            render_req.avatar_scale = vp_actions.avatar_scale;
            if (skin_snap.frame_sequence != 0ULL) {
                render_req.spring_colliders = skin_snap.spring_colliders.data();
                render_req.spring_collider_count = skin_snap.spring_collider_count;
                render_req.spring_segments = skin_snap.spring_segments.data();
                render_req.spring_segment_count = skin_snap.spring_segment_count;
            }
            if (active_backend == renderer::RenderBackend::DirectX11 && current_avatar != nullptr) {
                render_req.rt_enabled = false;
                if (dx11_bridge.render(*current_avatar, npr_renderer.lighting(), camera)) {
                    dx11_bridge.fill_request(render_req);
                }
            }

            uint64_t tl_val = 0;
            const bool has_avatar_ready = (current_avatar != nullptr && current_avatar->requirements.vertex_count > 0);
            if (has_avatar_ready) {
                if (auto sub_rc = npr_renderer.submit_critical_and_signal(render_req, tl_val); !sub_rc) {
                    if (rhi->is_device_lost() && attempt_device_recovery()) {
                        continue;
                    }
                    AETHER_LOG_ERROR("renderer", "submit_critical_and_signal failed: %s", sub_rc.error().c_str());
                    render_exit_code.store(9, std::memory_order_release);
                    break;
                }
                if (render_req.external_color_image != VK_NULL_HANDLE) {
                    dx11_bridge.on_submitted(tl_val);
                }
            }
            slot_submitted[frame.frame_slot] = true;

            npr_renderer.record_ui_composite(frame.cmd, render_req);
            swapchain.begin_swapchain_pass(frame, clear_rgba);

            const auto fg_out = npr_renderer.graph().outputs();
            ui::DevShellAvatarView avatar_view{};
            avatar_view.viewport_texture = vp_tex_set;
            avatar_view.orbit_orb_texture = orb_tex_set;
            avatar_view.viewport_w = fg_out.width;
            avatar_view.viewport_h = fg_out.height;
            avatar_view.wireframe_supported =
                (active_backend == renderer::RenderBackend::Vulkan) && npr_renderer.wireframe_supported();
            {
                const float cp = std::cos(camera.pitch_rad);
                const float sp = std::sin(camera.pitch_rad);
                const float cy = std::cos(camera.yaw_rad);
                const float sy = std::sin(camera.yaw_rad);
                avatar_view.camera_eye = {
                    camera.target_x + camera.distance * cp * sy,
                    camera.target_y + camera.distance * sp,
                    camera.target_z + camera.distance * cp * cy,
                };
                avatar_view.camera_target = {camera.target_x, camera.target_y, camera.target_z};
                avatar_view.camera_fovy_rad = camera.fovy_rad;
                avatar_view.has_camera = true;
                if (current_avatar != nullptr) {
                    const float min_y = current_avatar->desc.aabb_min[1];
                    const float max_y = current_avatar->desc.aabb_max[1];
                    avatar_view.gizmo_pivot_y = min_y + (max_y - min_y) * 0.56f;
                }
            }
            const auto job_state = asset_job.state();
            avatar_view.is_loading = (job_state == renderer::AssetJobState::Loading);
            avatar_view.load_progress = avatar_view.is_loading ? asset_job.progress() : 0.0f;
            avatar_view.load_error = (job_state == renderer::AssetJobState::Failed) ? asset_job.last_error() : "";
            if (current_avatar != nullptr) {
                avatar_view.has_avatar = true;
                avatar_view.source_path = current_avatar->source_path.c_str();
                avatar_view.format_name = source_format_name(current_avatar->desc.source_format);
                avatar_view.vertex_count = current_avatar->requirements.vertex_count;
                avatar_view.index_count = current_avatar->requirements.index_count;
                avatar_view.submesh_count = current_avatar->requirements.submesh_count;
                avatar_view.material_count = current_avatar->requirements.material_count;
                avatar_view.joint_count = current_avatar->requirements.joint_count;
                avatar_view.texture_count = current_avatar->requirements.texture_count;
                avatar_view.spring_count = current_avatar->requirements.spring_joint_count;
                avatar_view.morph_count = current_avatar->requirements.morph_target_count;
                avatar_view.mapped_humanoid_bones = current_avatar->mapped_humanoid_count();
                avatar_view.warnings_mask = current_avatar->desc.warnings_mask;
                avatar_view.avatar_height_m = current_avatar->desc.avatar_height_m;
                avatar_view.open_query_ms = current_avatar->stats.open_query_ms;
                avatar_view.fill_ms = current_avatar->stats.fill_ms;
                avatar_view.gpu_upload_ms = current_avatar->stats.gpu_upload_ms;
            }

            ui::DevShellRenderView render_view{};
            {
                const auto& gp_latest = npr_renderer.graph().timing().latest_snapshot();
                const auto& den = npr_renderer.graph().denoiser();
                render_view.rt_supported = rhi->rt_supported();
                render_view.rt_enabled = render_req.rt_enabled;
                render_view.debug_view = static_cast<uint8_t>(render_req.debug_view);
                render_view.denoiser_requested = vp_actions.denoiser_kind;
                render_view.denoiser_active_name = den.active_name();
                render_view.denoiser_fallback_triggered = den.fallback_triggered();
                switch (den.fallback_reason()) {
                case renderer::DenoiserFallbackReason::SdkUnavailable:
                    render_view.denoiser_fallback_reason = "Fallback: vendor SDK unavailable";
                    break;
                case renderer::DenoiserFallbackReason::RuntimeError:
                    render_view.denoiser_fallback_reason = "Fallback: runtime execution error";
                    break;
                case renderer::DenoiserFallbackReason::BudgetExceeded:
                    render_view.denoiser_fallback_reason = "Fallback: 120f p95 > 0.80 ms budget";
                    break;
                default:
                    render_view.denoiser_fallback_reason = "";
                    break;
                }
                render_view.render_backend = static_cast<uint8_t>(active_backend);
                {
                    const auto bs = dx11_bridge.stats();
                    std::snprintf(backend_detail.data(), backend_detail.size(), "%s%s, %ux%u",
                                  bs.shared_texture ? "shared texture" : "CPU copy", bs.warp ? " (WARP)" : "", bs.width,
                                  bs.height);
                    render_view.backend_detail = backend_detail.data();
                    render_view.dx11_render_ms = bs.last_render_ms;
                    const auto gs = gc.stats();
                    render_view.gc_retired_pending = gs.retired_pending;
                    render_view.gc_reclaimed_total = gs.retired_reclaimed_total;
                    render_view.gc_bytes_reclaimed = gs.bytes_reclaimed_total;
                }
                render_view.show_skeleton_heatmap = vp_actions.show_skeleton_heatmap;
                render_view.show_checkerboard = vp_actions.show_checkerboard;
                render_view.show_orbit_orb = vp_actions.show_orbit_orb;
                render_view.baked_face_sdf_applied = baked_face_sdf_applied;
                render_view.key_azimuth_deg = npr_renderer.lighting().lights[0].orbit_azimuth_deg;
                render_view.key_elevation_deg = npr_renderer.lighting().lights[0].orbit_elevation_deg;
                render_view.key_intensity = npr_renderer.lighting().lights[0].intensity;
                render_view.fill_intensity = npr_renderer.lighting().lights[1].intensity;
                render_view.rim_intensity = npr_renderer.lighting().lights[2].intensity;
                render_view.live2d_flatness = npr_renderer.lighting().global_live2d_flatness;
                render_view.bloom_intensity = npr_renderer.lighting().bloom_intensity;
                render_view.outline_scale = npr_renderer.lighting().outline_width_scale;
                render_view.timing_valid = gp_latest.valid;
                render_view.total_gpu_ms = gp_latest.gpu_total_ms;
                render_view.rt_gpu_ms = gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::RayTrace)] +
                                        gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::CsmFallback)];
                render_view.denoise_gpu_ms = gp_latest.denoiser_ms;
                render_view.denoise_p95_120f_ms = den.rolling_p95_ms();
                render_view.npr_gpu_ms = gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::Live2dNpr)] +
                                         gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::OutlineHull)] +
                                         gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::CreaseLines)];
                render_view.post_gpu_ms = gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::PostBloom)] +
                                          gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::ToneMap)];
                render_view.encode_gpu_ms = gp_latest.pass_ms[static_cast<size_t>(rhi::GpuPassId::OutputEncode)];
                render_view.device_recovery_count = device_recovery_count;
                render_view.last_recovery_msg = last_recovery_msg.data();
                if (vram_budget_mb_cached == 0ULL || (rendered_frames % 30ULL) == 0ULL) {
                    const rhi::VramBudgetInfo vb = rhi->query_vram_budget();
                    vram_usage_mb_cached = vb.usage_bytes / (1024ULL * 1024ULL);
                    vram_budget_mb_cached = vb.budget_bytes / (1024ULL * 1024ULL);
                }
                render_view.present_fps = present_fps_ema;
                render_view.vram_usage_mb = vram_usage_mb_cached;
                render_view.vram_budget_mb = vram_budget_mb_cached;
                render_view.gpu_adapters = &gpu_adapters;
            }

            uint64_t t3_start = 0;
            uint64_t ui_elapsed_us = 0;

            {
                const uint64_t ui_begin = now_us();
                ImGui_ImplVulkan_NewFrame();
                ImGui_ImplWin32_NewFrame();
                if (frame.extent.width > 0U && frame.extent.height > 0U) {
                    io.DisplaySize = ImVec2(static_cast<float>(frame.extent.width),
                                            static_cast<float>(frame.extent.height));
                    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                }

                {
                    AETHER_HOT_PATH_SCOPE;
                    AETHER_ZONE("T3_RenderAndUI");
                    t3_start = ui_begin;

                    track_hud = pipeline->latest_tracking_hud();
                    telem_cache = pipeline->latest_telemetry();
                    std::snprintf(telem_cache.present_mode, sizeof(telem_cache.present_mode), "%s",
                                  swapchain.present_mode_name());
                    telem_cache.vk_validation_errors = rhi->validation_error_count();
                    telem_cache.input_dropped = shared.input_ring.dropped_critical();
                    telem_cache.active_w = frame.extent.width;
                    telem_cache.active_h = frame.extent.height;

                    ui::DevShellTrackingView tracking_view{};
                    tracking_view.enabled = track_hud.enabled;
                    tracking_view.ready = track_hud.ready;
                    tracking_view.using_synthetic_camera = track_hud.using_synthetic_camera;
                    tracking_view.camera_active = track_hud.camera_active;
                    tracking_view.roi_locked = track_hud.roi_locked;
                    tracking_view.active_ep = track_hud.active_ep;
                    tracking_view.selected_ep_override = track_hud.selected_ep_override;
                    tracking_view.camera_width = track_hud.camera_width;
                    tracking_view.camera_height = track_hud.camera_height;
                    tracking_view.camera_fps = track_hud.camera_fps;
                    tracking_view.frames_processed = track_hud.frames_processed;
                    tracking_view.demotion_count = track_hud.demotion_count;
                    tracking_view.preprocess_ms = track_hud.preprocess_ms;
                    tracking_view.inference_ms = track_hud.inference_ms;
                    tracking_view.rolling_infer_ms = track_hud.rolling_infer_ms;
                    tracking_view.spring_sim_ms = skin_snap.spring_sim_ms;
                    tracking_view.face_confidence = track_hud.face_confidence;
                    tracking_view.upper_body_confidence = track_hud.upper_body_confidence;
                    tracking_view.left_hand_confidence = track_hud.left_hand_confidence;
                    tracking_view.right_hand_confidence = track_hud.right_hand_confidence;
                    tracking_view.jaw_open_preview = skin_snap.frame.arkit_preview[17]; // jawOpen
                    tracking_view.blink_l_preview = skin_snap.frame.arkit_preview[0];   // eyeBlinkLeft
                    tracking_view.blink_r_preview = skin_snap.frame.arkit_preview[7];   // eyeBlinkRight
                    tracking_view.valid_flags = skin_snap.frame.tracking_valid_flags;
                    tracking_view.active_ep_label = track_hud.active_ep_label;
                    tracking_view.camera_label = track_hud.camera_label;
                    tracking_view.camera_symlink = track_hud.camera_symlink;
                    tracking_view.neutral_calibrated = track_hud.neutral_calibrated;
                    tracking_view.stable_lock_frames = track_hud.stable_lock_frames;
                    tracking_view.head_euler_deg = {track_hud.head_euler_deg[0], track_hud.head_euler_deg[1],
                                                    track_hud.head_euler_deg[2]};
                    if (track_hud.camera_active && !track_hud.using_synthetic_camera && preview_img_initialized &&
                        preview_cache != nullptr && preview_cache->valid && preview_tex_set != VK_NULL_HANDLE) {
                        tracking_view.live_preview_texture_id =
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(preview_tex_set));
                        tracking_view.live_preview_width = tracking::LiveCameraPreviewFrame::kPreviewWidth;
                        tracking_view.live_preview_height = tracking::LiveCameraPreviewFrame::kPreviewHeight;
                        tracking_view.live_preview_valid = true;
                        const auto& ov = preview_cache->overlay;
                        tracking_view.live_preview_face_locked = ov.face_locked;
                        tracking_view.live_preview_face_roi = {ov.face_roi.cx, ov.face_roi.cy, ov.face_roi.width,
                                                               ov.face_roi.height};
                        const uint32_t kp_count = std::min<uint32_t>(ov.keypoint_count, 24U);
                        for (uint32_t k = 0; k < kp_count; ++k) {
                            tracking_view.live_preview_keypoints[k] = {ov.keypoints_norm[k * 2U + 0U],
                                                                       ov.keypoints_norm[k * 2U + 1U]};
                            tracking_view.live_preview_keypoint_valid[k] = 1U;
                        }
                        tracking_view.live_preview_axes_valid = ov.head_axes_valid;
                        if (ov.head_axes_valid) {
                            for (size_t a = 0; a < 4U; ++a) {
                                tracking_view.live_preview_head_axes[a] = {ov.head_axes_norm[a * 2U + 0U],
                                                                           ov.head_axes_norm[a * 2U + 1U]};
                            }
                        }
                        tracking_view.neutral_calibrated = ov.neutral_calibrated;
                        tracking_view.stable_lock_frames = ov.stable_lock_frames;
                        tracking_view.head_euler_deg = {ov.head_euler_deg.pitch, ov.head_euler_deg.yaw,
                                                        ov.head_euler_deg.roll};
                    }

                    ui::DevShellOutputView output_view{};
                    output_view.width = active_out_cfg.width;
                    output_view.height = active_out_cfg.height;
                    output_view.fps = active_out_cfg.fps;
                    output_view.premul_alpha = active_out_cfg.premul_alpha;
                    output_view.non_alpha_bg = static_cast<uint8_t>(active_out_cfg.non_alpha_bg);
                    output_view.spout_enabled = active_out_cfg.spout_enabled;
                    output_view.spout_active = out_status.spout_active;
                    output_view.spout_using_cpu_fallback = out_status.spout_using_cpu_fallback;
                    output_view.spout_adapter_mismatch = out_status.spout_adapter_mismatch;
                    output_view.spout_frames_published = out_status.spout_frames_published;
                    output_view.spout_skipped_mutex = out_status.spout_skipped_mutex;
                    output_view.spout_publish_ms = out_status.spout_publish_ms;
                    output_view.vcam_enabled = active_out_cfg.vcam_enabled;
                    output_view.vcam_active = out_status.vcam_active;
                    output_view.vcam_using_ext_host_mem = out_status.vcam_using_ext_host_mem;
                    output_view.vcam_rgb_pin_connected = out_status.vcam_rgb_pin_connected;
                    output_view.vcam_frames_published = out_status.vcam_frames_published;
                    output_view.vcam_dma_ms = out_status.vcam_dma_ms;
                    output_view.ndi_enabled = active_out_cfg.ndi_enabled;
                    output_view.ndi_active = out_status.ndi_active;
                    output_view.ndi_runtime_available = out_status.ndi_runtime_available;
                    output_view.ndi_format = static_cast<uint8_t>(active_out_cfg.ndi_format);
                    output_view.ndi_frames_sent = out_status.ndi_frames_sent;
                    output_view.ndi_send_ms = out_status.ndi_send_ms;
                    output_view.osc_enabled = active_out_cfg.osc_enabled;
                    output_view.osc_active = out_status.osc_active;
                    output_view.osc_packets_sent = out_status.osc_packets_sent;
                    output_view.dsp_connected = out_status.dsp_connected;
                    output_view.dsp_voice_active = out_status.dsp_voice_active;
                    output_view.dsp_input_rms_db = out_status.dsp_input_rms_db;
                    output_view.dsp_reported_latency_ms = out_status.dsp_reported_latency_ms;
                    output_view.dsp_recommended_delay_ms = out_status.dsp_recommended_delay_ms;
                    output_view.video_latency_ms = out_status.video_latency_ms;
                    output_view.latency_jitter_ms = out_status.latency_jitter_ms;
                    output_view.dsp_user_offset_ms = active_out_cfg.dsp_user_offset_ms;
                    output_view.dsp_stale_count = out_status.dsp_stale_count;
                    output_view.late_latch_us = last_late_latch_us;

                    shell.drain_logs();
                    pump_t3_inputs(shared, input_echo, io);
                    ImGui::NewFrame();

                    shell.workspace().set_host_status(host_status);
                    shell.workspace().sync_plugins(plugin_vms);
                    studio_actions.viewport = vp_actions;
                    shell.render_studio(static_cast<double>(ui_begin) * 1e-6, *rhi, telem_cache, input_echo,
                                        imgui_arena, ffi_ok, avatar_view, tracking_view, render_view, output_view,
                                        studio_actions);
                    vp_actions = studio_actions.viewport;

                    ImGui::Render();
                }

                ui_elapsed_us = now_us() - ui_begin;
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.cmd);
            }

            const VkSemaphore wait_sem = has_avatar_ready ? rhi->frame_timeline() : VK_NULL_HANDLE;
            if (auto end_rc = swapchain.end_frame(frame, wait_sem, tl_val, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
                !end_rc) {
                if (rhi->is_device_lost() && attempt_device_recovery()) {
                    continue;
                }
                AETHER_LOG_ERROR("rhi", "end_frame failed: %s", end_rc.error().c_str());
                render_exit_code.store(9, std::memory_order_release);
                break;
            }

            const uint64_t t3_submit_done_us = now_us();
            const bool has_valid_tracking_frame = !host_status.tracking_paused &&
                                                  (skin_snap.frame_sequence != 0ULL) &&
                                                  (skin_snap.t1_capture_us != 0ULL) &&
                                                  ((skin_snap.frame.tracking_valid_flags & 0x07u) != 0u);
            if (has_avatar_ready && tl_val > 0ULL && (out_due || opts.smoke_frames > 0U)) {
                last_out_submit_us = out_now_us;
                const auto gout = npr_renderer.graph().outputs();
                output::OutputRingSlot out_slot{};
                out_slot.frame_sequence = skin_snap.frame_sequence;
                out_slot.frame_timeline_value =
                    (render_req.encode_output_bgra || render_req.encode_output_nv12) ? tl_val : 0ULL;
                out_slot.output_slot = gout.output_slot;
                out_slot.width = gout.output_width;
                out_slot.height = gout.output_height;
                out_slot.fps = active_out_cfg.fps;
                out_slot.bgra_image = render_req.encode_output_bgra ? gout.bgra8_image : VK_NULL_HANDLE;
                out_slot.bgra_straight_image =
                    (render_req.encode_output_bgra && render_req.need_straight_alpha) ? gout.bgra8_straight_image
                                                                                      : VK_NULL_HANDLE;
                out_slot.bottom_up_image =
                    (render_req.encode_output_bgra && render_req.need_bottom_up) ? gout.bottom_up_bgra8_image
                                                                                 : VK_NULL_HANDLE;
                out_slot.nv12_y_image = render_req.encode_output_nv12 ? gout.nv12_y_image : VK_NULL_HANDLE;
                out_slot.nv12_uv_image = render_req.encode_output_nv12 ? gout.nv12_uv_image : VK_NULL_HANDLE;
                out_slot.has_bottom_up = render_req.encode_output_bgra && render_req.need_bottom_up;
                out_slot.tracking_active = has_valid_tracking_frame;
                out_slot.t1_capture_us = has_valid_tracking_frame ? skin_snap.t1_capture_us : 0ULL;
                out_slot.t2_start_us = skin_snap.t2_start_us;
                out_slot.t2_done_us = skin_snap.t2_done_us;
                out_slot.t3_start_us = t3_start;
                out_slot.t3_submit_us = t3_submit_done_us;
                out_slot.ui_cpu_us = ui_elapsed_us;
                out_slot.late_latch_sample_to_write_us = last_late_latch_us;
                out_slot.active_ep = skin_snap.frame.active_ep;
                pipeline->submit_output_slot(out_slot);
            } else {
                OutputWorkItem out_item{};
                out_item.frame_sequence = skin_snap.frame_sequence;
                out_item.tracking_active = has_valid_tracking_frame;
                out_item.t1_capture_us = has_valid_tracking_frame ? skin_snap.t1_capture_us : 0ULL;
                out_item.t2_start_us = skin_snap.t2_start_us;
                out_item.t2_done_us = skin_snap.t2_done_us;
                out_item.t3_start_us = t3_start;
                out_item.t3_submit_us = t3_submit_done_us;
                out_item.ui_cpu_us = ui_elapsed_us;
                pipeline->submit_rendered_frame(out_item);
            }

            AETHER_FRAME_MARK();
            ++rendered_frames;
            if (opts.smoke_frames > 0 && rendered_frames >= opts.smoke_frames) {
                shared.quit_requested.store(true, std::memory_order_release);
                ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                break;
            }

            if (opts.smoke_frames == 0U) {
                uint32_t cap_fps = 60U;
                switch (vp_actions.target_fps_mode) {
                case 0:
                    cap_fps = 30U;
                    break;
                case 1:
                    cap_fps = 60U;
                    break;
                case 2:
                    cap_fps = 120U;
                    break;
                default:
                    cap_fps = 0U;
                    break;
                }
                if (vp_actions.background_eco_mode && !on_air && ::GetForegroundWindow() != hwnd) {
                    cap_fps = (cap_fps == 0U) ? 30U : std::min(cap_fps, 30U);
                }
                if (cap_fps > 0U) {
                    const uint64_t period_us = 1000000ULL / static_cast<uint64_t>(cap_fps);
                    next_pace_us = (next_pace_us == 0ULL) ? (loop_begin_us + period_us) : (next_pace_us + period_us);
                    const uint64_t now_after = now_us();
                    if (now_after > next_pace_us + period_us) {
                        next_pace_us = now_after + period_us;
                    }
                    phase_timer.wait_until_us(next_pace_us, phase_timer.is_high_res() ? 250ULL : 1200ULL);
                } else {
                    next_pace_us = 0ULL;
                }
            }
            const uint64_t present_done_us = now_us();
            if (last_present_stamp_us > 0ULL && present_done_us > last_present_stamp_us) {
                const float inst_fps = 1000000.0f / static_cast<float>(present_done_us - last_present_stamp_us);
                present_fps_ema =
                    (present_fps_ema <= 0.1f) ? inst_fps : (present_fps_ema * 0.90f + inst_fps * 0.10f);
            }
            last_present_stamp_us = present_done_us;
        }

        // Stop T1/T2/T4 before rhi->wait_idle() & resource destruction so T4 transfer_queue never races shutdown.
        pipeline->stop();
        rhi->wait_idle();
        const auto dx11_final = dx11_bridge.stats();
        dx11_bridge.release();
        const uint32_t gc_flushed = gc.flush_all_epochs();
        const GcStats gc_final = gc.stats();

        const auto final_telem = pipeline->latest_telemetry();
        AETHER_LOG_INFO(
            "smoke",
            "Frames=%llu, VK_Errors=%llu, UI_OS_Fallback=%llu, UI_SteadyGrowth=%zu, HP_Viol=%llu, UI_p95=%.3fms",
            static_cast<unsigned long long>(rendered_frames),
            static_cast<unsigned long long>(rhi->validation_error_count()),
            static_cast<unsigned long long>(imgui_arena.os_fallback_allocs()), imgui_arena.steady_bump_growth_bytes(),
            static_cast<unsigned long long>(HotPathGuard::total_violations()),
            static_cast<double>(final_telem.ui_cpu.p95_ms));

        std::printf("[AETHER_SMOKE] frames=%llu vk_errors=%llu vk_warnings=%llu hot_path_violations=%llu "
                    "imgui_os_fallbacks=%llu imgui_steady_growth=%zu ui_cpu_mean_ms=%.3f ui_cpu_p95_ms=%.3f\n",
                    static_cast<unsigned long long>(rendered_frames),
                    static_cast<unsigned long long>(rhi->validation_error_count()),
                    static_cast<unsigned long long>(rhi->validation_warning_count()),
                    static_cast<unsigned long long>(HotPathGuard::total_violations()),
                    static_cast<unsigned long long>(imgui_arena.os_fallback_allocs()),
                    imgui_arena.steady_bump_growth_bytes(), static_cast<double>(final_telem.ui_cpu.mean_ms),
                    static_cast<double>(final_telem.ui_cpu.p95_ms));
        std::printf("[AETHER_SMOKE_EXT] backend=%s dx11_frames=%llu dx11_path=%s gc_reclaimed=%llu "
                    "gc_flushed_at_exit=%u gc_bytes=%llu\n",
                    active_backend == renderer::RenderBackend::DirectX11 ? "dx11" : "vulkan",
                    static_cast<unsigned long long>(dx11_final.frames),
                    dx11_final.frames == 0ULL ? "none" : (dx11_final.shared_texture ? "shared" : "cpu"),
                    static_cast<unsigned long long>(gc_final.retired_reclaimed_total), gc_flushed,
                    static_cast<unsigned long long>(gc_final.bytes_reclaimed_total));
        std::fflush(stdout);

        if (rhi->validation_error_count() > 0 || imgui_arena.os_fallback_allocs() > 0 ||
            HotPathGuard::total_violations() > 0) {
            render_exit_code.store(10, std::memory_order_release);
        }

        asset_job.reset();
        destroy_preview_resources(true);
        if (vp_tex_set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(vp_tex_set);
            vp_tex_set = VK_NULL_HANDLE;
        }
        if (orb_tex_set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(orb_tex_set);
            orb_tex_set = VK_NULL_HANDLE;
        }
        destroy_vp_sampler();
        npr_renderer.destroy();
        current_avatar.reset();
        shell.shutdown();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        swapchain.destroy();
        imgui_arena.shutdown();
        aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    });

    // T0 Win32 message loop (`GetMessageW` blocks cleanly on T0 until WM_QUIT or smoke completion).
    MSG msg{};
    while (!shared.quit_requested.load(std::memory_order_acquire)) {
        const BOOL gm = ::GetMessageW(&msg, nullptr, 0, 0);
        if (gm <= 0) {
            shared.quit_requested.store(true, std::memory_order_release);
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (t3_thread.joinable()) {
        t3_thread.join();
    }
    pipeline->stop();
    pipeline.reset();
    dialog_job.join();
    native_mgr.reset();
    if (plugin_host != nullptr) {
        aether_host_destroy(plugin_host);
        plugin_host = nullptr;
    }
    rhi->destroy();
    delete rhi;

    if (::IsWindow(hwnd)) {
        ::DestroyWindow(hwnd);
    }
    ::UnregisterClassW(wc.lpszClassName, hinst);
    aether_runtime_shutdown();
    CrashHandler::shutdown();
    return render_exit_code.load(std::memory_order_acquire);
#else
    (void)opts;
    return 1;
#endif
}

} // namespace aether::app
