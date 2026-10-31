// The ONLINE lobby's look; see lobby_style.hpp.
//
// The font is the game's own (src/enhancements/sharp_font: the 95 glyphs in the ROM, enlarged eight
// times with Super-xBR), served to ImGui by a font loader that scales those pictures to whatever size
// ImGui asks for. The game maps characters to glyphs with the table D_80085930 (func_15042C40: lower
// case as upper case, a space four texels), read from RDRAM once the game has it. Characters the game
// has no glyph for come from ImGui's own font, merged behind it.
// The button pictures are the ones the game shows (src/enhancements/pad_icons), packed into ImGui's
// font atlas as rectangles of their own.
// The panel is planked with the wood of the MULTI menus' wheel: flat resource 4343 (the flat stream's
// entry 4341, see menu_art), a 32x64 CI8 texture with its 256-colour RGBA16 palette after it, odd
// rows stored with their 4-byte groups swapped as LoadBlock leaves them in TMEM. Found by dumping the
// textures at the MULTI menu (CONKER_DUMP_TEXTURES) and matching the dump's bytes against the flat
// resources; decoded here, it equals RT64's dump of it (texture hash 2ef66c27bf9ba282) pixel for pixel.
#include "platform/lobby_style.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <string>

#include "imgui/imgui_internal.h"
#include "librecomp/game.hpp"
#include "enhancements/pad_icons/button_art.hpp"
#include "enhancements/pad_icons/pad_icons.hpp"
#include "enhancements/sharp_font/sharp_font.hpp"
#include "enhancements/sharp_menus/menu_art.hpp"

namespace conker::lobby_style {

namespace {

constexpr uint32_t kCharacterTable = 0x85930;  // D_80085930: the character of each glyph
constexpr int kGlyphs = 95;
constexpr int kSpaceGlyph = kGlyphs + 1;  // ImGui's index for the space (0 is "none")
constexpr float kLineTexels = 15.0f;      // a line of text, in the game's texels
constexpr float kSpaceTexels = 4.0f;      // func_150428D4 advances a space this far

bool g_no_font = false;  // the ROM's font was not to be had: ImGui's stays

std::vector<sharp_font::LargeGlyph> g_glyphs;
std::array<uint8_t, kGlyphs> g_characters{};
ImFont* g_font = nullptr;
float g_scale = 1.0f;

// --- the font -------------------------------------------------------------------------------------

int glyph_index(ImFontAtlas*, ImFontConfig*, ImWchar codepoint) {
    if (codepoint == ' ') {
        return kSpaceGlyph;
    }
    const ImWchar c = (codepoint >= 'a' && codepoint <= 'z') ? codepoint - 0x20 : codepoint;
    for (int i = 0; i < kGlyphs; i++) {
        if (g_characters[i] == c && c != 0) {
            return i + 1;
        }
    }
    return 0;
}

bool contains_glyph(ImFontAtlas*, ImFontConfig*, int index) { return index != 0; }

bool baked_init(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
    if (!src->MergeMode) {
        baked->Ascent = std::ceil(baked->Size * 12.0f / kLineTexels);
        baked->Descent = baked->Ascent - baked->Size;
    }
    return true;
}

// The glyph `scale` pixels a texel, from its enlarged picture: each pixel the mean of the picture's
// pixels it covers.
std::vector<uint8_t> resample(const sharp_font::LargeGlyph& g, float scale, int w, int h) {
    std::vector<uint8_t> out(size_t(w) * h);
    const int sw = g.width * g.scale, sh = g.height * g.scale;
    const float step = g.scale / scale;  // picture pixels a pixel
    for (int y = 0; y < h; y++) {
        const int y0 = std::clamp(int(y * step), 0, sh - 1), y1 = std::clamp(int(std::ceil((y + 1) * step)), y0 + 1, sh);
        for (int x = 0; x < w; x++) {
            const int x0 = std::clamp(int(x * step), 0, sw - 1), x1 = std::clamp(int(std::ceil((x + 1) * step)), x0 + 1, sw);
            int sum = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) sum += g.alpha[size_t(sy) * sw + sx];
            out[size_t(y) * w + x] = uint8_t(sum / ((y1 - y0) * (x1 - x0)));
        }
    }
    return out;
}

bool load_glyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, int index, ImFontGlyph* out_glyph,
                float* out_advance_x) {
    const float texel = baked->Size / kLineTexels;  // pixels a texel, in layout
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    if (index == kSpaceGlyph) {
        if (out_advance_x != nullptr) {
            *out_advance_x = kSpaceTexels * texel;
        } else {
            out_glyph->AdvanceX = kSpaceTexels * texel;
        }
        return true;
    }
    const sharp_font::LargeGlyph& g = g_glyphs[index - 1];
    // A texel further than the game's spacing: its letters touch at this size.
    const float advance = (g.advance + 0.6f) * texel;
    if (out_advance_x != nullptr) {
        *out_advance_x = advance;
        return true;
    }
    out_glyph->AdvanceX = advance;
    const float raster = texel * density;  // pixels a texel, in the atlas
    const int w = std::max(1, int(std::ceil(g.width * raster))), h = std::max(1, int(std::ceil(g.height * raster)));
    const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, w, h);
    if (pack == ImFontAtlasRectId_Invalid) {
        return false;
    }
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack);
    const std::vector<uint8_t> pixels = resample(g, raster, w, h);
    const float top = (g.top + 1.0f) * texel;
    out_glyph->X0 = 0.0f;
    out_glyph->Y0 = top;
    out_glyph->X1 = w / density;
    out_glyph->Y1 = top + h / density;
    out_glyph->Visible = true;
    out_glyph->PackId = pack;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, pixels.data(), ImTextureFormat_Alpha8, w);
    return true;
}

