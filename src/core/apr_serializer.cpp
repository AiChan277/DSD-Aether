#include "core/apr_serializer.hpp"

#include "core/zip_archive.hpp"

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bcrypt.h>
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace aether::core {
namespace {

struct RustStringGuard {
    char* ptr{nullptr};
    ~RustStringGuard() {
        if (ptr != nullptr) {
            aether_string_free(ptr);
            ptr = nullptr;
        }
    }
    RustStringGuard() = default;
    RustStringGuard(const RustStringGuard&) = delete;
    RustStringGuard& operator=(const RustStringGuard&) = delete;
};

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    std::u8string u8;
    u8.reserve(utf8.size());
    for (const char ch : utf8) {
        u8.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(u8);
}

[[nodiscard]] std::string utf8_from_path(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    std::string out;
    out.reserve(u8.size());
    for (const char8_t ch : u8) {
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

[[nodiscard]] bool is_safe_archive_relpath(std::string_view p) noexcept {
    if (p.empty() || p.front() == '/' || p.front() == '\\' || p.find(':') != std::string_view::npos) {
        return false;
    }
    size_t pos = 0u;
    while (pos < p.size()) {
        const size_t next = p.find_first_of("/\\", pos);
        const std::string_view seg = (next == std::string_view::npos) ? p.substr(pos) : p.substr(pos, next - pos);
        if (seg == "..") {
            return false;
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1u;
    }
    return true;
}

[[nodiscard]] float clean_float(float v, float fallback) noexcept {
    return std::isfinite(v) ? v : fallback;
}

template <size_t N> void read_float_array(const nlohmann::json& j, const char* key, std::array<float, N>& dst) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != N) {
        return;
    }
    for (size_t i = 0; i < N; ++i) {
        if ((*it)[i].is_number()) {
            dst[i] = clean_float((*it)[i].get<float>(), dst[i]);
        }
    }
}

[[nodiscard]] bool is_valid_png_bytes(std::span<const uint8_t> bytes) noexcept {
    static constexpr std::array<uint8_t, 8> kPngSig{0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    return bytes.size() >= 24u && std::equal(kPngSig.begin(), kPngSig.end(), bytes.begin());
}

[[nodiscard]] Expected<AprManifest> parse_manifest_json(std::span<const uint8_t> bytes) {
    try {
        const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (!j.is_object()) {
            return fail(ErrorCode::ParseError, ".apr manifest.json is not a JSON object");
        }
        if (j.value("format", std::string{}) != "APR") {
            return fail(ErrorCode::ParseError, "Invalid .apr format identifier (expected 'APR')");
        }
        const std::string ver = j.value("schema_version", std::string{});
        if (ver != AprSerializer::kSchemaVersion) {
            return fail(ErrorCode::Unsupported, "Unsupported .apr schema_version: " + ver);
        }

        AprManifest m{};
        m.format = "APR";
        m.schema_version = ver;
        m.project_uuid = j.value("project_uuid", std::string{});
        m.project_name = j.value("project_name", std::string{});
        m.app_version = j.value("app_version", std::string{"1.0.4"});
        m.created_utc = j.value("created_utc", std::string{});
        m.modified_utc = j.value("modified_utc", std::string{});

        if (const auto s_it = j.find("summary"); s_it != j.end() && s_it->is_object()) {
            m.summary.avatar_display_name = s_it->value("avatar_display_name", std::string{});
            m.summary.avatar_format = s_it->value("avatar_format", std::string{});
            m.summary.active_alfx_name = s_it->value("active_alfx_name", std::string{});
            m.summary.active_light_count = s_it->value("active_light_count", 0u);
            m.summary.render_tier = s_it->value("render_tier", std::string{"HybridRT"});
            m.summary.has_thumbnail = s_it->value("has_thumbnail", false);
        }

        const auto c_it = j.find("checksums_sha256");
        if (c_it == j.end() || !c_it->is_object()) {
            return fail(ErrorCode::ParseError, ".apr manifest.json missing checksums_sha256");
        }
        m.sha256_project_json = c_it->value("project.json", std::string{});
        m.sha256_lighting_alfx = c_it->value("lighting.alfx", std::string{});
        m.sha256_node_graph_json = c_it->value("node_graph.json", std::string{});
        if (m.sha256_project_json.empty() || m.sha256_lighting_alfx.empty() || m.sha256_node_graph_json.empty()) {
            return fail(ErrorCode::ParseError, ".apr manifest.json checksums_sha256 incomplete");
        }
        return m;
    } catch (const std::exception& e) {
        return fail(ErrorCode::ParseError, std::string("Failed to parse .apr manifest.json: ") + e.what());
    }
}

[[nodiscard]] nlohmann::json serialize_project_json(const StudioStateStore& st,
                                                    const std::filesystem::path& dst_apr_path) {
    std::string rel_path = st.avatar.path_relative;
    if (rel_path.empty() && !st.avatar.path_absolute.empty() && dst_apr_path.has_parent_path()) {
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::relative(path_from_utf8(st.avatar.path_absolute), dst_apr_path.parent_path(), ec);
        if (!ec && !rel.empty()) {
            rel_path = utf8_from_path(rel);
            std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
        }
    }

    nlohmann::json custom_presets_arr = nlohmann::json::array();
    for (const auto& cp : st.camera.custom_presets) {
        custom_presets_arr.push_back({
            {"id", cp.id},
            {"name", cp.name},
            {"target", {cp.target[0], cp.target[1], cp.target[2]}},
            {"distance", cp.distance},
            {"yaw_deg", cp.yaw_deg},
            {"pitch_deg", cp.pitch_deg},
            {"fov_y_deg", cp.fov_y_deg},
            {"avatar_offset", {cp.avatar_offset[0], cp.avatar_offset[1], cp.avatar_offset[2]}},
            {"avatar_rot_y_deg", cp.avatar_rot_y_deg},
            {"avatar_scale", cp.avatar_scale},
            {"has_avatar_xform", cp.has_avatar_xform},
        });
    }

    return nlohmann::json{
        {"schema_version", AprSerializer::kSchemaVersion},
        {"meta",
         {{"project_uuid", st.meta.project_uuid},
          {"project_name", st.meta.project_name},
          {"app_version", st.meta.app_version},
          {"created_utc", st.meta.created_utc},
          {"modified_utc", st.meta.modified_utc},
          {"avatar_display_name", st.meta.avatar_display_name},
          {"render_tier", st.meta.render_tier}}},
        {"avatar",
         {{"format", st.avatar.format},
          {"path_absolute", st.avatar.path_absolute},
          {"path_relative", rel_path},
          {"file_sha256", st.avatar.file_sha256},
          {"root_transform",
           {{"pos", {st.avatar.root_pos[0], st.avatar.root_pos[1], st.avatar.root_pos[2]}},
            {"rot_deg", {st.avatar.root_rot_deg[0], st.avatar.root_rot_deg[1], st.avatar.root_rot_deg[2]}},
            {"uniform_scale", st.avatar.uniform_scale}}},
          {"physics",
           {{"enabled", st.avatar.physics_enabled},
            {"spring_stiffness", st.avatar.spring_stiffness},
            {"spring_damping", st.avatar.spring_damping},
            {"pmx_rigidbody_damping", st.avatar.pmx_rigidbody_damping},
            {"pmx_velocity_clamp", st.avatar.pmx_velocity_clamp}}},
          {"active_expression_slot", st.avatar.active_expression_slot},
          {"expression_gain", st.avatar.expression_gain}}},
        {"lighting_link",
         {{"mode", st.lighting_link.mode},
          {"external_alfx_path", st.lighting_link.external_alfx_path},
          {"last_synced_utc", st.lighting_link.last_synced_utc}}},
        {"camera",
         {{"active_preset_id", st.camera.active_preset_id},
          {"aspect_mode", st.camera.aspect_mode},
          {"transition_smooth_sec", st.camera.transition_smooth_sec},
          {"live_camera_state",
           {{"target",
             {st.camera.live_camera_state.target[0], st.camera.live_camera_state.target[1],
              st.camera.live_camera_state.target[2]}},
            {"distance", st.camera.live_camera_state.distance},
            {"yaw_deg", st.camera.live_camera_state.yaw_deg},
            {"pitch_deg", st.camera.live_camera_state.pitch_deg},
            {"fov_y_deg", st.camera.live_camera_state.fov_y_deg}}},
          {"custom_presets", std::move(custom_presets_arr)}}},
        {"lighting_stage_camera",
         {{"view_mode", st.lighting_stage_camera.view_mode},
          {"quad_view_enabled", st.lighting_stage_camera.quad_view_enabled},
          {"pip_visible", st.lighting_stage_camera.pip_visible},
          {"diagnostic_mode", st.lighting_stage_camera.diagnostic_mode},
          {"show_schematics", st.lighting_stage_camera.show_schematics},
          {"show_grid", st.lighting_stage_camera.show_grid},
          {"preview_25d_flatten", st.lighting_stage_camera.preview_25d_flatten},
          {"stage_cam_state",
           {{"target",
             {st.lighting_stage_camera.target[0], st.lighting_stage_camera.target[1],
              st.lighting_stage_camera.target[2]}},
            {"distance", st.lighting_stage_camera.distance},
            {"yaw_deg", st.lighting_stage_camera.yaw_deg},
            {"pitch_deg", st.lighting_stage_camera.pitch_deg},
            {"ortho_half_height", st.lighting_stage_camera.ortho_half_height}}}}},
        {"tracking_and_audio",
         {{"camera_device_id", st.tracking_and_audio.camera_device_id},
          {"npu_tier", st.tracking_and_audio.npu_tier},
          {"hand_tracking_enabled", st.tracking_and_audio.hand_tracking_enabled},
          {"show_tracking_hud", st.tracking_and_audio.show_tracking_hud},
          {"tracking_hud_collapsed", st.tracking_and_audio.tracking_hud_collapsed},
          {"npu_movement_bounds",
           {{"min",
             {st.tracking_and_audio.npu_movement_bounds_min[0], st.tracking_and_audio.npu_movement_bounds_min[1],
              st.tracking_and_audio.npu_movement_bounds_min[2]}},
            {"max",
             {st.tracking_and_audio.npu_movement_bounds_max[0], st.tracking_and_audio.npu_movement_bounds_max[1],
              st.tracking_and_audio.npu_movement_bounds_max[2]}}}},
          {"eye_blink_gain", st.tracking_and_audio.eye_blink_gain},
          {"brow_gain", st.tracking_and_audio.brow_gain},
          {"mouth_gain", st.tracking_and_audio.mouth_gain},
          {"audio_lipsync_gain", st.tracking_and_audio.audio_lipsync_gain},
          {"audio_device_id", st.tracking_and_audio.audio_device_id}}},
        {"outputs",
         {{"spout_enabled", st.outputs.spout_enabled},
          {"spout_sender_name", st.outputs.spout_sender_name},
          {"spout_premul_alpha", st.outputs.spout_premul_alpha},
          {"non_alpha_bg", static_cast<int>(st.outputs.non_alpha_bg)},
          {"custom_bg_rgb", {st.outputs.custom_bg_rgb[0], st.outputs.custom_bg_rgb[1], st.outputs.custom_bg_rgb[2]}},
          {"vcam_enabled", st.outputs.vcam_enabled},
          {"ndi_enabled", st.outputs.ndi_enabled},
          {"osc_enabled", st.outputs.osc_enabled},
          {"osc_host", st.outputs.osc_host},
          {"osc_port", st.outputs.osc_port},
          {"resolution_width", st.outputs.resolution_width},
          {"resolution_height", st.outputs.resolution_height},
          {"fps", st.outputs.fps},
          {"dsp_user_offset_ms", st.outputs.dsp_user_offset_ms}}},
    };
}

[[nodiscard]] Expected<void> deserialize_project_json(std::span<const uint8_t> bytes, StudioStateStore& st) {
    try {
        const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (!j.is_object()) {
            return fail(ErrorCode::ParseError, "project.json is not a JSON object");
        }
        if (const std::string ver = j.value("schema_version", std::string{});
            !ver.empty() && ver != AprSerializer::kSchemaVersion) {
            return fail(ErrorCode::Unsupported, "Unsupported project.json schema_version: " + ver);
        }

        if (const auto it = j.find("meta"); it != j.end() && it->is_object()) {
            st.meta.project_uuid = it->value("project_uuid", st.meta.project_uuid);
            st.meta.project_name = it->value("project_name", st.meta.project_name);
            st.meta.app_version = it->value("app_version", st.meta.app_version);
            st.meta.created_utc = it->value("created_utc", st.meta.created_utc);
            st.meta.modified_utc = it->value("modified_utc", st.meta.modified_utc);
            st.meta.avatar_display_name = it->value("avatar_display_name", st.meta.avatar_display_name);
            st.meta.render_tier = it->value("render_tier", st.meta.render_tier);
        }

        if (const auto av = j.find("avatar"); av != j.end() && av->is_object()) {
            st.avatar.format = av->value("format", st.avatar.format);
            st.avatar.path_absolute = av->value("path_absolute", st.avatar.path_absolute);
            st.avatar.path_relative = av->value("path_relative", st.avatar.path_relative);
            st.avatar.file_sha256 = av->value("file_sha256", st.avatar.file_sha256);
            st.avatar.active_expression_slot = av->value("active_expression_slot", st.avatar.active_expression_slot);
            st.avatar.expression_gain = clean_float(av->value("expression_gain", st.avatar.expression_gain), 1.0f);
            if (const auto rt = av->find("root_transform"); rt != av->end() && rt->is_object()) {
                read_float_array(*rt, "pos", st.avatar.root_pos);
                read_float_array(*rt, "rot_deg", st.avatar.root_rot_deg);
                st.avatar.uniform_scale = clean_float(rt->value("uniform_scale", st.avatar.uniform_scale), 1.0f);
            }
            if (const auto ph = av->find("physics"); ph != av->end() && ph->is_object()) {
                st.avatar.physics_enabled = ph->value("enabled", st.avatar.physics_enabled);
                st.avatar.spring_stiffness =
                    clean_float(ph->value("spring_stiffness", st.avatar.spring_stiffness), 1.0f);
                st.avatar.spring_damping = clean_float(ph->value("spring_damping", st.avatar.spring_damping), 1.0f);
                st.avatar.pmx_rigidbody_damping =
                    clean_float(ph->value("pmx_rigidbody_damping", st.avatar.pmx_rigidbody_damping), 0.92f);
                st.avatar.pmx_velocity_clamp =
                    clean_float(ph->value("pmx_velocity_clamp", st.avatar.pmx_velocity_clamp), 6.5f);
            }
        }

        if (const auto ll = j.find("lighting_link"); ll != j.end() && ll->is_object()) {
            st.lighting_link.mode = ll->value("mode", st.lighting_link.mode);
            st.lighting_link.external_alfx_path = ll->value("external_alfx_path", st.lighting_link.external_alfx_path);
            st.lighting_link.last_synced_utc = ll->value("last_synced_utc", st.lighting_link.last_synced_utc);
        }

        if (const auto cam = j.find("camera"); cam != j.end() && cam->is_object()) {
            st.camera.active_preset_id = cam->value("active_preset_id", st.camera.active_preset_id);
            st.camera.aspect_mode = cam->value("aspect_mode", st.camera.aspect_mode);
            st.camera.transition_smooth_sec =
                clean_float(cam->value("transition_smooth_sec", st.camera.transition_smooth_sec), 0.22f);
            if (const auto lc = cam->find("live_camera_state"); lc != cam->end() && lc->is_object()) {
                read_float_array(*lc, "target", st.camera.live_camera_state.target);
                st.camera.live_camera_state.distance =
                    clean_float(lc->value("distance", st.camera.live_camera_state.distance), 1.45f);
                st.camera.live_camera_state.yaw_deg =
                    clean_float(lc->value("yaw_deg", st.camera.live_camera_state.yaw_deg), 0.0f);
                st.camera.live_camera_state.pitch_deg =
                    clean_float(lc->value("pitch_deg", st.camera.live_camera_state.pitch_deg), -2.0f);
                st.camera.live_camera_state.fov_y_deg =
                    clean_float(lc->value("fov_y_deg", st.camera.live_camera_state.fov_y_deg), 24.0f);
            }
            if (const auto cp_arr = cam->find("custom_presets"); cp_arr != cam->end() && cp_arr->is_array()) {
                st.camera.custom_presets.clear();
                for (const auto& item : *cp_arr) {
                    if (!item.is_object()) {
                        continue;
                    }
                    ProjectCustomCameraPreset cp{};
                    cp.id = item.value("id", std::string{});
                    cp.name = item.value("name", std::string{});
                    read_float_array(item, "target", cp.target);
                    cp.distance = clean_float(item.value("distance", cp.distance), 1.45f);
                    cp.yaw_deg = clean_float(item.value("yaw_deg", cp.yaw_deg), 0.0f);
                    cp.pitch_deg = clean_float(item.value("pitch_deg", cp.pitch_deg), 0.0f);
                    cp.fov_y_deg = clean_float(item.value("fov_y_deg", cp.fov_y_deg), 24.0f);
                    read_float_array(item, "avatar_offset", cp.avatar_offset);
                    cp.avatar_rot_y_deg = clean_float(item.value("avatar_rot_y_deg", cp.avatar_rot_y_deg), 0.0f);
                    cp.avatar_scale = clean_float(item.value("avatar_scale", cp.avatar_scale), 1.0f);
                    cp.has_avatar_xform = item.value("has_avatar_xform", cp.has_avatar_xform);
                    st.camera.custom_presets.push_back(std::move(cp));
                }
            }
        }

        if (const auto lsc = j.find("lighting_stage_camera"); lsc != j.end() && lsc->is_object()) {
            st.lighting_stage_camera.view_mode = lsc->value("view_mode", st.lighting_stage_camera.view_mode);
            st.lighting_stage_camera.quad_view_enabled =
                lsc->value("quad_view_enabled", st.lighting_stage_camera.quad_view_enabled);
            st.lighting_stage_camera.pip_visible = lsc->value("pip_visible", st.lighting_stage_camera.pip_visible);
            st.lighting_stage_camera.diagnostic_mode =
                lsc->value("diagnostic_mode", st.lighting_stage_camera.diagnostic_mode);
            st.lighting_stage_camera.show_schematics =
                lsc->value("show_schematics", st.lighting_stage_camera.show_schematics);
            st.lighting_stage_camera.show_grid = lsc->value("show_grid", st.lighting_stage_camera.show_grid);
            st.lighting_stage_camera.preview_25d_flatten =
                lsc->value("preview_25d_flatten", st.lighting_stage_camera.preview_25d_flatten);
            if (const auto scs = lsc->find("stage_cam_state"); scs != lsc->end() && scs->is_object()) {
                read_float_array(*scs, "target", st.lighting_stage_camera.target);
                st.lighting_stage_camera.distance =
                    clean_float(scs->value("distance", st.lighting_stage_camera.distance), 3.1f);
                st.lighting_stage_camera.yaw_deg =
                    clean_float(scs->value("yaw_deg", st.lighting_stage_camera.yaw_deg), 38.0f);
                st.lighting_stage_camera.pitch_deg =
                    clean_float(scs->value("pitch_deg", st.lighting_stage_camera.pitch_deg), 24.0f);
                st.lighting_stage_camera.ortho_half_height =
                    clean_float(scs->value("ortho_half_height", st.lighting_stage_camera.ortho_half_height), 1.35f);
            }
        }

        if (const auto tr = j.find("tracking_and_audio"); tr != j.end() && tr->is_object()) {
            st.tracking_and_audio.camera_device_id =
                tr->value("camera_device_id", st.tracking_and_audio.camera_device_id);
            st.tracking_and_audio.npu_tier = tr->value("npu_tier", st.tracking_and_audio.npu_tier);
            st.tracking_and_audio.hand_tracking_enabled =
                tr->value("hand_tracking_enabled", st.tracking_and_audio.hand_tracking_enabled);
            st.tracking_and_audio.show_tracking_hud =
                tr->value("show_tracking_hud", st.tracking_and_audio.show_tracking_hud);
            st.tracking_and_audio.tracking_hud_collapsed =
                tr->value("tracking_hud_collapsed", st.tracking_and_audio.tracking_hud_collapsed);
            if (const auto bnd = tr->find("npu_movement_bounds"); bnd != tr->end() && bnd->is_object()) {
                read_float_array(*bnd, "min", st.tracking_and_audio.npu_movement_bounds_min);
                read_float_array(*bnd, "max", st.tracking_and_audio.npu_movement_bounds_max);
            }
            st.tracking_and_audio.eye_blink_gain =
                clean_float(tr->value("eye_blink_gain", st.tracking_and_audio.eye_blink_gain), 1.0f);
            st.tracking_and_audio.brow_gain =
                clean_float(tr->value("brow_gain", st.tracking_and_audio.brow_gain), 1.0f);
            st.tracking_and_audio.mouth_gain =
                clean_float(tr->value("mouth_gain", st.tracking_and_audio.mouth_gain), 1.0f);
            st.tracking_and_audio.audio_lipsync_gain =
                clean_float(tr->value("audio_lipsync_gain", st.tracking_and_audio.audio_lipsync_gain), 1.0f);
            st.tracking_and_audio.audio_device_id = tr->value("audio_device_id", st.tracking_and_audio.audio_device_id);
        }

        if (const auto out_it = j.find("outputs"); out_it != j.end() && out_it->is_object()) {
            st.outputs.spout_enabled = out_it->value("spout_enabled", st.outputs.spout_enabled);
            st.outputs.spout_sender_name = out_it->value("spout_sender_name", st.outputs.spout_sender_name);
            st.outputs.spout_premul_alpha = out_it->value("spout_premul_alpha", st.outputs.spout_premul_alpha);
            st.outputs.non_alpha_bg = static_cast<uint8_t>(
                std::clamp(out_it->value("non_alpha_bg", static_cast<int>(st.outputs.non_alpha_bg)), 0, 2));
            read_float_array(*out_it, "custom_bg_rgb", st.outputs.custom_bg_rgb);
            st.outputs.vcam_enabled = out_it->value("vcam_enabled", st.outputs.vcam_enabled);
            st.outputs.ndi_enabled = out_it->value("ndi_enabled", st.outputs.ndi_enabled);
            st.outputs.osc_enabled = out_it->value("osc_enabled", st.outputs.osc_enabled);
            st.outputs.osc_host = out_it->value("osc_host", st.outputs.osc_host);
            st.outputs.osc_port = static_cast<uint16_t>(
                std::clamp(out_it->value("osc_port", static_cast<int>(st.outputs.osc_port)), 1, 65535));
            st.outputs.resolution_width =
                std::clamp(out_it->value("resolution_width", st.outputs.resolution_width), 320u, 7680u);
            st.outputs.resolution_height =
                std::clamp(out_it->value("resolution_height", st.outputs.resolution_height), 240u, 4320u);
            st.outputs.fps = std::clamp(out_it->value("fps", st.outputs.fps), 24u, 240u);
            st.outputs.dsp_user_offset_ms =
                clean_float(out_it->value("dsp_user_offset_ms", st.outputs.dsp_user_offset_ms), 0.0f);
        }

        return {};
    } catch (const std::exception& e) {
        return fail(ErrorCode::ParseError, std::string("Failed to parse .apr project.json: ") + e.what());
    }
}

[[nodiscard]] std::string trim_copy(std::string_view sv) {
    size_t b = 0u;
    size_t e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1u]))) {
        --e;
    }
    return std::string(sv.substr(b, e - b));
}

