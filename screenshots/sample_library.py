#!/usr/bin/env python3
"""Builds a library of sample manga archives with original art for the screenshots.

Usage: sample_library.py OUTPUT

Writes the art to OUTPUT/art and the archives to OUTPUT/documents. Rendering the art needs
rsvg-convert and the macOS system fonts.
"""
import math
import random
import subprocess
import sys
import zipfile
from pathlib import Path

W, H = 1060, 1500
LETTER = "Comic Sans MS"
TITLE = "Futura"


def defs():
    return """<defs>
  <pattern id="tl" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
    <circle cx="5" cy="5" r="1.5" fill="#000"/></pattern>
  <pattern id="tm" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
    <circle cx="5" cy="5" r="2.6" fill="#000"/></pattern>
  <pattern id="td" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
    <circle cx="5" cy="5" r="3.7" fill="#000"/></pattern>
  <pattern id="tw" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
    <rect width="10" height="10" fill="#000"/><circle cx="5" cy="5" r="2.2" fill="#fff"/></pattern>
  <linearGradient id="gdown" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient>
  <linearGradient id="gup" x1="0" y1="1" x2="0" y2="0">
    <stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient>
  <mask id="fadedown" maskContentUnits="objectBoundingBox"><rect width="1" height="1" fill="url(#gdown)"/></mask>
  <mask id="fadeup" maskContentUnits="objectBoundingBox"><rect width="1" height="1" fill="url(#gup)"/></mask>
</defs>"""


def svg(body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
            f'{defs()}<rect width="{W}" height="{H}" fill="#fff"/>{body}</svg>')


def esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, lines, size, family=LETTER, weight="bold", fill="#000", anchor="middle", extra="",
         leading=1.18):
    if isinstance(lines, str):
        lines = [lines]
    first = y - (len(lines) - 1) * size * leading / 2
    spans = "".join(f'<tspan x="{x}" y="{first + i * size * leading:.1f}">{esc(line)}</tspan>'
                    for i, line in enumerate(lines))
    return (f'<text font-family="{family}" font-size="{size}" font-weight="{weight}" fill="{fill}" '
            f'text-anchor="{anchor}" dominant-baseline="central" {extra}>{spans}</text>')


def points(pts):
    return " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)


def panel(index, x, y, w, h, content):
    rect = f'<rect x="{x}" y="{y}" width="{w}" height="{h}"'
    return (f'<clipPath id="p{index}">{rect}/></clipPath><g clip-path="url(#p{index})">'
            f'{rect} fill="#fff"/>{content}</g>{rect} fill="none" stroke="#000" stroke-width="5"/>')


def outlined(content, width=10):
    """Separates dark shapes from a dark background with a white outline."""
    return f'<g stroke="#fff" stroke-width="{width}" paint-order="stroke">{content}</g>{content}'


def bubble(cx, cy, rx, ry, lines, tail, size=30):
    tx, ty = tail
    angle = math.atan2(ty - cy, tx - cx)
    spread = 0.22
    base = [(cx + rx * 0.82 * math.cos(angle + d), cy + ry * 0.82 * math.sin(angle + d)) for d in (-spread, spread)]
    tail_pts = [base[0], (tx, ty), base[1]]
    return (f'<polygon points="{points(tail_pts)}" fill="#fff" stroke="#000" stroke-width="4" stroke-linejoin="round"/>'
            f'<ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="{ry}" fill="#fff" stroke="#000" stroke-width="4"/>'
            f'<polygon points="{points(tail_pts)}" fill="#fff"/>'
            + text(cx, cy, lines, size))


def caption(x, y, w, h, lines, size=24):
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="#fff" stroke="#000" stroke-width="3"/>'
            + text(x + w / 2, y + h / 2, lines, size, weight="normal"))


def skyline(x0, x1, base, rng, lo=60, hi=230, windows=True, fill="#000"):
    out = []
    x = x0
    while x < x1:
        w = rng.randint(38, 96)
        h = rng.randint(lo, hi)
        out.append(f'<rect x="{x}" y="{base - h}" width="{w + 1}" height="{h + 400}" fill="{fill}"/>')
        if rng.random() < 0.3:
            out.append(f'<rect x="{x + w / 2 - 2}" y="{base - h - 26}" width="4" height="26" fill="{fill}"/>')
        if windows:
            for wy in range(base - h + 14, base - 10, 22):
                for wx in range(x + 8, x + w - 10, 16):
                    if rng.random() < 0.28:
                        out.append(f'<rect x="{wx}" y="{wy}" width="7" height="10" fill="#fff"/>')
        x += w + rng.randint(0, 6)
    return "".join(out)


