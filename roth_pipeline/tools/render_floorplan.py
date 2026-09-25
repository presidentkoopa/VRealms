#!/usr/bin/env python3
"""
Render a top-down floorplan SVG from a parsed ROTH map (output of parse_raw.py).

Two jobs: let a human identify which sectors are which room, and independently
validate that the geometry parsed correctly — coherent architecture means the
vertex/face/sector cross-references resolved right.

Usage:
    python render_floorplan.py out/STUDY1.json -o out/STUDY1.svg [--no-labels]
"""

import argparse
import json
from pathlib import Path

WALL = "#1b1b1f"      # one-sided: solid wall
PORTAL = "#7bb0d8"    # two-sided: opening between sectors
OBJECT = "#e0603a"    # placed object
START = "#31c06a"     # player start


def render(m: dict, labels: bool = True) -> str:
    verts = m["verticesSection"]["vertices"]
    faces = m["facesSection"]["faces"]
    sectors = m["sectorsSection"]["sectors"]
    meta = m["mapMetadataSection"]

    xs = [v["x"] for v in verts]
    ys = [v["y"] for v in verts]
    pad = 200
    min_x, max_x = min(xs) - pad, max(xs) + pad
    min_y, max_y = min(ys) - pad, max(ys) + pad
    w, h = max_x - min_x, max_y - min_y

    # ROTH +Y is "north"; SVG +Y is down. Flip so the plan reads right way up.
    def px(x):
        return x - min_x

    def py(y):
        return max_y - y

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
        f'width="{w // 6}" height="{h // 6}">',
        f'<rect width="{w}" height="{h}" fill="#f4f1ea"/>',
        '<g stroke-linecap="round">',
    ]

    # Walls: draw portals first so solid walls sit on top
    for two_sided in (True, False):
        colour = PORTAL if two_sided else WALL
        width = 6 if two_sided else 14
        out.append(f'<g stroke="{colour}" stroke-width="{width}">')
        for f in faces:
            if ("sisterFaceIndex" in f) != two_sided:
                continue
            a, b = verts[f["vertexIndex01"]], verts[f["vertexIndex02"]]
            out.append(
                f'<line x1="{px(a["x"])}" y1="{py(a["y"])}" '
                f'x2="{px(b["x"])}" y2="{py(b["y"])}"/>'
            )
        out.append("</g>")
    out.append("</g>")

    # Objects — dense clusters are furnished rooms
    out.append(f'<g fill="{OBJECT}" fill-opacity="0.85">')
    for s in sectors:
        for o in s["objectInformation"]:
            out.append(f'<circle cx="{px(o["posX"])}" cy="{py(o["posY"])}" r="22"/>')
    out.append("</g>")

    # Player start
    out.append(
        f'<circle cx="{px(meta["initPosX"])}" cy="{py(meta["initPosY"])}" '
        f'r="60" fill="none" stroke="{START}" stroke-width="16"/>'
    )

    if labels:
        out.append('<g font-family="monospace" font-size="46" fill="#555" text-anchor="middle">')
        for i, s in enumerate(sectors):
            pts = []
            for j in range(s["facesCount"]):
                f = faces[s["firstFaceIndex"] + j]
                pts.append(verts[f["vertexIndex01"]])
                pts.append(verts[f["vertexIndex02"]])
            if not pts:
                continue
            cx = sum(p["x"] for p in pts) / len(pts)
            cy = sum(p["y"] for p in pts) / len(pts)
            out.append(f'<text x="{px(cx)}" y="{py(cy)}">{i}</text>')
        out.append("</g>")

    out.append("</svg>")
    return "\n".join(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("json", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--no-labels", action="store_true")
    args = ap.parse_args()

    m = json.loads(args.json.read_text())
    svg = render(m, labels=not args.no_labels)
    out = args.out or args.json.with_suffix(".svg")
    out.write_text(svg)
    print(f"wrote {out}  ({len(svg) // 1024} KB)")


if __name__ == "__main__":
    main()
