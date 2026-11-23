#include "core/alfx_serializer.hpp"

#include "core/zip_archive.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

namespace aether::core {
namespace {

using renderer::configure_fixture_defaults;
using renderer::LightingRig;
using renderer::PassiveFlagMode;
using renderer::StudioFixtureProfile;
using renderer::StudioGelPreset;
using renderer::StudioGoboPattern;
using renderer::StudioLightSpec;

[[nodiscard]] std::string current_utc_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
#if defined(_WIN32)
    if (gmtime_s(&utc_tm, &t) != 0) {
        return "1970-01-01T00:00:00Z";
    }
#else
    if (gmtime_r(&t, &utc_tm) == nullptr) {
        return "1970-01-01T00:00:00Z";
    }
#endif
    char buf[32]{};
    const int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc_tm.tm_year + 1900,
                                utc_tm.tm_mon + 1, utc_tm.tm_mday, utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
    return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string("1970-01-01T00:00:00Z");
}

template <size_t N> void copy_name_buf(std::array<char, N>& dst, std::string_view src) noexcept {
    dst.fill('\0');
    const size_t n = std::min(src.size(), N - 1u);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

[[nodiscard]] std::string read_name_buf(const std::array<char, 40>& src) {
    size_t len = 0u;
    while (len < src.size() && src[len] != '\0') {
        ++len;
    }
    return std::string(src.data(), len);
}

[[nodiscard]] float sanitize_float(float v, float fallback) noexcept {
    return std::isfinite(v) ? v : fallback;
}

[[nodiscard]] const char* fixture_to_string(StudioFixtureProfile prof) noexcept {
    switch (prof) {
    case StudioFixtureProfile::RectSoftbox:
        return "RectSoftbox";
    case StudioFixtureProfile::Octabox:
        return "Octabox";
    case StudioFixtureProfile::Stripbox:
        return "Stripbox";
    case StudioFixtureProfile::ParabolicUmbrella:
        return "ParabolicUmbrella";
    case StudioFixtureProfile::LanternChinaBall:
        return "LanternChinaBall";
    case StudioFixtureProfile::ArriSkyPanel:
        return "ARRI_SkyPanel_S60C";
    case StudioFixtureProfile::ArriFresnel:
        return "ARRI_Fresnel";
    case StudioFixtureProfile::DedolightSpot:
        return "Dedolight_Spot";
    case StudioFixtureProfile::EllipsoidalLeko:
        return "Ellipsoidal_Leko";
    case StudioFixtureProfile::BroadcastRing:
        return "Broadcast_Ring";
    case StudioFixtureProfile::LedTubeAstera:
        return "Astera_TitanTube";
    case StudioFixtureProfile::NaturalSunCieSky:
        return "NaturalSun_CieSky";
    }
    return "RectSoftbox";
}

[[nodiscard]] StudioFixtureProfile fixture_from_json(const nlohmann::json& j) {
    if (const auto it = j.find("fixture"); it != j.end() && it->is_number_integer()) {
        return static_cast<StudioFixtureProfile>(std::clamp(it->get<int>(), 0, 11));
    }
    if (const auto it = j.find("fixture_type"); it != j.end() && it->is_string()) {
        const std::string& s = it->get_ref<const std::string&>();
        if (s == "Octabox")
            return StudioFixtureProfile::Octabox;
        if (s == "Stripbox")
            return StudioFixtureProfile::Stripbox;
        if (s == "ParabolicUmbrella")
            return StudioFixtureProfile::ParabolicUmbrella;
        if (s == "LanternChinaBall")
            return StudioFixtureProfile::LanternChinaBall;
        if (s == "ARRI_SkyPanel_S60C" || s == "ArriSkyPanel")
            return StudioFixtureProfile::ArriSkyPanel;
        if (s == "ARRI_Fresnel" || s == "ArriFresnel")
            return StudioFixtureProfile::ArriFresnel;
        if (s == "Dedolight_Spot" || s == "DedolightSpot")
            return StudioFixtureProfile::DedolightSpot;
        if (s == "Ellipsoidal_Leko" || s == "EllipsoidalLeko")
            return StudioFixtureProfile::EllipsoidalLeko;
        if (s == "Broadcast_Ring" || s == "BroadcastRing")
            return StudioFixtureProfile::BroadcastRing;
        if (s == "Astera_TitanTube" || s == "LedTubeAstera")
            return StudioFixtureProfile::LedTubeAstera;
        if (s == "NaturalSun_CieSky" || s == "NaturalSunCieSky")
            return StudioFixtureProfile::NaturalSunCieSky;
    }
    return StudioFixtureProfile::RectSoftbox;
}

[[nodiscard]] const char* passive_to_string(PassiveFlagMode mode) noexcept {
    switch (mode) {
    case PassiveFlagMode::None:
        return "None";
    case PassiveFlagMode::NegativeFillBlack:
        return "NegativeFillBlack";
    case PassiveFlagMode::BounceWhite:
        return "BounceWhite";
    case PassiveFlagMode::BounceSilver:
        return "BounceSilver";
    case PassiveFlagMode::BounceGold:
        return "BounceGold";
    }
    return "None";
}

[[nodiscard]] PassiveFlagMode passive_from_json(const nlohmann::json& j) {
    if (const auto it = j.find("passive_flag"); it != j.end() && it->is_number_integer()) {
        return static_cast<PassiveFlagMode>(std::clamp(it->get<int>(), 0, 4));
    }
    if (const auto it = j.find("passive_type"); it != j.end() && it->is_string()) {
        const std::string& s = it->get_ref<const std::string&>();
        if (s == "NegativeFillBlack")
            return PassiveFlagMode::NegativeFillBlack;
        if (s == "BounceWhite")
            return PassiveFlagMode::BounceWhite;
        if (s == "BounceSilver")
            return PassiveFlagMode::BounceSilver;
        if (s == "BounceGold")
            return PassiveFlagMode::BounceGold;
    }
    return PassiveFlagMode::None;
}

void read_vec3(const nlohmann::json& obj, const char* key, std::array<float, 3>& out) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() != 3u) {
        return;
    }
    for (size_t i = 0; i < 3u; ++i) {
        if ((*it)[i].is_number()) {
            out[i] = sanitize_float((*it)[i].get<float>(), out[i]);
        }
    }
}

void read_vec4(const nlohmann::json& obj, const char* key, float (&out)[4]) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() != 4u) {
        return;
    }
    for (size_t i = 0; i < 4u; ++i) {
        if ((*it)[i].is_number()) {
            out[i] = sanitize_float((*it)[i].get<float>(), out[i]);
        }
    }
}