def train(x, y, cars, car_w=230, car_h=64, gap=10, body="#000", window="#fff", lit=0.85, rng=None):
    rng = rng or random.Random(1)
    out = []
    for c in range(cars):
        cx = x + c * (car_w + gap)
        r = 18 if c == 0 else 6
        out.append(f'<rect x="{cx}" y="{y}" width="{car_w}" height="{car_h}" rx="{r}" fill="{body}"/>')
        for wx in range(int(cx + 22), int(cx + car_w - 30), 34):
            if rng.random() < lit:
                out.append(f'<rect x="{wx}" y="{y + 14}" width="22" height="22" rx="3" fill="{window}"/>')
        out.append(f'<rect x="{cx + 6}" y="{y + car_h - 10}" width="{car_w - 12}" height="4" fill="{window}" opacity="0.6"/>')
    return "".join(out)


def gear(cx, cy, r, teeth, depth=0.16, hole=0.32):
    pts = []
    for i in range(teeth * 4):
        a = 2 * math.pi * i / (teeth * 4)
        rr = r if (i % 4) in (1, 2) else r * (1 - depth)
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts) + " Z"
    hr = r * hole
    d += f" M{cx + hr},{cy} A{hr},{hr} 0 1 0 {cx - hr},{cy} A{hr},{hr} 0 1 0 {cx + hr},{cy} Z"
    return d


