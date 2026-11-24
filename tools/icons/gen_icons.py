#!/usr/bin/env python3
"""Generate 4-bpp anti-aliased UI icons from MIT/ISC Lucide SVGs."""

import argparse
import math
import os
import re
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

SVG_TOKEN_RE = re.compile(r"[A-Za-z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def svg_arc_to_points(x0, y0, rx, ry, phi_deg, fa, fs, x1, y1, steps=48):
    """Sample an SVG elliptical arc (W3C F.6.5/F.6.6) into polyline points."""
    if x0 == x1 and y0 == y1:
        return []
    rx = abs(rx)
    ry = abs(ry)
    if rx == 0 or ry == 0:
        return [(x1, y1)]

    phi = math.radians(phi_deg % 360.0)
    cos_p = math.cos(phi)
    sin_p = math.sin(phi)

    dx2 = (x0 - x1) / 2.0
    dy2 = (y0 - y1) / 2.0
    x1p = cos_p * dx2 + sin_p * dy2
    y1p = -sin_p * dx2 + cos_p * dy2

    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1.0:
        s = math.sqrt(lam)
        rx *= s
        ry *= s

    rx2, ry2 = rx * rx, ry * ry
    x1p2, y1p2 = x1p * x1p, y1p * y1p
    num = max(0.0, rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2)
    den = rx2 * y1p2 + ry2 * x1p2
    factor = math.sqrt(num / den) if den > 1e-12 else 0.0
    if fa == fs:
        factor = -factor

    cxp = factor * (rx * y1p) / ry
    cyp = factor * (-ry * x1p) / rx

    cx = cos_p * cxp - sin_p * cyp + (x0 + x1) / 2.0
    cy = sin_p * cxp + cos_p * cyp + (y0 + y1) / 2.0

    def vec_angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        mag = math.hypot(ux, uy) * math.hypot(vx, vy)
        ang = math.acos(max(-1.0, min(1.0, dot / mag))) if mag > 1e-12 else 0.0
        if ux * vy - uy * vx < 0:
            ang = -ang
        return ang

    theta1 = vec_angle(1.0, 0.0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dtheta = vec_angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if fs == 0 and dtheta > 0:
        dtheta -= 2.0 * math.pi
    elif fs == 1 and dtheta < 0:
        dtheta += 2.0 * math.pi

    pts = []
    for i in range(1, steps + 1):
        t = theta1 + dtheta * (i / steps)
        px = cx + cos_p * rx * math.cos(t) - sin_p * ry * math.sin(t)
        py = cy + sin_p * rx * math.cos(t) + cos_p * ry * math.sin(t)
        pts.append((px, py))
    return pts


def parse_path_subpaths(d_str):
    tokens = SVG_TOKEN_RE.findall(d_str)
    idx = 0
    cmd = ""
    cx = cy = 0.0
    sx = sy = 0.0
    subpaths = []
    cur = []

    def next_float():
        nonlocal idx
        v = float(tokens[idx])
        idx += 1
        return v

    def has_num():
        return idx < len(tokens) and not tokens[idx].isalpha()

    while idx < len(tokens):
        if tokens[idx].isalpha():
            cmd = tokens[idx]
            idx += 1
            if cmd in ("Z", "z"):
                if cur:
                    cur.append((sx, sy))
                    subpaths.append((cur, True))
                    cur = []
                cx, cy = sx, sy
                continue
        if cmd in ("M", "m"):
            rel = cmd == "m"
            nx = next_float() + (cx if rel else 0.0)
            ny = next_float() + (cy if rel else 0.0)
            if cur:
                subpaths.append((cur, False))
            cur = [(nx, ny)]
            cx, cy = nx, ny
            sx, sy = nx, ny
            cmd = "l" if rel else "L"
            while has_num():
                nx = next_float() + (cx if rel else 0.0)
                ny = next_float() + (cy if rel else 0.0)
                cur.append((nx, ny))
                cx, cy = nx, ny
        elif cmd in ("L", "l"):
            rel = cmd == "l"
            while has_num():
                nx = next_float() + (cx if rel else 0.0)
                ny = next_float() + (cy if rel else 0.0)
                cur.append((nx, ny))
                cx, cy = nx, ny
        elif cmd in ("H", "h"):
            rel = cmd == "h"
            while has_num():
                nx = next_float() + (cx if rel else 0.0)
                cur.append((nx, cy))
                cx = nx
        elif cmd in ("V", "v"):
            rel = cmd == "v"
            while has_num():
                ny = next_float() + (cy if rel else 0.0)
                cur.append((cx, ny))
                cy = ny
        elif cmd in ("A", "a"):
            rel = cmd == "a"
            while has_num():
                rx = next_float()
                ry = next_float()
                rot = next_float()
                fa = int(next_float())
                fs = int(next_float())
                nx = next_float() + (cx if rel else 0.0)
                ny = next_float() + (cy if rel else 0.0)
                cur.extend(svg_arc_to_points(cx, cy, rx, ry, rot, fa, fs, nx, ny))
                cx, cy = nx, ny
        else:
            raise ValueError(f"Unsupported SVG path command: {cmd}")
    if cur:
        subpaths.append((cur, False))
    return subpaths


def rect_to_subpaths(x, y, w, h, rx):
    rx = min(rx, w / 2.0, h / 2.0)
    if rx <= 0:
        return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)], True)]
    pts = []
    # Top edge -> TR arc -> Right edge -> BR arc -> Bottom -> BL arc -> Left -> TL arc
    pts.append((x + rx, y))
    pts.append((x + w - rx, y))
    pts.extend(svg_arc_to_points(x + w - rx, y, rx, rx, 0, 0, 1, x + w, y + rx, 16))
    pts.append((x + w, y + h - rx))
    pts.extend(svg_arc_to_points(x + w, y + h - rx, rx, rx, 0, 0, 1, x + w - rx, y + h, 16))
    pts.append((x + rx, y + h))
    pts.extend(svg_arc_to_points(x + rx, y + h, rx, rx, 0, 0, 1, x, y + h - rx, 16))
    pts.append((x, y + rx))
    pts.extend(svg_arc_to_points(x, y + rx, rx, rx, 0, 0, 1, x + rx, y, 16))
    return [(pts, True)]


