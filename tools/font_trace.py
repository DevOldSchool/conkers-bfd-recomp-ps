"""Redraws a font glyph at a higher resolution with crisp edges (used by tools/font_pack.py).

A glyph's 16 grey levels are read as ink coverage: the letters were drawn larger and scaled down,
so a texel of 160 is two-thirds covered, not a dim stroke. Each partly covered texel gets a
straight edge (Youngs' piecewise-linear interface reconstruction, from volume-of-fluid methods):
its direction from the coverage around it, its position so that it covers exactly the texel's
share. A texel brighter than its neighbours on both sides along some direction is the middle of a
thin stroke and gets a band of that area instead, and one brighter than all of them a dot.
Scaled back down, the shape gives the original texels again, so letters keep their weight and
open counters (P, R, D, B).

The edges of neighbouring texels are then joined into one outline, its steps straightened while
deliberate corners stay sharp, and the strokes thinned to the weight the original appears to have
(in the game the dim edge texels show dark, so the eye reads only the bright core). A thin stroke
is laid along the centre of every run of touching texels the original draws, so faint diagonal
joins and tails (the comma, the C inside the copyright sign) stay connected. A thin dark outline
goes around the result.

Pure Python, no dependencies; coordinates are in texels with (0, 0) the glyph image's top left.
"""

import math

FULL = 240  # the font's brightest intensity: a fully covered texel

# Tuning, in texels unless noted.
SUB = 8            # subpixels per texel for the intermediate shape
CLOSE = 0.2        # blur that closes hairline gaps between neighbouring texels' edges
SPECK = 0.12       # outlines enclosing less area than this (texels²) are dropped
THIN = 0.3         # how much thicker strokes are thinned on each side
KEEP = 0.2         # half-width of the core no stroke is thinned below
CONNECT = 72       # intensity of texels joined by the centre-line strokes
STRAIGHT = 0.25    # outline deviation straightened away
CORNER = 45        # degrees of turn kept as a sharp corner rather than rounded
EDGE = 0.18        # width of the dark outline
GAP = 0.25         # cracks and notches narrower than this are filled

SQUARE = [(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)]
NEIGHBOURS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))


# Polygon helpers.

def _clip(poly, nx, ny, d):
    """The part of a convex polygon where nx * x + ny * y >= d."""
    out = []
    for i in range(len(poly)):
        p, q = poly[i], poly[(i + 1) % len(poly)]
        fp, fq = nx * p[0] + ny * p[1] - d, nx * q[0] + ny * q[1] - d
        if fp >= 0:
            out.append(p)
        if (fp >= 0) != (fq >= 0):
            t = fp / (fp - fq)
            out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
    return out


def _signed_area(poly):
    return sum(poly[i - 1][0] * poly[i][1] - poly[i][0] * poly[i - 1][1] for i in range(len(poly))) / 2


def _solve(area_of, target, lo, hi):
    """The parameter in [lo, hi] at which the monotonic area_of reaches target."""
    rising = area_of(hi) > area_of(lo)
    for _ in range(40):
        mid = (lo + hi) / 2
        if (area_of(mid) < target) == rising:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def _contains(point, loop):
    x, y = point
    inside = False
    for i in range(len(loop)):
        (x0, y0), (x1, y1) = loop[i - 1], loop[i]
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            inside = not inside
    return inside


# Coverage reconstruction.