def flower(cx, cy, r, petals=5, fill="#fff", stroke="#000"):
    out = []
    for i in range(petals):
        a = 2 * math.pi * i / petals
        px, py = cx + r * 0.9 * math.cos(a), cy + r * 0.9 * math.sin(a)
        out.append(f'<ellipse cx="{px:.1f}" cy="{py:.1f}" rx="{r * 0.62:.1f}" ry="{r * 0.4:.1f}" '
                   f'transform="rotate({math.degrees(a):.1f} {px:.1f} {py:.1f})" fill="{fill}" stroke="{stroke}" stroke-width="4"/>')
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{r * 0.38:.1f}" fill="url(#td)" stroke="{stroke}" stroke-width="4"/>')
    return "".join(out)


def kite(cx, cy, s, angle, fill="url(#tm)", tail_len=5):
    pts = [(0, -s), (s * 0.62, -s * 0.18), (0, s * 1.05), (-s * 0.62, -s * 0.18)]
    body = (f'<polygon points="{points(pts)}" fill="#fff" stroke="#000" stroke-width="5"/>'
            f'<polygon points="{points([pts[0], pts[1], (0, -s * 0.18)])}" fill="{fill}"/>'
            f'<polygon points="{points([(0, -s * 0.18), pts[3], pts[2]])}" fill="{fill}"/>'
            f'<line x1="0" y1="{-s}" x2="0" y2="{s * 1.05}" stroke="#000" stroke-width="4"/>'
            f'<line x1="{-s * 0.62}" y1="{-s * 0.18}" x2="{s * 0.62}" y2="{-s * 0.18}" stroke="#000" stroke-width="4"/>')
    tail = [f"M0,{s * 1.05}"]
    for i in range(1, 9):
        tail.append(f"Q{(-1) ** i * s * 0.25:.1f},{s * 1.05 + i * s * 0.22 - s * 0.11:.1f} 0,{s * 1.05 + i * s * 0.22:.1f}")
    bows = "".join(f'<polygon points="{points([(-s * 0.12, s * 1.05 + k * s * 0.42 - s * 0.06), (s * 0.12, s * 1.05 + k * s * 0.42 + s * 0.06), (s * 0.12, s * 1.05 + k * s * 0.42 - s * 0.06), (-s * 0.12, s * 1.05 + k * s * 0.42 + s * 0.06)])}" fill="#000"/>'
                   for k in range(1, tail_len))
    return (f'<g transform="translate({cx} {cy}) rotate({angle})">'
            f'<path d="{" ".join(tail)}" fill="none" stroke="#000" stroke-width="3"/>{bows}{body}</g>')


def focus_lines(cx, cy, rect, n, inner, rng, color="#000"):
    x, y, w, h = rect
    far = math.hypot(w, h)
    out = []
    for _ in range(n):
        a = rng.uniform(0, 2 * math.pi)
        r0 = inner * rng.uniform(1.0, 1.6)
        width = rng.uniform(0.006, 0.02)
        p0 = (cx + r0 * math.cos(a), cy + r0 * math.sin(a))
        p1 = (cx + far * math.cos(a - width), cy + far * math.sin(a - width))
        p2 = (cx + far * math.cos(a + width), cy + far * math.sin(a + width))
        out.append(f'<polygon points="{points([p0, p1, p2])}" fill="{color}"/>')
    return "".join(out)


def speed_lines(rect, n, rng, color="#000"):
    x, y, w, h = rect
    out = []
    for _ in range(n):
        ly = rng.uniform(y, y + h)
        length = rng.uniform(w * 0.25, w * 0.9)
        lx = rng.uniform(x - w * 0.1, x + w - length * 0.5)
        thick = rng.uniform(1.2, 4.5)
        out.append(f'<polygon points="{points([(lx, ly), (lx + length, ly - thick / 2), (lx + length, ly + thick / 2)])}" fill="{color}"/>')
    return "".join(out)


def seated_figure(x, y, s):
    """Silhouette of a seated passenger seen from the side, facing left. (x, y) is the seat point."""
    head = f'<circle cx="{x - 4 * s}" cy="{y - 205 * s}" r="{46 * s}"/>'
    hair = (f'<path d="M{x - 52 * s},{y - 214 * s} Q{x - 40 * s},{y - 268 * s} {x + 6 * s},{y - 256 * s} '
            f'Q{x + 50 * s},{y - 246 * s} {x + 46 * s},{y - 196 * s} L{x + 60 * s},{y - 168 * s} L{x + 30 * s},{y - 176 * s} '
            f'L{x + 34 * s},{y - 150 * s} L{x + 8 * s},{y - 168 * s} Z"/>')
    body = (f'<path d="M{x - 22 * s},{y - 166 * s} L{x + 20 * s},{y - 166 * s} Q{x + 66 * s},{y - 150 * s} {x + 70 * s},{y - 96 * s} '
            f'L{x + 74 * s},{y - 6 * s} L{x - 120 * s},{y - 4 * s} L{x - 132 * s},{y + 104 * s} L{x - 170 * s},{y + 104 * s} '
            f'L{x - 160 * s},{y - 34 * s} Q{x - 150 * s},{y - 52 * s} {x - 40 * s},{y - 50 * s} L{x - 52 * s},{y - 120 * s} '
            f'Q{x - 52 * s},{y - 158 * s} {x - 22 * s},{y - 166 * s} Z"/>')
    shapes = head + hair + body
    return f'<g fill="#fff" stroke="#fff" stroke-width="{10 * s}">{shapes}</g><g fill="#000">{shapes}</g>'


def standing_figure(x, y, s, strap=True):
    """Silhouette of a standing passenger, optionally holding a strap. (x, y) is between the feet."""
    head = f'<circle cx="{x}" cy="{y - 440 * s}" r="{44 * s}"/>'
    hair = (f'<path d="M{x - 47 * s},{y - 430 * s} Q{x - 52 * s},{y - 496 * s} {x},{y - 494 * s} Q{x + 54 * s},{y - 492 * s} '
            f'{x + 50 * s},{y - 424 * s} L{x + 40 * s},{y - 444 * s} L{x + 34 * s},{y - 418 * s} L{x + 22 * s},{y - 448 * s} Z"/>')
    coat = (f'<path d="M{x - 26 * s},{y - 400 * s} L{x + 26 * s},{y - 400 * s} Q{x + 78 * s},{y - 390 * s} {x + 82 * s},{y - 330 * s} '
            f'L{x + 96 * s},{y - 120 * s} L{x + 40 * s},{y - 110 * s} L{x + 34 * s},{y} L{x + 6 * s},{y} L{x},{y - 100 * s} '
            f'L{x - 6 * s},{y} L{x - 34 * s},{y} L{x - 40 * s},{y - 110 * s} L{x - 96 * s},{y - 120 * s} L{x - 82 * s},{y - 330 * s} '
            f'Q{x - 78 * s},{y - 390 * s} {x - 26 * s},{y - 400 * s} Z"/>')
    arm = (f'<path d="M{x + 60 * s},{y - 372 * s} L{x + 92 * s},{y - 520 * s} L{x + 118 * s},{y - 516 * s} '
           f'L{x + 90 * s},{y - 352 * s} Z"/>')
    strap_lines = (f'<line x1="{x + 104 * s}" y1="{y - 700 * s}" x2="{x + 104 * s}" y2="{y - 560 * s}" stroke="#000" stroke-width="{6 * s}"/>'
                   f'<circle cx="{x + 104 * s}" cy="{y - 538 * s}" r="{24 * s}" fill="none" stroke="#000" stroke-width="{8 * s}"/>')
    shapes = (arm if strap else "") + head + hair + coat
    return ((strap_lines if strap else "") + f'<g fill="#fff" stroke="#fff" stroke-width="{10 * s}">{shapes}</g>'
            f'<g fill="#000">{shapes}</g>')


def ticket(cx, cy, angle):
    return (f'<g transform="translate({cx} {cy}) rotate({angle})">'
            f'<rect x="-150" y="-86" width="300" height="172" rx="8" fill="#fff" stroke="#000" stroke-width="5"/>'
            f'<rect x="-136" y="-72" width="272" height="144" rx="4" fill="none" stroke="#000" stroke-width="2" stroke-dasharray="8 6"/>'
            f'<rect x="-136" y="-72" width="272" height="40" fill="#000"/>'
            + text(0, -52, "KASUMI LINE", 24, family=TITLE, fill="#fff", extra='font-stretch="condensed" letter-spacing="3"')
            + text(0, 4, "KASUMI → ? ? ?", 30)
            + text(0, 48, "No. 0417   23:58", 22, weight="normal")
            + '<circle cx="112" cy="40" r="14" fill="#000"/></g>')


def page_number(n):
    return text(W / 2, H - 34, str(n), 22, weight="normal")


def night_train_page_12(rng):
    m, g = 56, 22
    top, bottom = 64, H - 72
    p1 = (m, top, W - 2 * m, 420)
    p2y = top + 420 + g
    p2 = (m, p2y, 560, 470)
    p3 = (m + 560 + g, p2y, W - 2 * m - 560 - g, 470)
    p4y = p2y + 470 + g
    p4 = (m, p4y, W - 2 * m, bottom - p4y)

    x, y, w, h = p1
    c1 = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="url(#td)" mask="url(#fadedown)"/>'
          f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="url(#tl)"/>'
          f'<circle cx="{x + w - 170}" cy="{y + 120}" r="74" fill="#fff"/>'
          f'<circle cx="{x + w - 170}" cy="{y + 120}" r="74" fill="none" stroke="#000" stroke-width="3"/>'
          f'<circle cx="{x + w - 170}" cy="{y + 120}" r="92" fill="none" stroke="#fff" stroke-width="6" opacity="0.8"/>'
          + f'<rect x="{x}" y="{y + 300}" width="{w}" height="16" fill="#000"/>'
          + "".join(f'<rect x="{px}" y="{y + 300}" width="18" height="140" fill="#000"/>' for px in range(x + 40, x + w, 150))
          + outlined(train(x + 180, y + 232, 3, rng=rng))
          + skyline(x - 10, x + w + 10, y + h + 4, rng, lo=40, hi=95)
          + text(x + 300, y + 168, "KA-TUNK", 74, family="Marker Felt", weight="normal", fill="#fff",
                 extra=f'stroke="#000" stroke-width="5" paint-order="stroke" transform="rotate(-9 {x + 300} {y + 168})"')
          + text(x + 640, y + 214, "KA-TUNK", 48, family="Marker Felt", weight="normal", fill="#fff",
                 extra=f'stroke="#000" stroke-width="4" paint-order="stroke" transform="rotate(-6 {x + 640} {y + 214})"')
          + caption(x + 20, y + 20, 330, 80, ["23:58. The last train", "out of Kasumi Station."]))

    x, y, w, h = p2
    c2 = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="url(#tl)"/>'
          f'<rect x="{x + 40}" y="{y + 40}" width="{w - 80}" height="200" fill="#000"/>'
          + speed_lines((x + 40, y + 50, w - 80, 180), 34, rng, color="#fff")
          + f'<rect x="{x + 40}" y="{y + 40}" width="{w - 80}" height="200" fill="none" stroke="#000" stroke-width="14"/>'
          f'<line x1="{x + w / 2}" y1="{y + 40}" x2="{x + w / 2}" y2="{y + 240}" stroke="#000" stroke-width="12"/>'
          f'<rect x="{x}" y="{y + 330}" width="{w}" height="40" fill="url(#td)"/>'
          f'<rect x="{x}" y="{y + 366}" width="{w}" height="120" fill="#000"/>'
          + seated_figure(x + 360, y + 352, 1.05)
          + bubble(x + 150, y + 250, 120, 78, ["IS THIS...", "THE LAST", "TRAIN?"], (x + 290, y + 150 + 8), size=26))

    x, y, w, h = p3
    c3 = (focus_lines(x + w / 2, y + h / 2, p3, 150, 120, rng)
          + ticket(x + w / 2, y + h / 2 + 10, -12)
          + text(x + w - 60, y + 60, "?", 64, family="Marker Felt", weight="normal"))

    x, y, w, h = p4
    c4 = (speed_lines((x, y, w, h), 120, rng)
          + f'<rect x="{x + 600}" y="{y}" width="{w - 600}" height="{h}" fill="url(#tm)"/>'
          + f'<rect x="{x + 650}" y="{y + 40}" width="270" height="250" fill="#fff" stroke="#000" stroke-width="10"/>'
          + skyline(x + 650, x + 920, y + 290, random.Random(7), lo=40, hi=150, windows=False, fill="url(#td)")
          + f'<rect x="{x}" y="{y + 18}" width="{w}" height="10" fill="#000"/>'
          + standing_figure(x + 800, y + h + 30, 0.78)
          + bubble(x + 300, y + 150, 210, 96, ["NOBODY EVER GETS", "OFF AT THE NEXT", "STOP, KID."], (x + 640, y + 120), size=30)
          + caption(x + 30, y + h - 96, 300, 66, ["Next stop: Hollow Hill."]))

    body = (panel(1, *p1, c1) + panel(2, *p2, c2) + panel(3, *p3, c3) + panel(4, *p4, c4) + page_number(12))
    return svg(body)


