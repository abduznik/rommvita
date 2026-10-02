#!/usr/bin/env python3
"""Generates the app art (icon0, LiveArea bg/startup, in-app logo).
Needs inkscape + ImageMagick. A PS Vita drawn like the RomM logo: a big, half
cropped silhouette on lavender, split diagonally into two purples, with the
peach/salmon face buttons."""
import subprocess, os

OUT = os.path.dirname(os.path.abspath(__file__)) + "/.."
LAV, LAV2, PURPLE, DARK, PEACH, SALMON = "#ede4f1", "#bda5cf", "#553f99", "#38286a", "#e5c7a7", "#e1a38e"

# Vita front view in its own coordinates: flat top, rounded belly, kept compact.
BODY = "M 140 128 H 385 C 428 128 452 152 452 195 C 452 262 428 338 355 338 H 170 C 100 338 70 262 70 195 C 70 152 96 128 140 128 Z"
SCALE, TX, TY = 1.5, -232.0, -132.0     # right half of the Vita, bleeding off the left edge

def to_art(x, y):   # canvas point -> Vita coordinates (for the diagonal split)
    return (x - TX) / SCALE, (y - TY) / SCALE

def tile(size_px):
    """The square logo tile, drawn in a 460x460 space."""
    d1, d2, d3 = to_art(460, 0), to_art(460, 460), to_art(0, 460)
    return f'''
  <rect width="460" height="460" fill="{LAV}"/>
  <polygon points="460,0 460,460 0,460" fill="{LAV2}"/>
  <g transform="translate({TX},{TY}) scale({SCALE})">
    <clipPath id="body"><path d="{BODY}"/></clipPath>
    <g clip-path="url(#body)">
      <rect x="-300" y="-300" width="1200" height="1200" fill="{PURPLE}"/>
      <polygon points="{d1[0]:.1f},{d1[1]:.1f} {d2[0]:.1f},{d2[1]:.1f} {d3[0]:.1f},{d3[1]:.1f}" fill="{DARK}"/>
    </g>
    <rect x="128" y="158" width="214" height="132" rx="14" fill="{LAV}"/>
    <circle cx="395" cy="178" r="15" fill="{PEACH}"/><circle cx="368" cy="205" r="15" fill="{PEACH}"/>
    <circle cx="422" cy="205" r="15" fill="{SALMON}"/><circle cx="395" cy="232" r="15" fill="{SALMON}"/>
    <circle cx="395" cy="292" r="19" fill="{LAV2}"/>
  </g>'''

def svg_for(w, h):
    s = min(w, h) / 460
    ox, oy = (w - 460 * s) / 2, (h - 460 * s) / 2
    bg = "" if w == h else f'<rect width="{w}" height="{h}" fill="{DARK}"/>'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
  {bg}
  <svg x="{ox}" y="{oy}" width="{460*s}" height="{460*s}" viewBox="0 0 460 460">{tile(0)}</svg>
</svg>'''

def make(w, h, name, dest, ss=4):
    sp, big = f"/tmp/claude-1000/art/{name}.svg", f"/tmp/claude-1000/art/{name}_big.png"
    os.makedirs("/tmp/claude-1000/art", exist_ok=True)
    open(sp, "w").write(svg_for(w, h))
    subprocess.run(["inkscape", sp, "-o", big, "-w", str(w * ss), "-h", str(h * ss)], check=True, capture_output=True)
    subprocess.run(["magick", big, "-filter", "Lanczos", "-resize", f"{w}x{h}!", "-background", LAV,
                    "-alpha", "remove", "-alpha", "off", "-colors", "256", "PNG8:" + dest], check=True)

os.makedirs(f"{OUT}/sce_sys/livearea/contents", exist_ok=True)
make(128, 128, "icon0", f"{OUT}/sce_sys/icon0.png")
make(840, 500, "bg", f"{OUT}/sce_sys/livearea/contents/bg.png")
make(280, 158, "startup", f"{OUT}/sce_sys/livearea/contents/startup.png")
make(512, 512, "logo", f"{OUT}/assets/logo.png")
print("ok")
