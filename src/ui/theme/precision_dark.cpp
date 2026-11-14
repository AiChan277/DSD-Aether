#include "ui/theme/precision_dark.hpp"

#include "core/log.hpp"
#include "ui/theme/icons.hpp"

#include <imgui_freetype.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace aether::ui {

namespace {

constexpr uint64_t kBlockMagicArena = 0x4145544855494152ULL; // 'AETHUIAR'
constexpr uint64_t kBlockMagicFallback = 0x414554484F534642ULL;

uint32_t choose_bin(size_t size) noexcept {
    const size_t min_sz = size_t{1} << ImGuiArenaAllocator::kMinBinShift;
    const size_t target = size < min_sz ? min_sz : size;
    uint32_t shift = static_cast<uint32_t>(ImGuiArenaAllocator::kMinBinShift);
    while ((size_t{1} << shift) < target && shift < ImGuiArenaAllocator::kMaxBinShift) {
        ++shift;
    }
    return shift - static_cast<uint32_t>(ImGuiArenaAllocator::kMinBinShift);
}

void* imgui_arena_alloc_cb(size_t sz, void* user_data) noexcept {
    auto* arena = static_cast<ImGuiArenaAllocator*>(user_data);
    return arena != nullptr ? arena->alloc(sz) : nullptr;
}

void imgui_arena_free_cb(void* ptr, void* user_data) noexcept {
    auto* arena = static_cast<ImGuiArenaAllocator*>(user_data);
    if (arena != nullptr) {
        arena->free(ptr);
    }
}

void merge_icons_and_cjk(ImGuiIO& io, float px_size, float scale, const std::string& icon_font_path,
                         const std::string& jp_font_path, bool include_jp) {
    static const ImWchar kIconRanges[] = {
        static_cast<ImWchar>(icons::kIconRangeMin),
        static_cast<ImWchar>(icons::kIconRangeMax),
        0,
    };

    std::error_code ec;
    if (!icon_font_path.empty() && std::filesystem::exists(icon_font_path, ec)) {
        ImFontConfig icfg{};
        icfg.MergeMode = true;
        icfg.PixelSnapH = true;
        icfg.GlyphMinAdvanceX = std::round(13.0f * scale);
        icfg.GlyphOffset = ImVec2(0.0f, std::round(1.0f * scale));
        icfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        io.Fonts->AddFontFromFileTTF(icon_font_path.c_str(), px_size, &icfg, kIconRanges);
    }
    if (include_jp && !jp_font_path.empty() && std::filesystem::exists(jp_font_path, ec)) {
        ImFontConfig jcfg{};
        jcfg.MergeMode = true;
        jcfg.PixelSnapH = true;
        jcfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        io.Fonts->AddFontFromFileTTF(jp_font_path.c_str(), px_size, &jcfg, io.Fonts->GetGlyphRangesJapanese());
    }
}

} // namespace

bool ImGuiArenaAllocator::init(size_t capacity_bytes) noexcept {
    shutdown();
    capacity_ = capacity_bytes;
    if (capacity_ > 0) {
#if defined(_WIN32)
        base_ = static_cast<uint8_t*>(_aligned_malloc(capacity_, 64));
#else
        base_ = static_cast<uint8_t*>(std::aligned_alloc(64, capacity_));
#endif
    }
    if (base_ == nullptr) {
        capacity_ = 0;
        return false;
    }
    cursor_ = 0;
    live_bytes_ = 0;
    peak_live_bytes_ = 0;
    warmup_done_ = false;
    steady_bump_growth_ = 0;
    os_fallback_allocs_ = 0;
    total_allocs_ = 0;
    free_bins_.fill(nullptr);
    return true;
}

void ImGuiArenaAllocator::shutdown() noexcept {
    if (base_ != nullptr) {
#if defined(_WIN32)
        _aligned_free(base_);
#else
        std::free(base_);
#endif
        base_ = nullptr;
    }
    capacity_ = 0;
    cursor_ = 0;
    live_bytes_ = 0;
    free_bins_.fill(nullptr);
}