[[nodiscard]] std::string strip_quotes(std::string_view sv) {
    std::string s = trim_copy(sv);
    if (s.size() >= 2u && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1u, s.size() - 2u);
    }
    return s;
}

using TomlTable = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] TomlTable parse_flat_toml_sections(std::string_view text) {
    TomlTable out;
    std::string section;
    size_t pos = 0u;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1u;

        bool in_quote = false;
        char quote_ch = '\0';
        size_t comment_pos = line.size();
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (!in_quote && (c == '"' || c == '\'')) {
                in_quote = true;
                quote_ch = c;
            } else if (in_quote && c == quote_ch) {
                in_quote = false;
            } else if (!in_quote && c == '#') {
                comment_pos = i;
                break;
            }
        }
        const std::string trimmed = trim_copy(line.substr(0u, comment_pos));
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            std::string_view inner(trimmed);
            if (inner.size() >= 4u && inner[1] == '[' && inner[inner.size() - 2u] == ']') {
                section = trim_copy(inner.substr(2u, inner.size() - 4u));
            } else {
                section = trim_copy(inner.substr(1u, inner.size() - 2u));
            }
            continue;
        }
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = trim_copy(std::string_view(trimmed).substr(0u, eq));
        const std::string v = trim_copy(std::string_view(trimmed).substr(eq + 1u));
        std::string fq;
        if (section.empty()) {
            fq = k;
        } else {
            fq = section;
            fq += '.';
            fq += k;
        }
        out[fq] = v;
    }
    return out;
}