const ImFontLoader* game_font_loader() {
    static ImFontLoader loader = [] {
        ImFontLoader l;
        l.Name = "conker";
        l.FontSrcGetGlyphIndexFromCodepoint = glyph_index;
        l.FontSrcContainsGlyph = contains_glyph;
        l.FontBakedInit = baked_init;
        l.FontBakedLoadGlyph = load_glyph;
        return l;
    }();
    return &loader;
}

void make_font(const uint8_t* rdram) {
    if (g_font != nullptr || g_no_font || rdram == nullptr) {
        return;
    }
    // Filled in when the game boots: digits first, then the letters.
    for (int i = 0; i < kGlyphs; i++) {
        g_characters[i] = rdram[(kCharacterTable + i) ^ 3];
    }
    if (g_characters[0] != '0' || g_characters[10] != 'A') {
        return;
    }
    if (g_glyphs.empty()) {
        g_glyphs = sharp_font::large_glyphs(recomp::get_rom());
        if (g_glyphs.size() != kGlyphs) {
            g_no_font = true;
            return;
        }
    }
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImFontConfig config;
    std::snprintf(config.Name, sizeof(config.Name), "Conker");
    config.FontLoader = game_font_loader();
    config.SizePixels = 20.0f;
    g_font = atlas->AddFont(&config);
    ImFontConfig fallback;
    fallback.MergeMode = true;
    fallback.SizePixels = 20.0f;
    atlas->AddFontDefault(&fallback);
}

// --- the button pictures --------------------------------------------------------------------------

struct Art {
    ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
    int width = 0, height = 0;
};
std::map<std::string, Art> g_art;  // "a" and "b": the pad's A and B, or Enter and Esc
pad_icons::Style g_art_style = pad_icons::Style::Original;

constexpr int kArtSize = 64;  // pixels for a 32x32-texel picture in the atlas

