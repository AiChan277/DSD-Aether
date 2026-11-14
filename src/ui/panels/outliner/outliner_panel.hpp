#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <span>

namespace aether::ui {

/**
 * Z1 Outliner & Project Panel (`§12.4`, `§12.7`, `§12.13`, `§12.15`).
 * - Live Studio (260 px): Scene tree, Quick Expressions (`F1-F8`), Output toggles + inline errors, Avatar loader.
 * - Rigging (280 px): Filterable, `ImGuiListClipper`-clipped skeleton/morph tree with `F2` rename and `H`/`Alt+H`.
 * - Zero per-frame `std::string` allocations.
 */
class OutlinerPanel {
  public:
    void set_focus_filter_requested() noexcept { focus_filter_next_frame_ = true; }
    void set_sample_paths(const char* ref_vrm, const char* vrm1, const char* fbx, const char* pmx) noexcept;
    void set_avatar_path_buf(const char* path) noexcept;

    void draw(StudioMode mode, bool on_air, const DevShellAvatarView& avatar, const DevShellOutputView& output_view,
              std::span<OutlinerBoneInfo> bones, std::span<OutlinerMaterialInfo> materials,
              std::span<ExpressionSlotInfo> expressions, std::span<const LoadedPluginVm> plugins, ParamStore& params,
              UndoStack& undo, const UiFontSet& fonts, UiSelection& selection, StudioActions& actions);

  private:
    std::array<char, 64> filter_buf_{};
    std::array<char, 512> path_buf_{};
    std::array<char, 512> sample_ref_vrm_{};
    std::array<char, 512> sample_vrm1_{};
    std::array<char, 512> sample_fbx_{};
    std::array<char, 512> sample_pmx_{};
    bool focus_filter_next_frame_{false};
    int32_t renaming_bone_idx_{-1};
    std::array<char, 64> rename_buf_{};
};

} // namespace aether::ui