def _cells(image):
    """Each texel's shape: ("full",), ("empty",), ("edge", nx, ny, d), ("band", nx, ny, half) or
    ("dot", radius), relative to the texel's centre."""
    h, w = len(image), len(image[0])

    def f(x, y):
        return min(image[y][x], FULL) / FULL if 0 <= x < w and 0 <= y < h else 0.0

    cells = [[None] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            c = f(x, y)
            if c <= 0.02:
                cells[y][x] = ("empty",)
                continue
            if c >= 0.98:
                cells[y][x] = ("full",)
                continue
            # The middle of a thin stroke: the brightest texel around, darker on both sides.
            brightest = max(f(x + dx, y + dy) for dx, dy in NEIGHBOURS)
            ridges = []
            if brightest <= c + 0.15:
                for dx, dy in ((1, 0), (0, 1), (1, 1), (1, -1)):
                    a, b = f(x - dx, y - dy), f(x + dx, y + dy)
                    if c > a + 0.05 and c > b + 0.05:
                        ridges.append((dx, dy, max(a, b)))
            if len(ridges) == 4:
                cells[y][x] = ("dot", math.sqrt(c / math.pi))
                continue
            if ridges:
                dx, dy, _ = min(ridges, key=lambda r: r[2])
                length = math.hypot(dx, dy)
                nx, ny = dx / length, dy / length
                half = _solve(lambda t: abs(_signed_area(_clip(_clip(SQUARE, nx, ny, -t), -nx, -ny, -t))), c, 0.0, 1.0)
                cells[y][x] = ("band", nx, ny, half)
                continue
            # An edge facing the more covered side (Youngs' 3x3 gradient).
            gx = (f(x + 1, y - 1) + 2 * f(x + 1, y) + f(x + 1, y + 1)) - (f(x - 1, y - 1) + 2 * f(x - 1, y) + f(x - 1, y + 1))
            gy = (f(x - 1, y + 1) + 2 * f(x, y + 1) + f(x + 1, y + 1)) - (f(x - 1, y - 1) + 2 * f(x, y - 1) + f(x + 1, y - 1))
            g = math.hypot(gx, gy)
            if g < 1e-6:
                cells[y][x] = ("dot", math.sqrt(c / math.pi))
                continue
            nx, ny = gx / g, gy / g
            d = _solve(lambda t: abs(_signed_area(_clip(SQUARE, nx, ny, t))), c, -1.0, 1.0)
            cells[y][x] = ("edge", nx, ny, d)
    return cells


def _inked(cell, px, py):
    kind = cell[0]
    if kind == "edge":
        return cell[1] * px + cell[2] * py >= cell[3]
    if kind == "band":
        return abs(cell[1] * px + cell[2] * py) <= cell[3]
    if kind == "dot":
        return px * px + py * py <= cell[1] * cell[1]
    return kind == "full"


def _coverage(image, sub, samples=4):
    """The reconstructed shape's coverage at sub subpixels per texel."""
    cells = _cells(image)
    h, w = len(image), len(image[0])
    out = [[0.0] * (w * sub) for _ in range(h * sub)]
    for Y in range(h * sub):
        ty = Y // sub
        for X in range(w * sub):
            cell = cells[ty][X // sub]
            if cell[0] in ("full", "empty"):
                out[Y][X] = 1.0 if cell[0] == "full" else 0.0
                continue
            hits = 0
            for sy in range(samples):
                py = (Y + (sy + 0.5) / samples) / sub - ty - 0.5
                for sx in range(samples):
                    hits += _inked(cell, (X + (sx + 0.5) / samples) / sub - X // sub - 0.5, py)
            out[Y][X] = hits / (samples * samples)
    return out


# Raster helpers.

def _blur(grid, sigma):
    if sigma <= 0:
        return grid
    radius = max(1, int(math.ceil(sigma * 3)))
    kernel = [math.exp(-(i * i) / (2 * sigma * sigma)) for i in range(-radius, radius + 1)]
    total = sum(kernel)
    kernel = [k / total for k in kernel]
    h, w = len(grid), len(grid[0])
    rows = [[sum(kernel[k + radius] * grid[y][min(max(x + k, 0), w - 1)] for k in range(-radius, radius + 1))
             for x in range(w)] for y in range(h)]
    return [[sum(kernel[k + radius] * rows[min(max(y + k, 0), h - 1)][x] for k in range(-radius, radius + 1))
             for x in range(w)] for y in range(h)]


def _distance(mask):
    """Chamfer (3-4) distance from each set pixel to the nearest unset one, in pixels."""
    h, w = len(mask), len(mask[0])
    d = [[10 ** 6 if mask[y][x] else 0 for x in range(w)] for y in range(h)]
    for passes, ys, xs in ((((-1, 0, 3), (0, -1, 3), (-1, -1, 4), (1, -1, 4)), range(h), range(w)),
                           (((1, 0, 3), (0, 1, 3), (1, 1, 4), (-1, 1, 4)), range(h - 1, -1, -1), range(w - 1, -1, -1))):
        for y in ys:
            for x in xs:
                if d[y][x]:
                    for dx, dy, c in passes:
                        if 0 <= x + dx < w and 0 <= y + dy < h:
                            d[y][x] = min(d[y][x], d[y + dy][x + dx] + c)
    return [[v / 3 for v in row] for row in d]


def _max_filter(grid, radius):
    h, w = len(grid), len(grid[0])
    rows = [[max(row[max(0, x - radius):x + radius + 1]) for x in range(w)] for row in grid]
    return [[max(rows[yy][x] for yy in range(max(0, y - radius), min(h, y + radius + 1))) for x in range(w)]
            for y in range(h)]


def _skeleton(mask):
    """Zhang-Suen thinning."""
    m = [row[:] for row in mask]
    h, w = len(m), len(m[0])
    changed = True
    while changed:
        changed = False
        for step in (0, 1):
            remove = []
            for y in range(1, h - 1):
                for x in range(1, w - 1):
                    if not m[y][x]:
                        continue
                    p = [m[y - 1][x], m[y - 1][x + 1], m[y][x + 1], m[y + 1][x + 1],
                         m[y + 1][x], m[y + 1][x - 1], m[y][x - 1], m[y - 1][x - 1]]
                    if not 2 <= sum(p) <= 6:
                        continue
                    if sum(1 for i in range(8) if not p[i] and p[(i + 1) % 8]) != 1:
                        continue
                    if step == 0 and (p[0] * p[2] * p[4] or p[2] * p[4] * p[6]):
                        continue
                    if step == 1 and (p[0] * p[2] * p[6] or p[0] * p[4] * p[6]):
                        continue
                    remove.append((x, y))
            for x, y in remove:
                m[y][x] = 0
            changed = changed or bool(remove)
    return m


def _stroke_texels(image):
    """Texels that are part of a stroke's middle: bright ones, and fainter ones that are the
    middle of a thin stroke (no darker than their neighbours on both sides along some direction),
    leaving out the soft edges of thicker strokes."""
    h, w = len(image), len(image[0])

    def v(x, y):
        return image[y][x] if 0 <= x < w and 0 <= y < h else 0

    keep = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            c = v(x, y)
            if c >= 200:
                keep[y][x] = True
            elif c >= CONNECT:
                keep[y][x] = any(c >= v(x - dx, y - dy) and c >= v(x + dx, y + dy)
                                 for dx, dy in ((1, 0), (0, 1), (1, 1), (1, -1)))
    return keep


def _prune(skeleton, length):
    """Removes skeleton branches shorter than length pixels that end in a free end (the spurs
    thinning leaves at the corners of texel blocks), keeping the full length of longer ones."""
    h, w = len(skeleton), len(skeleton[0])
    on = {(x, y) for y in range(h) for x in range(w) if skeleton[y][x]}

    def neighbours(p, pixels):
        return [(p[0] + dx, p[1] + dy) for dx, dy in NEIGHBOURS if (p[0] + dx, p[1] + dy) in pixels]

    kept = set(on)
    for _ in range(length):
        ends = [p for p in kept if len(neighbours(p, kept)) <= 1]
        if not ends:
            break
        kept -= set(ends)
    # Grow the surviving branches back to their full length along the original skeleton.
    for _ in range(length):
        ends = [p for p in kept if len(neighbours(p, kept)) <= 1]
        grown = {q for p in ends for q in neighbours(p, on) if q not in kept}
        if not grown:
            break
        kept |= grown
    if not kept:
        kept = on  # a dot or short dash: nothing longer to keep
    return [[1 if (x, y) in kept else 0 for x in range(w)] for y in range(h)]


def _connect(image, cov, shape, sub):
    """Lays a stroke KEEP texels either side of the centre line of every run of touching stroke
    texels, where that line lies in the reconstructed shape (so it does not run past the end of a
    stroke)."""
    h, w = len(image), len(image[0])
    stroke = _stroke_texels(image)
    # One texel of margin so the skeleton never touches the edge.
    mask = [[1 if 0 <= Y // sub - 1 < h and 0 <= X // sub - 1 < w and stroke[Y // sub - 1][X // sub - 1] else 0
             for X in range((w + 2) * sub)] for Y in range((h + 2) * sub)]
    skeleton = _prune(_skeleton(mask), sub)
    r = KEEP * sub
    for y, row in enumerate(skeleton):
        for x, on in enumerate(row):
            if not on:
                continue
            cx, cy = x + 0.5 - sub, y + 0.5 - sub
            if not (0 <= int(cy) < h * sub and 0 <= int(cx) < w * sub) or shape[int(cy)][int(cx)] < 0.12:
                continue
            for Y in range(max(0, int(cy - r - 1)), min(h * sub, int(cy + r + 2))):
                for X in range(max(0, int(cx - r - 1)), min(w * sub, int(cx + r + 2))):
                    if (X + 0.5 - cx) ** 2 + (Y + 0.5 - cy) ** 2 <= r * r:
                        cov[Y][X] = 1.0
    return cov


def _thin(cov, sub):
    """Erodes the shape by THIN texels, but no stroke below a core KEEP texels either side of its
    centre."""
    mask = [[1 if c >= 0.5 else 0 for c in row] for row in cov]
    d = [[v / sub for v in row] for row in _distance(mask)]
    ridge = _max_filter(d, int(round(0.75 * sub)))
    return [[1.0 if d[y][x] > 0 and d[y][x] >= min(THIN, ridge[y][x] - KEEP) else 0.0
             for x in range(len(d[0]))] for y in range(len(d))]


def _close(cov, sub):
    """Fills cracks and notches narrower than GAP texels (a morphological closing), which the
    outline would otherwise draw as dark lines; counters and real gaps are wider."""
    r = GAP / 2 * sub
    ink = [[1 if c >= 0.5 else 0 for c in row] for row in cov]
    outside = _distance([[1 - v for v in row] for row in ink])
    grown = [[1 if d <= r else 0 for d in row] for row in outside]
    inside = _distance(grown)
    return [[1.0 if d > r else 0.0 for d in row] for row in inside]


# Outlines.

def _trace(grid, level):
    """Marching squares: closed loops where grid crosses level, in pixel units (pixel centres at
    +0.5)."""
    h, w = len(grid), len(grid[0])

    def v(x, y):
        return grid[y][x] if 0 <= x < w and 0 <= y < h else 0.0

    points = {}
    links = {}

    def point(key):
        if key not in points:
            kind, x, y = key
            x1, y1 = (x + 1, y) if kind == "h" else (x, y + 1)
            a, b = v(x, y), v(x1, y1)
            t = (level - a) / (b - a)
            points[key] = (x + 0.5 + (x1 - x) * t, y + 0.5 + (y1 - y) * t)
        return key

    for y in range(-1, h):
        for x in range(-1, w):
            tl, tr, br, bl = v(x, y) >= level, v(x + 1, y) >= level, v(x + 1, y + 1) >= level, v(x, y + 1) >= level
            top, right, bottom, left = ("h", x, y), ("v", x + 1, y), ("h", x, y + 1), ("v", x, y)
            case = tl * 8 + tr * 4 + br * 2 + bl
            pairs = {
                1: [(left, bottom)], 2: [(bottom, right)], 3: [(left, right)], 4: [(top, right)],
                6: [(top, bottom)], 7: [(left, top)], 8: [(left, top)], 9: [(top, bottom)],
                11: [(top, right)], 12: [(left, right)], 13: [(bottom, right)], 14: [(left, bottom)],
            }.get(case, [])
            if case in (5, 10):
                centre = (v(x, y) + v(x + 1, y) + v(x + 1, y + 1) + v(x, y + 1)) / 4 >= level
                if (case == 5) == centre:
                    pairs = [(left, top), (bottom, right)]
                else:
                    pairs = [(top, right), (left, bottom)]
            for a, b in pairs:
                links.setdefault(point(a), []).append(point(b))
                links.setdefault(b, []).append(a)

    loops = []
    seen = set()
    for start in links:
        if start in seen:
            continue
        loop = [start]
        seen.add(start)
        previous, current = None, start
        while True:
            following = [n for n in links[current] if n != previous]
            if not following or following[0] == start or following[0] in seen:
                break
            previous, current = current, following[0]
            loop.append(current)
            seen.add(current)
        loops.append([points[k] for k in loop])
    return loops


def _simplify(points, tolerance):
    """Ramer-Douglas-Peucker."""
    if len(points) < 3:
        return points
    a, b = points[0], points[-1]
    length = math.dist(a, b)
    worst, index = 0.0, 0
    for i in range(1, len(points) - 1):
        p = points[i]
        if length == 0:
            off = math.dist(p, a)
        else:
            off = abs((b[0] - a[0]) * (a[1] - p[1]) - (a[0] - p[0]) * (b[1] - a[1])) / length
        if off > worst:
            worst, index = off, i
    if worst <= tolerance:
        return [a, b]
    return _simplify(points[:index + 1], tolerance)[:-1] + _simplify(points[index:], tolerance)


def _round_gentle(loop, passes=3):
    """Chaikin corner cutting at vertices turning less than CORNER degrees, so curves round off
    and deliberate corners stay sharp."""
    pts = loop
    for _ in range(passes):
        n = len(pts)
        out = []
        for i in range(n):
            a, p, b = pts[i - 1], pts[i], pts[(i + 1) % n]
            v1, v2 = (p[0] - a[0], p[1] - a[1]), (b[0] - p[0], b[1] - p[1])
            l1, l2 = math.hypot(*v1), math.hypot(*v2)
            turn = 0.0
            if l1 and l2:
                turn = math.degrees(math.acos(max(-1.0, min(1.0, (v1[0] * v2[0] + v1[1] * v2[1]) / (l1 * l2)))))
            if turn >= CORNER:
                out.append(p)
            else:
                out.append((p[0] + (a[0] - p[0]) * 0.25, p[1] + (a[1] - p[1]) * 0.25))
                out.append((p[0] + (b[0] - p[0]) * 0.25, p[1] + (b[1] - p[1]) * 0.25))
        pts = out
    return pts


def _straighten(loop):
    """Straight edges between the loop's two farthest points, then gentle bends rounded."""
    n = len(loop)
    if n < 6:
        return loop
    far = max(range(n), key=lambda i: math.dist(loop[0], loop[i]))
    other = max(range(n), key=lambda i: math.dist(loop[far], loop[i]))
    a, b = sorted((far, other))
    pts = _simplify(loop[a:b + 1], STRAIGHT)[:-1] + _simplify(loop[b:] + loop[:a + 1], STRAIGHT)[:-1]
    return _round_gentle(pts) if len(pts) >= 3 else loop


def _offset(loops, distance):
    """Every loop moved outward from the ink by distance, with mitred corners."""
    result = []
    for n, loop in enumerate(loops):
        hole = sum(_contains(loop[0], other) for m, other in enumerate(loops) if m != n) % 2 == 1
        pts = loop if (_signed_area(loop) > 0) != hole else loop[::-1]
        out = []
        for i in range(len(pts)):
            (ax, ay), (px, py), (bx, by) = pts[i - 1], pts[i], pts[(i + 1) % len(pts)]
            normals = []
            for (x0, y0), (x1, y1) in (((ax, ay), (px, py)), ((px, py), (bx, by))):
                length = math.hypot(x1 - x0, y1 - y0) or 1.0
                normals.append(((y1 - y0) / length, -(x1 - x0) / length))
            mx, my = normals[0][0] + normals[1][0], normals[0][1] + normals[1][1]
            length = math.hypot(mx, my)
            if length < 1e-6:
                mx, my, length = normals[0][0], normals[0][1], 1.0
            mx, my = mx / length, my / length
            reach = distance / max(mx * normals[0][0] + my * normals[0][1], 0.4)
            out.append((px + mx * reach, py + my * reach))
        result.append(out)
    return result


def _fill(loops, width, height, scale, samples=8):
    """Even-odd coverage of loops at scale pixels per texel, exact across each scanline."""
    w, h = width * scale, height * scale
    cover = [[0.0] * w for _ in range(h)]
    edges = []
    for loop in loops:
        for i in range(len(loop)):
            (x0, y0), (x1, y1) = loop[i], loop[(i + 1) % len(loop)]
            if y0 != y1:
                edges.append((x0 * scale, y0 * scale, x1 * scale, y1 * scale))
    weight = 1.0 / samples
    for py in range(h):
        row = cover[py]
        for s in range(samples):
            y = py + (s + 0.5) / samples
            xs = sorted(x0 + (y - y0) * (x1 - x0) / (y1 - y0)
                        for x0, y0, x1, y1 in edges if (y0 <= y < y1) or (y1 <= y < y0))
            for i in range(0, len(xs) - 1, 2):
                a, b = max(xs[i], 0.0), min(xs[i + 1], float(w))
                if b <= a:
                    continue
                ia, ib = int(a), int(b)
                if ia == ib:
                    row[ia] += (b - a) * weight
                    continue
                row[ia] += (ia + 1 - a) * weight
                for x in range(ia + 1, min(ib, w)):
                    row[x] += weight
                if ib < w:
                    row[ib] += (b - ib) * weight
    return [[min(c, 1.0) for c in row] for row in cover]


def outlines(image):
    """The redrawn glyph's outlines, in texels."""
    cov = _blur(_coverage(image, SUB), CLOSE * SUB)
    # Thinned first, then the centre-line strokes, so thinning never breaks a stroke apart.
    cov = _blur(_close(_connect(image, _thin(cov, SUB), cov, SUB), SUB), 0.6)
    loops = [[(x / SUB, y / SUB) for x, y in loop] for loop in _trace(cov, 0.5) if len(loop) > 2]
    loops = [loop for loop in loops if abs(_signed_area(loop)) >= SPECK]
    return [_straighten(loop) for loop in loops]


def redraw(image, scale):
    """The glyph at scale pixels per texel as rows of RGBA: white ink inside a thin dark outline,
    ink brightness and opacity at most FULL as in the original."""
    loops = outlines(image)
    width, height = len(image[0]), len(image)
    ink = _fill(loops, width, height, scale)
    edge = _fill(_offset(loops, EDGE), width, height, scale)
    rows = []
    for ink_row, edge_row in zip(ink, edge):
        row = []
        for i, e in zip(ink_row, edge_row):
            a = max(i, e)
            c = round(FULL * i / a) if a > 0 else 0
            row.append((c, c, c, round(FULL * a)))
        rows.append(row)
    return rows