void put_art(const std::string& name, const pad_icons::Picture& picture) {
    // Only what is drawn: the pictures leave room round a button for the game's layout.
    int x0 = picture.width, y0 = picture.height, x1 = 0, y1 = 0;
    for (int y = 0; y < picture.height; y++)
        for (int x = 0; x < picture.width; x++)
            if (picture.rgba[(size_t(y) * picture.width + x) * 4 + 3] > 8) {
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
            }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    const int h = kArtSize;
    const int w = std::clamp(kArtSize * (x1 - x0) / (y1 - y0), 1, kArtSize * 4);
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    Art& art = g_art[name];
    if (art.rect == ImFontAtlasRectId_Invalid || art.width != w) {
        art.rect = atlas->AddCustomRect(w, h);
    }
    art.width = w, art.height = h;
    ImFontAtlasRect r;
    if (art.rect == ImFontAtlasRectId_Invalid || !atlas->GetCustomRect(art.rect, &r)) {
        return;
    }
    ImTextureData* tex = atlas->TexData;
    const float sx = float(x1 - x0) / w, sy = float(y1 - y0) / h;
    for (int y = 0; y < h; y++) {
        uint8_t* row = static_cast<uint8_t*>(tex->GetPixelsAt(r.x, r.y + y));
        for (int x = 0; x < w; x++) {
            int sum[4] = {};
            int count = 0;
            const int py0 = y0 + int(y * sy), py1 = std::max(py0 + 1, y0 + int((y + 1) * sy));
            const int px0 = x0 + int(x * sx), px1 = std::max(px0 + 1, x0 + int((x + 1) * sx));
            for (int py = py0; py < py1; py++)
                for (int px = px0; px < px1; px++, count++) {
                    const uint8_t* p = &picture.rgba[(size_t(py) * picture.width + px) * 4];
                    // Premultiplied while averaging, so that transparent pixels add no colour.
                    for (int c = 0; c < 3; c++) sum[c] += p[c] * p[3];
                    sum[3] += p[3];
                }
            uint8_t* out = row + x * 4;
            for (int c = 0; c < 3; c++) out[c] = uint8_t(sum[3] > 0 ? sum[c] / sum[3] : 0);
            out[3] = uint8_t(count > 0 ? sum[3] / count : 0);
        }
    }
    ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, w, h);
}

// The wheel's wood as it stands (32 wide, 64 high), with its edge texels repeated round it: the
// boards meet their repeats mirrored, so filtering at a tile's edge blends with the edge itself, not
// with the atlas (or with the far side, which drew a light line at every repeat).
constexpr int kPlankResource = 4343;
constexpr int kPlankLength = 64, kPlankWidth = 32;
Art g_wood;
bool g_wood_tried = false;

void make_wood() {
    if (g_wood_tried) {
        return;
    }
    g_wood_tried = true;
    const std::vector<uint8_t> bytes = menu_art::load_resource(kPlankResource);
    if (bytes.size() != size_t(kPlankWidth * kPlankLength + 512)) {
        std::fprintf(stderr, "[conker] lobby: no wood (resource %d is %zu bytes); drawing planks\n", kPlankResource,
                     bytes.size());
        return;
    }
    const auto texel = [&](int x, int y) {  // the texture as stored: 32 wide, 64 high
        x = std::clamp(x, 0, kPlankWidth - 1), y = std::clamp(y, 0, kPlankLength - 1);
        const uint8_t index = bytes[size_t(y) * kPlankWidth + (x ^ ((y & 1) ? 4 : 0))];
        const int c = (bytes[kPlankWidth * kPlankLength + index * 2] << 8) | bytes[kPlankWidth * kPlankLength + index * 2 + 1];
        return std::array<uint8_t, 4>{uint8_t(((c >> 11) & 31) * 255 / 31), uint8_t(((c >> 6) & 31) * 255 / 31),
                                      uint8_t(((c >> 1) & 31) * 255 / 31), uint8_t(255)};
    };
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->TexPixelsUseColors = true;
    const int w = kPlankWidth + 2, h = kPlankLength + 2;
    g_wood.rect = atlas->AddCustomRect(w, h);
    g_wood.width = kPlankWidth, g_wood.height = kPlankLength;
    ImFontAtlasRect r;
    if (g_wood.rect == ImFontAtlasRectId_Invalid || !atlas->GetCustomRect(g_wood.rect, &r)) {
        g_wood.rect = ImFontAtlasRectId_Invalid;
        return;
    }
    for (int y = 0; y < h; y++) {
        uint8_t* row = static_cast<uint8_t*>(atlas->TexData->GetPixelsAt(r.x, r.y + y));
        for (int x = 0; x < w; x++) {
            const std::array<uint8_t, 4> p = texel(x - 1, y - 1);
            std::memcpy(row + x * 4, p.data(), 4);
        }
    }
    ImFontAtlasTextureBlockQueueUpload(atlas, atlas->TexData, r.x, r.y, w, h);
}