def night_train_page_47(rng):
    m, g = 56, 22
    top, bottom = 64, H - 72
    p1 = (m, top, W - 2 * m, 900)
    p2y = top + 900 + g
    p2 = (m, p2y, 440, bottom - p2y)
    p3 = (m + 440 + g, p2y, W - 2 * m - 440 - g, bottom - p2y)

    x, y, w, h = p1
    river_y = y + 520
    deck_y = river_y + 120
    arches = "".join(
        f'<path d="M{ax},{deck_y + 20} L{ax + 170},{deck_y + 20} L{ax + 170},{y + h} L{ax + 146},{y + h} '
        f'L{ax + 146},{deck_y + 150} Q{ax + 85},{deck_y + 44} {ax + 24},{deck_y + 150} L{ax + 24},{y + h} L{ax},{y + h} Z" fill="#000"/>'
        for ax in range(x - 60, x + w, 170))
    c1 = (f'<rect x="{x}" y="{y}" width="{w}" height="{river_y - y}" fill="url(#tm)" mask="url(#fadedown)"/>'
          f'<rect x="{x}" y="{y}" width="{w}" height="{river_y - y}" fill="url(#tl)"/>'
          f'<circle cx="{x + 300}" cy="{y + 250}" r="190" fill="#fff"/>'
          f'<circle cx="{x + 300}" cy="{y + 250}" r="190" fill="none" stroke="#000" stroke-width="3"/>'
          + "".join(f'<circle cx="{rng.randint(x, x + w)}" cy="{rng.randint(y, y + 300)}" r="{rng.choice([2, 3, 4])}" fill="#fff"/>' for _ in range(40))
          + skyline(x - 10, x + w + 10, river_y - 14, rng, lo=24, hi=110, windows=False, fill="#000")
          + f'<rect x="{x}" y="{river_y - 14}" width="{w}" height="14" fill="#000"/>'
          + f'<rect x="{x}" y="{river_y}" width="{w}" height="{h}" fill="url(#tw)"/>'
          + "".join(f'<rect x="{x + 300 - rw / 2:.0f}" y="{river_y + 14 + i * 24}" width="{rw:.0f}" height="6" fill="#fff"/>'
                    for i, rw in enumerate([320, 260, 300, 200, 240, 140, 180, 90, 120, 60, 80, 40, 50]))
          + f'<rect x="{x}" y="{deck_y}" width="{w}" height="24" fill="#000"/>'
          + f'<rect x="{x}" y="{deck_y - 34}" width="{w}" height="6" fill="#000"/>'
          + "".join(f'<rect x="{px}" y="{deck_y - 34}" width="6" height="34" fill="#000"/>' for px in range(x, x + w, 40))
          + arches
          + outlined(train(x + 300, deck_y - 70, 4, car_w=170, car_h=66, rng=rng))
          + caption(x + w - 380, y + 30, 350, 80, ["Under a full moon, it", "crossed the river alone."]))

    x, y, w, h = p2
    c2 = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="url(#tm)"/>'
          f'<rect x="{x + 40}" y="{y + 110}" width="{w - 80}" height="150" fill="#fff" stroke="#000" stroke-width="8"/>'
          + text(x + w / 2, y + 170, "HOLLOW HILL", 52, family=TITLE, extra='font-stretch="condensed"')
          + text(x + 70, y + 226, "← KASUMI", 18, weight="normal", anchor="start")
          + text(x + w - 70, y + 226, "NOWHERE →", 18, weight="normal", anchor="end")
          + f'<rect x="{x + 70}" y="{y + 260}" width="14" height="{h}" fill="#000"/>'
          + f'<rect x="{x + w - 84}" y="{y + 260}" width="14" height="{h}" fill="#000"/>')

    x, y, w, h = p3
    c3 = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="#000"/>'
          + focus_lines(x + w / 2, y + h / 2, p3, 90, 150, rng, color="#fff")
          + f'<rect x="{x + w / 2 - 170}" y="{y + h / 2 - 70}" width="340" height="140" rx="12" fill="#fff" stroke="#000" stroke-width="6"/>'
          + text(x + w / 2, y + h / 2, "23:59", 96, family="Avenir Next Condensed", weight="900")
          + text(x + w - 90, y + 70, "TIK", 44, family="Marker Felt", weight="normal", fill="#fff"))

    body = panel(1, *p1, c1) + panel(2, *p2, c2) + panel(3, *p3, c3) + page_number(47)
    return svg(body)