void read_vec4(const nlohmann::json& obj, const char* key, std::array<float, 4>& out) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() != 4u) {
        return;
    }
    for (size_t i = 0; i < 4u; ++i) {
        if ((*it)[i].is_number()) {
            out[i] = sanitize_float((*it)[i].get<float>(), out[i]);
        }
    }
}

[[nodiscard]] nlohmann::json serialize_light_v101(const StudioLightSpec& s, uint32_t slot_index) {
    char id_buf[24]{};
    std::snprintf(id_buf, sizeof(id_buf), "light_%02u", slot_index);

    const uint32_t mask = s.channel_mask;
    return nlohmann::json{
        {"id", std::string(id_buf)},
        {"slot_index", slot_index},
        {"name", read_name_buf(s.name)},
        {"enabled", s.enabled},
        {"solo", s.solo},
        {"cast_shadow", s.cast_shadow},
        {"fixture", static_cast<int>(s.fixture)},
        {"fixture_type", fixture_to_string(s.fixture)},
        {"passive_flag", static_cast<int>(s.passive_flag)},
        {"passive_type", passive_to_string(s.passive_flag)},
        {"transform",
         {{"use_orbit_polar", s.use_orbit_angles},
          {"azimuth_deg", s.orbit_azimuth_deg},
          {"elevation_deg", s.orbit_elevation_deg},
          {"distance_m", s.orbit_distance_m},
          {"position_ws", {s.position_ws[0], s.position_ws[1], s.position_ws[2]}},
          {"roll_deg", s.roll_deg},
          {"aim_lock", s.aim_lock},
          {"aim_target_mode", "AvatarHead"},
          {"target_ws", {s.target_ws[0], s.target_ws[1], s.target_ws[2]}}}},
        {"photometry",
         {{"intensity_ev100", s.intensity_ev100},
          {"exposure_stops", s.exposure_stops},
          {"kelvin_cct", s.kelvin_cct},
          {"green_magenta_tint", s.green_magenta},
          {"linear_tint_rgb", {s.tint_rgb[0], s.tint_rgb[1], s.tint_rgb[2]}},
          {"catchlight_weight", s.catchlight_weight},
          {"shadow_bias_scale", s.shadow_bias_scale},
          {"channel_mask", mask},
          {"channels",
           {{"face", (mask & AETHER_LIGHT_CHANNEL_FACE) != 0u},
            {"hair", (mask & AETHER_LIGHT_CHANNEL_HAIR) != 0u},
            {"body", (mask & AETHER_LIGHT_CHANNEL_BODY) != 0u},
            {"eye_spec", (mask & AETHER_LIGHT_CHANNEL_CATCHLIGHT) != 0u}}}}},
        {"aperture_and_modifiers",
         {{"width_or_outer_m", s.width_or_outer_m},
          {"height_or_inner_m", s.height_or_inner_m},
          {"spot_inner_deg", s.spot_inner_deg},
          {"spot_outer_deg", s.spot_outer_deg},
          {"softbox_enabled", s.softbox_enabled},
          {"grid_enabled", s.grid_enabled},
          {"grid_angle_deg", s.grid_angle_deg},
          {"barn_doors_enabled", s.barn_doors_enabled},
          {"barn_door_cuts_deg",
           {s.barn_door_cuts_deg[0], s.barn_door_cuts_deg[1], s.barn_door_cuts_deg[2], s.barn_door_cuts_deg[3]}},
          {"diffusion_enabled", s.diffusion_enabled},
          {"diffusion_density", s.diffusion_density},
          {"gel_enabled", s.gel_enabled},
          {"gel", static_cast<int>(s.gel)},
          {"coaxial_lock", s.coaxial_lock},
          {"window_portal", s.window_portal},
          {"gobo", static_cast<int>(s.gobo)},
          {"ies_profile", static_cast<int>(s.ies_profile)}}},
    };
}

