// Rare's button pictures are pre-rendered 3D buttons, seen from the front and above with a key
// light on the right, A, B and START each in a dark and a lit frame. These are drawn the same way:
// each button is a heightfield (a rounded cap with its symbol printed on or carved into its top)
// ray-marched from the same tilt and lit from the same side, then reduced to 4 pixels for each
// texel of the game's picture. A lit frame keeps the button sharp and lights it from inside: a dark
// cap's symbol glows and throws its colour over the cap and past its edge; a coloured cap glows a
// flat, saturated colour, as Rare's do.
//
// Symbols and letters are signed distances from strokes (a small rounded stroke font for the few
// letters the labels need), so they are drawn at any size without a font file.
#include "enhancements/pad_icons/button_art.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

namespace conker::pad_icons {

namespace {

constexpr int G = 384;   // heightfield samples across the world square [-1, 1]^2
constexpr int R = 256;   // render size, reduced to the picture's size
constexpr int kScale = 4;
constexpr float kPi = 3.14159265f;
constexpr float kTilt = 52 * kPi / 180;  // the camera looks down this far from level, like Rare's

using Rgb = std::array<float, 3>;
using Plane = std::vector<float>;

Rgb colour(uint32_t hex) {
    return {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f};
}

// --- symbols ---------------------------------------------------------------------------------------

struct Vec {
    float x, y;
};
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
float length(Vec a) { return std::sqrt(dot(a, a)); }

// A symbol: strokes of a given half-width and filled triangles, as a signed distance (world units,
// x right, y towards the front of the cap) that is negative inside.
struct Symbol {
    struct Line { Vec a, b; float half; };
    struct Arc { Vec centre; float radius, start, sweep, half; };  // angles in radians, y down
    struct Triangle { Vec p[3]; };
    std::vector<Line> lines;
    std::vector<Arc> arcs;
    std::vector<Triangle> triangles;

    float distance(Vec p) const {
        float d = 1e9f;
        for (const Line& l : lines) {
            const Vec ab = l.b - l.a;
            const float t = std::clamp(dot(p - l.a, ab) / std::max(dot(ab, ab), 1e-9f), 0.0f, 1.0f);
            d = std::min(d, length(p - (l.a + ab * t)) - l.half);
        }
        for (const Arc& a : arcs) {
            const Vec q = p - a.centre;
            float turn = std::atan2(q.y, q.x) - a.start;
            const float full = 2 * kPi;
            if (a.sweep >= 0) turn = std::fmod(std::fmod(turn, full) + full, full);
            else turn = -std::fmod(std::fmod(-turn, full) + full, full);
            float edge;
            if (std::abs(turn) <= std::abs(a.sweep)) {
                edge = std::abs(length(q) - a.radius);
            } else {
                const Vec e0 = a.centre + Vec{std::cos(a.start), std::sin(a.start)} * a.radius;
                const Vec e1 = a.centre + Vec{std::cos(a.start + a.sweep), std::sin(a.start + a.sweep)} * a.radius;
                edge = std::min(length(p - e0), length(p - e1));
            }
            d = std::min(d, edge - a.half);
        }
        for (const Triangle& t : triangles) {
            float inside = -1e9f;
            const Vec centre = (t.p[0] + t.p[1] + t.p[2]) * (1.0f / 3);
            for (int i = 0; i < 3; i++) {
                const Vec a = t.p[i], b = t.p[(i + 1) % 3];
                const Vec edge = b - a;
                Vec normal = Vec{edge.y, -edge.x} * (1 / length(edge));
                if (dot(centre - a, normal) > 0) normal = normal * -1.0f;  // pointing out of the triangle
                inside = std::max(inside, dot(p - a, normal));
            }
            d = std::min(d, inside);
        }
        return d;
    }