def cover(title_lines, volume, art, dark=False, subtitle="", title_size=150):
    bg = "#000" if dark else "#fff"
    fg = "#fff" if dark else "#000"
    title_block = text(W / 2, 250, title_lines, title_size, family=TITLE, fill=fg,
                       extra=f'font-stretch="condensed" stroke="{bg}" stroke-width="10" paint-order="stroke" letter-spacing="2"',
                       leading=0.98)
    vol = (f'<circle cx="{W - 140}" cy="{H - 150}" r="92" fill="{fg}"/>'
           + text(W - 140, H - 178, "VOL.", 30, family=TITLE, fill=bg)
           + text(W - 140, H - 132, str(volume), 84, family=TITLE, fill=bg, extra='font-stretch="condensed"'))
    sub = (text(70, H - 120, subtitle, 34, family="Avenir Next Condensed", weight="bold", fill=fg, anchor="start",
                extra=f'stroke="{bg}" stroke-width="10" paint-order="stroke"') if subtitle else "")
    frame = f'<rect x="28" y="28" width="{W - 56}" height="{H - 56}" fill="none" stroke="{fg}" stroke-width="6"/>'
    return svg(f'<rect width="{W}" height="{H}" fill="{bg}"/>{art}{frame}{title_block}{sub}{vol}')