void deserialize_light_v101(const nlohmann::json& j, StudioLightSpec& s) {
    s = StudioLightSpec{};
    s.fixture = fixture_from_json(j);
    configure_fixture_defaults(s, s.fixture);
    if (const auto it = j.find("name"); it != j.end() && it->is_string()) {
        copy_name_buf(s.name, it->get_ref<const std::string&>());
    }
    s.enabled = j.value("enabled", s.enabled);
    s.solo = j.value("solo", s.solo);
    s.cast_shadow = j.value("cast_shadow", s.cast_shadow);
    s.passive_flag = passive_from_json(j);

    const nlohmann::json& tr = (j.contains("transform") && j["transform"].is_object()) ? j["transform"] : j;
    s.use_orbit_angles = tr.value("use_orbit_polar", tr.value("use_orbit_angles", s.use_orbit_angles));
    s.orbit_azimuth_deg = sanitize_float(tr.value("azimuth_deg", tr.value("orbit_azimuth_deg", s.orbit_azimuth_deg)),
                                         s.orbit_azimuth_deg);
    s.orbit_elevation_deg = sanitize_float(
        tr.value("elevation_deg", tr.value("orbit_elevation_deg", s.orbit_elevation_deg)), s.orbit_elevation_deg);
    s.orbit_distance_m =
        sanitize_float(tr.value("distance_m", tr.value("orbit_distance_m", s.orbit_distance_m)), s.orbit_distance_m);
    s.roll_deg = sanitize_float(tr.value("roll_deg", s.roll_deg), s.roll_deg);
    s.aim_lock = tr.value("aim_lock", s.aim_lock);
    read_vec3(tr, "position_ws", s.position_ws);
    if (tr.contains("target_ws")) {
        read_vec3(tr, "target_ws", s.target_ws);
    } else if (tr.contains("aim_target_custom_ws")) {
        read_vec3(tr, "aim_target_custom_ws", s.target_ws);
    }

    const nlohmann::json& ph = (j.contains("photometry") && j["photometry"].is_object()) ? j["photometry"] : j;
    s.intensity_ev100 = sanitize_float(ph.value("intensity_ev100", s.intensity_ev100), s.intensity_ev100);
    s.exposure_stops = sanitize_float(ph.value("exposure_stops", s.exposure_stops), s.exposure_stops);
    s.kelvin_cct = sanitize_float(ph.value("kelvin_cct", s.kelvin_cct), s.kelvin_cct);
    s.green_magenta =
        sanitize_float(ph.value("green_magenta_tint", ph.value("green_magenta", s.green_magenta)), s.green_magenta);
    if (ph.contains("linear_tint_rgb")) {
        read_vec3(ph, "linear_tint_rgb", s.tint_rgb);
    } else {
        read_vec3(ph, "tint_rgb", s.tint_rgb);
    }
    s.catchlight_weight = sanitize_float(ph.value("catchlight_weight", s.catchlight_weight), s.catchlight_weight);
    s.shadow_bias_scale = sanitize_float(ph.value("shadow_bias_scale", s.shadow_bias_scale), s.shadow_bias_scale);
    if (ph.contains("channel_mask") && ph["channel_mask"].is_number_unsigned()) {
        s.channel_mask = ph["channel_mask"].get<uint32_t>() & AETHER_LIGHT_CHANNEL_ALL;
    } else if (const auto ch = ph.find("channels"); ch != ph.end() && ch->is_object()) {
        uint32_t mask = 0u;
        if (ch->value("face", true))
            mask |= AETHER_LIGHT_CHANNEL_FACE;
        if (ch->value("hair", true))
            mask |= AETHER_LIGHT_CHANNEL_HAIR;
        if (ch->value("body", true))
            mask |= AETHER_LIGHT_CHANNEL_BODY;
        if (ch->value("eye_spec", true))
            mask |= AETHER_LIGHT_CHANNEL_CATCHLIGHT;
        s.channel_mask = mask;
    }

    const nlohmann::json& mod = (j.contains("aperture_and_modifiers") && j["aperture_and_modifiers"].is_object())
                                    ? j["aperture_and_modifiers"]
                                    : j;
    s.width_or_outer_m = sanitize_float(mod.value("width_or_outer_m", s.width_or_outer_m), s.width_or_outer_m);
    s.height_or_inner_m = sanitize_float(mod.value("height_or_inner_m", s.height_or_inner_m), s.height_or_inner_m);
    s.spot_inner_deg = sanitize_float(mod.value("spot_inner_deg", s.spot_inner_deg), s.spot_inner_deg);
    s.spot_outer_deg = sanitize_float(mod.value("spot_outer_deg", s.spot_outer_deg), s.spot_outer_deg);
    s.softbox_enabled = mod.value("softbox_enabled", s.softbox_enabled);
    s.grid_enabled = mod.value("grid_enabled", s.grid_enabled);
    s.grid_angle_deg = sanitize_float(mod.value("grid_angle_deg", s.grid_angle_deg), s.grid_angle_deg);
    s.barn_doors_enabled = mod.value("barn_doors_enabled", s.barn_doors_enabled);
    read_vec4(mod, "barn_door_cuts_deg", s.barn_door_cuts_deg);
    s.diffusion_enabled = mod.value("diffusion_enabled", s.diffusion_enabled);
    s.diffusion_density = sanitize_float(mod.value("diffusion_density", s.diffusion_density), s.diffusion_density);
    s.gel_enabled = mod.value("gel_enabled", s.gel_enabled);
    s.gel = static_cast<StudioGelPreset>(std::clamp(mod.value("gel", static_cast<int>(s.gel)), 0, 11));
    s.coaxial_lock = mod.value("coaxial_lock", s.coaxial_lock);
    s.window_portal = mod.value("window_portal", s.window_portal);
    s.gobo = static_cast<StudioGoboPattern>(std::clamp(mod.value("gobo", static_cast<int>(s.gobo)), 0, 4));
    s.ies_profile = static_cast<int8_t>(std::clamp(mod.value("ies_profile", static_cast<int>(s.ies_profile)), -1, 7));
}

[[nodiscard]] AlfxMetadata default_meta_for_rig(std::string_view active_name) {
    AlfxMetadata meta{};
    meta.name = active_name.empty() ? std::string("Custom Rig") : std::string(active_name);
    meta.created_utc = current_utc_iso8601();
    return meta;
}

} // namespace

Expected<std::string> AlfxSerializer::serialize_to_json_string(const lighting::StudioLightingStore& store,
                                                               const AlfxMetadata* meta_opt) {
    return serialize_rig_to_json_string(store.rig(), store.active_alfx_name(), meta_opt);
}

