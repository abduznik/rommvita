#!/usr/bin/env python3
"""Generates the app art (icon0, LiveArea bg/startup, in-app logo).
Needs inkscape + ImageMagick. A PS Vita drawn like the RomM logo (cropped silhouette, diagonal
split, peach/salmon buttons) in the exact navy/periwinkle palette of the Freegosy icon."""
import subprocess, os

OUT = os.path.dirname(os.path.abspath(__file__)) + "/.."
LAV, LAV2, PURPLE, DARK, PEACH, SALMON = "#7981a8", "#7981a8", "#244060", "#142b42", "#efc9a0", "#eda186"
SCREEN = "#7981a8"   # exact colours sampled from the Freegosy app icon

# Vita front view in its own coordinates: flat top, rounded belly, kept compact.
BODY = "M -300 104 H 318 C 346 104 352 128 380 128 C 428 128 452 152 452 198 C 452 272 425 352 350 352 H -300 Z"
SCALE, TX, TY = 1.35, -164.0, -81.0     # 

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
    <rect x="-300" y="124" width="610" height="204" rx="15" fill="{SCREEN}"/>
    <!-- face buttons: diamond -->
    <circle cx="378" cy="177" r="13.5" fill="{PEACH}"/><circle cx="354" cy="201" r="13.5" fill="{PEACH}"/>
    <circle cx="402" cy="201" r="13.5" fill="{SALMON}"/><circle cx="378" cy="225" r="13.5" fill="{SALMON}"/>
    <!-- analog stick with ring -->
    <circle cx="373" cy="278" r="28" fill="{LAV2}"/>
    <circle cx="373" cy="278" r="22.5" fill="{DARK}"/>
    <circle cx="373" cy="278" r="15" fill="{LAV2}"/>
  </g>'''

def svg_for(w, h):
    s = min(w, h) / 460
    ox, oy = (w - 460 * s) / 2, (h - 460 * s) / 2
    bg = "" if w == h else f'<rect width="{w}" height="{h}" fill="{DARK}"/>'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
  {bg}
  <svg x="{ox}" y="{oy}" width="{460*s}" height="{460*s}" viewBox="0 0 460 460">{tile(0)}</svg>
</svg>'''

def bg_svg(w, h):
    """Plain background for the LiveArea: navy, dark diagonal and a periwinkle band, no drawing."""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
  <rect width="{w}" height="{h}" fill="{PURPLE}"/>
  <polygon points="{w},0 {w},{h} 0,{h}" fill="{DARK}"/>
  <rect x="{w*0.82}" width="{w*0.18}" height="{h}" fill="{LAV}"/>
</svg>'''

def make(w, h, name, dest, ss=4, plain=False):
    sp, big = f"/tmp/claude-1000/art/{name}.svg", f"/tmp/claude-1000/art/{name}_big.png"
    os.makedirs("/tmp/claude-1000/art", exist_ok=True)
    open(sp, "w").write(bg_svg(w, h) if plain else svg_for(w, h))
    subprocess.run(["inkscape", sp, "-o", big, "-w", str(w * ss), "-h", str(h * ss)], check=True, capture_output=True)
    subprocess.run(["magick", big, "-filter", "Lanczos", "-resize", f"{w}x{h}!", "-background", LAV,
                    "-alpha", "remove", "-alpha", "off", "-colors", "256", "PNG8:" + dest], check=True)

os.makedirs(f"{OUT}/sce_sys/livearea/contents", exist_ok=True)
make(128, 128, "icon0", f"{OUT}/sce_sys/icon0.png")
make(840, 500, "bg", f"{OUT}/sce_sys/livearea/contents/bg.png", plain=True)
make(280, 158, "startup", f"{OUT}/sce_sys/livearea/contents/startup.png")
make(512, 512, "logo", f"{OUT}/assets/logo.png")
print("ok")