void* ImGuiArenaAllocator::alloc(size_t size) noexcept {
    const size_t safe_req = size == 0 ? 1U : size;
    ++total_allocs_;
    const uint32_t bin = choose_bin(safe_req);
    const size_t payload_cap = size_t{1} << (bin + kMinBinShift);

    if (bin < kNumBins && free_bins_[bin] != nullptr) {
        BlockHeader* blk = free_bins_[bin];
        free_bins_[bin] = blk->next;
        blk->next = nullptr;
        blk->req_size = static_cast<uint32_t>(safe_req);
        live_bytes_ += payload_cap;
        if (live_bytes_ > peak_live_bytes_) {
            peak_live_bytes_ = live_bytes_;
        }
        return reinterpret_cast<uint8_t*>(blk) + sizeof(BlockHeader);
    }

    const size_t total_block = sizeof(BlockHeader) + payload_cap;
    const size_t aligned_cursor = (cursor_ + 15U) & ~size_t{15U};
    if (base_ != nullptr && aligned_cursor <= capacity_ && total_block <= (capacity_ - aligned_cursor)) {
        uint8_t* ptr = base_ + aligned_cursor;
        cursor_ = aligned_cursor + total_block;
        if (warmup_done_) {
            steady_bump_growth_ += total_block;
        }
        auto* blk = reinterpret_cast<BlockHeader*>(ptr);
        blk->magic = kBlockMagicArena;
        blk->bin_idx = bin;
        blk->req_size = static_cast<uint32_t>(safe_req);
        blk->next = nullptr;
        live_bytes_ += payload_cap;
        if (live_bytes_ > peak_live_bytes_) {
            peak_live_bytes_ = live_bytes_;
        }
        return ptr + sizeof(BlockHeader);
    }

    ++os_fallback_allocs_;
#if defined(_WIN32)
    auto* raw = static_cast<uint8_t*>(_aligned_malloc(sizeof(BlockHeader) + safe_req, 16));
#else
    auto* raw = static_cast<uint8_t*>(std::aligned_alloc(16, sizeof(BlockHeader) + safe_req));
#endif
    if (raw == nullptr) {
        return nullptr;
    }
    auto* blk = reinterpret_cast<BlockHeader*>(raw);
    blk->magic = kBlockMagicFallback;
    blk->bin_idx = UINT32_MAX;
    blk->req_size = static_cast<uint32_t>(safe_req);
    blk->next = nullptr;
    return raw + sizeof(BlockHeader);
}

void ImGuiArenaAllocator::free(void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    auto* blk = reinterpret_cast<BlockHeader*>(static_cast<uint8_t*>(ptr) - sizeof(BlockHeader));
    if (blk->magic == kBlockMagicArena) {
        if (blk->bin_idx < kNumBins) {
            const size_t payload_cap = size_t{1} << (blk->bin_idx + kMinBinShift);
            live_bytes_ = live_bytes_ >= payload_cap ? (live_bytes_ - payload_cap) : 0;
            blk->next = free_bins_[blk->bin_idx];
            free_bins_[blk->bin_idx] = blk;
        }
        return;
    }
    if (blk->magic == kBlockMagicFallback) {
#if defined(_WIN32)
        _aligned_free(blk);
#else
        std::free(blk);
#endif
    }
}

void ImGuiArenaAllocator::mark_warmup_complete() noexcept {
    warmup_done_ = true;
    steady_bump_growth_ = 0;
}

bool install_imgui_arena(ImGuiArenaAllocator& arena) noexcept {
    if (arena.capacity_bytes() == 0 && !arena.init()) {
        return false;
    }
    ImGui::SetAllocatorFunctions(&imgui_arena_alloc_cb, &imgui_arena_free_cb, &arena);
    ImGuiFreeType::SetAllocatorFunctions(&imgui_arena_alloc_cb, &imgui_arena_free_cb, &arena);
    return true;
}