Expected<std::string> AlfxSerializer::serialize_rig_to_json_string(const LightingRig& rig, std::string_view active_name,
                                                                   const AlfxMetadata* meta_opt) {
    try {
        AlfxMetadata meta = meta_opt ? *meta_opt : default_meta_for_rig(active_name);
        if (meta.name.empty()) {
            meta.name = active_name.empty() ? std::string("Custom Rig") : std::string(active_name);
        }
        if (meta.created_utc.empty()) {
            meta.created_utc = current_utc_iso8601();
        }

        nlohmann::json lights_arr = nlohmann::json::array();
        const uint32_t n = std::min(rig.studio_light_count, renderer::kMaxStudioLights);
        for (uint32_t i = 0; i < n; ++i) {
            lights_arr.push_back(serialize_light_v101(rig.studio_lights[i], i));
        }

        const auto& vp = rig.vrm_mmd_parity;
        const auto& st = rig.satsuei;
        const auto& sg = rig.split_grading;

        const nlohmann::json doc{
            {"format", "ALFX"},
            {"schema_version", kSchemaVersion},
            {"meta",
             {{"name", meta.name},
              {"author", meta.author},
              {"description", meta.description},
              {"tags", meta.tags},
              {"created_utc", meta.created_utc},
              {"app_version", meta.app_version},
              {"preview_palette_hex", meta.preview_palette_hex}}},
            {"lighting_rig",
             {{"dominant_key_light_id", rig.dominant_key_light_id},
              {"circadian_sun_slot_id", rig.circadian_sun_slot_id},
              {"multi_light_soft_knee_threshold", rig.multi_light_soft_knee_threshold},
              {"studio_light_count", n},
              {"lights", std::move(lights_arr)}}},
            {"environment_and_circadian",
             {{"ambient_floor", rig.ambient_floor},
              {"environment_sky_strength", rig.environment_sky_strength},
              {"ground_bounce_strength", rig.ground_bounce_strength},
              {"circadian_enabled", rig.circadian_enabled},
              {"circadian_timezone", rig.circadian_timezone},
              {"circadian_time_minutes", rig.circadian_time_minutes},
              {"bg_rgba", {rig.bg_rgba[0], rig.bg_rgba[1], rig.bg_rgba[2], rig.bg_rgba[3]}}}},
            {"shading_and_rt_pt",
             {{"lighting_mode", static_cast<int>(rig.lighting_mode)},
              {"global_live2d_flatness", rig.global_live2d_flatness},
              {"face_normal_flatten", rig.face_normal_flatten},
              {"neck_seam_blend", rig.neck_seam_blend},
              {"cel_band_sharpness", rig.cel_band_sharpness},
              {"shadow_warm_cool_shift", rig.shadow_warm_cool_shift},
              {"iro_tsuke_strength", rig.iro_tsuke_strength},
              {"crease_strength", rig.crease_strength},
              {"hair_highlight_intensity", rig.hair_highlight_intensity},
              {"rim_step_strength", rig.rim_step_strength},
              {"sdf_feather", rig.sdf_feather},
              {"hair_shadow_offset_px", rig.hair_shadow_offset_px},
              {"hair_shadow_strength", rig.hair_shadow_strength},
              {"rim_t_max", rig.rim_t_max},
              {"bounce_t_max", rig.bounce_t_max},
              {"bounce_strength", rig.bounce_strength},
              {"toon_ramp_shift", rig.toon_ramp_shift},
              {"toon_ramp_toony", rig.toon_ramp_toony},
              {"outline_width_scale", rig.outline_width_scale},
              {"crease_threshold_deg", rig.crease_threshold_deg},
              {"crease_disguise_enabled", rig.crease_disguise_enabled},
              {"crease_disguise_strength", rig.crease_disguise_strength},
              {"eye_catchlight_intensity", rig.eye_catchlight_intensity},
              {"eye_catchlight_size", rig.eye_catchlight_size},
              {"indirect_bleed_strength", rig.indirect_bleed_strength},
              {"rim_depth_offset_px", rig.rim_depth_offset_px},
              {"vrm_mmd_parity",
               {{"auto_toon_harmonizer_enabled", vp.auto_toon_harmonizer_enabled},
                {"vrm_cel_crispness_boost", vp.vrm_cel_crispness_boost},
                {"vrm_shadow_chroma_lift", vp.vrm_shadow_chroma_lift},
                {"vrm_auto_edge_scale_enabled", vp.vrm_auto_edge_scale_enabled},
                {"face_internal_edge_suppress_cm", vp.face_internal_edge_suppress_cm},
                {"unified_surface_group_crease", vp.unified_surface_group_crease},
                {"vrm_pre_cel_clarity_strength", vp.vrm_pre_cel_clarity_strength},
                {"vrm_hair_angel_ring_boost", vp.vrm_hair_angel_ring_boost},
                {"pmx_alpha_bleed_padding", vp.pmx_alpha_bleed_padding},
                {"pmx_strict_material_draw_order", vp.pmx_strict_material_draw_order}}}}},
            {"satsuei_post_fx",
             {{"bloom_threshold", rig.bloom_threshold},
              {"bloom_intensity", rig.bloom_intensity},
              {"tone_mode", rig.tone_mode},
              {"enable_2d_filter_pro", rig.enable_2d_filter_pro},
              {"painterly_flatten_strength", rig.painterly_flatten_strength},
              {"dog_ink_boost", rig.dog_ink_boost},
              {"enable_paper_warmth", rig.enable_paper_warmth},
              {"paper_warmth_strength", rig.paper_warmth_strength},
              {"satsuei",
               {{"light_wrap_strength", st.light_wrap_strength},
                {"halation_strength", st.halation_strength},
                {"halation_threshold", st.halation_threshold},
                {"backscatter_strength", st.backscatter_strength}}},
              {"split_grading",
               {{"avatar_contrast", sg.avatar_contrast},
                {"avatar_saturation", sg.avatar_saturation},
                {"avatar_warmth", sg.avatar_warmth},
                {"avatar_ev_bias", sg.avatar_ev_bias},
                {"bg_contrast", sg.bg_contrast},
                {"bg_saturation", sg.bg_saturation},
                {"bg_exposure", sg.bg_exposure},
                {"master_grade_strength", sg.master_grade_strength}}}}},
        };
        return doc.dump(2);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Failed to serialize .alfx JSON: ") + e.what());
    }
}

