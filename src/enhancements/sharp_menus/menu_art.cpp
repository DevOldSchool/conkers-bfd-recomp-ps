// The game loads its menu sprites with func_1510D0EC: flat resource N is the Nth RZIP stream of
// an area starting at ROM 0x1A37E0, and the game data's halfword table D_80091D20 gives each
// stream's compressed size (zero for an empty slot). The menu's signs and headings are 32x32
// RGBA32 tiles, one resource each, side by side in consecutive resources. Format documented by
// the decomp (docs/evidence/assets/interface/us_hud_menu_assets.md, docs/rzip-assets.md).
#include "enhancements/sharp_menus/menu_art.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <stdexcept>

#include "librecomp/game.hpp"
#include "runtime/memory.hpp"
#include "runtime/rzip.hpp"

namespace conker::menu_art {

namespace {

constexpr uint32_t kResourceRom = 0x1A37E0;
constexpr uint32_t kSizeTable = 0x80091D20;  // D_80091D20, in the game data image
constexpr int kResourceCount = 7762;
constexpr int kTile = 32;

struct Resources {
    std::vector<uint32_t> offsets;  // ROM offset of each resource's stream
    std::vector<uint16_t> sizes;
};

const Resources& resources() {
    static const Resources table = [] {
        Resources out;
        std::span<const uint8_t> rom = recomp::get_rom();
        try {
            if (rom.size() < memory::kGameArchiveEnd) {
                return out;
            }
            const rzip::GameArchive archive = rzip::parse_game_archive(
                rom.subspan(memory::kGameArchiveRom, memory::kGameArchiveEnd - memory::kGameArchiveRom));
            const size_t table = kSizeTable - memory::kGameDataVram;
            if (archive.data.size() < table + kResourceCount * 2) {
                return out;
            }
            uint32_t offset = kResourceRom;
            for (int i = 0; i < kResourceCount; i++) {
                const uint16_t size = uint16_t(archive.data[table + i * 2] << 8 | archive.data[table + i * 2 + 1]);
                out.offsets.push_back(offset);
                out.sizes.push_back(size);
                offset += size;
            }
            if (offset > rom.size()) {
                out = {};
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[conker] menu art: %s\n", error.what());
            out = {};
        }
        return out;
    }();
    return table;
}

// --- Super-xBR -----------------------------------------------------------------------------------

using Pixel = std::array<double, 4>;
using Plane = std::vector<Pixel>;  // w x h, premultiplied

constexpr double kW1 = -0.129633, kW2 = 0.129633 + 0.5;
constexpr double kW3 = -0.175068, kW4 = 0.175068 + 0.5;
constexpr double kEdgeWeights[6] = {2.0, 1.0, -1.0, 4.0, -1.0, 1.0};
constexpr double kRotatedWeights[6] = {2.0, 0.0, 0.0, 0.0, 0.0, 0.0};

double edge_value(const Pixel& p) {
    return 0.75 * (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) + 0.25 * p[3];
}

// How much stronger the edge across one diagonal of the 4x4 neighbourhood m[x][y] is than across
// the other.
double diagonal_edge(const double m[4][4], const double* wp) {
    auto df = [](double a, double b) { return std::abs(a - b); };
    const double dw1 = wp[0] * (df(m[0][2], m[1][1]) + df(m[1][1], m[2][0]) + df(m[1][3], m[2][2]) + df(m[2][2], m[3][1])) +
                       wp[1] * (df(m[0][3], m[1][2]) + df(m[2][1], m[3][0])) +
                       wp[2] * (df(m[0][3], m[2][1]) + df(m[1][2], m[3][0])) +
                       wp[3] * df(m[1][2], m[2][1]) +
                       wp[4] * (df(m[0][2], m[2][0]) + df(m[1][3], m[3][1])) +
                       wp[5] * (df(m[0][1], m[1][0]) + df(m[2][3], m[3][2]));
    const double dw2 = wp[0] * (df(m[0][1], m[1][2]) + df(m[1][2], m[2][3]) + df(m[1][0], m[2][1]) + df(m[2][1], m[3][2])) +
                       wp[1] * (df(m[0][0], m[1][1]) + df(m[2][2], m[3][3])) +
                       wp[2] * (df(m[0][0], m[2][2]) + df(m[1][1], m[3][3])) +
                       wp[3] * df(m[1][1], m[2][2]) +
                       wp[4] * (df(m[1][0], m[3][2]) + df(m[0][1], m[2][3])) +
                       wp[5] * (df(m[0][2], m[1][3]) + df(m[2][0], m[3][1]));
    return dw1 - dw2;
}

// The centre of the neighbourhood p[x][y], interpolated along the diagonal with the weaker edge,
// each channel clamped to the four pixels around it.
Pixel interpolate(const Pixel* p[4][4], const double y[4][4], const double* wp, double wa, double wb) {
    const Pixel *a, *b, *c, *d;
    if (diagonal_edge(y, wp) <= 0) {
        a = p[0][3], b = p[3][0], c = p[1][2], d = p[2][1];
    } else {
        a = p[0][0], b = p[3][3], c = p[1][1], d = p[2][2];
    }
    Pixel out;
    for (int k = 0; k < 4; k++) {
        const double v = wa * ((*a)[k] + (*b)[k]) + wb * ((*c)[k] + (*d)[k]);
        const double lo = std::min({(*p[1][1])[k], (*p[2][1])[k], (*p[1][2])[k], (*p[2][2])[k]});
        const double hi = std::max({(*p[1][1])[k], (*p[2][1])[k], (*p[1][2])[k], (*p[2][2])[k]});
        out[k] = std::clamp(v, lo, hi);
    }
    return out;
}

// One Super-xBR 2x step.
Plane double_size(const Plane& img, int w, int h) {
    const int ow = w * 2, oh = h * 2;
    Plane out(size_t(ow) * oh);
    auto src = [&](int x, int y) -> const Pixel& {
        return img[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)];
    };
    auto dst = [&](int x, int y) -> const Pixel& {
        return out[size_t(std::clamp(y, 0, oh - 1)) * ow + std::clamp(x, 0, ow - 1)];
    };
    const Pixel* p[4][4];
    double y4[4][4];
    auto fill_y = [&] {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) y4[i][j] = edge_value(*p[i][j]);
    };
    // Pass 1: each original pixel, and the pixel between it and its three neighbours below right.
    for (int y = 0; y < oh; y += 2) {
        for (int x = 0; x < ow; x += 2) {
            const int cx = x / 2, cy = y / 2;
            for (int sx = -1; sx < 3; sx++)
                for (int sy = -1; sy < 3; sy++) p[sx + 1][sy + 1] = &src(cx + sx, cy + sy);
            fill_y();
            const Pixel centre = interpolate(p, y4, kEdgeWeights, kW1, kW2);
            const Pixel& o = img[size_t(cy) * w + cx];
            out[size_t(y) * ow + x] = out[size_t(y) * ow + x + 1] = out[size_t(y + 1) * ow + x] = o;
            out[size_t(y + 1) * ow + x + 1] = centre;
        }
    }
    // Pass 2: the other two pixels of each block, from neighbourhoods rotated by 45 degrees.
    for (int y = 0; y < oh; y += 2) {
        for (int x = 0; x < ow; x += 2) {
            for (int sx = -1; sx < 3; sx++)
                for (int sy = -1; sy < 3; sy++) p[sx + 1][sy + 1] = &dst(sx + sy + x, sx - sy + y);
            fill_y();
            const Pixel right = interpolate(p, y4, kRotatedWeights, kW3, kW4);
            for (int sx = -1; sx < 3; sx++)
                for (int sy = -1; sy < 3; sy++) p[sx + 1][sy + 1] = &dst(sx + sy - 1 + x, sx - sy + 1 + y);
            fill_y();
            const Pixel below = interpolate(p, y4, kRotatedWeights, kW3, kW4);
            out[size_t(y) * ow + x + 1] = right;
            out[size_t(y + 1) * ow + x] = below;
        }
    }
    // Pass 3: refine every pixel, from the bottom right.
    std::vector<double> ys(out.size());
    for (size_t i = 0; i < out.size(); i++) ys[i] = edge_value(out[i]);
    for (int y = oh - 1; y >= 0; y--) {
        for (int x = ow - 1; x >= 0; x--) {
            for (int sx = -2; sx < 2; sx++) {
                for (int sy = -2; sy < 2; sy++) {
                    const int cx = std::clamp(x + sx, 0, ow - 1), cy = std::clamp(y + sy, 0, oh - 1);
                    p[sx + 2][sy + 2] = &out[size_t(cy) * ow + cx];
                    y4[sx + 2][sy + 2] = ys[size_t(cy) * ow + cx];
                }
            }
            const Pixel v = interpolate(p, y4, kEdgeWeights, kW1, kW2);
            out[size_t(y) * ow + x] = v;
            ys[size_t(y) * ow + x] = edge_value(v);
        }
    }
    return out;
}

double cubic(double t) {  // Keys, a = -0.5
    t = std::abs(t);
    if (t < 1) return (1.5 * t - 2.5) * t * t + 1;
    if (t < 2) return ((-0.5 * t + 2.5) * t - 4) * t + 2;
    return 0;
}

} // namespace