void apply_precision_dark_style(ImGuiStyle& style, float dpi_scale, bool high_contrast) noexcept {
    set_high_contrast_theme(high_contrast);
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::clamp(dpi_scale, 0.75f, 2.5f);

    style = ImGuiStyle{};

    style.WindowPadding = ImVec2(t.panel_pad_px, t.panel_pad_px);
    style.FramePadding = ImVec2(t.panel_pad_px, 3.0f);
    style.ItemSpacing = ImVec2(t.panel_pad_px, t.unit_px);
    style.ItemInnerSpacing = ImVec2(t.unit_px, t.unit_px);
    style.IndentSpacing = 14.0f;
    style.ScrollbarSize = t.scrollbar_px;
    style.GrabMinSize = 8.0f;

    const float border_px = hairline_px(scale);
    style.WindowBorderSize = border_px;
    style.ChildBorderSize = border_px;
    style.PopupBorderSize = border_px;
    style.FrameBorderSize = border_px;
    style.TabBorderSize = 0.0f;

    style.WindowRounding = t.radius_panel_px;
    style.ChildRounding = t.radius_panel_px;
    style.FrameRounding = t.radius_control_px;
    style.PopupRounding = t.radius_popover_px;
    style.ScrollbarRounding = t.radius_popover_px;
    style.GrabRounding = 2.0f;
    style.TabRounding = t.radius_panel_px;

    style.DockingSeparatorSize = t.splitter_px;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    style.ScaleAllSizes(scale);
    // Keep physical hairlines crisp after ScaleAllSizes
    style.WindowBorderSize = border_px;
    style.ChildBorderSize = border_px;
    style.PopupBorderSize = border_px;
    style.FrameBorderSize = border_px;

    auto* c = style.Colors;
    c[ImGuiCol_Text] = t.text_primary;
    c[ImGuiCol_TextDisabled] = t.text_disabled;
    c[ImGuiCol_WindowBg] = t.bg_panel;
    c[ImGuiCol_ChildBg] = t.bg_panel;
    c[ImGuiCol_PopupBg] = t.bg_overlay;
    c[ImGuiCol_Border] = t.border_default;
    c[ImGuiCol_BorderShadow] = t.transparent;

    c[ImGuiCol_FrameBg] = t.bg_inset;
    c[ImGuiCol_FrameBgHovered] = t.bg_hover;
    c[ImGuiCol_FrameBgActive] = t.bg_selected;

    c[ImGuiCol_TitleBg] = t.bg_app;
    c[ImGuiCol_TitleBgActive] = t.bg_app;
    c[ImGuiCol_TitleBgCollapsed] = t.bg_app;

    c[ImGuiCol_MenuBarBg] = t.bg_app;
    c[ImGuiCol_ScrollbarBg] = t.bg_panel;
    c[ImGuiCol_ScrollbarGrab] = t.scrollbar_thumb;
    c[ImGuiCol_ScrollbarGrabHovered] = t.scrollbar_hover;
    c[ImGuiCol_ScrollbarGrabActive] = t.accent;

    c[ImGuiCol_CheckMark] = t.accent;
    c[ImGuiCol_SliderGrab] = t.accent;
    c[ImGuiCol_SliderGrabActive] = t.accent_hover;

    c[ImGuiCol_Button] = t.bg_raised;
    c[ImGuiCol_ButtonHovered] = t.bg_hover;
    c[ImGuiCol_ButtonActive] = t.accent_pressed;

    c[ImGuiCol_Header] = t.bg_selected;
    c[ImGuiCol_HeaderHovered] = t.bg_hover;
    c[ImGuiCol_HeaderActive] = t.accent_pressed;

    c[ImGuiCol_Separator] = t.border_default;
    c[ImGuiCol_SeparatorHovered] = t.accent;
    c[ImGuiCol_SeparatorActive] = t.accent_pressed;

    c[ImGuiCol_ResizeGrip] = t.border_strong;
    c[ImGuiCol_ResizeGripHovered] = t.accent;
    c[ImGuiCol_ResizeGripActive] = t.accent_pressed;

    c[ImGuiCol_Tab] = t.bg_app;
    c[ImGuiCol_TabHovered] = t.bg_hover;
    c[ImGuiCol_TabSelected] = t.bg_panel;
    c[ImGuiCol_TabSelectedOverline] = t.accent;
    c[ImGuiCol_TabDimmed] = t.bg_app;
    c[ImGuiCol_TabDimmedSelected] = t.bg_panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = t.border_strong;

    c[ImGuiCol_DockingPreview] = t.docking_preview;
    c[ImGuiCol_DockingEmptyBg] = t.bg_app;

    c[ImGuiCol_PlotLines] = t.accent;
    c[ImGuiCol_PlotHistogram] = t.viz_t1;
    c[ImGuiCol_TableHeaderBg] = t.bg_raised;
    c[ImGuiCol_TableBorderStrong] = t.border_default;
    c[ImGuiCol_TableBorderLight] = t.border_subtle;
    c[ImGuiCol_TableRowBg] = t.bg_panel;
    c[ImGuiCol_TableRowBgAlt] = t.bg_inset;

    c[ImGuiCol_ModalWindowDimBg] = t.scrim_modal;
    c[ImGuiCol_NavCursor] = t.accent;
}