Expected<AlfxDocument> AlfxSerializer::deserialize_from_bytes(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return fail(ErrorCode::InvalidArg, "Empty .alfx payload");
    }

    // Optional support for ZIP-wrapped .alfx containers (e.g., future archive bundles).
    std::vector<uint8_t> json_storage;
    std::vector<uint8_t> preview_png;
    std::span<const uint8_t> json_bytes = bytes;
    if (bytes.size() >= 4u && bytes[0] == 0x50u && bytes[1] == 0x4Bu && bytes[2] == 0x03u && bytes[3] == 0x04u) {
        auto zr = ZipReader::open_memory(std::vector<uint8_t>(bytes.begin(), bytes.end()));
        if (!zr) {
            return make_unexpected(zr.error());
        }
        auto verified = zr.value().verify_all();
        if (!verified) {
            return make_unexpected(verified.error());
        }
        const char* entry_name = zr.value().contains("lighting.alfx")
                                     ? "lighting.alfx"
                                     : (zr.value().contains("lighting_rig.json") ? "lighting_rig.json" : nullptr);
        if (entry_name == nullptr) {
            return fail(ErrorCode::ParseError, "ZIP .alfx container missing lighting_rig.json / lighting.alfx");
        }
        auto payload = zr.value().read_entry(entry_name);
        if (!payload) {
            return make_unexpected(payload.error());
        }
        if (zr.value().contains("preview.png")) {
            if (auto png = zr.value().read_entry("preview.png"); png) {
                preview_png = std::move(png.value());
            }
        }
        json_storage = std::move(payload.value());
        json_bytes = json_storage;
    }

    auto hash_res = sha256_hex(json_bytes);
    if (!hash_res) {
        return make_unexpected(hash_res.error());
    }

    try {
        const nlohmann::json j = nlohmann::json::parse(json_bytes.begin(), json_bytes.end());
        if (!j.is_object()) {
            return fail(ErrorCode::ParseError, ".alfx root must be a JSON object");
        }
        if (j.value("format", std::string{}) != "ALFX") {
            return fail(ErrorCode::ParseError, "Invalid .alfx format tag (expected 'ALFX')");
        }
        const std::string ver = j.value("schema_version", std::string{});
        if (ver != "1.0.0" && ver != "1.0.1") {
            return fail(ErrorCode::Unsupported, "Unsupported .alfx schema_version: " + ver);
        }

        AlfxDocument out{};
        out.schema_version = ver;
        out.sha256_hex = std::move(hash_res.value());
        out.optional_preview_png = std::move(preview_png);
        out.rig = LightingRig{};
        out.rig.studio_light_count = 0u;

        if (const auto m_it = j.find("meta"); m_it != j.end() && m_it->is_object()) {
            out.meta.name = m_it->value("name", out.meta.name);
            out.meta.author = m_it->value("author", out.meta.author);
            out.meta.description = m_it->value("description", out.meta.description);
            out.meta.created_utc = m_it->value("created_utc", out.meta.created_utc);
            out.meta.app_version = m_it->value("app_version", out.meta.app_version);
            if (const auto t_it = m_it->find("tags"); t_it != m_it->end() && t_it->is_array()) {
                for (const auto& tag : *t_it) {
                    if (tag.is_string()) {
                        out.meta.tags.push_back(tag.get<std::string>());
                    }
                }
            }
            if (const auto p_it = m_it->find("preview_palette_hex");
                p_it != m_it->end() && p_it->is_array() && p_it->size() == 3u) {
                for (size_t k = 0; k < 3u; ++k) {
                    if ((*p_it)[k].is_string()) {
                        out.meta.preview_palette_hex[k] = (*p_it)[k].get<std::string>();
                    }
                }
            }
        }

        if (const auto lr_it = j.find("lighting_rig"); lr_it != j.end() && lr_it->is_object()) {
            out.rig.multi_light_soft_knee_threshold =
                sanitize_float(lr_it->value("multi_light_soft_knee_threshold", out.rig.multi_light_soft_knee_threshold),
                               out.rig.multi_light_soft_knee_threshold);
            out.rig.circadian_sun_slot_id =
                std::clamp(lr_it->value("circadian_sun_slot_id", out.rig.circadian_sun_slot_id), -1,
                           static_cast<int32_t>(renderer::kMaxStudioLights) - 1);

            std::vector<std::string> slot_ids;
            if (const auto l_arr = lr_it->find("lights"); l_arr != lr_it->end() && l_arr->is_array()) {
                for (const auto& item : *l_arr) {
                    if (out.rig.studio_light_count >= renderer::kMaxStudioLights) {
                        break;
                    }
                    if (!item.is_object()) {
                        continue;
                    }
                    deserialize_light_v101(item, out.rig.studio_lights[out.rig.studio_light_count++]);
                    slot_ids.push_back(item.value("id", std::string{}));
                }
            }

            if (const auto dk = lr_it->find("dominant_key_light_id"); dk != lr_it->end()) {
                if (dk->is_number_unsigned() || dk->is_number_integer()) {
                    const int idx = std::max(0, dk->get<int>());
                    out.rig.dominant_key_light_id = static_cast<uint32_t>(idx);
                } else if (dk->is_string()) {
                    const std::string& want = dk->get_ref<const std::string&>();
                    for (size_t idx = 0; idx < slot_ids.size(); ++idx) {
                        if (slot_ids[idx] == want) {
                            out.rig.dominant_key_light_id = static_cast<uint32_t>(idx);
                            break;
                        }
                    }
                }
            }
            if (out.rig.studio_light_count > 0u) {
                out.rig.dominant_key_light_id =
                    std::min(out.rig.dominant_key_light_id, out.rig.studio_light_count - 1u);
            } else {
                out.rig.dominant_key_light_id = 0u;
            }
        }

        if (const auto env_it = j.find("environment_and_circadian"); env_it != j.end() && env_it->is_object()) {
            out.rig.ambient_floor =
                sanitize_float(env_it->value("ambient_floor", out.rig.ambient_floor), out.rig.ambient_floor);
            out.rig.environment_sky_strength =
                sanitize_float(env_it->value("environment_sky_strength", out.rig.environment_sky_strength),
                               out.rig.environment_sky_strength);
            out.rig.ground_bounce_strength =
                sanitize_float(env_it->value("ground_bounce_strength", out.rig.ground_bounce_strength),
                               out.rig.ground_bounce_strength);
            out.rig.circadian_enabled = env_it->value("circadian_enabled", out.rig.circadian_enabled);
            out.rig.circadian_timezone = env_it->value("circadian_timezone", out.rig.circadian_timezone);
            out.rig.circadian_time_minutes =
                std::clamp(env_it->value("circadian_time_minutes", out.rig.circadian_time_minutes), -1, 1439);
            read_vec4(*env_it, "bg_rgba", out.rig.bg_rgba);
        }

        if (const auto sh_it = j.find("shading_and_rt_pt"); sh_it != j.end() && sh_it->is_object()) {
            out.rig.lighting_mode = static_cast<uint8_t>(
                std::clamp(sh_it->value("lighting_mode", static_cast<int>(out.rig.lighting_mode)), 0, 2));
            out.rig.global_live2d_flatness = sanitize_float(
                sh_it->value("global_live2d_flatness", out.rig.global_live2d_flatness), out.rig.global_live2d_flatness);
            out.rig.face_normal_flatten = sanitize_float(
                sh_it->value("face_normal_flatten", out.rig.face_normal_flatten), out.rig.face_normal_flatten);
            out.rig.neck_seam_blend =
                sanitize_float(sh_it->value("neck_seam_blend", out.rig.neck_seam_blend), out.rig.neck_seam_blend);
            out.rig.cel_band_sharpness = sanitize_float(sh_it->value("cel_band_sharpness", out.rig.cel_band_sharpness),
                                                        out.rig.cel_band_sharpness);
            out.rig.shadow_warm_cool_shift = sanitize_float(
                sh_it->value("shadow_warm_cool_shift", out.rig.shadow_warm_cool_shift), out.rig.shadow_warm_cool_shift);
            out.rig.iro_tsuke_strength = sanitize_float(sh_it->value("iro_tsuke_strength", out.rig.iro_tsuke_strength),
                                                        out.rig.iro_tsuke_strength);
            out.rig.crease_strength =
                sanitize_float(sh_it->value("crease_strength", out.rig.crease_strength), out.rig.crease_strength);
            out.rig.hair_highlight_intensity =
                sanitize_float(sh_it->value("hair_highlight_intensity", out.rig.hair_highlight_intensity),
                               out.rig.hair_highlight_intensity);
            out.rig.rim_step_strength =
                sanitize_float(sh_it->value("rim_step_strength", out.rig.rim_step_strength), out.rig.rim_step_strength);
            out.rig.sdf_feather = sanitize_float(sh_it->value("sdf_feather", out.rig.sdf_feather), out.rig.sdf_feather);
            out.rig.hair_shadow_offset_px = sanitize_float(
                sh_it->value("hair_shadow_offset_px", out.rig.hair_shadow_offset_px), out.rig.hair_shadow_offset_px);
            out.rig.hair_shadow_strength = sanitize_float(
                sh_it->value("hair_shadow_strength", out.rig.hair_shadow_strength), out.rig.hair_shadow_strength);
            out.rig.rim_t_max = sanitize_float(sh_it->value("rim_t_max", out.rig.rim_t_max), out.rig.rim_t_max);
            out.rig.bounce_t_max =
                sanitize_float(sh_it->value("bounce_t_max", out.rig.bounce_t_max), out.rig.bounce_t_max);
            out.rig.bounce_strength =
                sanitize_float(sh_it->value("bounce_strength", out.rig.bounce_strength), out.rig.bounce_strength);
            out.rig.toon_ramp_shift =
                sanitize_float(sh_it->value("toon_ramp_shift", out.rig.toon_ramp_shift), out.rig.toon_ramp_shift);
            out.rig.toon_ramp_toony =
                sanitize_float(sh_it->value("toon_ramp_toony", out.rig.toon_ramp_toony), out.rig.toon_ramp_toony);
            out.rig.outline_width_scale = sanitize_float(
                sh_it->value("outline_width_scale", out.rig.outline_width_scale), out.rig.outline_width_scale);
            out.rig.crease_threshold_deg = sanitize_float(
                sh_it->value("crease_threshold_deg", out.rig.crease_threshold_deg), out.rig.crease_threshold_deg);
            out.rig.crease_disguise_enabled = sh_it->value("crease_disguise_enabled", out.rig.crease_disguise_enabled);
            out.rig.crease_disguise_strength =
                sanitize_float(sh_it->value("crease_disguise_strength", out.rig.crease_disguise_strength),
                               out.rig.crease_disguise_strength);
            out.rig.eye_catchlight_intensity =
                sanitize_float(sh_it->value("eye_catchlight_intensity", out.rig.eye_catchlight_intensity),
                               out.rig.eye_catchlight_intensity);
            out.rig.eye_catchlight_size = sanitize_float(
                sh_it->value("eye_catchlight_size", out.rig.eye_catchlight_size), out.rig.eye_catchlight_size);
            out.rig.indirect_bleed_strength =
                sanitize_float(sh_it->value("indirect_bleed_strength", out.rig.indirect_bleed_strength),
                               out.rig.indirect_bleed_strength);
            out.rig.rim_depth_offset_px = sanitize_float(
                sh_it->value("rim_depth_offset_px", out.rig.rim_depth_offset_px), out.rig.rim_depth_offset_px);

            if (const auto vp_it = sh_it->find("vrm_mmd_parity"); vp_it != sh_it->end() && vp_it->is_object()) {
                auto& vp = out.rig.vrm_mmd_parity;
                vp.auto_toon_harmonizer_enabled =
                    vp_it->value("auto_toon_harmonizer_enabled", vp.auto_toon_harmonizer_enabled);
                vp.vrm_cel_crispness_boost = sanitize_float(
                    vp_it->value("vrm_cel_crispness_boost", vp.vrm_cel_crispness_boost), vp.vrm_cel_crispness_boost);
                vp.vrm_shadow_chroma_lift = sanitize_float(
                    vp_it->value("vrm_shadow_chroma_lift", vp.vrm_shadow_chroma_lift), vp.vrm_shadow_chroma_lift);
                vp.vrm_auto_edge_scale_enabled =
                    vp_it->value("vrm_auto_edge_scale_enabled", vp.vrm_auto_edge_scale_enabled);
                vp.face_internal_edge_suppress_cm =
                    sanitize_float(vp_it->value("face_internal_edge_suppress_cm", vp.face_internal_edge_suppress_cm),
                                   vp.face_internal_edge_suppress_cm);
                vp.unified_surface_group_crease =
                    vp_it->value("unified_surface_group_crease", vp.unified_surface_group_crease);
                vp.vrm_pre_cel_clarity_strength =
                    sanitize_float(vp_it->value("vrm_pre_cel_clarity_strength", vp.vrm_pre_cel_clarity_strength),
                                   vp.vrm_pre_cel_clarity_strength);
                vp.vrm_hair_angel_ring_boost =
                    sanitize_float(vp_it->value("vrm_hair_angel_ring_boost", vp.vrm_hair_angel_ring_boost),
                                   vp.vrm_hair_angel_ring_boost);
                vp.pmx_alpha_bleed_padding = vp_it->value("pmx_alpha_bleed_padding", vp.pmx_alpha_bleed_padding);
                vp.pmx_strict_material_draw_order =
                    vp_it->value("pmx_strict_material_draw_order", vp.pmx_strict_material_draw_order);
            }
        }

        if (const auto fx_it = j.find("satsuei_post_fx"); fx_it != j.end() && fx_it->is_object()) {
            out.rig.bloom_threshold =
                sanitize_float(fx_it->value("bloom_threshold", out.rig.bloom_threshold), out.rig.bloom_threshold);
            out.rig.bloom_intensity =
                sanitize_float(fx_it->value("bloom_intensity", out.rig.bloom_intensity), out.rig.bloom_intensity);
            out.rig.tone_mode = fx_it->value("tone_mode", out.rig.tone_mode);
            out.rig.enable_2d_filter_pro = fx_it->value("enable_2d_filter_pro", out.rig.enable_2d_filter_pro);
            out.rig.painterly_flatten_strength =
                sanitize_float(fx_it->value("painterly_flatten_strength", out.rig.painterly_flatten_strength),
                               out.rig.painterly_flatten_strength);
            out.rig.dog_ink_boost =
                sanitize_float(fx_it->value("dog_ink_boost", out.rig.dog_ink_boost), out.rig.dog_ink_boost);
            out.rig.enable_paper_warmth = fx_it->value("enable_paper_warmth", out.rig.enable_paper_warmth);
            out.rig.paper_warmth_strength = sanitize_float(
                fx_it->value("paper_warmth_strength", out.rig.paper_warmth_strength), out.rig.paper_warmth_strength);

            if (const auto st_it = fx_it->find("satsuei"); st_it != fx_it->end() && st_it->is_object()) {
                out.rig.satsuei.light_wrap_strength =
                    sanitize_float(st_it->value("light_wrap_strength", out.rig.satsuei.light_wrap_strength),
                                   out.rig.satsuei.light_wrap_strength);
                out.rig.satsuei.halation_strength =
                    sanitize_float(st_it->value("halation_strength", out.rig.satsuei.halation_strength),
                                   out.rig.satsuei.halation_strength);
                out.rig.satsuei.halation_threshold =
                    sanitize_float(st_it->value("halation_threshold", out.rig.satsuei.halation_threshold),
                                   out.rig.satsuei.halation_threshold);
                out.rig.satsuei.backscatter_strength =
                    sanitize_float(st_it->value("backscatter_strength", out.rig.satsuei.backscatter_strength),
                                   out.rig.satsuei.backscatter_strength);
            }

            if (const auto sg_it = fx_it->find("split_grading"); sg_it != fx_it->end() && sg_it->is_object()) {
                auto& sg = out.rig.split_grading;
                sg.avatar_contrast =
                    sanitize_float(sg_it->value("avatar_contrast", sg.avatar_contrast), sg.avatar_contrast);
                sg.avatar_saturation =
                    sanitize_float(sg_it->value("avatar_saturation", sg.avatar_saturation), sg.avatar_saturation);
                sg.avatar_warmth = sanitize_float(sg_it->value("avatar_warmth", sg.avatar_warmth), sg.avatar_warmth);
                sg.avatar_ev_bias =
                    sanitize_float(sg_it->value("avatar_ev_bias", sg.avatar_ev_bias), sg.avatar_ev_bias);
                sg.bg_contrast = sanitize_float(sg_it->value("bg_contrast", sg.bg_contrast), sg.bg_contrast);
                sg.bg_saturation = sanitize_float(sg_it->value("bg_saturation", sg.bg_saturation), sg.bg_saturation);
                sg.bg_exposure = sanitize_float(sg_it->value("bg_exposure", sg.bg_exposure), sg.bg_exposure);
                sg.master_grade_strength = sanitize_float(
                    sg_it->value("master_grade_strength", sg.master_grade_strength), sg.master_grade_strength);
            }
        }

        return out;
    } catch (const std::exception& e) {
        return fail(ErrorCode::ParseError, std::string("Failed to parse .alfx JSON: ") + e.what());
    }
}

