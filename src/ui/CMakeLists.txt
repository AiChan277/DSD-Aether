add_library(aether_ui STATIC
    dev_shell.cpp
    dev_shell.hpp
    modals/graphics_settings_modal.cpp
    modals/graphics_settings_modal.hpp
    palette/command_palette.cpp
    palette/command_palette.hpp
    panels/inspector/inspector_panel.cpp
    panels/inspector/inspector_panel.hpp
    panels/nodegraph/node_canvas.cpp
    panels/nodegraph/node_canvas.hpp
    panels/nodegraph/timeline_console.cpp
    panels/nodegraph/timeline_console.hpp
    panels/outliner/outliner_panel.cpp
    panels/outliner/outliner_panel.hpp
    panels/telemetry/telemetry_bar.cpp
    panels/telemetry/telemetry_bar.hpp
    panels/viewport/camera_presets.cpp
    panels/viewport/camera_presets.hpp
    panels/viewport/viewport_panel.cpp
    panels/viewport/viewport_panel.hpp
    params/param_registry.cpp
    params/param_registry.hpp
    params/project_settings.cpp
    params/project_settings.hpp
    params/undo_stack.cpp
    params/undo_stack.hpp
    theme/anim.hpp
    theme/icons.hpp
    theme/precision_dark.cpp
    theme/precision_dark.hpp
    theme/tokens.hpp
    theme/tr.hpp
    widgets/feedback.cpp
    widgets/feedback.hpp
    widgets/property_row.cpp
    widgets/property_row.hpp
    wizard/hardware_optimizer_wizard.cpp
    wizard/hardware_optimizer_wizard.hpp
    workspace/studio_types.hpp
    workspace/studio_workspace.cpp
    workspace/studio_workspace.hpp
)

aether_apply_cxx_flags(aether_ui)

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Freetype REQUIRED)

target_include_directories(aether_ui PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(aether_ui PUBLIC
    aether_rhi
    imgui::imgui
    implot::implot
    imguizmo::imguizmo
    nlohmann_json::nlohmann_json
    Freetype::Freetype
)
