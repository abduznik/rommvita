#!/usr/bin/env python3
"""Generates the app art (icon0, LiveArea bg/startup, in-app logo).
Needs inkscape + ImageMagick. A PS Vita drawn like the RomM logo (cropped silhouette, diagonal
split, peach/salmon buttons) in the navy/slate palette shared with Freegosy."""
import subprocess, os

OUT = os.path.dirname(os.path.abspath(__file__)) + "/.."
LAV, LAV2, PURPLE, DARK, PEACH, SALMON = "#a9aed0", "#7c82a9", "#24405f", "#162b45", "#f1cfa6", "#eba08c"
SCREEN = "#d6d9ec"

# Vita front view in its own coordinates: flat top, rounded belly, kept compact.
BODY = "M -300 112 H 380 C 425 112 452 140 452 190 C 452 270 425 352 350 352 H -300 Z"
SCALE, TX, TY = 1.4, -187.0, -87.0     # 

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
    <rect x="152" y="165" width="190" height="125" rx="14" fill="{SCREEN}"/>
    <circle cx="395" cy="176" r="15" fill="{PEACH}"/><circle cx="368" cy="203" r="15" fill="{PEACH}"/>
    <circle cx="422" cy="203" r="15" fill="{SALMON}"/><circle cx="395" cy="230" r="15" fill="{SALMON}"/>
    <circle cx="395" cy="290" r="19" fill="{LAV2}"/>
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