def cover_iron_garden():
    art = (f'<rect y="{H * 0.42}" width="{W}" height="{H}" fill="url(#tl)"/>'
           f'<path d="{gear(760, 900, 420, 14)}" fill="#000" fill-rule="evenodd"/>'
           f'<path d="{gear(250, 1180, 230, 10)}" fill="url(#td)" stroke="#000" stroke-width="8" fill-rule="evenodd"/>'
           + "".join(f'<path d="M{fx},{H} C{fx - 40},{fy + 260} {fx + 60},{fy + 120} {fx},{fy}" fill="none" stroke="#000" stroke-width="9"/>'
                     f'<ellipse cx="{fx + 34}" cy="{fy + 150}" rx="34" ry="14" transform="rotate(-30 {fx + 34} {fy + 150})" fill="#000"/>'
                     + flower(fx, fy, 52) for fx, fy in [(160, 760), (420, 640), (560, 980), (900, 560)]))
    return cover(["IRON", "GARDEN"], 3, art, subtitle="Where the machines bloom")


def cover_night_train(rng, volume):
    if volume == 1:
        art = (f'<rect width="{W}" height="{H}" fill="url(#tw)" mask="url(#fadeup)"/>'
               f'<circle cx="720" cy="660" r="230" fill="#fff"/>'
               + "".join(f'<circle cx="{rng.randint(40, W - 40)}" cy="{rng.randint(420, 900)}" r="{rng.choice([2, 3, 4])}" fill="#fff"/>' for _ in range(50))
               + skyline(-10, W + 10, 1130, rng, lo=60, hi=260, fill="#000")
               + f'<rect y="1000" width="{W}" height="26" fill="#000"/>'
               + "".join(f'<rect x="{px}" y="1000" width="26" height="400" fill="#000"/>' for px in range(30, W, 180))
               + train(-60, 924, 5, car_w=250, car_h=78, rng=rng)
               + f'<rect y="1130" width="{W}" height="{H}" fill="#000"/>')
        return cover(["NIGHT", "TRAIN"], 1, art, dark=True, subtitle="The last train out of Kasumi")
    vx, vy = 640, 900
    rails = "".join(f'<polygon points="{points([(vx + dx * 9 - 9, H), (vx + dx, vy), (vx + dx + 2, vy), (vx + dx * 9 + 9, H)])}" fill="#fff"/>'
                    for dx in (-14, 14))
    ties = "".join(f'<rect x="{vx - 14 * (1 + t * 8) - 20 * t}" y="{vy + t * 600}" width="{28 * (1 + t * 8) + 40 * t}" height="{3 + t * 18}" fill="#fff"/>'
                   for t in [i / 10 for i in range(1, 11)])
    art = (f'<rect width="{W}" height="{H}" fill="url(#td)" mask="url(#fadedown)"/>'
           f'<rect width="{W}" height="{H}" fill="url(#tl)"/>'
           f'<polygon points="{points([(vx, vy), (W + 40, 600), (W + 40, 1240)])}" fill="#fff"/>'
           f'<circle cx="{vx}" cy="{vy}" r="34" fill="#fff"/>'
           f'<rect x="0" y="{vy}" width="{W}" height="{H}" fill="#000"/>' + rails + ties
           + f'<polygon points="{points([(0, 1010), (420, 960), (300, H), (0, H)])}" fill="#fff" stroke="#000" stroke-width="6"/>'
           + f'<polygon points="{points([(0, 1010), (420, 960), (300, H), (0, H)])}" fill="url(#tm)"/>'
           + f'<line x1="0" y1="1030" x2="410" y2="980" stroke="#fff" stroke-width="10"/>'
           + standing_figure(180, 1250, 0.62, strap=False))
    return cover(["NIGHT", "TRAIN"], 2, art, subtitle="Hollow Hill")


