#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <span>

namespace aether::ui {

/**
 * Z1 Outliner & Project Panel (`§12.4`, `§12.7`, `§12.13`, `§12.15`, broadcast overhaul §1 BUG-04/08).
 * - Default dock width 275 px * dpi. Full-width search live-filters scene items, bones, materials,
 *   expressions and outputs.
 * - Scene hierarchy lives in a recessed `##SceneTreeWell` (190 px * dpi) holding either the tree or a
 *   centered empty-state card (`Open Avatar...` / `Load Reference VRM`).
 * - Rigging: `ImGuiListClipper`-clipped skeleton tree with `F2` rename and `H` hide.
 * - Quick Expressions: 2-column key-badge + label grid, latched slots get an accent border.
 * - Outputs: checkbox rows with flush-right Active / Idle / Off pills and an `Inspect Output Settings...`
 *   ghost button that focuses the Inspector's `Output & Audio-DSP` section.
 * - Zero per-frame `std::string` allocations.
 */
class OutlinerPanel {
  public:
    void set_focus_filter_requested() noexcept { focus_filter_next_frame_ = true; }
    void set_sample_paths(const char* ref_vrm, const char* vrm1, const char* fbx, const char* pmx) noexcept;
    void set_avatar_path_buf(const char* path) noexcept;

    /** True once after `Inspect Output Settings...` was clicked (workspace forwards it to the Inspector). */
    [[nodiscard]] bool consume_inspect_outputs_request() noexcept {
        const bool r = inspect_outputs_requested_;
        inspect_outputs_requested_ = false;
        return r;
    }

    [[nodiscard]] const char* filter_text() const noexcept { return filter_buf_.data(); }

    void draw(StudioMode mode, bool on_air, const DevShellAvatarView& avatar, const DevShellOutputView& output_view,
              std::span<OutlinerBoneInfo> bones, std::span<OutlinerMaterialInfo> materials,
              std::span<ExpressionSlotInfo> expressions, std::span<const LoadedPluginVm> plugins, ParamStore& params,
              UndoStack& undo, const UiFontSet& fonts, UiSelection& selection, StudioActions& actions);

  private:
    void draw_scene_well(const DevShellAvatarView& avatar, const UiFontSet& fonts, UiSelection& selection,
                         StudioActions& actions, float scale);
    void draw_expression_grid(std::span<ExpressionSlotInfo> expressions, const UiFontSet& fonts, UiSelection& selection,
                              float scale);
    void draw_outputs(const DevShellOutputView& output_view, ParamStore& params, UndoStack& undo,
                      const UiFontSet& fonts, UiSelection& selection, StudioActions& actions, float scale);

    std::array<char, 64> filter_buf_{};
    std::array<char, 512> path_buf_{};
    std::array<char, 512> sample_ref_vrm_{};
    std::array<char, 512> sample_vrm1_{};
    std::array<char, 512> sample_fbx_{};
    std::array<char, 512> sample_pmx_{};
    std::array<uint32_t, 512> filtered_bone_indices_{};
    bool focus_filter_next_frame_{false};
    bool inspect_outputs_requested_{false};
    int32_t renaming_bone_idx_{-1};
    std::array<char, 64> rename_buf_{};
};

} // namespace aether::ui
