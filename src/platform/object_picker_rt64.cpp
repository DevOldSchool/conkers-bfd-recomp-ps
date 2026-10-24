// What is under the mouse, for the object inspector (src/enhancements/object_inspector).
//
// RT64 hands each frame's workload to on_workload once its draw calls are complete, before they
// are drawn (RT64::SetWorkloadHook, tools/patches/rt64-object-picking.patch). Every draw call
// carries the game's draw tag and the display list its triangles came from, and RT64 has already
// placed every vertex on the screen on the CPU (drawData.posScreen, as its own debugger's
// right click uses them). The mouse is taken from window to frame buffer pixels through where the
// last frame was presented, widened as Expand widens the picture.
//
// The draw calls are tried in the order they are drawn, as the depth buffer would: a triangle under
// the mouse is seen if its call does not compare depth or is no farther than what was seen there
// before, and it becomes the depth there if the call writes depth. Only frame buffers as wide as the
// screen's count (not the small ones shadows and effects draw into first), each with its own depth.
// Textured 2D rectangles count over everything drawn before them; text never does, nor do fills. A
// texture's transparent texels are not known, so a cut-out leaf hides what is behind its gaps. A
// rectangle drawing a sprite tile is named by the resource func_1510D0EC loaded there.
//
// The wheel steps through the things under the mouse, front to back: what is in front, then whatever
// else covers that spot, nearest first, so a cut-out's gaps can be looked through.
//
// Every draw call of the thing under the mouse is tinted cyan, and of the picked-out thing magenta,
// pulsing (RT64's debugger highlight colour, drawn over the call's own colour).
#include "platform/object_picker_rt64.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "enhancements/object_inspector/object_inspector.hpp"
#include "enhancements/sharp_menus/sharp_menus.hpp"
#include "hle/rt64_color_converter.h"
#include "hle/rt64_workload.h"
#include "rhi/rt64_render_hooks.h"

