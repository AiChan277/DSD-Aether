#pragma once

#include "aether/ui_state_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aether::core {

enum class KeyScope : uint8_t {
    Global = 0,
    Viewport = 1,
    NodeGraph = 2,
    Outliner = 3,
    Lighting = 4,
};

/**
 * Single authoritative Studio Action & Shortcut table (`Rev 3.1 §1.4`).
 * All menus, toolbars, modals, and shortcut dispatchers execute or query
 * IDs from this registry rather than scattered raw `ImGui::IsKeyPressed()` calls.
 */
enum class ActionId : uint16_t {
    OpenPalette = 0,
    ModeLiveStudio,
    ModeRigging,
    ModeLighting,
    ModeNodes,
    ProjectNew,
    ProjectOpen,
    ProjectSave,
    ProjectSaveAs,
    ImportAvatar,
    Undo,
    Redo,
    Preferences,
    ShortcutOverlay,
    ToggleOnAir,
    PauseTracking,
    CalibrateNeutral,
    ToggleRayTracing,
    MaximizeHoveredPanel,
    Expression1,
    Expression2,
    Expression3,
    Expression4,
    Expression5,
    Expression6,
    Expression7,
    Expression8,

    ViewportHeatmap,
    ViewportOrbitOrb,
    ViewportFrameHead,
    ViewportFrameBody,
    ViewportCam1,
    ViewportCam2,
    ViewportCam3,
    ViewportCam4,
    ViewportCam5,
    ViewportStoreCam1,
    ViewportStoreCam2,
    ViewportStoreCam3,
    ViewportStoreCam4,
    ViewportStoreCam5,
    ViewportPreset1,
    ViewportPreset2,
    ViewportPreset3,
    ViewportPreset4,
    ViewportPreset5,
    ViewportPreset6,
    GizmoSelect,
    GizmoMove,
    GizmoRotate,
    GizmoScale,
    GizmoLocalWorld,
    ViewportGrid,

    GraphAddNode,
    GraphDuplicate,
    GraphDelete,
    GraphFrame,
    GraphComment,
    GraphBypass,
    GraphRecompile,

    OutlinerRename,
    OutlinerFilter,
    OutlinerHide,
    OutlinerUnhideAll,
    OutlinerDelete,

    LightingQuadView,
    LightingPilotLight,

    // Rev 3.1 additions (appended to keep legacy action indices stable)
    OpenProjectManager,
    RedoAlt,
    ModalCancel,
    ExportAlfx,
    ImportAlfx,

    Count,
};

inline constexpr size_t kActionCount = static_cast<size_t>(ActionId::Count);

struct KeyBindingSpec {
    ActionId id;
    const char* name;
    const char* label;
    KeyScope scope;
    const char* default_chord;
    bool global_hotkey_capable;
    DirtyDomain dirty_domain;
};