void make_art() {
    pad_icons::Style style = pad_icons::style();
    if (style == pad_icons::Style::Original) {
        style = pad_icons::Style::N64;
    }
    if (style == g_art_style) {
        return;
    }
    g_art_style = style;
    ImGui::GetIO().Fonts->TexPixelsUseColors = true;
    if (style == pad_icons::Style::Keyboard) {
        for (const pad_icons::Picture& p : pad_icons::draw_keys({"ESC", "ENTER"})) {
            put_art(p.name == "ESC" ? "b" : "a", p);
        }
        return;
    }
    for (const pad_icons::Picture& p : pad_icons::draw_set(style, false)) {
        if (p.name == "a" || p.name == "b") {
            put_art(p.name, p);
        }
    }
}

// --- colours --------------------------------------------------------------------------------------

ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

// The MULTI join screen's planks and the menus' text.
const ImVec4 kWoodDark = rgb(46, 26, 14);
const ImVec4 kCream = rgb(250, 236, 200);
const ImVec4 kGold = rgb(236, 186, 72);

} // namespace

void prepare(const uint8_t* rdram) {
    make_font(rdram);
    // Once the game is running: before, the ROM is not loaded, and menu_art keeps its table of the
    // resources from the first look.
    if (g_font != nullptr) {
        make_wood();
    }
    make_art();
}

ImFont* font() { return g_font; }

ImU32 grey() { return IM_COL32(214, 188, 146, 255); }
ImU32 red() { return IM_COL32(255, 92, 72, 255); }
ImU32 green() { return IM_COL32(164, 226, 96, 255); }
ImU32 yellow() { return IM_COL32(246, 200, 76, 255); }

ImU32 player(int index) {
    static const ImU32 colours[4] = {IM_COL32(236, 60, 48, 255), IM_COL32(84, 120, 255, 255),
                                     IM_COL32(72, 196, 84, 255), IM_COL32(250, 206, 48, 255)};
    return colours[index & 3];
}

void apply(float scale) {
    g_scale = scale;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.ItemSpacing = ImVec2(12.0f, 7.0f);
    style.FramePadding = ImVec2(12.0f, 5.0f);
    style.WindowBorderSize = 3.0f;
    style.WindowRounding = 16.0f;
    // Buttons as the game's menu plates: rounded right round, with a gold rim.
    style.FrameRounding = 14.0f;
    style.FrameBorderSize = 2.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kCream;
    c[ImGuiCol_TextDisabled] = rgb(150, 120, 86);
    c[ImGuiCol_WindowBg] = kWoodDark;
    c[ImGuiCol_Border] = kGold;
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = rgb(30, 16, 6);
    c[ImGuiCol_FrameBg] = rgb(30, 17, 8, 0.92f);
    c[ImGuiCol_FrameBgHovered] = rgb(44, 26, 12);
    c[ImGuiCol_FrameBgActive] = rgb(56, 32, 14);
    c[ImGuiCol_Button] = rgb(64, 36, 16);
    c[ImGuiCol_ButtonHovered] = rgb(120, 72, 30);
    c[ImGuiCol_ButtonActive] = rgb(160, 100, 40);
    c[ImGuiCol_Header] = rgb(88, 50, 22);
    c[ImGuiCol_HeaderHovered] = rgb(120, 70, 30);
    c[ImGuiCol_HeaderActive] = rgb(140, 84, 36);
    c[ImGuiCol_Separator] = rgb(30, 16, 6, 0.9f);
    c[ImGuiCol_TableHeaderBg] = rgb(30, 17, 8, 0.7f);
    c[ImGuiCol_TableRowBgAlt] = rgb(0, 0, 0, 0.12f);
    c[ImGuiCol_TableBorderLight] = rgb(110, 70, 36);
    c[ImGuiCol_TableBorderStrong] = rgb(130, 86, 44);
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_TextSelectedBg] = rgb(180, 120, 50, 0.6f);
    c[ImGuiCol_CheckMark] = kGold;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale * (g_font == nullptr ? 1.4f : 1.12f);
}