std::vector<uint8_t> load_resource(int id) {
    const Resources& table = resources();
    if (id < 0 || id >= int(table.sizes.size()) || table.sizes[id] == 0) {
        return {};
    }
    try {
        return rzip::inflate_chunk(recomp::get_rom().subspan(table.offsets[id], table.sizes[id]));
    } catch (const std::exception&) {
        return {};
    }
}

std::optional<StoredTile> load_tile(int id) {
    std::vector<uint8_t> bytes = load_resource(id);
    if (bytes.size() != size_t(kTile * kTile * 4)) {
        return std::nullopt;
    }
    return bytes;
}

Image decode_tile(const StoredTile& tile) {
    Image image(kTile, kTile);
    for (int y = 0; y < kTile; y++) {
        for (int x = 0; x < kTile; x++) {
            const uint8_t* p = &tile[size_t(y * kTile + ((y & 1) ? x ^ 2 : x)) * 4];
            float* q = image.at(x, y);
            const float a = p[3] / 255.0f;
            q[0] = p[0] * a, q[1] = p[1] * a, q[2] = p[2] * a, q[3] = p[3];
        }
    }
    return image;
}

std::vector<uint8_t> to_rgba8(const Image& image) {
    std::vector<uint8_t> out(size_t(image.width) * image.height * 4);
    for (size_t i = 0; i < size_t(image.width) * image.height; i++) {
        const float* p = &image.pixels[i * 4];
        const float a = std::clamp(p[3], 0.0f, 255.0f);
        for (int k = 0; k < 3; k++) {
            const float v = a > 0 ? p[k] * 255.0f / a : 0.0f;
            out[i * 4 + k] = uint8_t(std::lround(std::clamp(v, 0.0f, 255.0f)));
        }
        out[i * 4 + 3] = uint8_t(std::lround(a));
    }
    return out;
}

