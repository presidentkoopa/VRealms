"""Rebuild vrealms.pk3 from vrealms_iwad/.

The pk3 is a plain zip and it GOES STALE: it is not rebuilt by the C++ build, so
a MAPINFO or palette change sits in the source tree while the game keeps loading
yesterday's package. That reads as "the change did nothing".

    python packiwad.py [src] [dest]

Defaults to vrealms_iwad/ -> build-dxr/RelWithDebInfo/vrealms.pk3.
"""

import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "vrealms_iwad")
dest = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    REPO, "build-dxr", "RelWithDebInfo", "vrealms.pk3")

if not os.path.isdir(src):
    sys.exit("no source directory: %s" % src)

n = 0
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(src):
        for f in sorted(files):
            p = os.path.join(d, f)
            arc = os.path.relpath(p, src).replace(os.sep, "/")
            z.write(p, arc)
            n += 1

print("packed %d lump(s) -> %s" % (n, dest))