// Planks drawn as shades with grain lines, where the wheel's wood is not to be had.
void drawn_planks(ImDrawList* draw, ImVec2 min, ImVec2 max, float plank, float rounding) {
    static const ImVec4 shades[] = {rgb(104, 58, 26), rgb(118, 68, 30), rgb(96, 52, 22), rgb(126, 74, 34), rgb(110, 62, 28)};
    int i = 0;
    for (float y = min.y; y < max.y; y += plank, i++) {
        const ImVec4 s = shades[i % 5];
        const float y1 = std::min(y + plank, max.y);
        const ImDrawFlags corners = (y == min.y ? ImDrawFlags_RoundCornersTop : 0) |
                                    (y1 >= max.y ? ImDrawFlags_RoundCornersBottom : 0);
        draw->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, y1), ImGui::GetColorU32(s), corners != 0 ? rounding : 0.0f,
                            corners != 0 ? corners : ImDrawFlags_RoundCornersNone);
        for (int line = 0; line < 5; line++) {
            const float base = y + plank * (0.15f + 0.17f * line) + std::sin(i * 3.1f + line) * 2.0f * g_scale;
            ImVec2 points[24];
            for (int p = 0; p < 24; p++) {
                const float x = min.x + (max.x - min.x) * p / 23.0f;
                points[p] = ImVec2(x, base + std::sin(p * 0.7f + i * 1.3f + line * 2.1f) * 1.6f * g_scale);
            }
            draw->AddPolyline(points, 24, ImGui::GetColorU32(ImVec4(s.x * 0.72f, s.y * 0.68f, s.z * 0.64f, 0.8f)),
                              ImDrawFlags_None, (line % 2 ? 1.0f : 1.6f) * g_scale);
        }
    }
}

// The wheel's wood: upright boards, each starting the texture at its own height and every other one
// mirrored, so that the repeats do not line up; shaded a little differently board by board. Its ends
// do not meet (the wheel shows it once), so along a board every other repeat is upside down.
void wood_planks(ImDrawList* draw, ImVec2 min, ImVec2 max, float board, float rounding) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImFontAtlasRect r;
    if (!atlas->GetCustomRect(g_wood.rect, &r)) {
        drawn_planks(draw, min, max, board, rounding);
        return;
    }
    const float texel = board / kPlankWidth;
    const float length = kPlankLength * texel;
    const ImVec2 uv_scale = atlas->TexUvScale;
    const float u0 = (r.x + 1) * uv_scale.x, v0 = (r.y + 1) * uv_scale.y;
    const float du = kPlankWidth * uv_scale.x, dv = kPlankLength * uv_scale.y;
    static const float shades[] = {0.62f, 0.54f, 0.68f, 0.50f, 0.58f};
    int column = 0;
    for (float x = min.x; x < max.x; x += board, column++) {
        const float x1 = std::min(x + board, max.x);
        float a = u0, b = u0 + du * (x1 - x) / board;
        if ((column & 1) != 0) {
            a = 2 * u0 + du - a, b = 2 * u0 + du - b;
        }
        const float shift = std::fmod(column * 37.0f, kPlankLength) * texel;
        const int s = int(255 * shades[column % 5]);
        const ImU32 tint = IM_COL32(s, s, s, 255);
        int repeat = 0;
        for (float y = min.y - shift; y < max.y; y += length, repeat++) {
            const float y0 = std::max(y, min.y), y1 = std::min(y + length, max.y);
            float c = v0 + dv * (y0 - y) / length, d = v0 + dv * (y1 - y) / length;
            if ((repeat & 1) != 0) {
                c = 2 * v0 + dv - c, d = 2 * v0 + dv - d;
            }
            ImDrawFlags corners = 0;
            if (y0 == min.y && x == min.x) corners |= ImDrawFlags_RoundCornersTopLeft;
            if (y0 == min.y && x1 >= max.x) corners |= ImDrawFlags_RoundCornersTopRight;
            if (y1 >= max.y && x == min.x) corners |= ImDrawFlags_RoundCornersBottomLeft;
            if (y1 >= max.y && x1 >= max.x) corners |= ImDrawFlags_RoundCornersBottomRight;
            draw->AddImageRounded(atlas->TexRef, ImVec2(x, y0), ImVec2(x1, y1), ImVec2(a, c), ImVec2(b, d), tint,
                                  corners != 0 ? rounding : 0.0f, corners != 0 ? corners : ImDrawFlags_RoundCornersNone);
        }
    }
    // The dark gaps between boards (no highlight: a light line every board read as a grid).
    for (float x = min.x + board; x < max.x; x += board) {
        draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(20, 10, 4, 200), 2.0f * g_scale);
    }
}