[[nodiscard]] bool parse_toml_float(const TomlTable& t, std::string_view key, float& out) noexcept {
    const auto it = t.find(key);
    if (it == t.end()) {
        return false;
    }
    char* endptr = nullptr;
    const float v = std::strtof(it->second.c_str(), &endptr);
    if (endptr != it->second.c_str() && std::isfinite(v)) {
        out = v;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_toml_bool(const TomlTable& t, std::string_view key, bool& out) noexcept {
    const auto it = t.find(key);
    if (it == t.end()) {
        return false;
    }
    if (it->second == "true") {
        out = true;
        return true;
    }
    if (it->second == "false") {
        out = false;
        return true;
    }
    return false;
}

template <size_t N> void parse_toml_float_array(const TomlTable& t, std::string_view key, std::array<float, N>& out) {
    const auto it = t.find(key);
    if (it == t.end() || it->second.size() < 3u || it->second.front() != '[') {
        return;
    }
    std::string inner = it->second.substr(1u);
    if (!inner.empty() && inner.back() == ']') {
        inner.pop_back();
    }
    size_t idx = 0u;
    size_t pos = 0u;
    while (pos < inner.size() && idx < N) {
        const size_t comma = inner.find(',', pos);
        const std::string tok = trim_copy(
            std::string_view(inner).substr(pos, comma == std::string::npos ? inner.size() - pos : comma - pos));
        char* endptr = nullptr;
        const float val = std::strtof(tok.c_str(), &endptr);
        if (endptr != tok.c_str() && std::isfinite(val)) {
            out[idx] = val;
        }
        ++idx;
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1u;
    }
}

void copy_light_name(std::array<char, 40>& dst, std::string_view src) noexcept {
    dst.fill('\0');
    const size_t n = std::min(src.size(), dst.size() - 1u);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

} // namespace

std::string AprSerializer::generate_uuid_v4() {
    std::array<uint8_t, 16> raw{};
#if defined(_WIN32)
    (void)BCryptGenRandom(nullptr, raw.data(), static_cast<ULONG>(raw.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#endif
    raw[6] = static_cast<uint8_t>((raw[6] & 0x0Fu) | 0x40u);
    raw[8] = static_cast<uint8_t>((raw[8] & 0x3Fu) | 0x80u);
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", raw[0],
                  raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12],
                  raw[13], raw[14], raw[15]);
    return std::string(buf);
}

std::string AprSerializer::current_utc_timestamp() {
#if defined(_WIN32)
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32]{};
    const int n = std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
                                st.wHour, st.wMinute, st.wSecond);
    return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string("1970-01-01T00:00:00Z");
#else
    return "1970-01-01T00:00:00Z";
#endif
}

