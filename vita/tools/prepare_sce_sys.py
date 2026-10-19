#!/usr/bin/env python3
"""Vita icon/LiveArea image converter.

Converts source art in sce_sys_src/ (any PNG/JPG, truecolor OK) to the format the Vita requires and writes it to sce_sys/.
- Size: icon0 128x128, bg 840x500, startup 280x158 (resized with a warning if different)
- Format: 8-bit palette (256 colors) PNG. Truecolor PNGs make VPK install fail or the LiveArea show blank
- Transparent pixels are composited onto black
- Source art is made by tools/make_app_art.py (no game characters or logos). Per-game screen border art is not bundled with the app

Usage (Windows, needs Pillow: pip install pillow):
    python tools/prepare_sce_sys.py
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "sce_sys_src")

# (source name, output path, size)
TARGETS = [
    ("icon0.png", "sce_sys/icon0.png", (128, 128)),
    ("bg.png", "sce_sys/livearea/contents/bg.png", (840, 500)),
    ("startup.png", "sce_sys/livearea/contents/startup.png", (280, 158)),
]


def find_source(name):
    base = os.path.splitext(name)[0]
    for ext in (".png", ".jpg", ".jpeg"):
        p = os.path.join(SRC, base + ext)
        if os.path.exists(p):
            return p
    return None


def convert(src, dst, size):
    im = Image.open(src)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
        im = Image.alpha_composite(bg, im)
    im = im.convert("RGB")
    if im.size != size:
        print("  warning: %s size %dx%d -> resized to %dx%d" % (os.path.basename(src), im.size[0], im.size[1], size[0], size[1]))
        im = im.resize(size, Image.LANCZOS)
    pal = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    pal.save(dst, optimize=True)
    print("  %s -> %s (%dx%d, 8-bit palette, %d bytes)" % (os.path.relpath(src, ROOT), dst, size[0], size[1],
                                                     os.path.getsize(os.path.join(ROOT, dst))))


def main():
    os.chdir(ROOT)
    done = 0
    for name, dst, size in TARGETS:
        src = find_source(name)
        if not src:
            print("  skipped: sce_sys_src/%s missing (keeping existing %s)" % (name, dst))
            continue
        convert(src, dst, size)
        done += 1
    if done:
        print("Done. sce_sys changed, so reinstall the VPK to apply it (replacing eboot.bin alone is not enough).")


if __name__ == "__main__":
    main()