Expected<void> AlfxSerializer::save_file(const std::filesystem::path& dst_path,
                                         const lighting::StudioLightingStore& store, const AlfxMetadata* meta_opt) {
    auto json_res = serialize_to_json_string(store, meta_opt);
    if (!json_res) {
        return make_unexpected(json_res.error());
    }
    const std::vector<uint8_t> bytes(json_res.value().begin(), json_res.value().end());
    return write_bytes_atomic(dst_path, bytes, [](const std::filesystem::path& tmp_path) -> Expected<void> {
        auto readback = read_file_bytes(tmp_path);
        if (!readback) {
            return make_unexpected(readback.error());
        }
        auto parsed = deserialize_from_bytes(readback.value());
        if (!parsed) {
            return make_unexpected(parsed.error());
        }
        return {};
    });
}

Expected<AlfxDocument> AlfxSerializer::load_file(const std::filesystem::path& src_path) {
    auto bytes = read_file_bytes(src_path);
    if (!bytes) {
        return make_unexpected(bytes.error());
    }
    return deserialize_from_bytes(bytes.value());
}

void AlfxSerializer::apply_to_store(const AlfxDocument& doc, lighting::StudioLightingStore& dst_store,
                                    AlfxImportMode mode) noexcept {
    LightingRig& dst = dst_store.rig();
    const LightingRig& src = doc.rig;

    switch (mode) {
    case AlfxImportMode::ReplaceAll: {
        dst = src;
        if (!doc.meta.name.empty()) {
            dst_store.set_active_alfx_name(doc.meta.name);
        }
        dst_store.mark_dirty();
        break;
    }
    case AlfxImportMode::LightsOnly: {
        dst.studio_lights = src.studio_lights;
        dst.studio_light_count = std::min(src.studio_light_count, renderer::kMaxStudioLights);
        dst.dominant_key_light_id =
            dst.studio_light_count > 0u ? std::min(src.dominant_key_light_id, dst.studio_light_count - 1u) : 0u;
        dst.circadian_sun_slot_id = src.circadian_sun_slot_id;
        dst.multi_light_soft_knee_threshold = src.multi_light_soft_knee_threshold;
        dst_store.mark_dirty();
        break;
    }
    case AlfxImportMode::ColorPostFxOnly: {
        dst.lighting_mode = src.lighting_mode;
        dst.global_live2d_flatness = src.global_live2d_flatness;
        dst.face_normal_flatten = src.face_normal_flatten;
        dst.neck_seam_blend = src.neck_seam_blend;
        dst.cel_band_sharpness = src.cel_band_sharpness;
        dst.shadow_warm_cool_shift = src.shadow_warm_cool_shift;
        dst.iro_tsuke_strength = src.iro_tsuke_strength;
        dst.crease_strength = src.crease_strength;
        dst.hair_highlight_intensity = src.hair_highlight_intensity;
        dst.rim_step_strength = src.rim_step_strength;
        dst.sdf_feather = src.sdf_feather;
        dst.hair_shadow_offset_px = src.hair_shadow_offset_px;
        dst.hair_shadow_strength = src.hair_shadow_strength;
        dst.rim_t_max = src.rim_t_max;
        dst.bounce_t_max = src.bounce_t_max;
        dst.bounce_strength = src.bounce_strength;
        dst.toon_ramp_shift = src.toon_ramp_shift;
        dst.toon_ramp_toony = src.toon_ramp_toony;
        dst.outline_width_scale = src.outline_width_scale;
        dst.crease_threshold_deg = src.crease_threshold_deg;
        dst.crease_disguise_enabled = src.crease_disguise_enabled;
        dst.crease_disguise_strength = src.crease_disguise_strength;
        dst.eye_catchlight_intensity = src.eye_catchlight_intensity;
        dst.eye_catchlight_size = src.eye_catchlight_size;
        dst.indirect_bleed_strength = src.indirect_bleed_strength;
        dst.rim_depth_offset_px = src.rim_depth_offset_px;
        dst.vrm_mmd_parity = src.vrm_mmd_parity;

        dst.bloom_threshold = src.bloom_threshold;
        dst.bloom_intensity = src.bloom_intensity;
        dst.tone_mode = src.tone_mode;
        dst.enable_2d_filter_pro = src.enable_2d_filter_pro;
        dst.painterly_flatten_strength = src.painterly_flatten_strength;
        dst.dog_ink_boost = src.dog_ink_boost;
        dst.enable_paper_warmth = src.enable_paper_warmth;
        dst.paper_warmth_strength = src.paper_warmth_strength;
        dst.satsuei = src.satsuei;
        dst.split_grading = src.split_grading;
        std::copy(std::begin(src.bg_rgba), std::end(src.bg_rgba), std::begin(dst.bg_rgba));
        dst_store.mark_dirty();
        break;
    }
    case AlfxImportMode::AdditiveAppendLights: {
        const uint32_t existing = std::min(dst.studio_light_count, renderer::kMaxStudioLights);
        const uint32_t incoming = std::min(src.studio_light_count, renderer::kMaxStudioLights);
        uint32_t written = existing;
        for (uint32_t i = 0; i < incoming && written < renderer::kMaxStudioLights; ++i) {
            dst.studio_lights[written] = src.studio_lights[i];
            if (dst.circadian_sun_slot_id < 0 &&
                src.studio_lights[i].fixture == StudioFixtureProfile::NaturalSunCieSky &&
                src.circadian_sun_slot_id == static_cast<int32_t>(i)) {
                dst.circadian_sun_slot_id = static_cast<int32_t>(written);
            }
            ++written;
        }
        dst.studio_light_count = written;
        if (existing == 0u && written > 0u) {
            dst.dominant_key_light_id = std::min(src.dominant_key_light_id, written - 1u);
        }
        dst_store.mark_dirty();
        break;
    }
    }
}