Expected<AprManifest> AprSerializer::read_manifest_fast(const std::filesystem::path& apr_path,
                                                        std::vector<uint8_t>* out_thumbnail_png) {
    auto zr = ZipReader::open(apr_path);
    if (!zr) {
        return make_unexpected(zr.error());
    }
    auto man_bytes = zr.value().read_entry("manifest.json");
    if (!man_bytes) {
        return make_unexpected(man_bytes.error());
    }
    auto man = parse_manifest_json(man_bytes.value());
    if (!man) {
        return man;
    }
    if (out_thumbnail_png != nullptr) {
        out_thumbnail_png->clear();
        if (zr.value().contains("thumbnail.png")) {
            if (auto thumb = zr.value().read_entry("thumbnail.png"); thumb) {
                *out_thumbnail_png = std::move(thumb.value());
            }
        }
    }
    return man;
}

Expected<AprLoadedBundle> AprSerializer::load_file(const std::filesystem::path& apr_path) {
    auto zr = ZipReader::open(apr_path);
    if (!zr) {
        return make_unexpected(zr.error());
    }
    auto crc_ok = zr.value().verify_all();
    if (!crc_ok) {
        return make_unexpected(crc_ok.error());
    }

    auto man_bytes = zr.value().read_entry("manifest.json");
    if (!man_bytes) {
        return make_unexpected(man_bytes.error());
    }
    auto man_res = parse_manifest_json(man_bytes.value());
    if (!man_res) {
        return make_unexpected(man_res.error());
    }

    auto proj_bytes = zr.value().read_entry("project.json");
    if (!proj_bytes) {
        return make_unexpected(proj_bytes.error());
    }
    auto alfx_bytes = zr.value().read_entry("lighting.alfx");
    if (!alfx_bytes) {
        return make_unexpected(alfx_bytes.error());
    }
    auto graph_bytes = zr.value().read_entry("node_graph.json");
    if (!graph_bytes) {
        return make_unexpected(graph_bytes.error());
    }

    const auto check_hash = [](std::span<const uint8_t> data, const std::string& expected_hex,
                               const char* name) -> Expected<void> {
        auto actual = sha256_hex(data);
        if (!actual) {
            return make_unexpected(actual.error());
        }
        if (actual.value() != expected_hex) {
            return fail(ErrorCode::ParseError, std::string("SHA-256 mismatch for ") + name);
        }
        return {};
    };
    if (auto r = check_hash(proj_bytes.value(), man_res.value().sha256_project_json, "project.json"); !r) {
        return make_unexpected(r.error());
    }
    if (auto r = check_hash(alfx_bytes.value(), man_res.value().sha256_lighting_alfx, "lighting.alfx"); !r) {
        return make_unexpected(r.error());
    }
    if (auto r = check_hash(graph_bytes.value(), man_res.value().sha256_node_graph_json, "node_graph.json"); !r) {
        return make_unexpected(r.error());
    }

    AprLoadedBundle bundle{};
    bundle.manifest = std::move(man_res.value());
    if (auto pr = deserialize_project_json(proj_bytes.value(), bundle.state); !pr) {
        return make_unexpected(pr.error());
    }
    auto alfx_doc = AlfxSerializer::deserialize_from_bytes(alfx_bytes.value());
    if (!alfx_doc) {
        return make_unexpected(alfx_doc.error());
    }
    bundle.lighting_rig = alfx_doc.value().rig;
    bundle.lighting_meta = alfx_doc.value().meta;

    try {
        const nlohmann::json gj = nlohmann::json::parse(graph_bytes.value().begin(), graph_bytes.value().end());
        if (!gj.is_object()) {
            return fail(ErrorCode::ParseError, ".apr node_graph.json must be a JSON object");
        }
        bundle.state.node_graph_json.assign(reinterpret_cast<const char*>(graph_bytes.value().data()),
                                            graph_bytes.value().size());
    } catch (const std::exception& e) {
        return fail(ErrorCode::ParseError, std::string("Invalid .apr node_graph.json: ") + e.what());
    }

    if (zr.value().contains("thumbnail.png")) {
        auto png = zr.value().read_entry("thumbnail.png");
        if (!png) {
            return make_unexpected(png.error());
        }
        bundle.thumbnail_png = std::move(png.value());
    }

    for (const auto& info : zr.value().entries()) {
        if (info.path.rfind("assets/", 0) == 0 && is_safe_archive_relpath(info.path)) {
            auto asset_bytes = zr.value().read_entry(info.path);
            if (!asset_bytes) {
                return make_unexpected(asset_bytes.error());
            }
            bundle.state.packed_assets.push_back(PackedAssetEntry{info.path, std::move(asset_bytes.value())});
        }
    }

    bundle.state.clear_dirty();
    return bundle;
}