inline constexpr std::array<KeyBindingSpec, kActionCount> kCanonicalActions{{
    {ActionId::OpenPalette, "global.palette", "Command Palette", KeyScope::Global, "Ctrl+K", false, DirtyDomain::None},
    {ActionId::ModeLiveStudio, "global.mode_live", "Mode: Live Studio", KeyScope::Global, "Ctrl+Shift+1", false,
     DirtyDomain::None},
    {ActionId::ModeRigging, "global.mode_rigging", "Mode: Rigging", KeyScope::Global, "Ctrl+Shift+2", false,
     DirtyDomain::None},
    {ActionId::ModeLighting, "global.mode_lighting", "Mode: Lighting", KeyScope::Global, "Ctrl+Shift+3", false,
     DirtyDomain::None},
    {ActionId::ModeNodes, "global.mode_nodes", "Mode: Nodes", KeyScope::Global, "Ctrl+Shift+4", false,
     DirtyDomain::None},
    {ActionId::ProjectNew, "file.new", "New Project", KeyScope::Global, "Ctrl+N", false, DirtyDomain::None},
    {ActionId::ProjectOpen, "file.open", "Open Project...", KeyScope::Global, "Ctrl+O", false, DirtyDomain::None},
    {ActionId::ProjectSave, "file.save", "Save Project", KeyScope::Global, "Ctrl+S", false, DirtyDomain::None},
    {ActionId::ProjectSaveAs, "file.save_as", "Save Project As...", KeyScope::Global, "Ctrl+Shift+S", false,
     DirtyDomain::None},
    {ActionId::ImportAvatar, "file.import_avatar", "Import Avatar...", KeyScope::Global, "Ctrl+I", false,
     DirtyDomain::AvatarState},
    {ActionId::Undo, "edit.undo", "Undo", KeyScope::Global, "Ctrl+Z", false, DirtyDomain::None},
    {ActionId::Redo, "edit.redo", "Redo", KeyScope::Global, "Ctrl+Y", false, DirtyDomain::None},
    {ActionId::Preferences, "app.preferences", "Preferences...", KeyScope::Global, "Ctrl+,", false, DirtyDomain::None},
    {ActionId::ShortcutOverlay, "app.shortcuts", "Keyboard Shortcuts", KeyScope::Global, "Ctrl+/", false,
     DirtyDomain::None},
    {ActionId::ToggleOnAir, "live.on_air", "Toggle ON AIR", KeyScope::Global, "Ctrl+Shift+L", false, DirtyDomain::None},
    {ActionId::PauseTracking, "tracking.pause", "Pause Tracking", KeyScope::Global, "Ctrl+P", false, DirtyDomain::None},
    {ActionId::CalibrateNeutral, "tracking.calibrate", "Calibrate Neutral", KeyScope::Global, "Ctrl+Shift+K", false,
     DirtyDomain::TrackingCfg},
    {ActionId::ToggleRayTracing, "render.toggle_rt", "Toggle Ray Tracing", KeyScope::Global, "Ctrl+Shift+R", false,
     DirtyDomain::EffectsFx},
    {ActionId::MaximizeHoveredPanel, "workspace.maximize", "Maximize Hovered Panel", KeyScope::Global, "Shift+Space",
     false, DirtyDomain::None},

    {ActionId::Expression1, "expr.slot_1", "Trigger Expression 1 (Joy)", KeyScope::Global, "F1", true,
     DirtyDomain::AvatarState},
    {ActionId::Expression2, "expr.slot_2", "Trigger Expression 2 (Surprised)", KeyScope::Global, "F2", true,
     DirtyDomain::AvatarState},
    {ActionId::Expression3, "expr.slot_3", "Trigger Expression 3 (Sorrow)", KeyScope::Global, "F3", true,
     DirtyDomain::AvatarState},
    {ActionId::Expression4, "expr.slot_4", "Trigger Expression 4 (Angry)", KeyScope::Global, "F4", true,
     DirtyDomain::AvatarState},
    {ActionId::Expression5, "expr.slot_5", "Trigger Expression 5 (Fun)", KeyScope::Global, "F5", true,
     DirtyDomain::AvatarState},
    {ActionId::Expression6, "expr.slot_6", "Trigger Expression 6 (Wink L)", KeyScope::Global, "F6", true,
     DirtyDomain::AvatarState},
    {ActionId::Expression7, "expr.slot_7", "Trigger Expression 7 (Wink R)", KeyScope::Global, "F7", true,
     DirtyDomain::AvatarState},
    {ActionId::Expression8, "expr.slot_8", "Trigger Expression 8 (Relaxed)", KeyScope::Global, "F8", true,
     DirtyDomain::AvatarState},

    {ActionId::ViewportHeatmap, "viewport.heatmap", "Toggle Confidence Heatmap", KeyScope::Viewport, "Alt+C", false,
     DirtyDomain::None},
    {ActionId::ViewportOrbitOrb, "viewport.orbit_orb", "Show Quick Key Orb", KeyScope::Viewport, "Alt+L", false,
     DirtyDomain::None},
    {ActionId::ViewportFrameHead, "viewport.frame_head", "Frame Head", KeyScope::Viewport, "F", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportFrameBody, "viewport.frame_body", "Frame Full Body", KeyScope::Viewport, "Shift+F", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportCam1, "viewport.cam_1", "Recall Camera 1", KeyScope::Viewport, "1", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportCam2, "viewport.cam_2", "Recall Camera 2", KeyScope::Viewport, "2", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportCam3, "viewport.cam_3", "Recall Camera 3", KeyScope::Viewport, "3", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportCam4, "viewport.cam_4", "Recall Camera 4", KeyScope::Viewport, "4", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportCam5, "viewport.cam_5", "Recall Camera 5", KeyScope::Viewport, "5", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportStoreCam1, "viewport.store_cam_1", "Store Camera 1", KeyScope::Viewport, "Ctrl+Alt+1", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportStoreCam2, "viewport.store_cam_2", "Store Camera 2", KeyScope::Viewport, "Ctrl+Alt+2", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportStoreCam3, "viewport.store_cam_3", "Store Camera 3", KeyScope::Viewport, "Ctrl+Alt+3", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportStoreCam4, "viewport.store_cam_4", "Store Camera 4", KeyScope::Viewport, "Ctrl+Alt+4", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportStoreCam5, "viewport.store_cam_5", "Store Camera 5", KeyScope::Viewport, "Ctrl+Alt+5", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportPreset1, "viewport.preset_1", "Framing Preset: Bust / Streamer", KeyScope::Global, "Ctrl+1",
     false, DirtyDomain::CameraState},
    {ActionId::ViewportPreset2, "viewport.preset_2", "Framing Preset: Face Close-Up", KeyScope::Global, "Ctrl+2", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportPreset3, "viewport.preset_3", "Framing Preset: Waist-Up", KeyScope::Global, "Ctrl+3", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportPreset4, "viewport.preset_4", "Framing Preset: Full Body", KeyScope::Global, "Ctrl+4", false,
     DirtyDomain::CameraState},
    {ActionId::ViewportPreset5, "viewport.preset_5", "Framing Preset: Stream Corner (Right)", KeyScope::Global,
     "Ctrl+5", false, DirtyDomain::CameraState},
    {ActionId::ViewportPreset6, "viewport.preset_6", "Framing Preset: Stream Corner (Left)", KeyScope::Global, "Ctrl+6",
     false, DirtyDomain::CameraState},
    {ActionId::GizmoSelect, "viewport.gizmo_select", "Select Tool", KeyScope::Viewport, "Q", false, DirtyDomain::None},
    {ActionId::GizmoMove, "viewport.gizmo_move", "Move Tool", KeyScope::Viewport, "W", false, DirtyDomain::None},
    {ActionId::GizmoRotate, "viewport.gizmo_rotate", "Rotate Tool", KeyScope::Viewport, "E", false, DirtyDomain::None},
    {ActionId::GizmoScale, "viewport.gizmo_scale", "Scale Tool", KeyScope::Viewport, "R", false, DirtyDomain::None},
    {ActionId::GizmoLocalWorld, "viewport.gizmo_space", "Toggle Local/World", KeyScope::Viewport, "X", false,
     DirtyDomain::None},
    {ActionId::ViewportGrid, "viewport.grid", "Toggle Grid", KeyScope::Viewport, "G", false, DirtyDomain::None},

    {ActionId::GraphAddNode, "graph.add_node", "Add Node Menu", KeyScope::NodeGraph, "Tab", false,
     DirtyDomain::NodeGraph},
    {ActionId::GraphDuplicate, "graph.duplicate", "Duplicate Selection", KeyScope::NodeGraph, "Ctrl+D", false,
     DirtyDomain::NodeGraph},
    {ActionId::GraphDelete, "graph.delete", "Delete Selection", KeyScope::NodeGraph, "Del", false,
     DirtyDomain::NodeGraph},
    {ActionId::GraphFrame, "graph.frame", "Frame Graph", KeyScope::NodeGraph, "F", false, DirtyDomain::None},
    {ActionId::GraphComment, "graph.comment", "Wrap in Comment Box", KeyScope::NodeGraph, "Ctrl+G", false,
     DirtyDomain::NodeGraph},
    {ActionId::GraphBypass, "graph.bypass", "Toggle Bypass Selected", KeyScope::NodeGraph, "Ctrl+B", false,
     DirtyDomain::NodeGraph},
    {ActionId::GraphRecompile, "graph.recompile", "Recompile Graph Now", KeyScope::NodeGraph, "Ctrl+Enter", false,
     DirtyDomain::NodeGraph},

    {ActionId::OutlinerRename, "outliner.rename", "Rename Item", KeyScope::Outliner, "Shift+F2", false,
     DirtyDomain::None},
    {ActionId::OutlinerFilter, "outliner.filter", "Filter Outliner", KeyScope::Outliner, "Ctrl+F", false,
     DirtyDomain::None},
    {ActionId::OutlinerHide, "outliner.hide", "Hide Selected", KeyScope::Outliner, "H", false, DirtyDomain::None},
    {ActionId::OutlinerUnhideAll, "outliner.unhide_all", "Unhide All", KeyScope::Outliner, "Alt+H", false,
     DirtyDomain::None},
    {ActionId::OutlinerDelete, "outliner.delete", "Delete Selected", KeyScope::Outliner, "Del", false,
     DirtyDomain::None},

    {ActionId::LightingQuadView, "lighting.quad_view", "Lighting: Toggle Quad View", KeyScope::Global, "Ctrl+Alt+Q",
     false, DirtyDomain::CameraState},
    {ActionId::LightingPilotLight, "lighting.pilot_light", "Lighting: Pilot Selected Light", KeyScope::Global,
     "Ctrl+Alt+P", false, DirtyDomain::LightingRig},

    {ActionId::OpenProjectManager, "file.project_manager", "Project Manager", KeyScope::Global, "Shift+1", false,
     DirtyDomain::None},
    {ActionId::RedoAlt, "edit.redo_alt", "Redo (Ctrl+Shift+Z)", KeyScope::Global, "Ctrl+Shift+Z", false,
     DirtyDomain::None},
    {ActionId::ModalCancel, "ui.modal_cancel", "Close Active Dialog", KeyScope::Global, "Escape", false,
     DirtyDomain::None},
    {ActionId::ExportAlfx, "lighting.export_alfx", "Export Lighting & FX (.alfx)...", KeyScope::Lighting, "", false,
     DirtyDomain::None},
    {ActionId::ImportAlfx, "lighting.import_alfx", "Import Lighting & FX (.alfx)...", KeyScope::Lighting, "", false,
     DirtyDomain::LightingRig},
}};

class ActionRegistry {
  public:
    [[nodiscard]] static const KeyBindingSpec& spec(ActionId id) noexcept {
        const size_t idx = static_cast<size_t>(id);
        return (idx < kActionCount) ? kCanonicalActions[idx] : kCanonicalActions[0];
    }

    [[nodiscard]] static const std::array<KeyBindingSpec, kActionCount>& all_specs() noexcept {
        return kCanonicalActions;
    }

    [[nodiscard]] static const KeyBindingSpec* find_by_name(std::string_view name) noexcept {
        for (const auto& spec_entry : kCanonicalActions) {
            if (name == spec_entry.name) {
                return &spec_entry;
            }
        }
        return nullptr;
    }
};

} // namespace aether::core
