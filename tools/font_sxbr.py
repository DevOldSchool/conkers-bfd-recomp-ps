"""Enlarges a font glyph with Super-xBR (used by tools/font_pack.py).

Super-xBR is Hyllian's edge-directed 2x upscaler for pixel art, applied repeatedly for 4x and 8x.
Each step keeps the original pixels, fills the pixel between four originals by interpolating along
whichever diagonal has the weaker edge, fills the remaining two from 45-degree rotated
neighbourhoods, then refines the whole image the same way. Every new value is clamped to the four
nearest pixels, so edges do not ring. The glyph keeps Rare's pixels and its soft grey edges; only
the enlargement is smoother and less blurred than the N64's three-point filtering.

Works on one channel (the glyph's intensity). Pure Python, no dependencies.
"""

WGT1 = 0.129633
WGT2 = 0.175068
W1, W2 = -WGT1, WGT1 + 0.5
W3, W4 = -WGT2, WGT2 + 0.5

EDGE_WEIGHTS = (2.0, 1.0, -1.0, 4.0, -1.0, 1.0)
ROTATED_WEIGHTS = (2.0, 0.0, 0.0, 0.0, 0.0, 0.0)


def _diagonal_edge(m, wp):
    """How much stronger the edge across one diagonal of the 4x4 neighbourhood m[x][y] is than
    across the other."""
    def df(a, b):
        return abs(a - b)

    dw1 = (wp[0] * (df(m[0][2], m[1][1]) + df(m[1][1], m[2][0]) + df(m[1][3], m[2][2]) + df(m[2][2], m[3][1]))
           + wp[1] * (df(m[0][3], m[1][2]) + df(m[2][1], m[3][0]))
           + wp[2] * (df(m[0][3], m[2][1]) + df(m[1][2], m[3][0]))
           + wp[3] * df(m[1][2], m[2][1])
           + wp[4] * (df(m[0][2], m[2][0]) + df(m[1][3], m[3][1]))
           + wp[5] * (df(m[0][1], m[1][0]) + df(m[2][3], m[3][2])))
    dw2 = (wp[0] * (df(m[0][1], m[1][2]) + df(m[1][2], m[2][3]) + df(m[1][0], m[2][1]) + df(m[2][1], m[3][2]))
           + wp[1] * (df(m[0][0], m[1][1]) + df(m[2][2], m[3][3]))
           + wp[2] * (df(m[0][0], m[2][2]) + df(m[1][1], m[3][3]))
           + wp[3] * df(m[1][1], m[2][2])
           + wp[4] * (df(m[1][0], m[3][2]) + df(m[0][1], m[2][3]))
           + wp[5] * (df(m[0][2], m[1][3]) + df(m[2][0], m[3][1])))
    return dw1 - dw2


def _interpolate(m, wp, wa, wb):
    """The centre of m, interpolated along the diagonal with the weaker edge, clamped to the four
    pixels around it."""
    lo = min(m[1][1], m[2][1], m[1][2], m[2][2])
    hi = max(m[1][1], m[2][1], m[1][2], m[2][2])
    if _diagonal_edge(m, wp) <= 0:
        value = wa * (m[0][3] + m[3][0]) + wb * (m[1][2] + m[2][1])
    else:
        value = wa * (m[0][0] + m[3][3]) + wb * (m[1][1] + m[2][2])
    return min(max(value, lo), hi)


def _double(img):
    """One Super-xBR 2x step of img[y][x]."""
    h, w = len(img), len(img[0])
    oh, ow = h * 2, w * 2
    out = [[0.0] * ow for _ in range(oh)]

    def src(x, y):
        return img[min(max(y, 0), h - 1)][min(max(x, 0), w - 1)]

    def dst(x, y):
        return out[min(max(y, 0), oh - 1)][min(max(x, 0), ow - 1)]

    # Pass 1: each original pixel, and the pixel between it and its three neighbours below right.
    for y in range(0, oh, 2):
        for x in range(0, ow, 2):
            cx, cy = x // 2, y // 2
            m = [[src(cx + sx, cy + sy) for sy in range(-1, 3)] for sx in range(-1, 3)]
            out[y][x] = out[y][x + 1] = out[y + 1][x] = img[cy][cx]
            out[y + 1][x + 1] = _interpolate(m, EDGE_WEIGHTS, W1, W2)
    # Pass 2: the other two pixels of each block, from neighbourhoods rotated by 45 degrees.
    for y in range(0, oh, 2):
        for x in range(0, ow, 2):
            m = [[dst(sx + sy + x, sx - sy + y) for sy in range(-1, 3)] for sx in range(-1, 3)]
            right = _interpolate(m, ROTATED_WEIGHTS, W3, W4)
            m = [[dst(sx + sy - 1 + x, sx - sy + 1 + y) for sy in range(-1, 3)] for sx in range(-1, 3)]
            below = _interpolate(m, ROTATED_WEIGHTS, W3, W4)
            out[y][x + 1] = right
            out[y + 1][x] = below
    # Pass 3: refine every pixel, from the bottom right.
    for y in range(oh - 1, -1, -1):
        for x in range(ow - 1, -1, -1):
            m = [[dst(x + sx, y + sy) for sy in range(-2, 2)] for sx in range(-2, 2)]
            out[y][x] = _interpolate(m, EDGE_WEIGHTS, W1, W2)
    return out


PAD = 2  # empty pixels around the glyph, as far as a 4x4 neighbourhood reaches


def enlarge(image, scale):
    """The glyph scale times larger as rows of RGBA, white with its intensity as alpha
    (R = G = B = A) as RT64 decodes the original I8 texture. scale must be a power of two.

    Several glyphs (0, 9, K, R, Y and others) have ink in the last column of their texture, with
    no empty column after it. Upscaling with the edge repeated would carry those strokes on to the
    texture's edge and cut them off there; the glyph is enlarged with empty pixels around it
    instead, so a stroke ends with its own soft falloff inside its last pixel.

    Super-xBR keeps each original pixel at the top left of its block, so pixel i's value sits at
    coordinate i as the N64 samples it, and the pack draws it without RT64's half-texel shift.
    The game's own rectangles would still cut glyphs short; src/platform/font_rects.cpp widens
    them over the empty rows and columns around each glyph."""
    if scale < 1 or scale & (scale - 1):
        raise ValueError("Super-xBR scales by powers of two")
    h, w = len(image), len(image[0])
    img = [[0.0] * (w + 2 * PAD) for _ in range(PAD)]
    img += [[0.0] * PAD + [float(v) for v in row] + [0.0] * PAD for row in image]
    img += [[0.0] * (w + 2 * PAD) for _ in range(PAD)]
    while len(img[0]) < (w + 2 * PAD) * scale:
        img = _double(img)
    edge = PAD * scale
    return [[(v, v, v, v) for v in (round(c) for c in row[edge:edge + w * scale])]
            for row in img[edge:edge + h * scale]]