Expected<void> AprSerializer::apply_loaded_graph_to_host(const AprLoadedBundle& bundle, AetherHostHandle rust_host) {
    if (rust_host == nullptr) {
        return fail(ErrorCode::InvalidArg, "Null AetherHostHandle");
    }
    const std::string& json_str = bundle.state.node_graph_json;
    const AetherStatus st = aether_graph_deserialize_json(rust_host, json_str.data(), json_str.size());
    if (st != AETHER_OK) {
        char err_buf[256]{};
        const uintptr_t err_len = aether_last_error(err_buf, sizeof(err_buf));
        const std::string msg = (err_len > 0u) ? std::string(err_buf, std::min<size_t>(err_len, sizeof(err_buf) - 1u))
                                               : std::string("aether_graph_deserialize_json failed");
        return fail(ErrorCode::ParseError, msg);
    }
    return {};
}

Expected<AprManifest> AprSerializer::save_file(const std::filesystem::path& dst_apr_path, const AprSaveInput& input) {
    if (input.state == nullptr || input.lighting == nullptr) {
        return fail(ErrorCode::InvalidArg, "AprSaveInput requires non-null state and lighting");
    }

    StudioStateStore state_copy = *input.state;
    if (state_copy.meta.project_uuid.empty()) {
        state_copy.meta.project_uuid = generate_uuid_v4();
    }
    if (state_copy.meta.project_name.empty() || state_copy.meta.project_name == "Untitled Project") {
        const std::string stem = utf8_from_path(dst_apr_path.stem());
        if (!stem.empty()) {
            state_copy.meta.project_name = stem;
        }
    }
    const std::string now_utc = current_utc_timestamp();
    if (state_copy.meta.created_utc.empty()) {
        state_copy.meta.created_utc = now_utc;
    }
    state_copy.meta.modified_utc = now_utc;
    state_copy.lighting_link.last_synced_utc = now_utc;

    std::string node_graph_json = state_copy.node_graph_json;
    if (input.rust_host != nullptr) {
        RustStringGuard str_guard{};
        uintptr_t out_len = 0u;
        const AetherStatus st = aether_graph_serialize_json(input.rust_host, &str_guard.ptr, &out_len);
        if (st != AETHER_OK || str_guard.ptr == nullptr) {
            return fail(ErrorCode::Internal, "aether_graph_serialize_json failed");
        }
        node_graph_json.assign(str_guard.ptr, static_cast<size_t>(out_len));
    } else if (node_graph_json.empty()) {
        node_graph_json = "{}";
    }

    std::string project_json_str;
    try {
        project_json_str = serialize_project_json(state_copy, dst_apr_path).dump(2);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Failed serializing project.json: ") + e.what());
    }

    auto alfx_str = AlfxSerializer::serialize_to_json_string(*input.lighting, input.lighting_meta);
    if (!alfx_str) {
        return make_unexpected(alfx_str.error());
    }

    std::vector<uint8_t> thumb_png;
    if (!input.thumbnail_pixels.empty() && input.thumb_width > 0u && input.thumb_height > 0u) {
        auto encoded = encode_thumbnail_png_480x270(input.thumbnail_pixels, input.thumb_width, input.thumb_height,
                                                    input.thumb_is_bgra);
        if (!encoded) {
            return make_unexpected(encoded.error());
        }
        thumb_png = std::move(encoded.value());
    } else if (!input.preencoded_thumbnail_png.empty()) {
        if (!is_valid_png_bytes(input.preencoded_thumbnail_png)) {
            return fail(ErrorCode::InvalidArg, "preencoded_thumbnail_png is not valid PNG data");
        }
        thumb_png.assign(input.preencoded_thumbnail_png.begin(), input.preencoded_thumbnail_png.end());
    }

    const std::vector<uint8_t> proj_bytes(project_json_str.begin(), project_json_str.end());
    const std::vector<uint8_t> alfx_bytes(alfx_str.value().begin(), alfx_str.value().end());
    const std::vector<uint8_t> graph_bytes(node_graph_json.begin(), node_graph_json.end());

    auto h_proj = sha256_hex(proj_bytes);
    auto h_alfx = sha256_hex(alfx_bytes);
    auto h_graph = sha256_hex(graph_bytes);
    if (!h_proj || !h_alfx || !h_graph) {
        return fail(ErrorCode::Internal, "SHA-256 computation failed while saving .apr");
    }

    AprManifest manifest{};
    manifest.format = "APR";
    manifest.schema_version = kSchemaVersion;
    manifest.project_uuid = state_copy.meta.project_uuid;
    manifest.project_name = state_copy.meta.project_name;
    manifest.app_version = state_copy.meta.app_version;
    manifest.created_utc = state_copy.meta.created_utc;
    manifest.modified_utc = state_copy.meta.modified_utc;
    manifest.summary.avatar_display_name = state_copy.meta.avatar_display_name;
    manifest.summary.avatar_format = state_copy.avatar.format;
    manifest.summary.active_alfx_name = std::string(input.lighting->active_alfx_name());
    manifest.summary.active_light_count = input.lighting->active_light_count();
    manifest.summary.render_tier = state_copy.meta.render_tier;
    manifest.summary.has_thumbnail = !thumb_png.empty();
    manifest.sha256_project_json = h_proj.value();
    manifest.sha256_lighting_alfx = h_alfx.value();
    manifest.sha256_node_graph_json = h_graph.value();

    std::string manifest_str;
    try {
        const nlohmann::json mj{
            {"format", manifest.format},
            {"schema_version", manifest.schema_version},
            {"project_uuid", manifest.project_uuid},
            {"project_name", manifest.project_name},
            {"app_version", manifest.app_version},
            {"created_utc", manifest.created_utc},
            {"modified_utc", manifest.modified_utc},
            {"summary",
             {{"avatar_display_name", manifest.summary.avatar_display_name},
              {"avatar_format", manifest.summary.avatar_format},
              {"active_alfx_name", manifest.summary.active_alfx_name},
              {"active_light_count", manifest.summary.active_light_count},
              {"render_tier", manifest.summary.render_tier},
              {"has_thumbnail", manifest.summary.has_thumbnail}}},
            {"checksums_sha256",
             {{"project.json", manifest.sha256_project_json},
              {"lighting.alfx", manifest.sha256_lighting_alfx},
              {"node_graph.json", manifest.sha256_node_graph_json}}},
        };
        manifest_str = mj.dump(2);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Failed serializing manifest.json: ") + e.what());
    }

    std::vector<ZipEntry> entries;
    entries.reserve(6u + state_copy.packed_assets.size());
    entries.push_back(ZipEntry{"manifest.json", std::vector<uint8_t>(manifest_str.begin(), manifest_str.end()), true});
    entries.push_back(ZipEntry{"project.json", proj_bytes, true});
    entries.push_back(ZipEntry{"lighting.alfx", alfx_bytes, true});
    entries.push_back(ZipEntry{"node_graph.json", graph_bytes, true});
    if (!thumb_png.empty()) {
        entries.push_back(ZipEntry{"thumbnail.png", std::move(thumb_png), false});
    }
    for (const auto& asset : state_copy.packed_assets) {
        std::string path = asset.archive_path;
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.rfind("assets/", 0) != 0) {
            path.insert(0, "assets/overrides/");
        }
        if (!is_safe_archive_relpath(path)) {
            return fail(ErrorCode::InvalidArg, "Unsafe packed asset path in .apr: " + asset.archive_path);
        }
        entries.push_back(ZipEntry{std::move(path), asset.bytes, true});
    }

    auto zip_bytes = ZipWriter::build_bytes(entries);
    if (!zip_bytes) {
        return make_unexpected(zip_bytes.error());
    }

    const auto verify_tmp = [&input](const std::filesystem::path& tmp_path) -> Expected<void> {
        auto zr = ZipReader::open(tmp_path);
        if (!zr) {
            return make_unexpected(zr.error());
        }
        if (auto v = zr.value().verify_all(); !v) {
            return v;
        }
        if (input.pre_commit_hook) {
            if (auto hook_res = input.pre_commit_hook(tmp_path); !hook_res) {
                return hook_res;
            }
        }
        return {};
    };

    auto commit_res = write_bytes_atomic(dst_apr_path, zip_bytes.value(), verify_tmp);
    if (!commit_res) {
        return make_unexpected(commit_res.error());
    }
    return manifest;
}