const std::array<AlfxBuiltinPresetInfo, 5>& AlfxSerializer::builtin_preset_catalog() {
    static const std::array<AlfxBuiltinPresetInfo, 5> kCatalog{{
        {"StudioCleanDaylight.alfx", "Studio Clean Daylight",
         AlfxMetadata{"Studio Clean Daylight",
                      "AETHER Studio",
                      "Neutral daylight key SkyPanel + lantern fill + crisp hair angel rim strip",
                      {"studio", "daylight", "broadcast", "3-point"},
                      "2026-11-20T00:00:00Z",
                      "1.0.4",
                      {"#FFF8ED", "#88B4FF", "#1E2430"}}},
        {"AnimeSunsetRim.alfx", "Anime Sunset Rim",
         AlfxMetadata{"Anime Sunset Rim",
                      "AETHER Studio",
                      "Golden hour CIE sun + classroom window portal + Shinkai halation wrap",
                      {"shinkai", "sunset", "golden-hour", "satsuei"},
                      "2026-11-20T00:00:00Z",
                      "1.0.4",
                      {"#FFB870", "#5B6EE1", "#1B2038"}}},
        {"CyberNeonStage.alfx", "Cyber Neon Stage",
         AlfxMetadata{"Cyber Neon Stage",
                      "AETHER Studio",
                      "Dual cyan & stage magenta tube lighting + tight Fresnel face anchor",
                      {"neon", "cyberpunk", "stage", "concert"},
                      "2026-11-20T00:00:00Z",
                      "1.0.4",
                      {"#00E8FF", "#FF3CA6", "#101223"}}},
        {"SoftPastelVrm.alfx", "Soft Pastel VRM",
         AlfxMetadata{"Soft Pastel VRM",
                      "AETHER Studio",
                      "High-key beauty coaxial ring + overhead octabox + white chin bounce for VRM",
                      {"pastel", "vrm", "beauty", "high-key"},
                      "2026-11-20T00:00:00Z",
                      "1.0.4",
                      {"#FFDDE8", "#BCE3FF", "#2A2433"}}},
        {"DramaticChiaroscuro.alfx", "Dramatic Chiaroscuro",
         AlfxMetadata{"Dramatic Chiaroscuro",
                      "AETHER Studio",
                      "High-contrast Fresnel split key + negative fill flag + Dedolight rim",
                      {"dramatic", "chiaroscuro", "rim", "contrast"},
                      "2026-11-20T00:00:00Z",
                      "1.0.4",
                      {"#FFE0A8", "#343B4E", "#0C0E14"}}},
    }};
    return kCatalog;
}

