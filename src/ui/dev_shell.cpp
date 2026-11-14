#include "ui/dev_shell.hpp"

#include "ui/workspace/studio_types.hpp"
#include "ui/workspace/studio_workspace.hpp"

#include <imgui.h>

namespace aether::ui {

DevShell::DevShell() : workspace_(std::make_unique<StudioWorkspace>()) {}
DevShell::~DevShell() = default;
DevShell::DevShell(DevShell&&) noexcept = default;
DevShell& DevShell::operator=(DevShell&&) noexcept = default;

void DevShell::init(const RuntimePaths& paths, const HardwareInventory& hw, float dpi_scale,
                    const char* initial_avatar_path, bool allow_disk_config) {
    if (!workspace_) {
        workspace_ = std::make_unique<StudioWorkspace>();
    }
    workspace_->init(paths, hw, dpi_scale, initial_avatar_path, allow_disk_config);
}

void DevShell::shutdown() {
    if (workspace_) {
        workspace_->shutdown();
    }
}

void DevShell::drain_logs() noexcept {
    if (workspace_) {
        workspace_->drain_logs();
    }
}

void DevShell::render(const rhi::RhiDevice& rhi, const TelemetrySnapshot& telemetry,
                      const DevShellInputEcho& input_echo, const ImGuiArenaAllocator& arena, bool ffi_layout_ok,
                      const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                      const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                      DevShellViewportActions& actions) {
    StudioActions full_actions{};
    full_actions.viewport = actions;
    render_studio(ImGui::GetTime(), rhi, telemetry, input_echo, arena, ffi_layout_ok, avatar, tracking, render_view,
                  output_view, full_actions);
    actions = full_actions.viewport;
}

void DevShell::render_studio(double now_s, const rhi::RhiDevice& rhi, const TelemetrySnapshot& telemetry,
                             const DevShellInputEcho& input_echo, const ImGuiArenaAllocator& arena, bool ffi_layout_ok,
                             const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                             const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                             StudioActions& actions) {
    if (workspace_) {
        workspace_->render(now_s, rhi, telemetry, input_echo, arena, ffi_layout_ok, avatar, tracking, render_view,
                           output_view, actions);
    }
}

StudioMode DevShell::mode() const noexcept {
    return workspace_ ? workspace_->mode() : StudioMode::LiveStudio;
}

} // namespace aether::ui
