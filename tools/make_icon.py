#!/usr/bin/env python3
# Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
"""Draws the minlsp app icon: an oscilloscope screen with a glowing 3:2
Lissajous figure over a harmonic spectrum, in the editor's colours.

    python3 tools/make_icon.py resources

writes resources/minlsp.png (1024 px), resources/about.png (256 px) and,
on macOS, resources/minlsp.icns. Needs numpy and Pillow; the results are
committed, so building minlsp does not need them.
"""
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 1024           # final size
K = 2              # supersampling
W = S * K

def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))

def squircle_mask(size, inset, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([inset, inset, size - inset, size - inset], radius, fill=255)
    return m

def main(out):
    inset, radius = 100 * K, 185 * K           # macOS icon grid: 824 px body
    body = squircle_mask(W, inset, radius)

    # background: deep indigo, lit from the top left
    yy, xx = np.mgrid[0:W, 0:W].astype(np.float32) / W
    t = np.clip(0.55 * xx + 0.75 * yy, 0, 1)
    top, bottom = np.array([34, 38, 70]), np.array([9, 10, 20])
    bg = top[None, None, :] * (1 - t[..., None]) + bottom[None, None, :] * t[..., None]
    glow = np.exp(-(((xx - 0.42) ** 2 + (yy - 0.40) ** 2) / 0.06))[..., None]
    bg = bg + glow * np.array([30, 40, 80])
    img = Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8), "RGB").convert("RGBA")

    # oscilloscope graticule
    grid = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    g = ImageDraw.Draw(grid)
    x0, x1 = inset + 70 * K, W - inset - 70 * K
    y0, y1 = inset + 70 * K, W - inset - 70 * K
    div = 8
    for i in range(div + 1):
        a = 70 if i in (0, div, div // 2) else 34
        x = x0 + (x1 - x0) * i / div
        y = y0 + (y1 - y0) * i / div
        g.line([(x, y0), (x, y1)], fill=(110, 170, 255, a), width=2 * K)
        g.line([(x0, y), (x1, y)], fill=(110, 170, 255, a), width=2 * K)
        for j in range(div * 5 + 1):                      # minor ticks on the axes
            if i == div // 2:
                tx = x0 + (x1 - x0) * j / (div * 5)
                ty = y0 + (y1 - y0) * j / (div * 5)
                g.line([(tx, y - 6 * K), (tx, y + 6 * K)], fill=(110, 170, 255, 60), width=K)
                g.line([(x - 6 * K, ty), (x + 6 * K, ty)], fill=(110, 170, 255, 60), width=K)
    img = Image.alpha_composite(img, grid)

    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    rx, ry = (x1 - x0) * 0.40, (y1 - y0) * 0.36

    # spectrum: harmonic partials with a formant-like envelope, rising from the floor
    spec = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    s = ImageDraw.Draw(spec)
    n = 28
    for k in range(n):
        f = k + 1
        env = np.exp(-((f - 4.0) / 5.5) ** 2) + 0.55 * np.exp(-((f - 15) / 4.0) ** 2) + 0.12
        h = (y1 - y0) * 0.30 * env / 1.15
        xk = x0 + (x1 - x0) * (k + 0.5) / n
        bw = (x1 - x0) / n * 0.46
        steps = 24
        for j in range(steps):                            # vertical fade
            yt = y1 - h * (j + 1) / steps
            yb = y1 - h * j / steps
            a = int(150 * (1 - j / steps) + 30)
            col = lerp((60, 200, 255), (150, 110, 255), j / steps)
            s.rectangle([xk - bw / 2, yt, xk + bw / 2, yb], fill=col + (a,))
    spec_glow = spec.filter(ImageFilter.GaussianBlur(10 * K))
    img = Image.alpha_composite(img, spec_glow)
    img = Image.alpha_composite(img, spec)

    # Lissajous 3:2 with a phase offset, a warm gradient along the curve
    tt = np.linspace(0, 2 * np.pi, 9000)
    px = cx + rx * np.sin(3 * tt)
    py = cy - (y1 - y0) * 0.05 - ry * np.sin(2 * tt)
    # the beam is drawn at twice the working size, then scaled down: smooth edges
    U = 2 * W
    curve = Image.new("RGBA", (U, U), (0, 0, 0, 0))
    core = Image.new("RGBA", (U, U), (0, 0, 0, 0))
    c, cc = ImageDraw.Draw(curve), ImageDraw.Draw(core)
    gold, orange, rose = (255, 214, 92), (255, 150, 60), (255, 96, 120)
    pts = [(2 * a, 2 * b) for a, b in zip(px, py)]
    for i in range(len(pts) - 1):
        u = (np.sin(tt[i]) + 1) / 2
        col = lerp(gold, orange, u / 0.7) if u < 0.7 else lerp(orange, rose, (u - 0.7) / 0.3)
        r1, r2 = 10 * K, 3 * K                          # round dabs along the path
        x, y = pts[i]
        c.ellipse([x - r1, y - r1, x + r1, y + r1], fill=col + (255,))
        cc.ellipse([x - r2, y - r2, x + r2, y + r2], fill=(255, 250, 235, 255))
    down = Image.Resampling.LANCZOS if hasattr(Image, "Resampling") else Image.LANCZOS
    curve = curve.resize((W, W), down)
    core = core.resize((W, W), down)
    core.putalpha(core.getchannel("A").point(lambda v: int(v * 0.8)))
    for blur, gain in ((44, 1.6), (18, 1.4), (6, 1.0)):   # phosphor glow
        halo = curve.filter(ImageFilter.GaussianBlur(blur * K))
        halo.putalpha(halo.getchannel("A").point(lambda v, g=gain: min(255, int(v * g))))
        img = Image.alpha_composite(img, halo)
    img = Image.alpha_composite(img, curve)
    img = Image.alpha_composite(img, core)

    # phosphor dot where the beam is
    beam = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    i = 5290
    bx, by = px[i], py[i]
    ImageDraw.Draw(beam).ellipse([bx - 22 * K, by - 22 * K, bx + 22 * K, by + 22 * K], fill=(255, 255, 240, 255))
    img = Image.alpha_composite(img, beam.filter(ImageFilter.GaussianBlur(18 * K)))
    img = Image.alpha_composite(img, beam.filter(ImageFilter.GaussianBlur(5 * K)))

    # glass: a soft highlight across the top, and a thin bright rim
    gl = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    ImageDraw.Draw(gl).ellipse([inset - 200 * K, inset - 520 * K, W - inset + 200 * K, inset + 300 * K],
                               fill=(255, 255, 255, 26))
    img = Image.alpha_composite(img, gl.filter(ImageFilter.GaussianBlur(30 * K)))
    rim = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle([inset + K, inset + K, W - inset - K, W - inset - K], radius,
                                          outline=(150, 180, 255, 90), width=4 * K)
    img = Image.alpha_composite(img, rim)

    # cut to the squircle and add a drop shadow
    img.putalpha(ImageChops.multiply(img.getchannel("A"), body))
    shadow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    sh = squircle_mask(W, inset, radius).filter(ImageFilter.GaussianBlur(22 * K))
    shadow.putalpha(sh.point(lambda v: int(v * 0.55)))
    shadow = ImageChops.offset(shadow, 0, 14 * K)
    final = Image.alpha_composite(shadow, img).resize((S, S), Image.Resampling.LANCZOS if hasattr(Image, 'Resampling') else Image.LANCZOS)

    os.makedirs(out, exist_ok=True)
    final.save(os.path.join(out, "minlsp.png"))
    final.resize((256, 256), Image.Resampling.LANCZOS if hasattr(Image, 'Resampling') else Image.LANCZOS).save(os.path.join(out, "about.png"))
    if sys.platform == "darwin":
        with tempfile.TemporaryDirectory() as tmp:
            iconset = os.path.join(tmp, "minlsp.iconset")
            os.makedirs(iconset)
            for size in (16, 32, 128, 256, 512):
                for scale in (1, 2):
                    px_ = size * scale
                    name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                    final.resize((px_, px_), Image.Resampling.LANCZOS if hasattr(Image, 'Resampling') else Image.LANCZOS).save(os.path.join(iconset, name))
            subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(out, "minlsp.icns")], check=True)

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "resources")