namespace conker::renderer {

namespace {

namespace inspector = conker::inspector;
using inspector::Hit;
using inspector::TagKind;

Hit g_picked;
uint32_t g_picked_serial = 0;
uint32_t g_frames = 0;

// The mouse in frame buffer pixels, if it is over the picture.
bool cursor_on_picture(float* x, float* y, RT64::PresentedPicture* picture, int* layer) {
    const inspector::Cursor cursor = inspector::cursor();
    *layer = cursor.layer;
    if (!cursor.inside || !RT64::GetPresentedPicture(*picture) || picture->width <= 0.0f || picture->height <= 0.0f ||
        picture->fbWidth == 0 || picture->fbHeight == 0) {
        return false;
    }
    const float window_x = cursor.x * picture->swapChainWidth;
    const float window_y = cursor.y * picture->swapChainHeight;
    const float across = (window_x - (picture->x + picture->width / 2.0f)) / (picture->width / 2.0f) * picture->widening;
    const float down = (window_y - (picture->y + picture->height / 2.0f)) / (picture->height / 2.0f);
    if (std::fabs(down) > 1.0f || std::fabs(across) > picture->widening) {
        return false;
    }
    *x = picture->fbWidth / 2.0f * (1.0f + across);
    *y = picture->fbHeight / 2.0f * (1.0f + down);
    return true;
}

// RT64's hash of the call's first texture, 0 if it has none.
uint64_t texture_hash(const RT64::Workload& workload, const RT64::DrawCall& desc) {
    if (desc.tileCount == 0 || desc.tileIndex >= workload.drawData.callTiles.size()) {
        return 0;
    }
    return workload.drawData.callTiles[desc.tileIndex].tmemHashOrID;
}

// The depth at which the triangle (screen corners) covers (x, y), or NaN.
float corner_depth(const hlslpp::float3 corners[3], float x, float y) {
    const float x0 = corners[0].x, y0 = corners[0].y;
    const float x1 = corners[1].x, y1 = corners[1].y;
    const float x2 = corners[2].x, y2 = corners[2].y;
    if (x < std::min({x0, x1, x2}) || x > std::max({x0, x1, x2}) || y < std::min({y0, y1, y2}) ||
        y > std::max({y0, y1, y2})) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-6f) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const float u = ((x - x0) * (y2 - y0) - (x2 - x0) * (y - y0)) / area;
    const float v = ((x1 - x0) * (y - y0) - (x - x0) * (y1 - y0)) / area;
    if (u < 0.0f || v < 0.0f || u + v > 1.0f) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return corners[0].z + (corners[1].z - corners[0].z) * u + (corners[2].z - corners[0].z) * v;
}

// The depth at which the call's nearest triangle covers (x, y), or NaN if none does. A triangle that
// reaches behind the near plane is clipped there first, as the GPU does: its corners behind the eye
// land anywhere on the screen.
float triangle_depth(const RT64::Workload& workload, const RT64::Projection& projection, const RT64::GameCall& call,
                     float x, float y) {
    const auto& draw_data = workload.drawData;
    const interop::RSPViewport& viewport = draw_data.rspViewports[projection.transformsIndex];
    const auto to_screen = [&](const hlslpp::float4& clip) {
        return hlslpp::float3(clip.xyz / hlslpp::float3(clip.w, -clip.w, clip.w)) * viewport.scale + viewport.translate;
    };
    float nearest = std::numeric_limits<float>::quiet_NaN();
    const auto consider = [&](const hlslpp::float3 corners[3]) {
        const float depth = corner_depth(corners, x, y);
        if (depth >= 0.0f && depth <= 1.0f && !(depth >= nearest)) {
            nearest = depth;
        }
    };
    for (uint32_t t = 0; t < call.callDesc.triangleCount; t++) {
        hlslpp::float3 corners[3];
        if (!projection.usesViewport()) {
            for (uint32_t i = 0; i < 3; i++) {
                const uint32_t vertex = call.meshDesc.rawVertexStart + t * 3 + i;
                corners[i] = hlslpp::float3(draw_data.triPosFloats[vertex * 4 + 0], draw_data.triPosFloats[vertex * 4 + 1],
                                            draw_data.triPosFloats[vertex * 4 + 2]);
            }
            consider(corners);
            continue;
        }
        uint32_t indices[3];
        hlslpp::float4 clip[3];
        int behind = 0;
        for (uint32_t i = 0; i < 3; i++) {
            indices[i] = draw_data.faceIndices[call.meshDesc.faceIndicesStart + t * 3 + i];
            clip[i] = draw_data.posTransformed[indices[i]];
            behind += float(clip[i].z + clip[i].w) < 0.0f ? 1 : 0;
        }
        if (behind == 0) {
            for (uint32_t i = 0; i < 3; i++) {
                corners[i] = draw_data.posScreen[indices[i]];
            }
            consider(corners);
            continue;
        }
        if (behind == 3) {
            continue;
        }
        // Keep the part in front of the near plane (z + w >= 0): a triangle or a quad.
        hlslpp::float4 kept[4];
        int count = 0;
        for (int i = 0; i < 3; i++) {
            const hlslpp::float4& a = clip[i];
            const hlslpp::float4& b = clip[(i + 1) % 3];
            const float da = float(a.z + a.w), db = float(b.z + b.w);
            if (da >= 0.0f) {
                kept[count++] = a;
            }
            if ((da >= 0.0f) != (db >= 0.0f)) {
                kept[count++] = a + (b - a) * (da / (da - db));
            }
        }
        for (int i = 1; i + 1 < count; i++) {
            const hlslpp::float3 fan[3] = {to_screen(kept[0]), to_screen(kept[i]), to_screen(kept[i + 1])};
            consider(fan);
        }
    }
    return nearest;
}

Hit make_hit(const RT64::Workload& workload, const RT64::GameCall& call, bool rect, float x, float y) {
    const RT64::DrawCall& desc = call.callDesc;
    Hit hit;
    hit.valid = true;
    hit.tag = desc.drawTag;
    hit.list = desc.drawList;
    hit.command = rect ? 0 : desc.drawCommand;
    hit.vertices = rect ? 0 : desc.drawVertices;
    hit.triangles = desc.triangleCount;
    hit.call = desc.callIndex;
    hit.texture = texture_hash(workload, desc);
    for (uint32_t i = 0; i < desc.loadCount; i++) {
        const uint32_t index = desc.loadIndex + i;
        if (index < workload.drawData.loadOperations.size() &&
            workload.drawData.loadOperations[index].type != RT64::LoadOperation::Type::TLUT) {
            hit.texture_address = workload.drawData.loadOperations[index].texture.address;
            break;
        }
    }
    hit.combiner[0] = desc.colorCombiner.L;
    hit.combiner[1] = desc.colorCombiner.H;
    hit.other_mode[0] = desc.otherMode.L;
    hit.other_mode[1] = desc.otherMode.H;
    hit.rect = rect;
    hit.screen_x = x;
    hit.screen_y = y;
    return hit;
}

// Everything at (x, y), front to back: first what the depth buffer would show at the end of the frame,
// then the rest by depth, one hit for each thing.
std::vector<Hit> find_hits(const RT64::Workload& workload, const RT64::PresentedPicture& picture, float x, float y) {
    struct Behind {
        float depth;
        Hit hit;
    };
    std::vector<Behind> all;
    Hit seen;
    const int32_t fixed_x = static_cast<int32_t>(std::lround(x * 4.0f));
    const int32_t fixed_y = static_cast<int32_t>(std::lround(y * 4.0f));
    for (uint32_t f = 0; f < workload.fbPairCount; f++) {
        const RT64::FramebufferPair& pair = workload.fbPairs[f];
        if (pair.colorImage.width != picture.fbWidth) {
            continue;
        }
        float depth_buffer = std::numeric_limits<float>::infinity();
        for (uint32_t p = 0; p < pair.projectionCount; p++) {
            const RT64::Projection& projection = pair.projections[p];
            const bool rects = projection.type == RT64::Projection::Type::Rectangle;
            for (uint32_t d = 0; d < projection.gameCallCount; d++) {
                const RT64::GameCall& call = projection.gameCalls[d];
                const RT64::DrawCall& desc = call.callDesc;
                if (inspector::tag_kind(desc.drawTag) == TagKind::Text) {
                    continue;
                }
                if (rects) {
                    if (desc.tileCount > 0 && desc.otherMode.cycleType() != G_CYC_FILL && desc.rect.contains(fixed_x, fixed_y)) {
                        seen = make_hit(workload, call, true, x, y);
                        all.push_back({-1.0f, seen});
                    }
                    continue;
                }
                const float depth = triangle_depth(workload, projection, call, x, y);
                if (std::isnan(depth)) {
                    continue;
                }
                all.push_back({depth, make_hit(workload, call, false, x, y)});
                if (desc.otherMode.zCmp() && depth > depth_buffer + 1e-5f) {
                    continue;
                }
                seen = all.back().hit;
                if (desc.otherMode.zUpd()) {
                    depth_buffer = depth;
                }
            }
        }
    }
    std::vector<Hit> hits;
    if (!seen.valid) {
        return hits;
    }
    hits.push_back(seen);
    std::stable_sort(all.begin(), all.end(), [](const Behind& a, const Behind& b) { return a.depth < b.depth; });
    for (const Behind& behind : all) {
        bool listed = false;
        for (const Hit& hit : hits) {
            listed |= inspector::same_thing(hit, behind.hit.tag, behind.hit.list, behind.hit.texture);
        }
        if (!listed) {
            hits.push_back(behind.hit);
        }
    }
    for (size_t i = 0; i < hits.size(); i++) {
        hits[i].layer = static_cast<int>(i);
        hits[i].layers = static_cast<int>(hits.size());
    }
    return hits;
}

void on_workload(RT64::Workload& workload) {
    g_frames++;
    if (!inspector::active()) {
        if (g_picked.valid) {
            g_picked = {};
            g_picked_serial++;
        }
        return;
    }
    Hit hovered;
    float x, y;
    RT64::PresentedPicture picture;
    int layer = 0;
    if (cursor_on_picture(&x, &y, &picture, &layer)) {
        const std::vector<Hit> hits = find_hits(workload, picture, x, y);
        if (!hits.empty()) {
            hovered = hits[std::min<size_t>(layer, hits.size() - 1)];
        }
    }
    if (hovered.valid && hovered.rect) {
        // A sprite tile func_1510D0EC loaded: its resource, and the sign it belongs to if it is one.
        const int resource = inspector::sprite_resource(hovered.texture_address);
        if (resource >= 0) {
            const std::string name = conker::sharp_menus::sprite_name(resource);
            const std::string sprite = name.empty() ? std::to_string(resource) : name + " (" + std::to_string(resource) + ")";
            std::strncpy(hovered.sprite, sprite.c_str(), sizeof(hovered.sprite) - 1);
        }
    }

    switch (inspector::take_click()) {
        case 1:
            g_picked = hovered;
            g_picked_serial++;
            break;
        case 3:
            g_picked = {};
            g_picked_serial++;
            break;
        default:
            break;
    }

    // Tint every draw call of both, counting them.
    const float pulse = 0.45f + 0.25f * std::sin(g_frames * 0.25f);
    const uint32_t picked_color = RT64::ColorConverter::RGBA32::toRGBA(hlslpp::float4(1.0f, 0.0f, 1.0f, pulse));
    const uint32_t hovered_color = RT64::ColorConverter::RGBA32::toRGBA(hlslpp::float4(0.0f, 1.0f, 1.0f, 0.35f));
    hovered.calls = 0;
    uint32_t picked_calls = 0;
    for (uint32_t f = 0; f < workload.fbPairCount; f++) {
        RT64::FramebufferPair& pair = workload.fbPairs[f];
        for (uint32_t p = 0; p < pair.projectionCount; p++) {
            RT64::Projection& projection = pair.projections[p];
            for (uint32_t d = 0; d < projection.gameCallCount; d++) {
                RT64::GameCall& call = projection.gameCalls[d];
                const uint32_t tag = call.callDesc.drawTag;
                const uint32_t list = call.callDesc.drawList;
                const uint64_t texture = texture_hash(workload, call.callDesc);
                const bool is_hovered = inspector::same_thing(hovered, tag, list, texture);
                if (inspector::same_thing(g_picked, tag, list, texture)) {
                    call.debuggerDesc.highlightColor = picked_color;
                    picked_calls++;
                } else if (is_hovered) {
                    call.debuggerDesc.highlightColor = hovered_color;
                }
                hovered.calls += is_hovered ? 1 : 0;
            }
        }
    }
    g_picked.calls = picked_calls;
    inspector::publish(hovered, g_picked, g_picked_serial);
}

}  // namespace

void install_object_picker() { RT64::SetWorkloadHook(&on_workload); }

void update_object_picker() { RT64::SetDrawTagging(inspector::active()); }

}  // namespace conker::renderer