UiFontSet load_precision_dark_fonts(const RuntimePaths& paths, float dpi_scale) {
    UiFontSet out{};
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.Fonts->FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();
    io.Fonts->FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;

    const PrecisionDarkTokens& t = tokens();
    const float scale = dpi_scale >= 0.5f ? dpi_scale : 1.0f;

    const std::string inter_regular = resolve_asset_path(paths, "fonts/Inter-Regular-tnum.ttf");
    const std::string inter_semibold = resolve_asset_path(paths, "fonts/Inter-SemiBold-tnum.ttf");
    const std::string jetbrains_mono = resolve_asset_path(paths, "fonts/JetBrainsMono-Regular.ttf");
    const std::string icons_ttf = resolve_asset_path(paths, "fonts/AetherIcons-Regular.ttf");
    const std::string jp_ttf = resolve_asset_path(paths, "fonts/NotoSansJP-Subset.ttf");

    ImFontConfig cfg{};
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;

    std::error_code ec;
    if (std::filesystem::exists(inter_regular, ec)) {
        const float body_px = std::round(t.type_body_px * scale);
        out.body = io.Fonts->AddFontFromFileTTF(inter_regular.c_str(), body_px, &cfg);
        merge_icons_and_cjk(io, body_px, scale, icons_ttf, jp_ttf, true);

        const float cap_px = std::round(t.type_caption_px * scale);
        out.caption = io.Fonts->AddFontFromFileTTF(inter_regular.c_str(), cap_px, &cfg);
        merge_icons_and_cjk(io, cap_px, scale, icons_ttf, jp_ttf, true);
    }
    if (std::filesystem::exists(inter_semibold, ec)) {
        const float strong_px = std::round(t.type_body_px * scale);
        out.body_strong = io.Fonts->AddFontFromFileTTF(inter_semibold.c_str(), strong_px, &cfg);
        merge_icons_and_cjk(io, strong_px, scale, icons_ttf, jp_ttf, false);

        const float title_px = std::round(t.type_title_px * scale);
        out.title = io.Fonts->AddFontFromFileTTF(inter_semibold.c_str(), title_px, &cfg);
        merge_icons_and_cjk(io, title_px, scale, icons_ttf, jp_ttf, false);

        const float disp_px = std::round(t.type_display_px * scale);
        out.display = io.Fonts->AddFontFromFileTTF(inter_semibold.c_str(), disp_px, &cfg);
        merge_icons_and_cjk(io, disp_px, scale, icons_ttf, jp_ttf, false);
    }
    if (std::filesystem::exists(jetbrains_mono, ec)) {
        const float mono_px = std::round(t.type_mono_px * scale);
        out.mono = io.Fonts->AddFontFromFileTTF(jetbrains_mono.c_str(), mono_px, &cfg);
        merge_icons_and_cjk(io, mono_px, scale, icons_ttf, jp_ttf, false);
    }

    if (out.body == nullptr) {
        AETHER_LOG_WARN("ui", "Bundled UI font missing (%s); falling back to ImGui ProggyClean", inter_regular.c_str());
        out.body = io.Fonts->AddFontDefault(&cfg);
    }
    if (out.body_strong == nullptr) {
        out.body_strong = out.body;
    }
    if (out.caption == nullptr) {
        out.caption = out.body;
    }
    if (out.title == nullptr) {
        out.title = out.body_strong;
    }
    if (out.display == nullptr) {
        out.display = out.title;
    }
    if (out.mono == nullptr) {
        out.mono = out.body;
    }

    io.FontDefault = out.body;
    io.Fonts->Build();
    return out;
}

void draw_elevation_shadow(ImDrawList* draw_list, ImVec2 p_min, ImVec2 p_max, float rounding) noexcept {
    if (draw_list == nullptr) {
        return;
    }
    const ImU32 col = tokens().col32_shadow_soft;
    for (int i = 1; i <= 3; ++i) {
        const float expand = static_cast<float>(i) * 2.0f;
        draw_list->AddRectFilled(ImVec2(p_min.x - expand, p_min.y - expand * 0.5f + 2.0f),
                                 ImVec2(p_max.x + expand, p_max.y + expand + 2.0f), col, rounding + expand);
    }
}

float hairline_px(float dpi_scale) noexcept {
    const float rounded = std::round(dpi_scale);
    return rounded > 1.0f ? rounded : 1.0f;
}

} // namespace aether::ui