    // Coverage on the heightfield grid, anti-aliased over a sample.
    Plane mask() const {
        Plane out(G * G);
        const float pixel = 2.0f / G;
        for (int i = 0; i < G; i++) {
            for (int j = 0; j < G; j++) {
                const float d = distance({(j + 0.5f) / G * 2 - 1, (i + 0.5f) / G * 2 - 1});
                out[i * G + j] = std::clamp(0.5f - d / pixel, 0.0f, 1.0f);
            }
        }
        return out;
    }
};

// The stroke font: each letter in a box one unit tall (y from -0.5 at the top to 0.5), its width,
// and its strokes; arcs run clockwise on screen for a positive sweep.
struct Glyph {
    float width;
    std::function<void(Symbol&, Vec origin, float scale, float half)> strokes;
};

void line(Symbol& s, Vec o, float k, float half, Vec a, Vec b) { s.lines.push_back({o + a * k, o + b * k, half}); }
void arc(Symbol& s, Vec o, float k, float half, Vec c, float r, float start_deg, float sweep_deg) {
    s.arcs.push_back({o + c * k, r * k, start_deg * kPi / 180, sweep_deg * kPi / 180, half});
}

const Glyph* glyph(char c) {
    static const Glyph a{0.74f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.33f, 0.5f}, {0, -0.5f});
        line(s, o, k, h, {0, -0.5f}, {0.33f, 0.5f});
        line(s, o, k, h, {-0.19f, 0.15f}, {0.19f, 0.15f});
    }};
    static const Glyph b{0.64f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.27f, -0.5f}, {-0.27f, 0.5f});
        line(s, o, k, h, {-0.27f, -0.5f}, {0.02f, -0.5f});
        arc(s, o, k, h, {0.02f, -0.255f}, 0.245f, -90, 180);
        line(s, o, k, h, {0.02f, -0.01f}, {-0.27f, -0.01f});
        line(s, o, k, h, {0.05f, -0.01f}, {-0.27f, -0.01f});
        arc(s, o, k, h, {0.05f, 0.245f}, 0.255f, -90, 180);
        line(s, o, k, h, {0.05f, 0.5f}, {-0.27f, 0.5f});
    }};
    static const Glyph l{0.52f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.22f, -0.5f}, {-0.22f, 0.5f});
        line(s, o, k, h, {-0.22f, 0.5f}, {0.26f, 0.5f});
    }};
    static const Glyph r{0.64f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.27f, -0.5f}, {-0.27f, 0.5f});
        line(s, o, k, h, {-0.27f, -0.5f}, {0.02f, -0.5f});
        arc(s, o, k, h, {0.02f, -0.245f}, 0.255f, -90, 180);
        line(s, o, k, h, {0.02f, 0.01f}, {-0.27f, 0.01f});
        line(s, o, k, h, {-0.02f, 0.01f}, {0.3f, 0.5f});
    }};
    static const Glyph s_{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.245f}, 0.255f, -25, -245);
        arc(s, o, k, h, {0, 0.245f}, 0.255f, -90, 245);
    }};
    static const Glyph t{0.62f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.29f, -0.5f}, {0.29f, -0.5f});
        line(s, o, k, h, {0, -0.5f}, {0, 0.5f});
    }};
    static const Glyph z{0.6f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.26f, -0.5f}, {0.26f, -0.5f});
        line(s, o, k, h, {0.26f, -0.5f}, {-0.26f, 0.5f});
        line(s, o, k, h, {-0.26f, 0.5f}, {0.28f, 0.5f});
    }};
    static const Glyph c_{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0.02f, -0.19f}, 0.31f, 180, 140);
        line(s, o, k, h, {-0.29f, -0.19f}, {-0.29f, 0.19f});
        arc(s, o, k, h, {0.02f, 0.19f}, 0.31f, 180, -140);
    }};
    static const Glyph e{0.56f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.24f, -0.5f}, {-0.24f, 0.5f});
        line(s, o, k, h, {-0.24f, -0.5f}, {0.26f, -0.5f});
        line(s, o, k, h, {-0.24f, 0}, {0.2f, 0});
        line(s, o, k, h, {-0.24f, 0.5f}, {0.26f, 0.5f});
    }};
    static const Glyph f{0.54f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.23f, -0.5f}, {-0.23f, 0.5f});
        line(s, o, k, h, {-0.23f, -0.5f}, {0.27f, -0.5f});
        line(s, o, k, h, {-0.23f, 0}, {0.2f, 0});
    }};
    static const Glyph i{0.4f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0, -0.5f}, {0, 0.5f});
        line(s, o, k, h, {-0.17f, -0.5f}, {0.17f, -0.5f});
        line(s, o, k, h, {-0.17f, 0.5f}, {0.17f, 0.5f});
    }};
    static const Glyph j{0.52f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0.22f, -0.5f}, {0.22f, 0.26f});
        arc(s, o, k, h, {-0.01f, 0.26f}, 0.23f, 0, 180);
    }};
    static const Glyph k_{0.62f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.26f, -0.5f}, {-0.26f, 0.5f});
        line(s, o, k, h, {0.27f, -0.5f}, {-0.26f, 0.1f});
        line(s, o, k, h, {-0.06f, -0.12f}, {0.3f, 0.5f});
    }};
    static const Glyph n{0.66f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.26f, 0.5f}, {-0.26f, -0.5f});
        line(s, o, k, h, {-0.26f, -0.5f}, {0.26f, 0.5f});
        line(s, o, k, h, {0.26f, 0.5f}, {0.26f, -0.5f});
    }};
    static const Glyph p_{0.62f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.27f, -0.5f}, {-0.27f, 0.5f});
        line(s, o, k, h, {-0.27f, -0.5f}, {0.02f, -0.5f});
        arc(s, o, k, h, {0.02f, -0.22f}, 0.28f, -90, 180);
        line(s, o, k, h, {0.02f, 0.06f}, {-0.27f, 0.06f});
    }};
    static const Glyph q{0.74f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.13f}, 0.37f, 180, 180);
        line(s, o, k, h, {0.37f, -0.13f}, {0.37f, 0.13f});
        arc(s, o, k, h, {0, 0.13f}, 0.37f, 0, 180);
        line(s, o, k, h, {-0.37f, 0.13f}, {-0.37f, -0.13f});
        line(s, o, k, h, {0.1f, 0.22f}, {0.38f, 0.56f});
    }};
    static const Glyph one{0.46f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0.04f, -0.5f}, {0.04f, 0.5f});
        line(s, o, k, h, {-0.18f, -0.3f}, {0.04f, -0.5f});
    }};
    static const Glyph two{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.22f}, 0.27f, 200, 190);
        line(s, o, k, h, {0.234f, -0.085f}, {-0.27f, 0.5f});
        line(s, o, k, h, {-0.27f, 0.5f}, {0.28f, 0.5f});
    }};
    static const Glyph x{0.66f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.28f, -0.5f}, {0.28f, 0.5f});
        line(s, o, k, h, {0.28f, -0.5f}, {-0.28f, 0.5f});
    }};
    static const Glyph y{0.66f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.29f, -0.5f}, {0, 0.02f});
        line(s, o, k, h, {0.29f, -0.5f}, {0, 0.02f});
        line(s, o, k, h, {0, 0.02f}, {0, 0.5f});
    }};
    static const Glyph v{0.68f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.3f, -0.5f}, {0, 0.5f});
        line(s, o, k, h, {0, 0.5f}, {0.3f, -0.5f});
    }};
    static const Glyph w{0.9f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.42f, -0.5f}, {-0.21f, 0.5f});
        line(s, o, k, h, {-0.21f, 0.5f}, {0, -0.2f});
        line(s, o, k, h, {0, -0.2f}, {0.21f, 0.5f});
        line(s, o, k, h, {0.21f, 0.5f}, {0.42f, -0.5f});
    }};
    static const Glyph o_{0.74f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.13f}, 0.37f, 180, 180);
        line(s, o, k, h, {0.37f, -0.13f}, {0.37f, 0.13f});
        arc(s, o, k, h, {0, 0.13f}, 0.37f, 0, 180);
        line(s, o, k, h, {-0.37f, 0.13f}, {-0.37f, -0.13f});
    }};
    static const Glyph h_{0.64f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.26f, -0.5f}, {-0.26f, 0.5f});
        line(s, o, k, h, {0.26f, -0.5f}, {0.26f, 0.5f});
        line(s, o, k, h, {-0.26f, 0}, {0.26f, 0});
    }};
    static const Glyph u{0.64f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.26f, -0.5f}, {-0.26f, 0.24f});
        arc(s, o, k, h, {0, 0.24f}, 0.26f, 180, -180);
        line(s, o, k, h, {0.26f, 0.24f}, {0.26f, -0.5f});
    }};
    static const Glyph m{0.8f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.33f, 0.5f}, {-0.33f, -0.5f});
        line(s, o, k, h, {-0.33f, -0.5f}, {0, 0.18f});
        line(s, o, k, h, {0, 0.18f}, {0.33f, -0.5f});
        line(s, o, k, h, {0.33f, -0.5f}, {0.33f, 0.5f});
    }};
    static const Glyph three{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.255f}, 0.245f, 200, 250);
        arc(s, o, k, h, {0, 0.245f}, 0.255f, -90, 250);
    }};
    static const Glyph four{0.64f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0.14f, 0.5f}, {0.14f, -0.5f});
        line(s, o, k, h, {0.14f, -0.5f}, {-0.28f, 0.2f});
        line(s, o, k, h, {-0.28f, 0.2f}, {0.3f, 0.2f});
    }};
    static const Glyph d{0.66f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.27f, -0.5f}, {-0.27f, 0.5f});
        line(s, o, k, h, {-0.27f, -0.5f}, {-0.05f, -0.5f});
        arc(s, o, k, h, {-0.05f, -0.18f}, 0.32f, -90, 90);
        line(s, o, k, h, {0.27f, -0.18f}, {0.27f, 0.18f});
        arc(s, o, k, h, {-0.05f, 0.18f}, 0.32f, 0, 90);
        line(s, o, k, h, {-0.05f, 0.5f}, {-0.27f, 0.5f});
    }};
    static const Glyph g{0.7f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0.02f, -0.13f}, 0.33f, -30, -150);
        line(s, o, k, h, {-0.31f, -0.13f}, {-0.31f, 0.13f});
        arc(s, o, k, h, {0.02f, 0.13f}, 0.33f, 180, -180);
        line(s, o, k, h, {0.35f, 0.13f}, {0.35f, 0.04f});
        line(s, o, k, h, {0.35f, 0.04f}, {0.05f, 0.04f});
    }};
    static const Glyph zero{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.2f}, 0.27f, 180, 180);
        line(s, o, k, h, {0.27f, -0.2f}, {0.27f, 0.2f});
        arc(s, o, k, h, {0, 0.2f}, 0.27f, 0, 180);
        line(s, o, k, h, {-0.27f, 0.2f}, {-0.27f, -0.2f});
    }};
    static const Glyph five{0.62f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0.26f, -0.5f}, {-0.22f, -0.5f});
        line(s, o, k, h, {-0.22f, -0.5f}, {-0.25f, -0.05f});
        arc(s, o, k, h, {0, 0.2f}, 0.29f, -120, 260);
    }};
    static const Glyph six{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, 0.2f}, 0.28f, 0, 360);
        line(s, o, k, h, {-0.28f, 0.2f}, {0.12f, -0.5f});
    }};
    static const Glyph seven{0.62f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.27f, -0.5f}, {0.27f, -0.5f});
        line(s, o, k, h, {0.27f, -0.5f}, {-0.08f, 0.5f});
    }};
    static const Glyph eight{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.27f}, 0.23f, 0, 360);
        arc(s, o, k, h, {0, 0.23f}, 0.27f, 0, 360);
    }};
    static const Glyph nine{0.62f, [](Symbol& s, Vec o, float k, float h) {
        arc(s, o, k, h, {0, -0.2f}, 0.28f, 0, 360);
        line(s, o, k, h, {0.28f, -0.2f}, {-0.12f, 0.5f});
    }};
    static const Glyph minus{0.5f, [](Symbol& s, Vec o, float k, float h) { line(s, o, k, h, {-0.18f, 0}, {0.18f, 0}); }};
    static const Glyph plus{0.6f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.24f, 0}, {0.24f, 0});
        line(s, o, k, h, {0, -0.24f}, {0, 0.24f});
    }};
    static const Glyph equals{0.6f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.24f, -0.13f}, {0.24f, -0.13f});
        line(s, o, k, h, {-0.24f, 0.13f}, {0.24f, 0.13f});
    }};
    static const Glyph star{0.6f, [](Symbol& s, Vec o, float k, float h) {
        for (float a : {90.0f, 30.0f, 150.0f}) {
            const Vec d{std::cos(a * kPi / 180) * 0.26f, std::sin(a * kPi / 180) * 0.26f};
            line(s, o, k, h, Vec{0, -0.12f} - d, Vec{0, -0.12f} + d);
        }
    }};
    static const Glyph open{0.42f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0.12f, -0.56f}, {-0.1f, -0.56f});
        line(s, o, k, h, {-0.1f, -0.56f}, {-0.1f, 0.56f});
        line(s, o, k, h, {-0.1f, 0.56f}, {0.12f, 0.56f});
    }};
    static const Glyph close{0.42f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.12f, -0.56f}, {0.1f, -0.56f});
        line(s, o, k, h, {0.1f, -0.56f}, {0.1f, 0.56f});
        line(s, o, k, h, {0.1f, 0.56f}, {-0.12f, 0.56f});
    }};
    static const Glyph slash{0.5f, [](Symbol& s, Vec o, float k, float h) { line(s, o, k, h, {0.18f, -0.56f}, {-0.18f, 0.56f}); }};
    static const Glyph backslash{0.5f, [](Symbol& s, Vec o, float k, float h) { line(s, o, k, h, {-0.18f, -0.56f}, {0.18f, 0.56f}); }};
    static const Glyph dot{0.3f, [](Symbol& s, Vec o, float k, float h) { line(s, o, k, h, {0, 0.48f}, {0, 0.5f}); }};
    static const Glyph comma{0.3f, [](Symbol& s, Vec o, float k, float h) { line(s, o, k, h, {0.03f, 0.42f}, {-0.05f, 0.64f}); }};
    static const Glyph semicolon{0.3f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0.02f, -0.1f}, {0.02f, -0.08f});
        line(s, o, k, h, {0.03f, 0.42f}, {-0.05f, 0.64f});
    }};
    static const Glyph colon{0.3f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {0, -0.1f}, {0, -0.08f});
        line(s, o, k, h, {0, 0.48f}, {0, 0.5f});
    }};
    static const Glyph quote{0.3f, [](Symbol& s, Vec o, float k, float h) { line(s, o, k, h, {0, -0.5f}, {0, -0.22f}); }};
    static const Glyph grave{0.36f, [](Symbol& s, Vec o, float k, float h) { line(s, o, k, h, {-0.08f, -0.5f}, {0.08f, -0.3f}); }};
    static const Glyph hash{0.66f, [](Symbol& s, Vec o, float k, float h) {
        line(s, o, k, h, {-0.08f, -0.5f}, {-0.16f, 0.5f});
        line(s, o, k, h, {0.16f, -0.5f}, {0.08f, 0.5f});
        line(s, o, k, h, {-0.28f, -0.17f}, {0.3f, -0.17f});
        line(s, o, k, h, {-0.3f, 0.17f}, {0.28f, 0.17f});
    }};
    switch (c) {
        case '#': return &hash;
        case 'D': return &d;
        case 'G': return &g;
        case '0': return &zero;
        case '5': return &five;
        case '6': return &six;
        case '7': return &seven;
        case '8': return &eight;
        case '9': return &nine;
        case '-': return &minus;
        case '+': return &plus;
        case '=': return &equals;
        case '*': return &star;
        case '[': return &open;
        case ']': return &close;
        case '(': return &open;
        case ')': return &close;
        case '/': return &slash;
        case '\\': return &backslash;
        case '.': return &dot;
        case ',': return &comma;
        case ';': return &semicolon;
        case ':': return &colon;
        case '\'': return &quote;
        case '`': return &grave;
        case 'A': return &a;
        case 'H': return &h_;
        case 'M': return &m;
        case 'O': return &o_;
        case 'U': return &u;
        case 'V': return &v;
        case 'W': return &w;
        case 'X': return &x;
        case 'Y': return &y;
        case '3': return &three;
        case '4': return &four;
        case 'B': return &b;
        case 'C': return &c_;
        case 'E': return &e;
        case 'F': return &f;
        case 'I': return &i;
        case 'J': return &j;
        case 'K': return &k_;
        case 'N': return &n;
        case 'P': return &p_;
        case 'Q': return &q;
        case 'L': return &l;
        case 'R': return &r;
        case 'S': return &s_;
        case 'T': return &t;
        case 'Z': return &z;
        case '1': return &one;
        case '2': return &two;
        default: return nullptr;
    }
}