AlfxDocument AlfxSerializer::build_builtin_preset(size_t index, const std::array<float, 3>& pivot_ws) {
    constexpr std::array<renderer::StudioRigPresetId, 5> kMapIds{
        renderer::StudioRigPresetId::ThreePointBroadcast,    renderer::StudioRigPresetId::ShinkaiGoldenHour,
        renderer::StudioRigPresetId::NeonCyberpunkDualColor, renderer::StudioRigPresetId::HighKeySoftBeauty,
        renderer::StudioRigPresetId::DramaticRimSplit,
    };
    const auto& catalog = builtin_preset_catalog();
    const size_t idx = std::min(index, size_t{4});

    AlfxDocument doc{};
    doc.schema_version = kSchemaVersion;
    doc.meta = catalog[idx].meta;
    doc.rig = LightingRig::default_three_point();
    renderer::apply_studio_rig_preset(doc.rig, kMapIds[idx], pivot_ws);

    // Fine-tune signature look traits per preset.
    switch (idx) {
    case 0: // StudioCleanDaylight
        doc.rig.ambient_floor = 0.28f;
        doc.rig.environment_sky_strength = 0.30f;
        break;
    case 1: // AnimeSunsetRim
        doc.rig.circadian_sun_slot_id = 0;
        doc.rig.ambient_floor = 0.22f;
        doc.rig.satsuei.light_wrap_strength = 0.34f;
        doc.rig.satsuei.halation_strength = 0.30f;
        break;
    case 2: // CyberNeonStage
        doc.rig.ambient_floor = 0.14f;
        doc.rig.bloom_intensity = 0.12f;
        break;
    case 3: // SoftPastelVrm
        doc.rig.ambient_floor = 0.34f;
        doc.rig.vrm_mmd_parity.vrm_cel_crispness_boost = 0.88f;
        doc.rig.vrm_mmd_parity.vrm_shadow_chroma_lift = 0.26f;
        break;
    case 4: // DramaticChiaroscuro
    default:
        doc.rig.ambient_floor = 0.10f;
        doc.rig.shadow_warm_cool_shift = 0.85f;
        break;
    }
    return doc;
}

Expected<void> AlfxSerializer::write_builtin_presets_dir(const std::filesystem::path& presets_alfx_dir,
                                                         bool overwrite) {
    std::error_code ec;
    std::filesystem::create_directories(presets_alfx_dir, ec);
    if (ec) {
        return fail(ErrorCode::IoError, "Failed to create presets/alfx directory: " + ec.message());
    }

    const auto& catalog = builtin_preset_catalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        const std::filesystem::path dst = presets_alfx_dir / catalog[i].filename;
        if (!overwrite && std::filesystem::exists(dst, ec)) {
            continue;
        }
        const AlfxDocument doc = build_builtin_preset(i);
        const lighting::StudioLightingStore store(doc.rig);
        auto res = save_file(dst, store, &doc.meta);
        if (!res) {
            return res;
        }
    }
    return {};
}

} // namespace aether::core
