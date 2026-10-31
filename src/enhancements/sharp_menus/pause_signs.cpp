// SAVE and LOAD, built from the letters of the game's own green signs.
//
// SAVE goes on CONT...'s frame and LOAD on QUIT's (src/enhancements/pause_save/pause_save.cpp).
// Their letters come from signs that lean the same way: S, A and E from ERASE, which rises to the
// right like CONT..., and L and A from PLAY, which falls like QUIT; O comes from CONT... No green
// sign has a V or a D, so the V is ERASE's A upside down with the inside between its legs
// cleared, and the D is the stem of PLAY's P joined to the right part of CONT...'s O.
//
// Everything is done at four times the game's size. Each source sign is enlarged with Super-xBR
// and turned level by its own tilt; each letter is found by its bright core and cut out halfway
// to its neighbours with a soft edge, so its glow comes along. The word is laid out level, turned
// to the target frame's tilt and laid over that frame, enlarged with its own lettering cleared;
// where glows overlap the brighter one wins. The unlit signs use the unlit letters at the same
// places. The game-size signs are the large ones scaled down into the frame's inside.
#include "enhancements/sharp_menus/pause_signs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>

namespace conker::pause_signs {

namespace {

using menu_art::Image;

constexpr int kScale = 4;
constexpr int kInset = 4;     // pause_save's inside: more than 4 pixels in from the outline
constexpr float kFeather = 3; // softness of the cuts between letters, in large pixels

// First tile of each sign's unlit pair; the lit pair follows.
constexpr int kCont = 2003, kQuit = 2044, kErase = 2007, kPlay = 2039;

struct Core {
    int x0, x1, y0, y1;
};

// A source sign at 4x, turned level.
struct Level {
    Image image;
    std::vector<float> inside;  // the frame's inside, turned the same way, 0..1
    double degrees = 0;         // its tilt
};

Level make_level(const Image& sign) {
    Level out;
    out.degrees = menu_art::tilt(sign);
    out.image = menu_art::rotate(menu_art::super_xbr(sign, kScale), -out.degrees);
    const std::vector<uint8_t> mask = menu_art::inside_mask(sign, kInset);
    Image big(sign.width * kScale, sign.height * kScale);
    for (int y = 0; y < big.height; y++)
        for (int x = 0; x < big.width; x++)
            if (mask[size_t(y / kScale) * sign.width + x / kScale]) big.at(x, y)[3] = 255;
    const Image turned = menu_art::rotate(big, -out.degrees);
    out.inside.resize(size_t(turned.width) * turned.height);
    for (size_t i = 0; i < out.inside.size(); i++) out.inside[i] = turned.pixels[i * 4 + 3] / 255.0f;
    return out;
}

float sample_inside(const Level& level, int x, int y) {
    if (x < 0 || y < 0 || x >= level.image.width || y >= level.image.height) return 0;
    return level.inside[size_t(y) * level.image.width + x];
}

// The bright cores of a lit sign's letters, left to right: inside the frame, among the brightest
// green of the whole sign.
std::vector<Core> find_cores(const Level& level) {
    const Image& im = level.image;
    std::vector<float> greens;
    auto straight = [&](int x, int y, int k) {
        const float* p = im.at(x, y);
        return p[3] > 0 ? p[k] * 255.0f / p[3] : 0.0f;
    };
    for (int y = 0; y < im.height; y++)
        for (int x = 0; x < im.width; x++)
            if (im.at(x, y)[3] > 200) greens.push_back(straight(x, y, 1));
    if (greens.empty()) return {};
    std::sort(greens.begin(), greens.end());
    const float threshold = greens[size_t(greens.size() * 0.7)];
    std::vector<uint8_t> core(size_t(im.width) * im.height, 0);
    for (int y = 0; y < im.height; y++) {
        for (int x = 0; x < im.width; x++) {
            const float g = straight(x, y, 1);
            core[size_t(y) * im.width + x] = im.at(x, y)[3] > 200 && sample_inside(level, x, y) > 0.5f &&
                                             g >= threshold && g > straight(x, y, 0) + 40;
        }
    }
    std::vector<Core> cores;
    std::vector<std::pair<int, int>> stack;
    for (int y = 0; y < im.height; y++) {
        for (int x = 0; x < im.width; x++) {
            if (!core[size_t(y) * im.width + x]) continue;
            Core c{x, x, y, y};
            int count = 0;
            core[size_t(y) * im.width + x] = 0;
            stack.emplace_back(x, y);
            while (!stack.empty()) {
                const auto [cx, cy] = stack.back();
                stack.pop_back();
                count++;
                c.x0 = std::min(c.x0, cx), c.x1 = std::max(c.x1, cx), c.y0 = std::min(c.y0, cy), c.y1 = std::max(c.y1, cy);
                const int next[4][2] = {{cx + 1, cy}, {cx - 1, cy}, {cx, cy + 1}, {cx, cy - 1}};
                for (const auto& n : next) {
                    if (n[0] >= 0 && n[1] >= 0 && n[0] < im.width && n[1] < im.height &&
                        core[size_t(n[1]) * im.width + n[0]]) {
                        core[size_t(n[1]) * im.width + n[0]] = 0;
                        stack.emplace_back(n[0], n[1]);
                    }
                }
            }
            if (count > 30) cores.push_back(c);
        }
    }
    std::sort(cores.begin(), cores.end(), [](const Core& a, const Core& b) { return a.x0 < b.x0; });
    return cores;
}

// Where the unlit sign sits relative to the lit one, in level pixels.
std::pair<int, int> offset_between(const Image& lit, const Image& unlit) {
    double best = -1;
    std::pair<int, int> result{0, 0};
    for (int dy = -8; dy <= 8; dy++) {
        for (int dx = -8; dx <= 8; dx++) {
            double diff = 0;
            for (int y = 0; y < lit.height; y++) {
                for (int x = 0; x < lit.width; x++) {
                    const int ux = x + dx, uy = y + dy;
                    const float b = (ux >= 0 && uy >= 0 && ux < unlit.width && uy < unlit.height) ? unlit.at(ux, uy)[3] : 0;
                    diff += std::abs(lit.at(x, y)[3] - b);
                }
            }
            if (best < 0 || diff < best) best = diff, result = {dx, dy};
        }
    }
    return result;
}

// A source sign's letters: its level images, the lit cores, and the rows a letter is cut from.
struct Source {
    Level level[2];
    std::pair<int, int> unlit_offset;  // add to lit coordinates to reach the unlit sign
    std::vector<Core> cores;
    int top = 0, bottom = 0;           // the rows cut out, around the letters' middle
};

std::optional<Source> make_source(int first, size_t letters) {
    Source source;
    for (int variant = 0; variant < 2; variant++) {
        const std::optional<Image> sign = menu_art::load_row(first + variant * 2, 2);
        if (!sign) return std::nullopt;
        source.level[variant] = make_level(*sign);
    }
    source.cores = find_cores(source.level[kLit]);
    if (source.cores.size() < letters) return std::nullopt;
    source.cores.resize(letters);
    std::vector<int> middles;
    for (const Core& c : source.cores) middles.push_back((c.y0 + c.y1) / 2);
    std::sort(middles.begin(), middles.end());
    const int middle = middles[middles.size() / 2];
    source.top = middle - 33;
    source.bottom = middle + 29;
    source.unlit_offset = offset_between(source.level[kLit].image, source.level[kUnlit].image);
    return source;
}

// The part of a level sign between two cut lines, each given by its x at the top and bottom row,
// limited to the frame's inside. Rows top..bottom; columns trimmed to what the cut lets through.
Image cut(const Source& source, int variant, float left_top, float left_bottom, float right_top, float right_bottom) {
    const Level& level = source.level[variant];
    const int dx = variant == kUnlit ? source.unlit_offset.first : 0;
    const int dy = variant == kUnlit ? source.unlit_offset.second : 0;
    const int top = source.top + dy, rows = source.bottom - source.top + 1;
    const int x_min = int(std::floor(std::min(left_top, left_bottom) + dx - kFeather));
    const int x_max = int(std::ceil(std::max(right_top, right_bottom) + dx + kFeather));
    Image wide(x_max - x_min + 1, rows);
    int first = wide.width, last = -1;
    for (int r = 0; r < rows; r++) {
        const float t = rows > 1 ? float(r) / float(rows - 1) : 0;
        const float xl = left_top + (left_bottom - left_top) * t + dx;
        const float xr = right_top + (right_bottom - right_top) * t + dx;
        for (int c = 0; c < wide.width; c++) {
            const int x = x_min + c, y = top + r;
            const float edge = std::clamp((x - xl) / kFeather + 0.5f, 0.0f, 1.0f) *
                               std::clamp((xr - x) / kFeather + 0.5f, 0.0f, 1.0f);
            if (edge > 0) first = std::min(first, c), last = std::max(last, c);
            if (x < 0 || y < 0 || x >= level.image.width || y >= level.image.height) continue;
            const float m = edge * sample_inside(level, x, y);
            const float* p = level.image.at(x, y);
            float* q = wide.at(c, r);
            for (int k = 0; k < 4; k++) q[k] = p[k] * m;
        }
    }
    if (last < first) return Image(1, rows);
    Image out(last - first + 1, rows);
    for (int r = 0; r < rows; r++) std::copy(wide.at(first, r), wide.at(last, r) + 4, out.at(0, r));
    return out;
}

// Letter k cut halfway to its neighbours' cores (or a little past its own at the ends).
Image letter(const Source& source, int variant, size_t k) {
    const std::vector<Core>& c = source.cores;
    const float left = k > 0 ? (c[k - 1].x1 + c[k].x0) / 2.0f : c[k].x0 - 8.0f;
    const float right = k + 1 < c.size() ? (c[k].x1 + c[k + 1].x0) / 2.0f : c[k].x1 + 8.0f;
    return cut(source, variant, left, left, right, right);
}

Image max_merge(const Image& a, const Image& b, int bx) {
    Image out(std::max(a.width, bx + b.width), std::max(a.height, b.height));
    for (int y = 0; y < a.height; y++)
        std::copy(a.at(0, y), a.at(0, y) + a.width * 4, out.at(0, y));
    for (int y = 0; y < b.height; y++) {
        for (int x = 0; x < b.width; x++) {
            float* q = out.at(bx + x, y);
            const float* p = b.at(x, y);
            for (int k = 0; k < 4; k++) q[k] = std::max(q[k], p[k]);
        }
    }
    return out;
}

// ERASE's A upside down, with everything between its legs cleared: the crossbar and the space it
// closed off. The legs are found on the lit A and the same cut is used on the unlit one.
std::optional<Image> letter_v(const Source& erase, int variant) {
    auto flipped = [](Image im) {
        for (int y = 0; y < im.height / 2; y++)
            std::swap_ranges(im.at(0, y), im.at(0, y) + im.width * 4, im.at(0, im.height - 1 - y));
        return im;
    };
    const Image lit = flipped(letter(erase, kLit, 2));
    Image a = flipped(letter(erase, variant, 2));
    std::vector<float> greens;
    for (int i = 0; i < lit.width * lit.height; i++)
        if (lit.pixels[size_t(i) * 4 + 1] > 0) greens.push_back(lit.pixels[size_t(i) * 4 + 1]);
    if (greens.empty()) return std::nullopt;
    std::sort(greens.begin(), greens.end());
    const float threshold = greens[size_t(greens.size() * 0.55)];
    auto runs = [&](int y) {
        std::vector<std::pair<int, int>> out;
        for (int x = 0; x < lit.width; x++) {
            if (lit.at(x, y)[1] <= threshold) continue;
            if (!out.empty() && out.back().second == x - 1) out.back().second = x;
            else out.emplace_back(x, x);
        }
        return out;
    };
    // Rows where the legs are apart: first the feet (now at the top), then the space under the
    // crossbar.
    std::vector<std::vector<int>> spans;
    int first_row = -1, last_row = -1;
    for (int y = 0; y < lit.height; y++) {
        const auto r = runs(y);
        if (!r.empty()) {
            if (first_row < 0) first_row = y;
            last_row = y;
        }
        if (r.size() >= 2) {
            if (spans.empty() || spans.back().back() != y - 1) spans.emplace_back();
            spans.back().push_back(y);
        }
    }
    if (spans.size() < 2) return std::nullopt;
    std::vector<double> ys, inner_left, inner_right;
    for (int s = 0; s < 2; s++) {
        for (int y : spans[s]) {
            const auto r = runs(y);
            ys.push_back(y), inner_left.push_back(r.front().second), inner_right.push_back(r.back().first);
        }
    }
    auto fit = [&](const std::vector<double>& xs) {  // x = slope * y + intercept
        const double n = double(ys.size());
        double my = 0, mx = 0;
        for (size_t i = 0; i < ys.size(); i++) my += ys[i], mx += xs[i];
        my /= n, mx /= n;
        double num = 0, den = 0;
        for (size_t i = 0; i < ys.size(); i++) num += (ys[i] - my) * (xs[i] - mx), den += (ys[i] - my) * (ys[i] - my);
        const double slope = den > 0 ? num / den : 0;
        return std::pair<double, double>{slope, mx - slope * my};
    };
    const auto [ls, li] = fit(inner_left);
    const auto [rs, ri] = fit(inner_right);
    for (int y = 0; y < a.height; y++) {
        const double xl = ls * y + li + 2, xr = rs * y + ri - 2;
        for (int x = 0; x < a.width; x++) {
            const double between = std::clamp((x - xl) / 3.0, 0.0, 1.0) * std::clamp((xr - x) / 3.0, 0.0, 1.0);
            float* p = a.at(x, y);
            for (int k = 0; k < 4; k++) p[k] *= float(1.0 - between);
        }
    }
    // Back on the A's line: turning it over moved its middle by this much.
    const int shift = (first_row + last_row) - (lit.height - 1);
    Image out(a.width, a.height);
    for (int y = 0; y < a.height; y++) {
        const int from = y + shift;
        if (from >= 0 && from < a.height) std::copy(a.at(0, from), a.at(0, from) + a.width * 4, out.at(0, y));
    }
    return out;
}

// The stem of PLAY's P and the right part of CONT...'s O.
Image letter_d(const Source& play, const Source& cont, int variant) {
    const Core& p = play.cores[0];
    const Image stem = cut(play, variant, p.x0 - 5.0f, p.x0 - 5.0f, p.x0 + 14.0f, p.x0 + 14.0f);
    const std::vector<Core>& c = cont.cores;
    const float right = (c[1].x1 + c[2].x0) / 2.0f;
    const Image bowl = cut(cont, variant, c[1].x0 + 11.0f, c[1].x0 + 11.0f, right, right);
    return max_merge(stem, bowl, stem.width - 8);
}

Image word(const std::vector<Image>& letters) {
    constexpr int kGap = -4;  // the cuts' soft edges overlap
    Image out = letters.front();
    for (size_t i = 1; i < letters.size(); i++) out = max_merge(out, letters[i], out.width + kGap);
    return out;
}

// The word on the target frame: the frame enlarged with its own lettering cleared, the word
// centred in its inside and turned to its tilt.
Image on_frame(const Image& frame, const Level& target, const Level& target_lit, std::pair<int, int> offset, const Image& letters) {
    Image cleared = frame;
    const std::vector<uint8_t> inside = menu_art::inside_mask(frame, kInset);
    for (size_t i = 0; i < inside.size(); i++)
        if (inside[i]) std::fill_n(&cleared.pixels[i * 4], 4, 0.0f);
    const Image base = menu_art::super_xbr(cleared, kScale);

    int x0 = target_lit.image.width, x1 = -1, y0 = target_lit.image.height, y1 = -1;
    for (int y = 0; y < target_lit.image.height; y++) {
        for (int x = 0; x < target_lit.image.width; x++) {
            if (sample_inside(target_lit, x, y) > 0.5f) {
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
        }
    }
    const double cx = (x0 + x1) / 2.0 + offset.first, cy = (y0 + y1) / 2.0 + offset.second;
    Image layer(target.image.width, target.image.height);
    const int lx = int(std::lround(cx - letters.width / 2.0)), ly = int(std::lround(cy - letters.height / 2.0));
    for (int y = 0; y < letters.height; y++) {
        for (int x = 0; x < letters.width; x++) {
            const int tx = lx + x, ty = ly + y;
            if (tx >= 0 && ty >= 0 && tx < layer.width && ty < layer.height) std::copy_n(letters.at(x, y), 4, layer.at(tx, ty));
        }
    }
    const Image turned = menu_art::rotate(layer, target.degrees);
    const int ox = (turned.width - base.width) / 2, oy = (turned.height - base.height) / 2;
    Image out = base;
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            const int tx = x + ox, ty = y + oy;
            if (tx < 0 || ty < 0 || tx >= turned.width || ty >= turned.height) continue;
            float* q = out.at(x, y);
            const float* p = turned.at(tx, ty);
            for (int k = 0; k < 4; k++) q[k] = std::max(q[k], p[k]);
        }
    }
    return out;
}

// The game-size sign: the frame as the game has it, and inside it the large sign scaled down.
// Super-xBR keeps each original pixel at the top left of its block, so pixel i is sampled around
// large pixel 4i.
Image game_size(const Image& frame, const Image& large) {
    constexpr float kWeights[5] = {0.5f, 1, 1, 1, 0.5f};
    const std::vector<uint8_t> inside = menu_art::inside_mask(frame, kInset);
    Image out = frame;
    for (int y = 0; y < frame.height; y++) {
        for (int x = 0; x < frame.width; x++) {
            if (!inside[size_t(y) * frame.width + x]) continue;
            float acc[4] = {}, total = 0;
            for (int j = -2; j <= 2; j++) {
                for (int i = -2; i <= 2; i++) {
                    const int lx = std::clamp(x * kScale + i, 0, large.width - 1);
                    const int ly = std::clamp(y * kScale + j, 0, large.height - 1);
                    const float w = kWeights[i + 2] * kWeights[j + 2];
                    for (int k = 0; k < 4; k++) acc[k] += w * large.at(lx, ly)[k];
                    total += w;
                }
            }
            for (int k = 0; k < 4; k++) out.at(x, y)[k] = acc[k] / total;
        }
    }
    return out;
}

std::unique_ptr<Signs> build() {
    const std::optional<Source> erase = make_source(kErase, 5);  // E R A S E
    const std::optional<Source> play = make_source(kPlay, 4);    // P L A Y
    const std::optional<Source> cont = make_source(kCont, 4);    // C O N T (then the dots)
    if (!erase || !play || !cont) return nullptr;
    auto signs = std::make_unique<Signs>();
    for (int sign = 0; sign < 2; sign++) {
        const int frame_tiles = sign == kSave ? kCont : kQuit;
        Level levels[2];
        std::optional<Image> frames[2];
        for (int variant = 0; variant < 2; variant++) {
            frames[variant] = menu_art::load_row(frame_tiles + variant * 2, 2);
            if (!frames[variant]) return nullptr;
            levels[variant] = make_level(*frames[variant]);
        }
        const std::pair<int, int> offset = offset_between(levels[kLit].image, levels[kUnlit].image);
        for (int variant = 0; variant < 2; variant++) {
            std::vector<Image> letters;
            if (sign == kSave) {
                const std::optional<Image> v = letter_v(*erase, variant);
                if (!v) return nullptr;
                letters = {letter(*erase, variant, 3), letter(*erase, variant, 2), *v, letter(*erase, variant, 4)};
            } else {
                const Core& a = play->cores[2];
                const float left = (play->cores[1].x1 + a.x0) / 2.0f;
                // PLAY's A and Y overlap: follow the A's right leg down.
                const Image play_a = cut(*play, variant, left, left, a.x1 - 10.0f, a.x1 + 8.0f);
                letters = {letter(*play, variant, 1), letter(*cont, variant, 1), play_a, letter_d(*play, *cont, variant)};
            }
            const Image large = on_frame(*frames[variant], levels[variant], levels[kLit],
                                         variant == kUnlit ? offset : std::pair<int, int>{0, 0}, word(letters));
            const Image small_sign = game_size(*frames[variant], large);
            signs->large[sign][variant] = large;
            signs->tiles[sign][variant] = {menu_art::encode_tile(small_sign, 0), menu_art::encode_tile(small_sign, 32)};
        }
    }
    return signs;
}

} // namespace

const Signs* get() {
    static std::once_flag once;
    static std::unique_ptr<Signs> signs;
    std::call_once(once, [] {
        try {
            signs = build();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[conker] pause signs: %s\n", error.what());
        }
        if (!signs) {
            std::fprintf(stderr, "[conker] pause signs: the ROM's green signs are not as expected\n");
        }
    });
    return signs.get();
}

} // namespace conker::pause_signs
