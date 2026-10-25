// What the B pads show where their B was: the PlayStation Square (src/enhancements/pad_icons/
// square_symbol.hpp), or for the keyboard an E, the key that is B. Each pad draws it as a stroke
// `width` wide inside an outline `outer` out from its middle: the square's outer half-width, and
// the E's half height, its stroke a little thinner than the square's so that its three bars and two
// gaps fit. Measured from the mark's middle in whatever units the pad gives `outer` and `width` in.
//
// For the square these are the very tests the pads made with reach, so its pads come out as before:
// `stroke` is between the reaches outer - width and outer, `enclosed` its counter, `outer_edge`
// whether a texel is nearer its outer edge than its inner one, and `out` the way that nearer edge
// faces out of the stroke. The E's outer edges are on its outline (and the end of its middle bar),
// its inner ones face the gaps between its bars, as a B's counters do.
#pragma once

#include <algorithm>
#include <cmath>

#include "enhancements/pad_icons/square_symbol.hpp"

namespace conker::pad_icons {

struct Mark {
    enum class Shape { Square, E };
    Shape shape = Shape::Square;
    float outer = 1, width = 0.5f;

    static constexpr float kEWide = 0.74f;    // the E's half width, of its half height
    static constexpr float kEStroke = 0.62f;  // its stroke, of the square's
    static constexpr float kEMiddle = 0.84f;  // its middle bar's length, of the others'

    // Signed distance from the stroke's edge: negative in it.
    float distance(float dx, float dy) const {
        if (shape == Shape::Square) return std::abs(square_symbol::reach(dx, dy) - (outer - width / 2)) - width / 2;
        const Nearest n = nearest(dx, dy);
        return n.inside ? -n.distance : n.distance;
    }

    bool stroke(float dx, float dy) const {
        if (shape == Shape::Square) {
            const float d = square_symbol::reach(dx, dy);
            return d >= outer - width && d <= outer;
        }
        return distance(dx, dy) <= 0;
    }

    // In what the stroke closes round: the square's counter (an E closes round nothing).
    bool enclosed(float dx, float dy) const {
        return shape == Shape::Square && square_symbol::reach(dx, dy) < outer - width;
    }

    // Inside the mark's outline: the square, or the box the E fills.
    bool within(float dx, float dy) const {
        if (shape == Shape::Square) return square_symbol::reach(dx, dy) < outer;
        return std::abs(dx) < outer * kEWide && std::abs(dy) < outer;
    }

    // Whether the stroke's nearer edge is on the mark's outside, not facing a counter or a gap.
    bool outer_edge(float dx, float dy) const {
        if (shape == Shape::Square) return square_symbol::reach(dx, dy) >= outer - width / 2;
        return nearest(dx, dy).outer;
    }

    // The way the stroke's nearer edge faces, out of the stroke: a unit vector.
    square_symbol::Facing out(float dx, float dy) const {
        if (shape == Shape::Square) {
            const square_symbol::Facing f = square_symbol::facing(dx, dy);
            const float sign = outer_edge(dx, dy) ? 1.0f : -1.0f;
            return {f.x * sign, f.y * sign};
        }
        const Nearest n = nearest(dx, dy);
        return {n.out_x, n.out_y};
    }

    // The way out from the mark's middle there, as the square's turn: the nearer edge's facing on
    // an outer edge, the other way on an inner one.
    float turn(float dx, float dy) const {
        if (shape == Shape::Square) return square_symbol::turn(dx, dy);
        const square_symbol::Facing f = out(dx, dy);
        const float sign = outer_edge(dx, dy) ? 1.0f : -1.0f;
        return std::atan2(f.y * sign, f.x * sign);
    }

    // How far across the stroke from its middle line, toward its outer edge: as reach - (outer -
    // width / 2) is for the square.
    float across(float dx, float dy) const {
        if (shape == Shape::Square) return square_symbol::reach(dx, dy) - (outer - width / 2);
        const float half = stroke_width() / 2, depth = -distance(dx, dy);
        return (outer_edge(dx, dy) ? 1.0f : -1.0f) * (half - depth);
    }

    // The stroke's width: the E's is thinner than the square's.
    float stroke_width() const { return shape == Shape::Square ? width : width * kEStroke; }

    // The E's middle lines, each from (x0, y0) to (x1, y1): its stem and its top, middle and bottom
    // bars, as a pen would draw it.
    struct Line {
        float x0, y0, x1, y1;
    };
    static constexpr int kELines = 4;
    void centre_lines(Line lines[kELines]) const {
        const float a = outer * kEWide, s = stroke_width(), stem = -a + s / 2;
        const float middle_end = -a + (2 * a) * kEMiddle - s / 2;
        lines[0] = {stem, -outer + s / 2, stem, outer - s / 2};
        lines[1] = {stem, -outer + s / 2, a - s / 2, -outer + s / 2};
        lines[2] = {stem, 0, middle_end, 0};
        lines[3] = {stem, outer - s / 2, a - s / 2, outer - s / 2};
    }

private:
    struct Nearest {
        float distance;      // to the E's outline
        bool inside;         // in its stroke
        bool outer;          // the nearest edge is an outer one
        float out_x, out_y;  // that edge faces this way, out of the stroke
    };

    // The E as a polygon, y down: round from its top left corner, each edge marked outer or inner.
    Nearest nearest(float px, float py) const {
        const float a = outer * kEWide, s = stroke_width(), t = outer;
        const float m = -a + (2 * a) * kEMiddle;  // the middle bar's end
        const float x[12] = {-a, a, a, -a + s, -a + s, m, m, -a + s, -a + s, a, a, -a};
        const float y[12] = {-t, -t, -t + s, -t + s, -s / 2, -s / 2, s / 2, s / 2, t - s, t - s, t, t};
        constexpr bool kOuter[12] = {true, true, false, false, false, true, false, false, false, true, true, true};
        Nearest best{1e30f, false, true, 0, 0};
        bool inside = false;
        for (int i = 0, j = 11; i < 12; j = i++) {
            // Crossings of a ray to the right, for inside; edge j runs from vertex j to vertex j + 1.
            if ((y[i] > py) != (y[j] > py) && px < (x[j] - x[i]) * (py - y[i]) / (y[j] - y[i]) + x[i]) inside = !inside;
        }
        for (int e = 0; e < 12; e++) {
            const int f = (e + 1) % 12;
            const float ex = x[f] - x[e], ey = y[f] - y[e], length2 = ex * ex + ey * ey;
            const float k = std::clamp(((px - x[e]) * ex + (py - y[e]) * ey) / length2, 0.0f, 1.0f);
            const float qx = x[e] + ex * k, qy = y[e] + ey * k;
            const float d = std::hypot(px - qx, py - qy);
            if (d < best.distance) {
                best.distance = d, best.outer = kOuter[e];
                // The polygon runs clockwise on screen (y down), so out of it is to the left of each edge.
                const float length = std::sqrt(length2);
                best.out_x = ey / length, best.out_y = -ex / length;
            }
        }
        best.inside = inside;
        return best;
    }
};

} // namespace conker::pad_icons