def cover_paper_kites(rng, volume):
    if volume == 2:
        art = (f'<rect width="{W}" height="{H}" fill="url(#tl)"/>'
               + "".join(f'<ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="{rx * 0.4}" fill="#fff"/>' for cx, cy, rx in
                         [(200, 560, 160), (330, 590, 120), (820, 860, 200), (700, 890, 120), (380, 1200, 220)])
               + "".join(f'<line x1="{kx}" y1="{ky}" x2="{W * 0.5}" y2="{H + 40}" stroke="#000" stroke-width="2"/>'
                         for kx, ky in [(260, 760), (640, 600), (850, 1080)])
               + kite(260, 760, 120, -14) + kite(640, 600, 150, 10, fill="url(#td)") + kite(850, 1080, 90, 22))
        return cover(["PAPER", "KITES"], 2, art, subtitle="Summer on the river")
    art = (f'<rect width="{W}" height="{H}" fill="#000"/>'
           + focus_lines(W / 2, 900, (0, 0, W, H), 120, 380, rng, color="#fff")
           + kite(W / 2, 820, 320, -8, fill="url(#tw)", tail_len=3))
    return cover(["PAPER", "KITES"], 3, art, dark=True, subtitle="The wind turns")


def cover_small_hours(rng):
    cx, cy, r = W / 2, 900, 330
    ticks = "".join(
        f'<line x1="{cx + (r - (40 if i % 5 == 0 else 20)) * math.cos(math.radians(i * 6)):.1f}" '
        f'y1="{cy + (r - (40 if i % 5 == 0 else 20)) * math.sin(math.radians(i * 6)):.1f}" '
        f'x2="{cx + (r - 6) * math.cos(math.radians(i * 6)):.1f}" y2="{cy + (r - 6) * math.sin(math.radians(i * 6)):.1f}" '
        f'stroke="#000" stroke-width="{10 if i % 5 == 0 else 4}"/>' for i in range(60))

    def hand(angle, length, width):
        a = math.radians(angle - 90)
        return (f'<line x1="{cx}" y1="{cy}" x2="{cx + length * math.cos(a):.1f}" y2="{cy + length * math.sin(a):.1f}" '
                f'stroke="#000" stroke-width="{width}" stroke-linecap="round"/>')
    art = (f'<rect width="{W}" height="{H}" fill="url(#td)"/>'
           + "".join(f'<circle cx="{rng.randint(40, W - 40)}" cy="{rng.randint(420, H - 40)}" r="{rng.choice([3, 4, 6])}" fill="#fff"/>' for _ in range(60))
           + f'<circle cx="{cx}" cy="{cy}" r="{r + 24}" fill="#000"/><circle cx="{cx}" cy="{cy}" r="{r}" fill="#fff"/>'
           + f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#tl)"/>' + ticks
           + hand(3 * 30 + 3.5, 190, 22) + hand(7 * 6, 280, 12) + f'<circle cx="{cx}" cy="{cy}" r="22" fill="#000"/>'
           + f'<path d="M{W - 230},{430} a120,120 0 1,0 120,150 a96,96 0 1,1 -120,-150 Z" fill="#fff"/>')
    return cover(["SMALL", "HOURS"], 1, art, dark=True, subtitle="Stories after midnight")


def cover_summit(rng):
    layers = []
    for i, (fill, base) in enumerate([("url(#tl)", 760), ("url(#tm)", 900), ("url(#td)", 1060), ("#000", 1220)]):
        pts = [(-20, H)]
        x = -20
        while x < W + 40:
            pts.append((x, base - rng.randint(0, 260 - i * 40)))
            x += rng.randint(90, 200)
        pts.append((W + 40, H))
        layers.append(f'<polygon points="{points(pts)}" fill="#fff"/><polygon points="{points(pts)}" fill="{fill}" stroke="#000" stroke-width="5"/>')
    art = (f'<circle cx="300" cy="620" r="120" fill="none" stroke="#000" stroke-width="6"/>' + "".join(layers))
    return cover(["SUMMIT"], 4, art, subtitle="Above the clouds", title_size=170)