def dist_point_to_segment(px, py, ax, ay, bx, by):
    vx = bx - ax
    vy = by - ay
    wx = px - ax
    wy = py - ay
    c1 = vx * wx + vy * wy
    if c1 <= 0.0:
        return math.hypot(px - ax, py - ay)
    c2 = vx * vx + vy * vy
    if c2 <= c1:
        return math.hypot(px - bx, py - by)
    t = c1 / c2
    return math.hypot(px - (ax + t * vx), py - (ay + t * vy))


def point_in_poly(px, py, poly):
    inside = False
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if ((y1 > py) != (y2 > py)) and (px < (x2 - x1) * (py - y1) / (y2 - y1 + 1e-18) + x1):
            inside = not inside
    return inside


def load_svg_shapes(svg_path):
    tree = ET.parse(svg_path)
    root = tree.getroot()
    shapes = []
    for elem in root:
        tag = elem.tag.split("}")[-1]
        if tag == "path":
            for pts, closed in parse_path_subpaths(elem.attrib["d"]):
                shapes.append((pts, closed))
        elif tag == "rect":
            x = float(elem.attrib.get("x", 0))
            y = float(elem.attrib.get("y", 0))
            w = float(elem.attrib["width"])
            h = float(elem.attrib["height"])
            rx = float(elem.attrib.get("rx", 0))
            shapes.extend(rect_to_subpaths(x, y, w, h, rx))
    return shapes


