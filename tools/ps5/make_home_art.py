#!/usr/bin/env python3
"""The PS5 home screen's background for the title: sce_sys/pic0.dds and pic1.dds.

    make_home_art.py <out_dir> [--image <picture>] [--png <file>]

Both are 3840x2160 BC7 (DDS with the DX10 header, one mip level), what the console reads and
PS5_Vulkan's tools/validate-assets.sh checks. By default the picture is the port's own composition:
its app icon (assets/app-icon.png) on a dark backdrop that suits the home screen, whose menus and
text are drawn over the left of it. --image uses a picture of one's own instead, cropped to 16:9
from the middle. --png also writes the picture as a PNG to look at. Never the game's own art.

Needs Pillow and etcpak (BC7); tools/ps5/build.sh's container has both.
"""
import argparse
import struct
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

import etcpak

WIDTH, HEIGHT = 3840, 2160
ICON = Path(__file__).resolve().parents[2] / "assets" / "app-icon.png"

# DXGI_FORMAT_BC7_UNORM and D3D10_RESOURCE_DIMENSION_TEXTURE2D, as the DX10 header names them.
DXGI_FORMAT_BC7_UNORM = 98
DIMENSION_TEXTURE2D = 3


def composition() -> Image.Image:
    """The app icon on a dark backdrop: a warm glow behind it, fading to near black at the edges."""
    # The backdrop: a radial falloff, computed small and scaled up (it is smooth, so nothing is lost).
    small_w, small_h = WIDTH // 8, HEIGHT // 8
    centre_x, centre_y = small_w * 0.66, small_h * 0.5
    radius = small_h * 0.95
    backdrop = Image.new("RGB", (small_w, small_h))
    pixels = backdrop.load()
    inner, outer = (46, 32, 20), (6, 5, 4)
    for y in range(small_h):
        for x in range(small_w):
            distance = min(1.0, (((x - centre_x) ** 2 + (y - centre_y) ** 2) ** 0.5) / radius)
            t = distance * distance * (3 - 2 * distance)  # smoothstep
            pixels[x, y] = tuple(round(a + (b - a) * t) for a, b in zip(inner, outer))
    picture = backdrop.resize((WIDTH, HEIGHT), Image.Resampling.BICUBIC).convert("RGBA")

    # The icon on the right two thirds, clear of the home screen's text on the left, over a soft
    # shadow of its own shape.
    icon = Image.open(ICON).convert("RGBA")
    size = round(HEIGHT * 0.56)
    icon = icon.resize((size, size), Image.Resampling.LANCZOS)
    left = round(WIDTH * 0.66 - size / 2)
    top = round(HEIGHT * 0.5 - size / 2)

    shadow = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    alpha = icon.getchannel("A").point(lambda a: a * 0.7)
    shadow.paste((0, 0, 0, 255), (left + 24, top + 40), alpha)
    shadow = shadow.filter(ImageFilter.GaussianBlur(48))
    picture = Image.alpha_composite(picture, shadow)
    picture.alpha_composite(icon, (left, top))

    # A faint vignette over everything, so the corners stay dark under the menus.
    vignette = Image.new("L", (small_w, small_h), 0)
    ImageDraw.Draw(vignette).ellipse((-small_w * 0.15, -small_h * 0.25, small_w * 1.15, small_h * 1.25), fill=255)
    vignette = vignette.filter(ImageFilter.GaussianBlur(small_h * 0.12)).resize((WIDTH, HEIGHT), Image.Resampling.BICUBIC)
    darkened = ImageChops.multiply(picture.convert("RGB"), Image.merge("RGB", [vignette] * 3))
    return Image.blend(picture.convert("RGB"), darkened, 0.35)


def from_picture(path: Path) -> Image.Image:
    """A picture of one's own, cropped to 16:9 from the middle and scaled to the home screen's size."""
    image = Image.open(path).convert("RGB")
    width, height = image.size
    if width * 9 > height * 16:
        crop_width = height * 16 // 9
        left = (width - crop_width) // 2
        image = image.crop((left, 0, left + crop_width, height))
    else:
        crop_height = width * 9 // 16
        top = (height - crop_height) // 2
        image = image.crop((0, top, width, top + crop_height))
    return image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def dds_bc7(image: Image.Image) -> bytes:
    """The picture as a DDS file: the classic header, the DX10 header and the BC7 blocks."""
    rgba = image.convert("RGBA")
    blocks = etcpak.compress_bc7(rgba.tobytes(), rgba.width, rgba.height)
    expected = (rgba.width // 4) * (rgba.height // 4) * 16
    if len(blocks) != expected:
        raise SystemExit(f"etcpak returned {len(blocks)} bytes of BC7, not {expected}")

    caps, height, width, pixel_format, linear_size, mip_map_count = 0x1, 0x2, 0x4, 0x1000, 0x80000, 0x20000
    flags = caps | height | width | pixel_format | linear_size | mip_map_count
    header = struct.pack(
        "<4sIIIIIII44x",
        b"DDS ", 124, flags, rgba.height, rgba.width, len(blocks), 0, 1,
    )
    four_cc = 0x4
    header += struct.pack("<II4s20x", 32, four_cc, b"DX10")
    texture = 0x1000
    header += struct.pack("<IIII4x", texture, 0, 0, 0)
    header += struct.pack("<IIIII", DXGI_FORMAT_BC7_UNORM, DIMENSION_TEXTURE2D, 0, 1, 0)
    assert len(header) == 148
    return header + blocks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--image", type=Path, help="a picture of your own, cropped to 16:9 from the middle")
    parser.add_argument("--png", type=Path, help="also write the picture as a PNG here")
    args = parser.parse_args()

    picture = from_picture(args.image) if args.image else composition()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    dds = dds_bc7(picture)
    for name in ("pic0.dds", "pic1.dds"):
        (args.out_dir / name).write_bytes(dds)
    if args.png:
        picture.save(args.png)
    print(f"wrote {args.out_dir / 'pic0.dds'} and pic1.dds ({len(dds)} bytes each)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