def cover_tidewater():
    waves = []
    for i in range(9):
        y0 = 760 + i * 90
        d = f"M-20,{y0}" + " q37,-60 75,0 t75,0" * 8 + f" L{W + 20},{H} L-20,{H} Z"
        waves.append(f'<path d="{d}" fill="{["url(#tl)", "url(#tm)", "url(#td)"][i % 3]}" stroke="#000" stroke-width="5"/>')
    lx, top, base = 790, 540, 830
    tower = [(lx - 66, base), (lx - 38, top), (lx + 38, top), (lx + 66, base)]
    stripes = "".join(f'<rect x="{lx - 80}" y="{top + k * 58 + 29}" width="160" height="29" fill="#000"/>' for k in range(5))
    art = (f'<polygon points="{points([(lx, top - 50), (40, top - 190), (40, top + 60)])}" fill="url(#tl)"/>'
           f'<clipPath id="tower"><polygon points="{points(tower)}"/></clipPath>'
           f'<polygon points="{points(tower)}" fill="#fff"/><g clip-path="url(#tower)">{stripes}</g>'
           f'<polygon points="{points(tower)}" fill="none" stroke="#000" stroke-width="8"/>'
           f'<rect x="{lx - 54}" y="{top - 10}" width="108" height="14" fill="#000"/>'
           f'<rect x="{lx - 34}" y="{top - 76}" width="68" height="66" fill="#fff" stroke="#000" stroke-width="8"/>'
           f'<rect x="{lx - 18}" y="{top - 62}" width="36" height="38" fill="#000"/>'
           f'<polygon points="{points([(lx - 48, top - 76), (lx, top - 118), (lx + 48, top - 76)])}" fill="#000"/>'
           f'<path d="M{lx - 200},{base + 20} Q{lx - 120},{base - 70} {lx},{base - 60} Q{lx + 140},{base - 70} {lx + 220},{base + 20} Z" fill="#000"/>'
           + "".join(waves))
    return cover(["TIDE", "WATER"], 1, art, subtitle="The lighthouse keeper")


def blank_page():
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{W // 10}" height="{H // 10}"><rect width="100%" height="100%" fill="#fff"/></svg>'


def art():
    rng = random.Random(2026)
    return {
        "iron-garden-3": cover_iron_garden(),
        "night-train-1": cover_night_train(rng, 1),
        "night-train-2": cover_night_train(rng, 2),
        "paper-kites-2": cover_paper_kites(rng, 2),
        "paper-kites-3": cover_paper_kites(rng, 3),
        "small-hours-1": cover_small_hours(rng),
        "summit-4": cover_summit(rng),
        "tidewater-1": cover_tidewater(),
        "night-train-1-p12": night_train_page_12(random.Random(12)),
        "night-train-1-p47": night_train_page_47(random.Random(47)),
        "blank": blank_page(),
    }


# Archive names, the art of their covers and drawn pages by page number, and their page counts.
# Other pages are blank. The screenshots read pages 12 and 47 of Night Train 01.
ARCHIVES = [
    ("Iron Garden 03.cbz", "iron-garden-3", {}, 24),
    ("Night Train 01.cbz", "night-train-1", {12: "night-train-1-p12", 47: "night-train-1-p47"}, 192),
    ("Night Train 02.cbz", "night-train-2", {}, 24),
    ("Paper Kites 02.cbz", "paper-kites-2", {}, 24),
    ("Paper Kites 03.cbz", "paper-kites-3", {}, 24),
    ("Small Hours 01.cbz", "small-hours-1", {}, 24),
    ("Summit 04.cbz", "summit-4", {}, 24),
    ("Tidewater 01.cbz", "tidewater-1", {}, 24),
]


def main(output):
    output = Path(output)
    art_dir = output / "art"
    library = output / "documents"
    art_dir.mkdir(parents=True, exist_ok=True)
    library.mkdir(parents=True, exist_ok=True)
    images = {}
    for name, content in art().items():
        svg_file = art_dir / f"{name}.svg"
        png_file = art_dir / f"{name}.png"
        svg_file.write_text(content)
        subprocess.run(["rsvg-convert", str(svg_file), "-o", str(png_file)], check=True)
        images[name] = png_file.read_bytes()
    for archive, cover_art, pages, count in ARCHIVES:
        with zipfile.ZipFile(library / archive, "w", zipfile.ZIP_STORED) as book:
            for number in range(1, count + 1):
                name = cover_art if number == 1 else pages.get(number, "blank")
                book.writestr(f"p{number:03d}.png", images[name])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: sample_library.py OUTPUT")
    main(sys.argv[1])