def rasterize_icon(svg_path, size_px, stroke_width=2.0, fill_interior=False, pad_px=0.5, gamma=0.88):
    shapes = load_svg_shapes(svg_path)
    # Map [0, 24] SVG viewBox into [pad_px, size_px - pad_px]
    inner = size_px - 2.0 * pad_px
    scale = inner / 24.0
    half_r = (stroke_width * 0.5) * scale

    scaled_shapes = []
    for pts, closed in shapes:
        spts = [(pad_px + x * scale, pad_px + y * scale) for (x, y) in pts]
        scaled_shapes.append((spts, closed))

    sub_n = 5
    inv_sub = 1.0 / sub_n
    samples = sub_n * sub_n
    nibbles = []

    for py in range(size_px):
        for px in range(size_px):
            cov = 0
            for sy in range(sub_n):
                uy = py + (sy + 0.5) * inv_sub
                for sx in range(sub_n):
                    ux = px + (sx + 0.5) * inv_sub
                    hit = False
                    for spts, closed in scaled_shapes:
                        if fill_interior and closed and point_in_poly(ux, uy, spts):
                            hit = True
                            break
                        for i in range(len(spts) - 1):
                            ax, ay = spts[i]
                            bx, by = spts[i + 1]
                            if dist_point_to_segment(ux, uy, ax, ay, bx, by) <= half_r:
                                hit = True
                                break
                        if hit:
                            break
                    if hit:
                        cov += 1
            if cov == 0:
                q = 0
            else:
                alpha = cov / samples
                v = alpha ** gamma
                q = min(15, max(1, int(round(v * 15.0))))
            nibbles.append(q)

    packed = bytearray()
    for i in range(0, len(nibbles), 2):
        hi = nibbles[i]
        lo = nibbles[i + 1] if (i + 1) < len(nibbles) else 0
        packed.append((hi << 4) | lo)
    return nibbles, packed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="Optional preview PNG path")
    args = parser.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
    svg_dir = os.path.join(root, "tools/icons/lucide")
    out_h = os.path.join(root, "src/ui/icons.h")
    out_cpp = os.path.join(root, "src/ui/icons_data.cpp")

    # (C symbol, svg basename, size px, stroke width, fill interior, pad px)
    specs = [
        ("kIconMic18",          "mic.svg",           18, 2.2, False, 0.5),
        ("kIconCheck12",        "check.svg",         12, 2.6, False, 0.5),
        ("kIconX12",            "x.svg",             12, 2.4, False, 0.5),
        ("kIconPlay12",         "play.svg",          12, 1.8, True,  0.8),
        ("kIconChevronRight12", "chevron-right.svg", 12, 2.4, False, 0.5),
        ("kIconCircleDashed14", "circle-dashed.svg", 14, 2.2, False, 0.5),
    ]

    h_lines = [
        "#pragma once",
        "#include <stdint.h>",
        "",
        "// 4-bpp anti-aliased UI icons generated from MIT/ISC Lucide SVGs (tools/icons/gen_icons.py).",
        "// Packed 2 pixels per byte: high nibble = even x, low nibble = odd x.",
        "struct AaIcon {",
        "  uint8_t        w;",
        "  uint8_t        h;",
        "  const uint8_t* data;",
        "};",
        "",
    ]
    for sym, *_ in specs:
        h_lines.append(f"extern const AaIcon {sym};")
    h_lines.append("")

    cpp_lines = [
        "/* Auto-generated by tools/icons/gen_icons.py from Lucide (ISC/MIT) — do not edit manually */",
        '#include "ui/icons.h"',
        "",
    ]

    previews = []
    for sym, fname, sz, sw, fill, pad in specs:
        nibbles, packed = rasterize_icon(os.path.join(svg_dir, fname), sz, sw, fill, pad)
        arr_name = f"kData_{sym}"
        cpp_lines.append(f"static const uint8_t {arr_name}[] = {{")
        for i in range(0, len(packed), 18):
            chunk = ",".join(f"0x{b:02X}" for b in packed[i:i + 18])
            cpp_lines.append(f"  {chunk},")
        cpp_lines.append("};")
        cpp_lines.append(f"const AaIcon {sym} = {{ {sz}u, {sz}u, {arr_name} }};")
        cpp_lines.append("")
        previews.append((sym, sz, nibbles))
        print(f"{sym}: {sz}x{sz} ({len(packed)} B)")

    with open(out_h, "w", encoding="utf-8") as f:
        f.write("\n".join(h_lines))
    with open(out_cpp, "w", encoding="utf-8") as f:
        f.write("\n".join(cpp_lines))

    if args.preview:
        zoom = 6
        cols = len(previews)
        sheet = Image.new("RGB", (cols * 140 + 20, 160), (12, 18, 30))
        draw = ImageDraw.Draw(sheet)
        for idx, (sym, sz, nibs) in enumerate(previews):
            ox = 20 + idx * 140
            # 1x preview
            for y in range(sz):
                for x in range(sz):
                    a = nibs[y * sz + x] / 15.0
                    c = (int(12 + (72 - 12) * a), int(18 + (198 - 18) * a), int(30 + (255 - 30) * a))
                    sheet.putpixel((ox + x, 20 + y), c)
            # 6x zoomed preview
            for y in range(sz):
                for x in range(sz):
                    a = nibs[y * sz + x] / 15.0
                    c = (int(12 + (242 - 12) * a), int(18 + (246 - 18) * a), int(30 + (255 - 30) * a))
                    draw.rectangle([ox + x * zoom, 48 + y * zoom, ox + (x + 1) * zoom - 1, 48 + (y + 1) * zoom - 1], fill=c)
        sheet.save(args.preview)
        print(f"Saved preview -> {args.preview}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