// `text` centred on the cap, its capitals `height` tall (world units).
Symbol letters(const std::string& text, float height) {
    constexpr float kGap = 0.14f, kWeight = 0.085f;  // between letters; half the stroke, of the height
    constexpr float kUnknown = 0.4f;  // a character the font has not: a space
    float width = -kGap;
    for (char c : text) width += (glyph(c) != nullptr ? glyph(c)->width : kUnknown) + kGap;
    Symbol s;
    float x = -width / 2;
    for (char c : text) {
        const Glyph* g = glyph(c);
        if (g == nullptr) {
            x += kUnknown + kGap;
            continue;
        }
        const float inset = kWeight;  // strokes are centred on the outline, so pull them inside it
        g->strokes(s, {(x + g->width / 2) * height, 0}, height * (1 - 2 * inset), kWeight * height);
        x += g->width + kGap;
    }
    return s;
}

enum class Kind { Cross, Square, Circle, Triangle, Menu, Create, View, Left, Right, Up, Down };

// A symbol `scale` across from its middle, strokes `width` wide; with a ring of radius `ring`.
Symbol shape(Kind kind, float scale, float width, float ring = 0, float ring_width = 0.05f, float forward = 0) {
    Symbol s;
    const Vec c{0, forward};
    const float k = scale;
    switch (kind) {
        case Kind::Cross:
            for (float angle : {45.0f, 135.0f}) {
                const Vec d{std::cos(angle * kPi / 180) * k, std::sin(angle * kPi / 180) * k};
                s.lines.push_back({c - d, c + d, width / 2});
            }
            break;
        case Kind::Square: {  // its strokes' round ends round its corners a little, as the pad's are
            const float h = k - width / 2;
            const Vec corners[4] = {{c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}};
            for (int i = 0; i < 4; i++) s.lines.push_back({corners[i], corners[(i + 1) % 4], width / 2});
            break;
        }
        case Kind::Circle:
            s.arcs.push_back({c, k - width / 2, 0, 2 * kPi, width / 2});
            break;
        case Kind::Triangle: {  // pointing up, its middle where a circle round it would have its
            const Vec top{c.x, c.y - k}, left{c.x - k * 0.95f, c.y + k * 0.62f}, right{c.x + k * 0.95f, c.y + k * 0.62f};
            s.lines.push_back({top, left, width / 2});
            s.lines.push_back({left, right, width / 2});
            s.lines.push_back({right, top, width / 2});
            break;
        }
        case Kind::Create:  // the DualSense's Create (Share): three short strokes fanning upwards
            for (float angle : {-30.0f, 0.0f, 30.0f}) {  // apart, as rays from a point below them
                const Vec d{std::sin(angle * kPi / 180), -std::cos(angle * kPi / 180)};
                const Vec from{c.x, c.y + k * 1.25f};
                s.lines.push_back({from + d * (k * 1.0f), from + d * (k * 1.9f), width / 2});
            }
            break;
        case Kind::View: {  // Xbox's View: two windows, one over the other's corner
            const float w = k * 0.62f, h = k * 0.48f;
            for (const Vec o : {Vec{c.x - k * 0.26f, c.y - k * 0.22f}, Vec{c.x + k * 0.26f, c.y + k * 0.22f}}) {
                const Vec corners[4] = {{o.x - w, o.y - h}, {o.x + w, o.y - h}, {o.x + w, o.y + h}, {o.x - w, o.y + h}};
                for (int i = 0; i < 4; i++) s.lines.push_back({corners[i], corners[(i + 1) % 4], width / 2});
            }
            break;
        }
        case Kind::Menu:
            for (int i = -1; i <= 1; i++) {
                const float y = c.y + i * k * 0.62f;
                s.lines.push_back({{c.x - k + width / 2, y}, {c.x + k - width / 2, y}, width / 2});
            }
            break;
        case Kind::Left:
        case Kind::Right: {
            const float sign = kind == Kind::Left ? -1.0f : 1.0f;
            s.triangles.push_back({{{c.x + sign * k, c.y}, {c.x - sign * k * 0.55f, c.y - k * 0.95f},
                                    {c.x - sign * k * 0.55f, c.y + k * 0.95f}}});
            break;
        }
        case Kind::Up:
        case Kind::Down: {
            const float sign = kind == Kind::Up ? -1.0f : 1.0f;
            s.triangles.push_back({{{c.x, c.y + sign * k}, {c.x - k * 0.95f, c.y - sign * k * 0.55f},
                                    {c.x + k * 0.95f, c.y - sign * k * 0.55f}}});
            break;
        }
    }
    if (ring > 0) s.arcs.push_back({c, ring - ring_width / 2, 0, 2 * kPi, ring_width / 2});
    return s;
}