StoredTile encode_tile(const Image& image, int x0) {
    StoredTile tile(size_t(kTile * kTile * 4));
    const std::vector<uint8_t> rgba = to_rgba8(image);
    for (int y = 0; y < kTile; y++) {
        for (int x = 0; x < kTile; x++) {
            const uint8_t* p = &rgba[(size_t(y) * image.width + x0 + x) * 4];
            std::copy(p, p + 4, &tile[size_t(y * kTile + ((y & 1) ? x ^ 2 : x)) * 4]);
        }
    }
    return tile;
}

std::optional<Image> load_row(int first, int count) {
    Image row(kTile * count, kTile);
    for (int i = 0; i < count; i++) {
        const std::optional<StoredTile> tile = load_tile(first + i);
        if (!tile) {
            return std::nullopt;
        }
        const Image image = decode_tile(*tile);
        for (int y = 0; y < kTile; y++) {
            std::copy(image.at(0, y), image.at(0, y) + kTile * 4, row.at(i * kTile, y));
        }
    }
    return row;
}

Image super_xbr(const Image& image, int scale) {
    if (scale < 1 || (scale & (scale - 1)) != 0) {
        throw std::invalid_argument("Super-xBR scales by powers of two");
    }
    constexpr int kPad = 2;  // as far as the 4x4 neighbourhood reaches, repeating the border
    int w = image.width + 2 * kPad, h = image.height + 2 * kPad;
    Plane plane(size_t(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float* p = image.at(std::clamp(x - kPad, 0, image.width - 1), std::clamp(y - kPad, 0, image.height - 1));
            plane[size_t(y) * w + x] = {p[0], p[1], p[2], p[3]};
        }
    }
    for (int s = 1; s < scale; s *= 2) {
        plane = double_size(plane, w, h);
        w *= 2, h *= 2;
    }
    Image out(image.width * scale, image.height * scale);
    const int edge = kPad * scale;
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            const Pixel& p = plane[size_t(y + edge) * w + x + edge];
            std::copy(p.begin(), p.end(), out.at(x, y));
        }
    }
    return out;
}

Image rotate(const Image& image, double degrees) {
    const double r = degrees * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    const int w = int(std::ceil(std::abs(image.width * c) + std::abs(image.height * s) - 1e-6));
    const int h = int(std::ceil(std::abs(image.width * s) + std::abs(image.height * c) - 1e-6));
    Image out(w, h);
    const double icx = image.width / 2.0, icy = image.height / 2.0, ocx = w / 2.0, ocy = h / 2.0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // Screen y points down, so a counter-clockwise turn samples the source turned back.
            const double dx = x + 0.5 - ocx, dy = y + 0.5 - ocy;
            const double sx = icx + dx * c - dy * s - 0.5, sy = icy + dx * s + dy * c - 0.5;
            const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
            double acc[4] = {};
            for (int j = -1; j <= 2; j++) {
                const int yy = y0 + j;
                if (yy < 0 || yy >= image.height) continue;
                const double wy = cubic(sy - yy);
                for (int i = -1; i <= 2; i++) {
                    const int xx = x0 + i;
                    if (xx < 0 || xx >= image.width) continue;
                    const double wgt = wy * cubic(sx - xx);
                    const float* p = image.at(xx, yy);
                    for (int k = 0; k < 4; k++) acc[k] += wgt * p[k];
                }
            }
            float* q = out.at(x, y);
            q[3] = float(std::clamp(acc[3], 0.0, 255.0));
            for (int k = 0; k < 3; k++) q[k] = float(std::clamp(acc[k], 0.0, double(q[3])));
        }
    }
    return out;
}

