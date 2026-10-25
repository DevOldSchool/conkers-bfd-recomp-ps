// Refilling where a B was on a pad whose wood grows in rings round its middle (the octagonal and
// hexagonal wooden pads): each texel is copied from the same ring further along the edge it is on,
// shifted by as much of the ring as the hole covers there, so a run of the hole becomes the run of
// grain just past it and the rings carry straight on through.
#pragma once

#include <cmath>
#include <cstdlib>
#include <vector>

namespace conker::pad_icons::ring_fill {

struct Step {
    float x, y;  // a unit vector along the ring's edge
};

// `hole(x, y)`: whether the texel is to be filled; `along(x, y)`: the way the ring's edge runs there;
// `usable(sx, sy, x, y)`: whether texel (sx, sy) may fill (x, y) (outside the hole, on its ring);
// `copy(sx, sy, x, y)` fills it. Returns how many texels it could not fill.
template <class Hole, class Along, class Usable, class Copy>
int fill(int width, int height, Hole hole, Along along, Usable usable, Copy copy) {
    auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height; };
    int left = 0;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            if (!hole(x, y)) continue;
            const Step d = along(x, y);
            auto point = [&](float k, int& sx, int& sy) {
                sx = int(std::lround(x + d.x * k)), sy = int(std::lround(y + d.y * k));
                return inside(sx, sy);
            };
            // How far the hole runs on each way along the ring.
            int ahead = 1, behind = 1, sx, sy;
            while (point(float(ahead), sx, sy) && hole(sx, sy)) ahead++;
            while (point(-float(behind), sx, sy) && hole(sx, sy)) behind++;
            const int run = ahead + behind - 1;
            bool done = false;
            for (int extra = 0; extra <= 16 && !done; extra++)
                for (const float k : {float(run + extra), -float(run + extra)})
                    if (!done && point(k, sx, sy) && usable(sx, sy, x, y)) copy(sx, sy, x, y), done = true;
            left += !done;
        }
    return left;
}

// What `fill` leaves (the pad's middle, where every ring is under the B), each four-connected piece
// copied whole from where it fits clear of the hole and is smoothest (`rough(sx, sy)`: how much a
// texel differs from its neighbours), so it is one piece of real grain rather than a patchwork of
// specks. Returns how many texels it could not fill.
template <class Hole, class Usable, class Rough, class Copy>
int fill_pieces(int width, int height, Hole hole, Usable usable, Rough rough, Copy copy) {
    std::vector<int> piece_of(size_t(width) * height, -1);
    std::vector<std::vector<int>> pieces;
    for (int start = 0; start < width * height; start++) {
        if (piece_of[start] >= 0 || !hole(start % width, start / width)) continue;
        std::vector<int> piece{start};
        piece_of[start] = int(pieces.size());
        for (size_t k = 0; k < piece.size(); k++) {
            const int x = piece[k] % width, y = piece[k] / width;
            const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& p : n)
                if (p[0] >= 0 && p[1] >= 0 && p[0] < width && p[1] < height && piece_of[p[1] * width + p[0]] < 0 &&
                    hole(p[0], p[1])) {
                    piece_of[p[1] * width + p[0]] = int(pieces.size());
                    piece.push_back(p[1] * width + p[0]);
                }
        }
        pieces.push_back(std::move(piece));
    }
    int left = 0;
    for (const std::vector<int>& piece : pieces) {
        int best_x = 0, best_y = 0;
        float best = -1;
        for (int oy = -height + 1; oy < height; oy++)
            for (int ox = -width + 1; ox < width; ox++) {
                float cost = 0;
                bool fits = true;
                for (size_t k = 0; k < piece.size() && fits; k++) {
                    const int sx = piece[k] % width + ox, sy = piece[k] / width + oy;
                    fits = sx >= 0 && sy >= 0 && sx < width && sy < height && usable(sx, sy);
                    if (fits) cost += rough(sx, sy);
                }
                if (fits && (best < 0 || cost < best)) best = cost, best_x = ox, best_y = oy;
            }
        if (best < 0) {
            left += int(piece.size());
            continue;
        }
        for (int i : piece) copy(i % width + best_x, i / width + best_y, i % width, i / width);
    }
    return left;
}

} // namespace conker::pad_icons::ring_fill