// --- drawing ---------------------------------------------------------------------------------------

struct Vec3 {
    float x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalise(Vec3 a) {
    const float l = std::sqrt(dot3(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

struct Button {
    float rx = 0.72f, ry = 0.72f, wall = 0.26f, dome = 0.10f, concave = 0, squareness = 2;
    // A PlayStation D-pad key: the way its inner end faces (the pad's middle), which tapers to a
    // point; rx is then its width and ry its length. None while zero.
    Vec inward{0, 0};
    float tilt = kTilt, row = 0.12f, shine = 60, spec = 0.9f;
    bool engrave = false;
    Rgb base{}, ink{};
    Symbol symbol;
};

Plane blur(const Plane& in, int size, float sigma) {
    const int radius = std::max(1, int(std::ceil(sigma * 4)));
    std::vector<float> kernel(2 * radius + 1);
    float sum = 0;
    for (int i = -radius; i <= radius; i++) sum += kernel[i + radius] = std::exp(-0.5f * i * i / (sigma * sigma));
    for (float& k : kernel) k /= sum;
    auto mirror = [size](int v) { return v < 0 ? -v - 1 : v >= size ? 2 * size - v - 1 : v; };
    Plane across(in.size()), out(in.size());
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float v = 0;
            for (int i = -radius; i <= radius; i++) v += in[y * size + mirror(x + i)] * kernel[i + radius];
            across[y * size + x] = v;
        }
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float v = 0;
            for (int i = -radius; i <= radius; i++) v += across[mirror(y + i) * size + x] * kernel[i + radius];
            out[y * size + x] = v;
        }
    return out;
}

constexpr float kGridScale = G / 768.0f;  // the blurs below were tuned on a 768 grid

// The button seen from the front: straight RGBA, R x R, and the symbol's coverage at each pixel.
struct Render {
    std::vector<Rgb> rgb;
    Plane alpha, ink;
};

Render render(const Button& b, float ambient = 0.22f, float shine = 0.9f) {
    Plane h(G * G), inside(G * G);
    for (int i = 0; i < G; i++) {
        for (int j = 0; j < G; j++) {
            float wx = float(j) / G * 2 - 1, wy = float(i) / G * 2 - 1;
            if (b.inward.x != 0 || b.inward.y != 0) {
                // Across and along the key, the inner end narrowing from just inside its middle
                // to a blunt point.
                const float along = wx * b.inward.x + wy * b.inward.y;
                const float across = wx * b.inward.y - wy * b.inward.x;
                constexpr float kTaperFrom = 0.3f, kTip = 0.22f;
                const float t = std::clamp((along / b.ry - kTaperFrom) / (1 - kTaperFrom), 0.0f, 1.0f);
                wx = across / (1 - (1 - kTip) * t);
                wy = along;
            }
            const float s = std::pow(std::pow(std::abs(wx / b.rx), b.squareness) + std::pow(std::abs(wy / b.ry), b.squareness),
                                     1 / b.squareness);
            const float top = std::pow(std::clamp(1 - s * s * s * s, 0.0f, 1.0f), 0.25f);
            const float v = b.wall * top + b.dome * std::clamp(1 - s * s, 0.0f, 1.0f) -
                            b.concave * std::clamp(1 - (s / 0.8f) * (s / 0.8f), 0.0f, 1.0f);
            inside[i * G + j] = s < 1;
            h[i * G + j] = s < 1 ? v : 0;
        }
    }
    h = blur(h, G, G / 300.0f);
    const Plane glyph = b.symbol.mask();
    const Plane carve = blur(glyph, G, b.engrave ? G / 260.0f : G / 220.0f);
    for (int i = 0; i < G * G; i++) h[i] += (b.engrave ? -0.07f : 0.012f) * carve[i] * inside[i];
    h = blur(h, G, 1.2f * kGridScale);
    const Plane ink = blur(glyph, G, std::max(0.5f, 1.0f * kGridScale));

    const float ct = std::cos(b.tilt), st = std::sin(b.tilt);
    Vec3 light{0.62f, -0.05f, 0.78f};
    light = normalise(light);
    const Vec3 view{0, st, ct};
    const Vec3 half = normalise(light + view);
    const Vec3 bounce_dir{-0.5f, 0.6f, 0.2f};
    Render out{std::vector<Rgb>(R * R), Plane(R * R), Plane(R * R)};
    constexpr int kSteps = 400;
    for (int sy = 0; sy < R; sy++) {
        const float row = (float(sy) / R * 2 - 1) * 0.92f + b.row;  // screen row: y cos t - z sin t
        for (int sx = 0; sx < R; sx++) {
            const int gj = std::clamp(int((float(sx) / R) * G), 0, G - 1);
            int hit = -1;
            for (int k = 0; k <= kSteps && hit < 0; k++) {
                const float z = 0.7f * (1 - float(k) / kSteps);
                const float y = (row + z * st) / ct;
                if (y <= -1 || y >= 1) continue;
                const int gi = std::clamp(int((y + 1) * 0.5f * G), 0, G - 1);
                const float v = h[gi * G + gj];
                if (v >= z && v > 1e-3f) hit = gi * G + gj;
            }
            if (hit < 0) continue;
            const int gi = hit / G;
            auto at = [&](int i, int j) { return h[std::clamp(i, 0, G - 1) * G + std::clamp(j, 0, G - 1)]; };
            const float spacing = 2.0f / G;
            const float gx = (at(gi, gj + 1) - at(gi, gj - 1)) / (2 * spacing);  // central differences
            const float gy = (at(gi + 1, gj) - at(gi - 1, gj)) / (2 * spacing);
            const Vec3 n = normalise(Vec3{-gx, -gy, 1});
            const float diffuse = std::clamp(dot3(n, light), 0.0f, 1.0f);
            const float specular = std::pow(std::clamp(dot3(n, half), 0.0f, 1.0f), b.shine) * b.spec * shine / 0.9f;
            const float bounce = std::clamp(dot3(n, normalise(bounce_dir)), 0.0f, 1.0f) * 0.12f;
            const float m = ink[hit];
            Rgb& c = out.rgb[sy * R + sx];
            for (int ch = 0; ch < 3; ch++) {
                const float albedo = b.base[ch] * (1 - m) + b.ink[ch] * m;
                c[ch] = std::clamp(albedo * (ambient + 0.95f * diffuse + bounce) + specular, 0.0f, 1.0f);
            }
            out.alpha[sy * R + sx] = 1;
            out.ink[sy * R + sx] = m;
        }
    }
    return out;
}

// The lit frame of a coloured cap, as Rare's: the cap glows a flat, saturated colour, keeps its
// highlight, its carving stays darker so it can be read, and a halo spills past its edge.
Render lit_colour(const Button& b) {
    Render r = render(b, 0.35f, 1.0f);
    const float top = std::max({b.base[0], b.base[1], b.base[2], 1e-3f});
    const Rgb glow{b.base[0] / top, b.base[1] / top, b.base[2] / top};
    const Plane halo = blur(r.alpha, R, R / 32.0f * 1.3f);
    for (int i = 0; i < R * R; i++) {
        Rgb& c = r.rgb[i];
        const float lum = (c[0] + c[1] + c[2]) / 3;
        const float hot = std::clamp(std::min({c[0], c[1], c[2]}) - 0.75f, 0.0f, 1.0f) * 4;
        Rgb lit;
        for (int ch = 0; ch < 3; ch++) {
            lit[ch] = glow[ch] * (0.72f + 0.4f * lum) + (c[ch] - lum) * 0.15f;
            lit[ch] = std::clamp(lit[ch] * (1 - r.ink[i] * 0.35f) + hot, 0.0f, 1.0f);
        }
        const float a = r.alpha[i];
        const float h = std::clamp(halo[i] * 0.9f, 0.0f, 0.85f);
        const float out_a = a + h * (1 - a);
        for (int ch = 0; ch < 3; ch++) c[ch] = std::clamp((lit[ch] * a + glow[ch] * h * (1 - a)) / std::max(out_a, 1e-6f), 0.0f, 1.0f);
        r.alpha[i] = out_a;
    }
    return r;
}

// The lit frame: a dark cap stays dark and sharp; its symbol lights up and throws its colour over
// the cap and, for a coloured symbol, past its edge.
Render lit(const Button& b) {
    if (std::max({b.base[0], b.base[1], b.base[2]}) > 0.3f) return lit_colour(b);
    Render r = render(b, 0.35f, 1.0f);
    const float mean = (b.ink[0] + b.ink[1] + b.ink[2]) / 3;
    float spread = 0;
    for (float v : b.ink) spread += (v - mean) * (v - mean);
    const bool grey = std::sqrt(spread / 3) < 0.1f;
    Rgb glow;
    for (int ch = 0; ch < 3; ch++) glow[ch] = std::clamp(b.ink[ch] * 1.2f + 0.1f, 0.0f, 1.0f);
    const Plane narrow = blur(r.ink, R, R / 32.0f * 0.5f), wide = blur(r.ink, R, R / 32.0f * 1.5f);
    const Plane halo = grey ? Plane(R * R, 0.0f) : blur(r.alpha, R, R / 32.0f * 1.2f);
    const float strength = grey ? 0.55f : 1.0f;  // grey and white symbols bloom less
    for (int i = 0; i < R * R; i++) {
        Rgb& c = r.rgb[i];
        const float m = r.ink[i], a = r.alpha[i];
        Rgb out_c;
        for (int ch = 0; ch < 3; ch++) {
            float v = c[ch] * 0.85f + glow[ch] * 0.07f;
            v = v * (1 - m) + std::clamp(glow[ch] * 1.15f + 0.25f, 0.0f, 1.0f) * m;
            out_c[ch] = std::clamp(v + glow[ch] * strength * (narrow[i] * 1.6f + wide[i] * 1.1f) * a, 0.0f, 1.0f);
        }
        const float h = std::clamp(halo[i] * 0.45f, 0.0f, 0.85f);
        const float out_a = a + h * (1 - a);
        for (int ch = 0; ch < 3; ch++) c[ch] = std::clamp((out_c[ch] * a + glow[ch] * h * (1 - a)) / std::max(out_a, 1e-6f), 0.0f, 1.0f);
        r.alpha[i] = out_a;
    }
    return r;
}

// The box [x0, x1) x [y0, y1) holding every pixel with alpha above `alpha`.
struct Box {
    float x0, x1, y0, y1;
};
Box opaque(const Render& r, float alpha) {
    Box b{float(R), 0, float(R), 0};
    for (int y = 0; y < R; y++)
        for (int x = 0; x < R; x++)
            if (r.alpha[y * R + x] > alpha) {
                b.x0 = std::min(b.x0, float(x)), b.x1 = std::max(b.x1, float(x + 1));
                b.y0 = std::min(b.y0, float(y)), b.y1 = std::max(b.y1, float(y + 1));
            }
    return b;
}

// `r` reduced to width x height pixels from the source box `from`, averaged on premultiplied alpha.
Picture reduce(const Render& r, Box from, int width, int height, std::string name) {
    Picture out{std::move(name), width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    const float sx = (from.x1 - from.x0) / width, sy = (from.y1 - from.y0) / height;
    const int n = std::max(2, int(std::ceil(std::max(sx, sy))) * 2);  // samples across each pixel
    auto sample = [&](float x, float y, std::array<float, 4>& acc) {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        if (ix < 0 || iy < 0 || ix >= R || iy >= R) return;
        const int i = iy * R + ix;
        const float a = r.alpha[i];
        for (int ch = 0; ch < 3; ch++) acc[ch] += r.rgb[i][ch] * a;
        acc[3] += a;
    };
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            std::array<float, 4> acc{};
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++)
                    sample(from.x0 + (x + (i + 0.5f) / n) * sx, from.y0 + (y + (j + 0.5f) / n) * sy, acc);
            for (float& v : acc) v /= float(n * n);
            uint8_t* p = &out.rgba[(size_t(y) * width + x) * 4];
            for (int ch = 0; ch < 3; ch++) p[ch] = uint8_t(std::lround(std::clamp(acc[3] > 0 ? acc[ch] / acc[3] : 0.0f, 0.0f, 1.0f) * 255));
            p[3] = uint8_t(std::lround(std::clamp(acc[3], 0.0f, 1.0f) * 255));
        }
    }
    return out;
}