void panel() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const float rounding = ImGui::GetStyle().WindowRounding;
    const float plank = 46.0f * g_scale;
    draw->PushClipRect(min, max, true);
    if (g_wood.rect != ImFontAtlasRectId_Invalid) {
        wood_planks(draw, min, max, 76.0f * g_scale, rounding);
    } else {
        drawn_planks(draw, min, max, plank, rounding);
        // The seams between planks, each with a highlight along the plank's top edge.
        for (float y = min.y + plank; y < max.y; y += plank) {
            draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), IM_COL32(28, 14, 6, 255), 2.5f * g_scale);
            draw->AddLine(ImVec2(min.x, y + 2.0f * g_scale), ImVec2(max.x, y + 2.0f * g_scale),
                          IM_COL32(255, 210, 150, 40), 1.0f * g_scale);
        }
    }
    // A dark wash over the middle, so that text stays readable over the grain.
    const float inset = 10.0f * g_scale;
    draw->AddRectFilled(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset),
                        IM_COL32(20, 10, 4, 120), rounding * 0.6f);
    draw->PopClipRect();
}

void heading(const std::string& text) {
    if (g_font == nullptr) {
        ImGui::TextUnformatted(text.c_str());
        return;
    }
    // Large gold letters with a dark outline, as the game's signs.
    const float size = ImGui::GetFontSize() * 1.8f;
    ImFont* f = ImGui::GetFont();
    const ImVec2 extent = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const float x = ImGui::GetCursorScreenPos().x + (ImGui::GetContentRegionAvail().x - extent.x) * 0.5f;
    const float y = ImGui::GetCursorScreenPos().y;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float o = 2.0f * g_scale;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0) draw->AddText(f, size, ImVec2(x + dx * o, y + dy * o + o), IM_COL32(30, 12, 4, 255), text.c_str());
    draw->AddText(f, size, ImVec2(x, y), ImGui::GetColorU32(kGold), text.c_str());
    ImGui::Dummy(ImVec2(extent.x, extent.y + o * 2));
}

void prompts(const std::vector<std::pair<std::string, std::string>>& controls) {
    if (g_art.empty()) {
        std::string text;
        for (const auto& [control, what] : controls) {
            const bool keys = pad_icons::style() == pad_icons::Style::Keyboard;
            const std::string name = control == "a" ? (keys ? "Enter" : "A") : (keys ? "Esc" : "B");
            text += (text.empty() ? "" : "    ") + name + ": " + what;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, grey());
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const float h = ImGui::GetFontSize() * 1.3f;
    bool first = true;
    for (const auto& [control, what] : controls) {
        const auto found = g_art.find(control);
        ImFontAtlasRect r;
        if (found == g_art.end() || !atlas->GetCustomRect(found->second.rect, &r)) {
            continue;
        }
        if (!first) {
            ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
        }
        first = false;
        const float w = h * found->second.width / found->second.height;
        ImGui::Image(atlas->TexRef, ImVec2(w, h), r.uv0, r.uv1);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, grey());
        ImGui::TextUnformatted(what.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace conker::lobby_style
