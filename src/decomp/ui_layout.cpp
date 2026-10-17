// The game's interface on screens of another shape ("Fill the screen"): the HUD's panels move to the screen's
// edges instead of staying in the PSP-sized middle.
//
// The interface is built from layout elements, each created with a fixed element id (f_088DD604(layout, id, ...));
// every frame the UI draw passes (f_088EA200, f_088EA438) walk lists of draw records - one per element, with its
// vertices - and draw them. Records whose element belongs to a panel at an edge are registered with the renderer
// (rt::anchor_2d), which moves their draws and scissor by the margins. Elements placed over the 3D scene (damage
// numbers, the lock-on cursor) are not listed and stay where the game puts them.

#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "psprecore/game/game.hpp"
#include "psprecore/runtime/system.hpp"

using namespace psprecore::game;

namespace {

// UI draw record (the lists at ui + 0x2CD0 and ui + 0x2CF8).
constexpr u32 kVertices = 8, kQuads = 12, kId = 24, kNext = 44;
constexpr u32 kVertexSize = 16;  // through-mode, u16 uv, 8888 colour, s16 position (VERTEXTYPE 0x80011E)

struct Anchor {
    u16 first, last;  // element ids
    s8 horizontal, vertical;
};

// Element ids by panel, from the recordings (FF7CC_UI_IDS lists them with where they are drawn). The left column
// (DMW, SP, the lines down to HP/MP/AP) moves as one, sideways only: its parts are joined.
constexpr Anchor kAnchors[] = {
    {0x07F9, 0x07FC, 1, -1},  // target name, top right
    {0x08CC, 0x09B0, 1, 1},   // command menu (with the entries parked off-screen), bottom right
    {0x0BAE, 0x0BB3, 1, 1},   // panel above the command menu, bottom right (also in the field)
    {0x1374, 0x1375, 1, -1},  // mail icon, top right
    {0x1388, 0x14C9, -1, 0},  // left column: frame, lines, HP/MP/AP, DMW reels and portraits
    {0x14E6, 0x14FC, -1, 0},
    {0x1662, 0x1678, -1, 0},  // SP, LIMIT, the column's backdrop
};

const Anchor* anchor_of(u32 id) {
    for (const Anchor& a : kAnchors)
        if (id >= a.first && id <= a.last && (a.horizontal || a.vertical)) return &a;
    return nullptr;
}

// FF7CC_UI_IDS=<file>: every element id drawn, once, with the box it is drawn in (for kAnchors).
void log_ids(u32 list) {
    static std::FILE* f = [] {
        const char* path = std::getenv("FF7CC_UI_IDS");
        return path ? std::fopen(path, "a") : nullptr;
    }();
    static bool seen[0x10000];
    if (!f) return;
    for (u32 r = load<u32>(list); r; r = load<u32>(r + kNext)) {
        const u32 id = load<u32>(r + kId) & 0xFFFF, v = load<u32>(r + kVertices), quads = load<u32>(r + kQuads);
        if (seen[id] || !v || !quads || quads > 4096) continue;
        seen[id] = true;
        int x0 = 99999, x1 = -99999, y0 = 99999, y1 = -99999;
        for (u32 i = 0; i < quads * 6; ++i) {
            const int x = load<s16>(v + i * kVertexSize + 8), y = load<s16>(v + i * kVertexSize + 10);
            x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
        }
        std::fprintf(f, "%04X\t%d\t%d\t%d\t%d\n", id, x0, x1, y0, y1);
    }
    std::fflush(f);
}

// On-screen box of each anchored group this frame: other 2D inside it (the panel's text, the DMW reels and digits,
// drawn by other code) moves with it.
struct Box {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
};

void register_anchors(u32 list, Box* boxes) {
    for (u32 r = load<u32>(list); r; r = load<u32>(r + kNext)) {
        const u32 v = load<u32>(r + kVertices), quads = load<u32>(r + kQuads);
        if (!v || !quads || quads > 4096) continue;
        const Anchor* a = anchor_of(load<u32>(r + kId) & 0xFFFF);
        // Elements not listed stay where the game puts them, also over a panel (damage numbers).
        psprecore::rt::System::current().layout_2d.anchor(v, v + quads * 6 * kVertexSize, a ? a->horizontal : 0, a ? a->vertical : 0);
        if (!a || load<u32>(r + 28)) continue;  // (+28: hidden)
        Box& b = boxes[a - kAnchors];
        for (u32 i = 0; i < quads * 6; ++i) {
            const float x = load<s16>(v + i * kVertexSize + 8), y = load<s16>(v + i * kVertexSize + 10);
            if (x < 0 || x > 480 || y < 0 || y > 272) continue;  // parked off-screen
            b.x0 = x < b.x0 ? x : b.x0, b.x1 = x > b.x1 ? x : b.x1;
            b.y0 = y < b.y0 ? y : b.y0, b.y1 = y > b.y1 ? y : b.y1;
        }
    }
}

}  // namespace

// The first UI draw pass, f_088EA200(ui); the second (f_088EA438) follows in the same frame.
GAME_FUNC(088EA200, ui_draw) {
    const u32 ui = arg(ctx, 0);
    psprecore::rt::System::current().layout_2d.clear();
    Box boxes[std::size(kAnchors)];
    for (const u32 list : {ui + 0x2CD0, ui + 0x2CF8}) {
        log_ids(list);
        register_anchors(list, boxes);
    }
    for (std::size_t i = 0; i < std::size(kAnchors); ++i) {
        Box b = boxes[i];
        if (b.x0 >= b.x1 || b.y0 >= b.y1) continue;
        constexpr float kEdge = 4, kBeyond = 1000;  // a panel at an edge takes in what reaches past it
        b.x0 = b.x0 <= kEdge ? -kBeyond : b.x0, b.x1 = b.x1 >= 480 - kEdge ? 480 + kBeyond : b.x1;
        b.y0 = b.y0 <= kEdge ? -kBeyond : b.y0, b.y1 = b.y1 >= 272 - kEdge ? 272 + kBeyond : b.y1;
        psprecore::rt::System::current().layout_2d.anchor_region(b.x0, b.y0, b.x1, b.y1, kAnchors[i].horizontal, kAnchors[i].vertical);
    }
    GAME_ORIGINAL(088EA200);
}