// --- the sets ----------------------------------------------------------------------------------

struct Entry {
    std::string name;
    int width, height;  // game texels
    bool has_lit;
    Button button;
};

Button with(Button b, Rgb base, Rgb ink, Symbol symbol, bool engrave = false) {
    b.base = base, b.ink = ink, b.symbol = std::move(symbol), b.engrave = engrave;
    return b;
}

// The keyboard set's keycap: dark, with a white legend; wide for Space and Enter.
Button keycap(bool wide) {
    Button key;
    key.squareness = 5, key.rx = key.ry = 0.7f, key.wall = 0.24f, key.dome = 0, key.concave = 0.05f;
    key.shine = 50, key.spec = 0.6f;
    if (wide) {
        key.rx = 0.94f, key.ry = 0.46f;
    }
    return key;
}

// The D-pad pushed one way: its cross outlined, the arm pushed filled in.
Symbol dpad(Kind way, float width) {
    constexpr float w = 0.19f, L = 0.66f;  // an arm's half width and the cross's reach
    const Vec outline[12] = {{w, -L}, {w, -w}, {L, -w}, {L, w}, {w, w}, {w, L},
                             {-w, L}, {-w, w}, {-L, w}, {-L, -w}, {-w, -w}, {-w, -L}};
    Symbol s;
    for (int i = 0; i < 12; i++) s.lines.push_back({outline[i], outline[(i + 1) % 12], width / 2});
    // The arm pushed, from the middle square's edge to its end, as two triangles.
    Vec a, b, c, d;
    switch (way) {
        case Kind::Up: a = {-w, -L}, b = {w, -L}, c = {w, -w}, d = {-w, -w}; break;
        case Kind::Down: a = {-w, w}, b = {w, w}, c = {w, L}, d = {-w, L}; break;
        case Kind::Left: a = {-L, -w}, b = {-w, -w}, c = {-w, w}, d = {-L, w}; break;
        default: a = {w, -w}, b = {L, -w}, c = {L, w}, d = {w, w}; break;
    }
    s.triangles.push_back({{a, b, c}});
    s.triangles.push_back({{a, c, d}});
    s.lines.push_back({a, c, width / 2});  // over the seam where the two meet
    return s;
}

