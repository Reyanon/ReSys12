#!/usr/bin/env python3
"""App icon and LiveArea pictures drawn from shapes and text (no game characters, logos or trademarks).

Same layout as ReiREM and ReST-V (based on ReST-V tools/make_app_art.py) with a red theme:
"Re" in white, "Sys12" in red, a red underline, a dark red gradient background, a perspective grid and scanlines.

Output (sources):
- vita/sce_sys_src/icon0.png (128x128), bg.png (840x500), startup.png (280x158)
  -> python vita/tools/prepare_sce_sys.py writes them to vita/sce_sys/ in the Vita format (8-bit palette)
- art_psd/*.psd: the same pictures split into layers (background, shadow, Re, Sys12, underline, subtitle) for
  editing (only when psd-tools is installed; not committed)
Usage (Pillow, optionally psd-tools): python tools/make_app_art.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME_RE, NAME_SYS = "Re", "Sys12"
BG0 = (26, 6, 8)        # top
BG1 = (60, 12, 16)      # bottom
GRID = (150, 36, 42)
ACCENT = (255, 52, 52)  # Sys12, underline
TEXT = (255, 255, 255)  # Re
SUB = (244, 206, 206)

FONT_CANDIDATES = [
    "C:/Windows/Fonts/ariblk.ttf", "C:/Windows/Fonts/arialbd.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]


def font(size):
    for f in FONT_CANDIDATES:
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def background(w, h):
    im = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):  # vertical gradient
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(BG0[i] * (1 - t) + BG1[i] * t) for i in range(3)))
    horizon = int(h * 0.62)  # perspective grid (lower part)
    for k in range(1, 12):
        y = horizon + int((h - horizon) * (k / 11) ** 1.8)
        d.line([(0, y), (w, y)], fill=GRID)
    for k in range(-12, 13):
        x0 = w / 2 + k * w / 24
        d.line([(w / 2 + k * w / 200, horizon), (x0 * 1.0 + k * w / 10, h)], fill=GRID)
    d.line([(0, horizon), (w, horizon)], fill=ACCENT, width=max(1, h // 120))
    for y in range(0, h, 6):  # scanlines
        d.line([(0, y), (w, y)], fill=(0, 0, 0), width=1)
    return im


class Art:
    """Layer list: (name, full-size RGBA). PNG files are flattened, PSD files keep the layers."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.layers = [("Background", background(w, h))]

    def layer(self, name):
        im = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        self.layers.append((name, im))
        return ImageDraw.Draw(im)

    def flat(self):
        out = self.layers[0][1].copy()
        for _, im in self.layers[1:]:
            out.alpha_composite(im)
        return out.convert("RGB")

    def save(self, png, psd_name):
        self.flat().save(png)
        try:
            from psd_tools import PSDImage
            from psd_tools.api.layers import PixelLayer
        except ImportError:
            return
        pd = os.path.join(ROOT, "art_psd")
        os.makedirs(pd, exist_ok=True)
        psd = PSDImage.new("RGB", (self.w, self.h))
        for name, im in self.layers:
            psd.append(PixelLayer.frompil(im, psd, name, 0, 0))
        psd.save(os.path.join(pd, psd_name))


def ink(text, f):
    """Ink box of the text (x0, y0, x1, y1) relative to the origin."""
    return ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=f)


def shadowed(art, name, xy, text, f, fill):
    sd = art.layer(name + " shadow")
    for dx, dy in ((3, 3), (2, 2)):
        sd.text((xy[0] + dx, xy[1] + dy), text, font=f, fill=(0, 0, 0, 255))
    art.layer(name).text(xy, text, font=f, fill=fill)


def icon(art, size_top, size_bot, gap):
    """Re (white) above Sys12 (red). The ink box of both lines and the underline is centred in the picture."""
    ft, fb = font(size_top), font(size_bot)
    bt, bb = ink(NAME_RE, ft), ink(NAME_SYS, fb)
    lw = max(2, size_top // 12)
    th = bt[3] - bt[1]
    bh = bb[3] - bb[1]
    total = th + gap + lw + gap + bh
    top = (art.h - total) / 2
    xt = (art.w - (bt[2] - bt[0])) / 2 - bt[0]
    shadowed(art, NAME_RE, (xt, top - bt[1]), NAME_RE, ft, TEXT)
    ul = top + th + gap
    art.layer("Underline").line([(xt + bt[0], ul + lw / 2), (xt + bt[2], ul + lw / 2)], fill=ACCENT, width=lw)
    xb = (art.w - (bb[2] - bb[0])) / 2 - bb[0]
    shadowed(art, NAME_SYS, (xb, ul + lw + gap - bb[1]), NAME_SYS, fb, ACCENT)


def banner(art, size, cy, sub=None, sub_size=0):
    """One line "ReSys12" (Re white + Sys12 red), underline, subtitle. The text ink is centred on cy."""
    title = NAME_RE + NAME_SYS
    f = font(size)
    w1 = ImageDraw.Draw(Image.new("L", (1, 1))).textlength(NAME_RE, font=f)
    b = ink(title, f)
    x = (art.w - (b[2] - b[0])) / 2 - b[0]
    y = cy - (b[1] + b[3]) / 2
    sd = art.layer("Title shadow")
    for dx, dy in ((3, 3), (2, 2)):
        sd.text((x + dx, y + dy), title, font=f, fill=(0, 0, 0, 255))
    art.layer(NAME_RE).text((x, y), NAME_RE, font=f, fill=TEXT)
    art.layer(NAME_SYS).text((x + w1, y), NAME_SYS, font=f, fill=ACCENT)
    lw = max(2, size // 12)
    uy = y + b[3] + max(3, size // 10)
    art.layer("Underline").line([(x + b[0], uy), (x + b[2], uy)], fill=ACCENT, width=lw)
    if sub:
        fs = font(sub_size)
        sb = ink(sub, fs)
        art.layer("Subtitle").text(((art.w - (sb[2] - sb[0])) / 2 - sb[0], uy + lw + max(4, size // 8) - sb[1]),
                                   sub, font=fs, fill=SUB)


def main():
    vs = os.path.join(ROOT, "vita", "sce_sys_src")
    os.makedirs(vs, exist_ok=True)
    a = Art(128, 128)
    icon(a, 50, 28, 6)
    a.save(os.path.join(vs, "icon0.png"), "vita_icon0.psd")
    a = Art(840, 500)
    banner(a, 120, 150, "arcade emulator", 34)
    a.save(os.path.join(vs, "bg.png"), "vita_bg.psd")
    a = Art(280, 158)
    banner(a, 46, 62, "START", 20)
    a.save(os.path.join(vs, "startup.png"), "vita_startup.psd")

    print("ok")


if __name__ == "__main__":
    main()