std::filesystem::path AprSerializer::resolve_avatar_path(const std::filesystem::path& apr_path,
                                                         const AvatarProjectState& avatar) {
    std::error_code ec;
    if (!avatar.path_relative.empty()) {
        const std::filesystem::path candidate =
            (apr_path.parent_path() / path_from_utf8(avatar.path_relative)).lexically_normal();
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    if (!avatar.path_absolute.empty()) {
        const std::filesystem::path abs_path = path_from_utf8(avatar.path_absolute);
        if (std::filesystem::exists(abs_path, ec)) {
            return abs_path;
        }
    }
    return !avatar.path_relative.empty()
               ? (apr_path.parent_path() / path_from_utf8(avatar.path_relative)).lexically_normal()
               : path_from_utf8(avatar.path_absolute);
}

bool LegacyAetherMigrator::is_legacy_aether_path(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }
    if (std::filesystem::is_directory(path, ec)) {
        return std::filesystem::exists(path / "project.toml", ec);
    }
    std::string ext = utf8_from_path(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".aether" || ext == ".json";
}

Expected<AprLoadedBundle> LegacyAetherMigrator::migrate_to_bundle(const std::filesystem::path& src_aether_path) {
    std::error_code ec;
    if (!std::filesystem::exists(src_aether_path, ec)) {
        return fail(ErrorCode::IoError, "Legacy .aether path does not exist");
    }

    AprLoadedBundle out{};
    out.manifest.format = "APR";
    out.manifest.schema_version = AprSerializer::kSchemaVersion;
    out.manifest.project_uuid = AprSerializer::generate_uuid_v4();
    out.manifest.project_name = utf8_from_path(src_aether_path.stem());
    if (out.manifest.project_name.empty()) {
        out.manifest.project_name = "Migrated_Project";
    }
    const std::string utc = AprSerializer::current_utc_timestamp();
    out.manifest.created_utc = utc;
    out.manifest.modified_utc = utc;

    out.state.meta.project_uuid = out.manifest.project_uuid;
    out.state.meta.project_name = out.manifest.project_name;
    out.state.meta.created_utc = utc;
    out.state.meta.modified_utc = utc;
    out.lighting_rig = renderer::LightingRig::default_three_point();

    if (std::filesystem::is_directory(src_aether_path, ec)) {
        auto proj_bytes = read_file_bytes(src_aether_path / "project.toml");
        if (!proj_bytes) {
            return make_unexpected(proj_bytes.error());
        }
        const std::string_view toml_sv(reinterpret_cast<const char*>(proj_bytes.value().data()),
                                       proj_bytes.value().size());
        const TomlTable kv = parse_flat_toml_sections(toml_sv);

        if (const auto it = kv.find("avatar.path"); it != kv.end()) {
            const std::string raw = strip_quotes(it->second);
            const std::filesystem::path p = path_from_utf8(raw);
            if (p.is_relative() && !raw.empty()) {
                out.state.avatar.path_relative = raw;
                std::replace(out.state.avatar.path_relative.begin(), out.state.avatar.path_relative.end(), '\\', '/');
                out.state.avatar.path_absolute = utf8_from_path((src_aether_path / p).lexically_normal());
            } else {
                out.state.avatar.path_absolute = raw;
            }
            const std::string ext = utf8_from_path(p.extension());
            if (ext == ".pmx" || ext == ".PMX") {
                out.state.avatar.format = "PMX";
            } else if (ext == ".vrm" || ext == ".VRM") {
                out.state.avatar.format = "VRM";
            } else if (!raw.empty()) {
                out.state.avatar.format = "GLTF";
            }
            out.state.meta.avatar_display_name = utf8_from_path(p.stem());
        }
        if (const auto it = kv.find("avatar.sha256"); it != kv.end()) {
            out.state.avatar.file_sha256 = strip_quotes(it->second);
        }
        if (const auto it = kv.find("avatar.import_settings.format");
            it != kv.end() && out.state.avatar.format.empty()) {
            out.state.avatar.format = strip_quotes(it->second);
        }
        (void)parse_toml_float(kv, "avatar.import_settings.uniform_scale", out.state.avatar.uniform_scale);
        (void)parse_toml_float(kv, "avatar.import_settings.scale", out.state.avatar.uniform_scale);
        (void)parse_toml_bool(kv, "avatar.import_settings.physics_enabled", out.state.avatar.physics_enabled);
        parse_toml_float_array(kv, "avatar.import_settings.root_pos", out.state.avatar.root_pos);

        float ambient_light = out.lighting_rig.ambient_floor;
        if (parse_toml_float(kv, "scene.lights.ambient_light", ambient_light) ||
            parse_toml_float(kv, "scene.lights.ambient_floor", ambient_light)) {
            out.lighting_rig.ambient_floor = std::clamp(ambient_light, 0.0f, 4.0f);
        }
        float key_ev = out.lighting_rig.studio_lights[0].intensity_ev100;
        if (parse_toml_float(kv, "scene.lights.key_ev", key_ev) ||
            parse_toml_float(kv, "scene.lights.key_intensity", key_ev)) {
            out.lighting_rig.studio_lights[0].intensity_ev100 = key_ev;
            copy_light_name(out.lighting_rig.studio_lights[0].name, "Migrated Key Light");
        }
        (void)parse_toml_float(kv, "scene.lights.key_az", out.lighting_rig.studio_lights[0].orbit_azimuth_deg);
        (void)parse_toml_float(kv, "scene.lights.key_el", out.lighting_rig.studio_lights[0].orbit_elevation_deg);
        (void)parse_toml_float(kv, "scene.lights.key_kelvin", out.lighting_rig.studio_lights[0].kelvin_cct);
        if (out.lighting_rig.studio_light_count >= 2u) {
            float fill_ev = out.lighting_rig.studio_lights[1].intensity_ev100;
            if (parse_toml_float(kv, "scene.lights.fill_ev", fill_ev)) {
                out.lighting_rig.studio_lights[1].intensity_ev100 = fill_ev;
                copy_light_name(out.lighting_rig.studio_lights[1].name, "Migrated Fill Light");
            }
        }
        if (out.lighting_rig.studio_light_count >= 3u) {
            float rim_ev = out.lighting_rig.studio_lights[2].intensity_ev100;
            if (parse_toml_float(kv, "scene.lights.rim_ev", rim_ev)) {
                out.lighting_rig.studio_lights[2].intensity_ev100 = rim_ev;
                copy_light_name(out.lighting_rig.studio_lights[2].name, "Migrated Rim Light");
            }
        }

        if (const auto it = kv.find("scene.camera.preset_id"); it != kv.end()) {
            out.state.camera.active_preset_id = strip_quotes(it->second);
        }
        if (const auto it = kv.find("scene.camera.aspect_mode"); it != kv.end()) {
            out.state.camera.aspect_mode = strip_quotes(it->second);
        }
        (void)parse_toml_float(kv, "scene.camera.fov_y_deg", out.state.camera.live_camera_state.fov_y_deg);
        (void)parse_toml_float(kv, "scene.camera.distance", out.state.camera.live_camera_state.distance);
        (void)parse_toml_float(kv, "scene.camera.yaw_deg", out.state.camera.live_camera_state.yaw_deg);
        (void)parse_toml_float(kv, "scene.camera.pitch_deg", out.state.camera.live_camera_state.pitch_deg);
        parse_toml_float_array(kv, "scene.camera.target", out.state.camera.live_camera_state.target);

        if (const auto it = kv.find("tracking.camera_device_id"); it != kv.end()) {
            out.state.tracking_and_audio.camera_device_id = strip_quotes(it->second);
        }
        (void)parse_toml_bool(kv, "tracking.hand_tracking_enabled", out.state.tracking_and_audio.hand_tracking_enabled);
        (void)parse_toml_bool(kv, "outputs.spout_enabled", out.state.outputs.spout_enabled);
        if (const auto it = kv.find("outputs.spout_sender_name"); it != kv.end()) {
            out.state.outputs.spout_sender_name = strip_quotes(it->second);
        }
        (void)parse_toml_bool(kv, "outputs.vcam_enabled", out.state.outputs.vcam_enabled);
        (void)parse_toml_bool(kv, "outputs.ndi_enabled", out.state.outputs.ndi_enabled);
        (void)parse_toml_bool(kv, "outputs.osc_enabled", out.state.outputs.osc_enabled);

        if (std::filesystem::exists(src_aether_path / "graph.json", ec)) {
            auto gbytes = read_file_bytes(src_aether_path / "graph.json");
            if (!gbytes) {
                return make_unexpected(gbytes.error());
            }
            try {
                const nlohmann::json gj = nlohmann::json::parse(gbytes.value().begin(), gbytes.value().end());
                if (!gj.is_object()) {
                    return fail(ErrorCode::ParseError, "Legacy graph.json is not a JSON object");
                }
                out.state.node_graph_json = gj.dump(2);
            } catch (const std::exception& e) {
                return fail(ErrorCode::ParseError, std::string("Invalid legacy graph.json: ") + e.what());
            }
        }
    } else {
        auto bytes = read_file_bytes(src_aether_path);
        if (!bytes) {
            return make_unexpected(bytes.error());
        }
        try {
            const nlohmann::json lj = nlohmann::json::parse(bytes.value().begin(), bytes.value().end());
            if (!lj.is_object()) {
                return fail(ErrorCode::ParseError, "Legacy .aether file is not a JSON object");
            }
            if (const auto av = lj.find("avatar"); av != lj.end() && av->is_object()) {
                out.state.avatar.path_absolute = av->value("path", av->value("path_absolute", std::string{}));
                out.state.avatar.format = av->value("format", std::string{});
                out.state.meta.avatar_display_name =
                    utf8_from_path(path_from_utf8(out.state.avatar.path_absolute).stem());
            }
            if (const auto l_it = lj.find("lighting"); l_it != lj.end() && l_it->is_object()) {
                out.lighting_rig.studio_lights[0].intensity_ev100 =
                    clean_float(l_it->value("key_ev", out.lighting_rig.studio_lights[0].intensity_ev100), 1.0f);
                out.lighting_rig.studio_lights[0].orbit_azimuth_deg =
                    clean_float(l_it->value("key_az", out.lighting_rig.studio_lights[0].orbit_azimuth_deg), 32.0f);
                out.lighting_rig.studio_lights[0].orbit_elevation_deg =
                    clean_float(l_it->value("key_el", out.lighting_rig.studio_lights[0].orbit_elevation_deg), 26.0f);
                out.lighting_rig.ambient_floor =
                    clean_float(l_it->value("ambient_floor", out.lighting_rig.ambient_floor), 0.28f);
            }
            if (const auto g_it = lj.find("graph"); g_it != lj.end() && g_it->is_object()) {
                out.state.node_graph_json = g_it->dump(2);
            }
        } catch (const std::exception& e) {
            return fail(ErrorCode::ParseError, std::string("Failed to parse legacy .aether: ") + e.what());
        }
    }

    out.lighting_meta.name = "Migrated_" + out.manifest.project_name;
    out.lighting_meta.created_utc = utc;

    out.state.dirty_mask = dirty_bit(DirtyDomain::ProjectMeta) | dirty_bit(DirtyDomain::AvatarState) |
                           dirty_bit(DirtyDomain::LightingRig) | dirty_bit(DirtyDomain::EffectsFx) |
                           dirty_bit(DirtyDomain::CameraState) | dirty_bit(DirtyDomain::NodeGraph) |
                           dirty_bit(DirtyDomain::TrackingCfg) | dirty_bit(DirtyDomain::OutputsCfg);
    out.state.dirty_generation = 1u;
    return out;
}

Expected<AprLoadedBundle> LegacyAetherMigrator::migrate_to_apr_file(const std::filesystem::path& src_aether_path,
                                                                    const std::filesystem::path& dst_apr_path,
                                                                    AetherHostHandle rust_host) {
    auto bundle = migrate_to_bundle(src_aether_path);
    if (!bundle) {
        return bundle;
    }
    if (rust_host != nullptr) {
        if (auto applied = AprSerializer::apply_loaded_graph_to_host(bundle.value(), rust_host); !applied) {
            return make_unexpected(applied.error());
        }
    }
    const lighting::StudioLightingStore store(bundle.value().lighting_rig);
    AprSaveInput input{};
    input.state = &bundle.value().state;
    input.lighting = &store;
    input.rust_host = rust_host;
    input.lighting_meta = &bundle.value().lighting_meta;
    auto man = AprSerializer::save_file(dst_apr_path, input);
    if (!man) {
        return make_unexpected(man.error());
    }
    bundle.value().manifest = std::move(man.value());
    bundle.value().state.clear_dirty();
    return bundle;
}

} // namespace aether::core
