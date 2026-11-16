"""trellis2_run.py -- the FALLBACK backend's one item, run by generate.py --backend trellis2 with the TRELLIS.2 build's
own python, in its code folder (so `trellis2` and `o_voxel` import from there).

    <TRELLIS.2 python> trellis2_run.py <input.png> <out.glb> <faces> <seed>

Follows TRELLIS.2's README example (github.com/microsoft/TRELLIS.2: Trellis2ImageTo3DPipeline.from_pretrained
("microsoft/TRELLIS.2-4B"), pipeline.run(image)[0], mesh.simplify(16777216), o_voxel.postprocess.to_glb(...,
decimation_target, texture_size), glb.export(path, extension_webp=...)). The to_glb arguments other than
decimation_target and texture_size are the README's as remembered, NOT checked against a running build: if this
fails, compare with that build's own example.py first (CLAUDE.md porting rule 4).
"""
import sys

import torch
from PIL import Image


def main():
    src, out, faces, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    from trellis2.pipelines import Trellis2ImageTo3DPipeline
    import o_voxel
    torch.manual_seed(seed)
    pipe = Trellis2ImageTo3DPipeline.from_pretrained("microsoft/TRELLIS.2-4B")
    pipe.cuda()
    mesh = pipe.run(Image.open(src).convert("RGBA"))[0]
    mesh.simplify(16777216)   # the README's nvdiffrast limit
    glb = o_voxel.postprocess.to_glb(
        vertices=mesh.vertices, faces=mesh.faces, attr_volume=mesh.attrs, coords=mesh.coords,
        attr_layout=mesh.layout, voxel_size=mesh.voxel_size, aabb=[[-0.5, -0.5, -0.5], [0.5, 0.5, 0.5]],
        decimation_target=faces, texture_size=1024, remesh=True, remesh_band=1, remesh_project=0, verbose=True)
    glb.export(out, extension_webp=False)   # PNG textures: the engine's reader decodes PNG/JPG, not WebP
    return 0


if __name__ == "__main__":
    sys.exit(main())