std::vector<Entry> entries(Style style, const Shown& shown) {
    Button face;  // A and B: a little smaller than Rare's, to clear the text beside them
    face.rx = face.ry = 0.66f;
    Button stick;
    stick.rx = stick.ry = 0.86f, stick.wall = 0.22f, stick.dome = 0, stick.concave = 0.10f, stick.shine = 30, stick.spec = 0.35f;
    Button shoulder;
    shoulder.squareness = 4, shoulder.wall = 0.2f, shoulder.dome = 0.04f, shoulder.tilt = 25 * kPi / 180;
    shoulder.row = -0.02f, shoulder.shine = 40, shoulder.spec = 0.6f;
    Button trigger = shoulder;  // tall, for Z
    trigger.rx = 0.46f, trigger.ry = 0.86f;
    Button bumper = shoulder;  // wide, for L and R
    bumper.rx = 0.9f, bumper.ry = 0.44f;
    Button wide;
    wide.rx = wide.ry = 0.76f;

    // Capitals about as tall as Arial Rounded Bold's at the sizes first chosen with it (`em`).
    auto label = [](const std::string& text, float em) { return letters(text, em * 0.8f); };
    std::vector<Entry> out;
    auto add = [&](const char* name, int w, int h, bool has_lit, Button b) { out.push_back({name, w, h, has_lit, std::move(b)}); };

    // An Xbox or PlayStation pad's controls, each drawn as it is on the pad: the face buttons'
    // coloured letters or symbols on black caps, the shoulders and triggers named, a stick pressed
    // named, the D-pad and the right stick (the C buttons by default) with an arrow the way pushed.
    using input::PadControl;
    const bool playstation = style == Style::PlayStation;
    const Rgb pad_cap = colour(playstation ? 0x1b1c20 : 0x1d1f22);
    const Rgb pad_ink = colour(playstation ? 0xc9ccd4 : 0xe8e8ea);
    Button options = wide;  // PlayStation's Options and Create (Share) are small and upright
    options.rx = 0.64f, options.ry = 0.84f;
    Button pill = wide;  // the touchpad and the microphone button: wide and low
    pill.rx = 0.94f, pill.ry = 0.5f, pill.dome = 0.02f;
    Button touchpad = wide;
    touchpad.rx = 0.94f, touchpad.ry = 0.54f, touchpad.dome = 0;
    Button dpad_cap = wide;  // Xbox: the whole D-pad, the arm pushed filled in
    dpad_cap.rx = dpad_cap.ry = 0.8f;
    // PlayStation: the D-pad is four keys, each with a little arrow; the one pushed, long the way
    // it points.
    auto dpad_key = [&](Kind way) {
        Button key = wide;
        key.squareness = 4, key.dome = 0.03f, key.wall = 0.22f;
        key.rx = 0.62f, key.ry = 0.88f;
        key.inward = way == Kind::Up ? Vec{0, 1} : way == Kind::Down ? Vec{0, -1} : way == Kind::Left ? Vec{1, 0} : Vec{-1, 0};
        Symbol arrow = shape(way, 0.17f, 0);  // towards the outer end, as on the pad
        for (Symbol::Triangle& t : arrow.triangles)
            for (Vec& p : t.p) p = p - key.inward * 0.3f;
        return with(key, pad_cap, pad_ink, arrow);
    };
    auto dpad_button = [&](Kind way) {
        return playstation ? dpad_key(way) : with(dpad_cap, pad_cap, pad_ink, dpad(way, 0.06f));
    };
    auto named = [&](auto xbox, auto ps) { return playstation ? ps : xbox; };
    auto pad_control = [&](PadControl control) -> Button {

        switch (control) {
            case PadControl::South:
                return playstation ? with(face, pad_cap, colour(0x7fb0f2), shape(Kind::Cross, 0.33f, 0.095f))
                                   : with(face, pad_cap, colour(0x4fd24f), label("A", 0.72f));
            case PadControl::East:
                return playstation ? with(face, pad_cap, colour(0xff6b6b), shape(Kind::Circle, 0.31f, 0.09f))
                                   : with(face, pad_cap, colour(0xff4a40), label("B", 0.72f));
            case PadControl::West:
                return playstation ? with(face, pad_cap, colour(0xf28fcf), shape(Kind::Square, 0.25f, 0.085f))
                                   : with(face, pad_cap, colour(0x3a8ef0), label("X", 0.72f));
            case PadControl::North:
                return playstation ? with(face, pad_cap, colour(0x3fd7b4), shape(Kind::Triangle, 0.3f, 0.085f, 0, 0.05f, 0.04f))
                                   : with(face, pad_cap, colour(0xf2c21b), label("Y", 0.72f));
            case PadControl::Start:
                return playstation ? with(options, pad_cap, pad_ink, shape(Kind::Menu, 0.36f, 0.11f))
                                   : with(wide, pad_cap, pad_ink, shape(Kind::Menu, 0.38f, 0.11f));
            case PadControl::Back:  // never mapped (the debug menu's), drawn all the same
                return playstation ? with(options, pad_cap, pad_ink, shape(Kind::Create, 0.36f, 0.075f))
                                   : with(wide, pad_cap, pad_ink, shape(Kind::View, 0.42f, 0.09f));
            case PadControl::Guide: return with(face, pad_cap, pad_ink, label(named("XBOX", "PS"), named(0.3f, 0.5f)));
            case PadControl::LeftStick: return with(stick, pad_cap, pad_ink, label(named("LS", "L3"), 0.5f));
            case PadControl::RightStick: return with(stick, pad_cap, pad_ink, label(named("RS", "R3"), 0.5f));
            case PadControl::LeftShoulder: return with(bumper, pad_cap, pad_ink, label(named("LB", "L1"), 0.56f));
            case PadControl::RightShoulder: return with(bumper, pad_cap, pad_ink, label(named("RB", "R1"), 0.56f));
            case PadControl::LeftTrigger: return with(trigger, pad_cap, pad_ink, label(named("LT", "L2"), 0.48f));
            case PadControl::RightTrigger: return with(trigger, pad_cap, pad_ink, label(named("RT", "R2"), 0.48f));
            case PadControl::DpadUp: return dpad_button(Kind::Up);
            case PadControl::DpadDown: return dpad_button(Kind::Down);
            case PadControl::DpadLeft: return dpad_button(Kind::Left);
            case PadControl::DpadRight: return dpad_button(Kind::Right);
            case PadControl::Misc: return with(playstation ? pill : wide, pad_cap, pad_ink, label(named("SHARE", "MIC"), named(0.24f, 0.3f)));
            // The paddles under the pad, as SDL numbers them: right then left upper, right then left
            // lower; on an Xbox Elite P1, P3, P2 and P4, on a DualSense Edge its right and left back
            // buttons, then its right and left Fn buttons.
            case PadControl::Paddle1: return with(bumper, pad_cap, pad_ink, label(named("P1", "RBACK"), named(0.56f, 0.3f)));
            case PadControl::Paddle2: return with(bumper, pad_cap, pad_ink, label(named("P3", "LBACK"), named(0.56f, 0.3f)));
            case PadControl::Paddle3: return with(bumper, pad_cap, pad_ink, label(named("P2", "R FN"), named(0.56f, 0.34f)));
            case PadControl::Paddle4: return with(bumper, pad_cap, pad_ink, label(named("P4", "L FN"), named(0.56f, 0.34f)));
            case PadControl::Touchpad: return with(touchpad, pad_cap, pad_ink, label("TOUCH", 0.26f));
            case PadControl::RightStickUp: return with(stick, pad_cap, pad_ink, shape(Kind::Up, 0.36f, 0, 0.66f));
            case PadControl::RightStickDown: return with(stick, pad_cap, pad_ink, shape(Kind::Down, 0.36f, 0, 0.66f));
            case PadControl::RightStickLeft: return with(stick, pad_cap, pad_ink, shape(Kind::Left, 0.36f, 0, 0.66f));
            default: return with(stick, pad_cap, pad_ink, shape(Kind::Right, 0.36f, 0, 0.66f));
        }
    };
    // A key as the keyboard set draws it: dark, with a white legend (the arrow keys an arrow), wide
    // for a long one.
    const Rgb key_cap = colour(0x26282d), key_ink = colour(0xeceef2);
    auto key_picture = [&](int scancode) {
        const std::string text = input::key_label(scancode);
        const Kind arrow = text == "UP" ? Kind::Up : text == "DOWN" ? Kind::Down : text == "LEFT" ? Kind::Left : Kind::Right;
        if (text == "UP" || text == "DOWN" || text == "LEFT" || text == "RIGHT") {
            return with(keycap(false), key_cap, key_ink, shape(arrow, 0.3f, 0));
        }
        const float em = text.size() == 1 ? 0.62f : text.size() <= 3 ? 0.44f : text.size() == 4 ? 0.4f : 0.34f;
        return with(keycap(text.size() > 3), key_cap, key_ink, label(text, em));
    };
    // Each picture as the control or key shown for its button; none for a button nothing presses.
    auto picture = [&](int shown) {
        return style == Style::Keyboard ? key_picture(shown) : pad_control(PadControl(shown));
    };
    auto pad_set = [&] {
        struct Place { const char* name; int w, h; bool has_lit; Slot slot; };
        constexpr Place kPlaces[] = {
            {"a", 32, 32, true, kSlotA}, {"b", 32, 32, true, kSlotB}, {"start", 32, 32, true, kSlotStart},
            {"z", 16, 32, false, kSlotZ}, {"l", 32, 16, false, kSlotL}, {"r", 32, 16, false, kSlotR},
            {"c_left", 32, 32, false, kSlotCLeft}, {"c_right", 32, 32, false, kSlotCRight},
            {"c_up", 32, 32, false, kSlotCUp}, {"c_down", 32, 32, false, kSlotCDown},
            {"c_left_small", 16, 16, false, kSlotCLeft}, {"c_right_small", 16, 16, false, kSlotCRight},
        };
        for (const Place& place : kPlaces) {
            if (shown[place.slot] != 0) add(place.name, place.w, place.h, place.has_lit, picture(shown[place.slot]));
        }
    };

    switch (style) {
        case Style::N64: {  // the N64 pad's own buttons, letters carved in
            const Rgb grey = colour(0x9a9da4), dark = colour(0x5b5e66), yellow = colour(0xf2c21b), deep = colour(0xc99a0c);
            Button start = wide;
            start.rx = start.ry = 0.8f;
            Button c = wide;
            c.rx = c.ry = 0.74f;
            add("a", 32, 32, true, with(face, colour(0x2c45c8), colour(0x1e2f8c), label("A", 0.72f), true));
            add("b", 32, 32, true, with(face, colour(0x1f9a3c), colour(0x146a28), label("B", 0.72f), true));
            add("start", 32, 32, true, with(start, colour(0xd42a2a), colour(0x8f1a1a), label("START", 0.3f), true));
            add("z", 16, 32, false, with(trigger, grey, dark, label("Z", 0.48f), true));
            add("l", 32, 16, false, with(bumper, grey, dark, label("L", 0.56f), true));
            add("r", 32, 16, false, with(bumper, grey, dark, label("R", 0.56f), true));
            add("c_left", 32, 32, false, with(c, yellow, deep, shape(Kind::Left, 0.34f, 0, 0, 0, 0.14f), true));
            add("c_right", 32, 32, false, with(c, yellow, deep, shape(Kind::Right, 0.34f, 0, 0, 0, 0.14f), true));
            add("c_up", 32, 32, false, with(c, yellow, deep, shape(Kind::Up, 0.34f, 0, 0, 0, 0.14f), true));
            add("c_down", 32, 32, false, with(c, yellow, deep, shape(Kind::Down, 0.34f, 0, 0, 0, 0.14f), true));
            add("c_left_small", 16, 16, false, with(c, yellow, deep, shape(Kind::Left, 0.34f, 0, 0, 0, 0.14f), true));
            add("c_right_small", 16, 16, false, with(c, yellow, deep, shape(Kind::Right, 0.34f, 0, 0, 0, 0.14f), true));
            break;
        }
        case Style::Xbox:         // black caps, coloured letters
        case Style::PlayStation:  // the DualShock 4's black caps and coloured symbols
            pad_set();
            break;
        case Style::Keyboard:  // dark keycaps with white legends, the keys that press each button
            pad_set();
            break;
        default:
            break;
    }
    return out;
}

