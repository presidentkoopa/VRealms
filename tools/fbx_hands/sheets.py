#!/usr/bin/env python3
"""Contact sheets of the 23 wired poses, rendered from the WRITTEN IQMs.

    python3 -I tools/fbx_hands/sheets.py [--rs /home/claude/rs_hands_ref] [--out tools/fbx_hands/sheets]

Skinning and the tile renderer are the owner's own tools, used as tools on our files:
RS_Hands tools/iqm_inspect.py (IQM.skin: CPU linear-blend skinning from the raw bytes) and
tools/render_wired_poses.py (draw: painter's algorithm, AZ 35 EL 18, one shared scale).

The view: our file frame is +X forward (fingers), +Y left, +Z back of hand. RS_Hands' file frame is
not that; the rotation that makes our open hand face the way the owner's tile 01 faces was found by
rendering all 24 axis permutations of our frame 0 and picking the one matching (fingers left, wrist
cut facing the viewer, thumb up, palm up): view = (y, -z, -x). The left hand goes through the same
hand is reflected back across the file's XZ plane, drawn with the same camera, and each finished tile
is flipped left-right: a true mirror image of the left hand, so its tiles are the mirror images of the
right sheet's and show the palm side, as the owner's tiles do.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

POSES = [
    (0, "POSE_OPEN"), (1, "POSE_POINT"), (2, "POSE_TRIGGER"), (3, "POSE_FIST"), (4, "POSE_PINCH"),
    (5, "POSE_THUMBOUT"), (6, "POSE_GRIPFIRE"), (7, "POSE_GRIP_TU"), (8, "POSE_READY_TD"),
    (9, "POSE_READY_TU"), (10, "POSE_FIRE_TU"), (513, "POSE_FIST_AUTHORED"),
    (1289, "POSE_HOLD_ROUND"), (1290, "POSE_HOLD_SHELL"), (1291, "POSE_INSERT"), (1292, "POSE_HOLD_SLIDE"),
    (1293, "POSE_HOLD_MAG"), (1294, "POSE_HOLD_FOREGRIP"), (1295, "POSE_HOLD_FOREND"), (1296, "POSE_REACH"),
    (1297, "POSE_SUPPORT"), (1298, "POSE_REACH_CLAW"), (1299, "POSE_SALUTE"),
]
TO_RS_VIEW = np.array([[0.0, 1.0, 0.0], [0.0, 0.0, -1.0], [-1.0, 0.0, 0.0]])   # det +1
TILE, COLS, AZ, EL = 420, 4, 35.0, 18.0


def font(sz):
    for n in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(n, sz)
        except OSError:
            pass
    return ImageFont.load_default()


def sheet(iqm_path, title, out_path, IQM, draw, rot, mirror_camera=False):
    m = IQM(iqm_path)
    V = TO_RS_VIEW
    if mirror_camera:
        V = TO_RS_VIEW @ np.diag([1.0, -1.0, 1.0])
    skins = [m.skin(fr) @ V.T for fr, _ in POSES]
    Ry, Rx = rot(AZ, EL)
    allp = np.concatenate([v @ Ry.T @ Rx.T for v in skins])
    mn, mx = allp.min(0), allp.max(0)
    centre, scale = (mn + mx) / 2, (mx - mn).max()
    pad, cap, head = 16, 40, 70
    rows = (len(POSES) + COLS - 1) // COLS
    W = COLS * TILE + pad * (COLS + 1)
    H = head + rows * (TILE + cap) + pad * (rows + 1)
    sh = Image.new("RGB", (W, H), (13, 15, 19))
    d = ImageDraw.Draw(sh)
    d.text((pad, 20), title, fill=(240, 244, 250), font=font(26))
    for i, ((fr, const), v) in enumerate(zip(POSES, skins)):
        img = draw(v, m.tris, TILE, AZ, EL, centre, scale, (18, 21, 26))
        if mirror_camera:
            img = ImageOps.mirror(img)
        di = ImageDraw.Draw(img)
        di.text((12, 10), "%02d" % (i + 1), fill=(255, 214, 120), font=font(26))
        di.text((TILE - 12, 14), "frame %d" % fr, fill=(120, 190, 255), font=font(18), anchor="ra")
        cx = pad + (i % COLS) * (TILE + pad)
        cy = head + pad + (i // COLS) * (TILE + cap + pad)
        sh.paste(img, (cx, cy))
        d.text((cx + 2, cy + TILE + 8), "%02d  %s" % (i + 1, const), fill=(235, 240, 248), font=font(20))
    sh.save(out_path)
    print("wrote", out_path, sh.size)
    return out_path


def side_by_side(a, b, out, la, lb):
    A, B = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    h = max(A.height, B.height)
    A = A.resize((int(A.width * h / A.height), h))
    B = B.resize((int(B.width * h / B.height), h))
    S = Image.new("RGB", (A.width + B.width + 20, h + 50), (0, 0, 0))
    S.paste(A, (0, 50)); S.paste(B, (A.width + 20, 50))
    d = ImageDraw.Draw(S)
    d.text((10, 10), la, fill=(255, 255, 255), font=font(28))
    d.text((A.width + 30, 10), lb, fill=(255, 255, 255), font=font(28))
    S.save(out)
    print("wrote", out, S.size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rs", default="/home/claude/rs_hands_ref")
    ap.add_argument("--models", default=os.path.join(REPO, "vrealms_iwad", "models", "hands"))
    ap.add_argument("--out", default=os.path.join(HERE, "sheets"))
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(a.rs, "tools"))
    from iqm_inspect import IQM                      # the owner's reader, used as a tool
    from render_wired_poses import draw, rot         # the owner's tile renderer, used as a tool
    os.makedirs(a.out, exist_ok=True)
    r = sheet(os.path.join(a.models, "hand_r.iqm"), "hand_r.iqm (FBX Hand_Low, right) -- the 23 wired poses",
              os.path.join(a.out, "hand_r_poses.png"), IQM, draw, rot)
    l = sheet(os.path.join(a.models, "hand_l.iqm"), "hand_l.iqm (FBX Hand_Low1, left) -- the 23 wired poses, seen in a mirror",
              os.path.join(a.out, "hand_l_poses.png"), IQM, draw, rot, mirror_camera=True)
    ref = os.path.join(a.rs, "docs", "images", "poses", "wired_poses_sheet.png")
    side_by_side(ref, r, os.path.join(a.out, "compare_right_vs_rs_hands.png"),
                 "OWNER: RS_Hands wired_poses_sheet.png", "OURS: hand_r.iqm")
    side_by_side(ref, l, os.path.join(a.out, "compare_left_vs_rs_hands.png"),
                 "OWNER: RS_Hands wired_poses_sheet.png", "OURS: hand_l.iqm (mirror image)")


if __name__ == "__main__":
    main()