double tilt(const Image& image) {
    double n = 0, mx = 0, my = 0;
    for (int y = 0; y < image.height; y++)
        for (int x = 0; x < image.width; x++)
            if (image.at(x, y)[3] > 128) n++, mx += x, my += y;
    if (n < 2) return 0;
    mx /= n, my /= n;
    double sxx = 0, syy = 0, sxy = 0;
    for (int y = 0; y < image.height; y++) {
        for (int x = 0; x < image.width; x++) {
            if (image.at(x, y)[3] > 128) {
                sxx += (x - mx) * (x - mx), syy += (y - my) * (y - my), sxy += (x - mx) * (y - my);
            }
        }
    }
    // The principal axis's angle; y points down, so counter-clockwise is a negative slope.
    double degrees = -0.5 * std::atan2(2 * sxy, sxx - syy) * 180.0 / 3.14159265358979323846;
    if (degrees > 90) degrees -= 180;
    if (degrees <= -90) degrees += 180;
    return degrees;
}

Box opaque_box(const Image& image, float alpha) {
    Box box{image.width, 0, image.height, 0};
    for (int y = 0; y < image.height; y++) {
        for (int x = 0; x < image.width; x++) {
            if (image.at(x, y)[3] > alpha) {
                box.x0 = std::min(box.x0, x), box.x1 = std::max(box.x1, x + 1);
                box.y0 = std::min(box.y0, y), box.y1 = std::max(box.y1, y + 1);
            }
        }
    }
    return box.x1 > box.x0 ? box : Box{};
}

Image resize(const Image& image, const Box& box, int width, int height) {
    Image out(width, height);
    const double sx = double(box.x1 - box.x0) / width, sy = double(box.y1 - box.y0) / height;
    for (int y = 0; y < height; y++) {
        const double top = box.y0 + y * sy, bottom = top + sy;
        for (int x = 0; x < width; x++) {
            const double left = box.x0 + x * sx, right = left + sx;
            double acc[4] = {}, total = 0;
            for (int yy = int(std::floor(top)); yy < int(std::ceil(bottom)); yy++) {
                const double wy = std::min(bottom, yy + 1.0) - std::max(top, double(yy));
                for (int xx = int(std::floor(left)); xx < int(std::ceil(right)); xx++) {
                    const double w = wy * (std::min(right, xx + 1.0) - std::max(left, double(xx)));
                    const float* p = image.at(std::clamp(xx, 0, image.width - 1), std::clamp(yy, 0, image.height - 1));
                    for (int k = 0; k < 4; k++) acc[k] += w * p[k];
                    total += w;
                }
            }
            for (int k = 0; k < 4; k++) out.at(x, y)[k] = float(acc[k] / total);
        }
    }
    return out;
}

std::vector<uint8_t> inside_mask(const Image& image, int inset) {
    const int w = image.width, h = image.height;
    std::vector<uint8_t> outside(size_t(w) * h, 0);
    std::vector<std::pair<int, int>> stack;
    auto push = [&](int x, int y) {
        if (x >= 0 && x < w && y >= 0 && y < h && !outside[size_t(y) * w + x] && image.at(x, y)[3] <= 60) {
            outside[size_t(y) * w + x] = 1;
            stack.emplace_back(x, y);
        }
    };
    for (int x = 0; x < w; x++) push(x, 0), push(x, h - 1);
    for (int y = 0; y < h; y++) push(0, y), push(w - 1, y);
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        push(x + 1, y), push(x - 1, y), push(x, y + 1), push(x, y - 1);
    }
    std::vector<uint8_t> inside(outside.size());
    for (size_t i = 0; i < inside.size(); i++) inside[i] = !outside[i];
    for (int pass = 0; pass < inset; pass++) {
        std::vector<uint8_t> next(inside.size(), 0);
        for (int y = 1; y < h - 1; y++) {
            for (int x = 1; x < w - 1; x++) {
                const size_t i = size_t(y) * w + x;
                next[i] = inside[i] && inside[i - 1] && inside[i + 1] && inside[i - w] && inside[i + w];
            }
        }
        inside.swap(next);
    }
    return inside;
}

} // namespace conker::menu_art