// One entry's pictures: the button, and its lit frame if it has one.
std::vector<Picture> draw(const Entry& e) {
    const Render plain = render(e.button);
    const int width = e.width * kScale, height = e.height * kScale;
    Box from{0, float(R), 0, float(R)};
    if (e.width == e.height) {
        // In the middle of the picture, where Rare's sit: the game places the picture expecting it
        // there (one drawn higher reaches into the line of text above in a speech bubble).
        const Box b = opaque(plain, 0.5f);
        const float dx = R / 2.0f - (b.x0 + b.x1) / 2, dy = R / 2.0f - (b.y0 + b.y1) / 2;
        from = {-dx, R - dx, -dy, R - dy};
    } else {
        // Cut from around the button, a little larger than it, in the picture's shape.
        const Box b = opaque(plain, 0.01f);
        const float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
        float w = (b.x1 - b.x0) * 1.08f, h = (b.y1 - b.y0) * 1.08f;
        w = std::max(w, h * width / height), h = std::max(h, w * height / width);
        from = {cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2};
    }
    std::vector<Picture> out{reduce(plain, from, width, height, e.name)};
    if (e.has_lit) out.push_back(reduce(lit(e.button), from, width, height, e.name + "_lit"));
    return out;
}

} // namespace

std::vector<Picture> draw_keys(const std::vector<std::string>& labels) {
    const Rgb cap = colour(0x26282d), white = colour(0xeceef2);
    std::vector<Picture> out;
    for (const std::string& text : labels) {
        const bool wide = text.size() > 3;
        std::vector<Picture> made = draw({text, 32, 32, false, with(keycap(wide), cap, white, letters(text, (wide ? 0.4f : 0.44f) * 0.8f))});
        out.push_back(std::move(made.front()));
    }
    return out;
}

std::vector<Picture> draw_set(Style style, const Shown& shown) {
    const std::vector<Entry> list = entries(style, shown);
    std::vector<std::vector<Picture>> made(list.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < std::max(1u, std::thread::hardware_concurrency()); i++) {
        workers.emplace_back([&] {
            for (size_t k = next++; k < list.size(); k = next++) made[k] = draw(list[k]);
        });
    }
    for (std::thread& w : workers) w.join();
    std::vector<Picture> out;
    for (auto& pictures : made)
        for (Picture& p : pictures) out.push_back(std::move(p));
    return out;
}

} // namespace conker::pad_icons
