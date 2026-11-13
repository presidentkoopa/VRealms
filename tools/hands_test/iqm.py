"""Read an IQM file the way the engine does (src/common/models/models_iqm.cpp IQMModel::Load).

Only what the hand tests need: joints (bind TRS, parents), the per-frame joint TRS decoded from the
quantised channels exactly as Load() does (offset + uint16 * scale, quaternion made unit), the
global bind matrices (baseframe), vertex positions and blend data. Everything stays in the FILE's
own coordinates; the engine's (x, z, y) load swap is applied by the callers that need render space.

Standard library + numpy. Run with python3 -I.
"""
import struct

import numpy as np


def quat_to_mat(q):
    """VSMatrix::multQuaternion's 3x3 (matrix.cpp:138-151), q = (x, y, z, w)."""
    x, y, z, w = q
    return np.array([
        [1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * w * z, 2 * x * z + 2 * w * y],
        [2 * x * y + 2 * w * z, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * w * x],
        [2 * x * z - 2 * w * y, 2 * y * z + 2 * w * x, 1 - 2 * x * x - 2 * y * y]])


def trs_matrix(t, q, s):
    """m.translate(t); m.multQuaternion(q); m.scale(s) -- models_iqm.cpp:207-210."""
    m = np.eye(4)
    m[:3, :3] = quat_to_mat(q) @ np.diag(s)
    m[:3, 3] = t
    return m


def unit(q):
    q = np.asarray(q, dtype=float)
    n = np.linalg.norm(q)
    return q / n if n > 0 else q


class IQM:
    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:16] != b"INTERQUAKEMODEL\0":
            raise ValueError("not an IQM: %s" % path)
        hdr = struct.unpack_from("<27I", data, 16)
        (version, filesize, flags, num_text, ofs_text, num_meshes, ofs_meshes, num_va, num_vertices,
         ofs_va, num_triangles, ofs_triangles, ofs_adjacency, num_joints, ofs_joints, num_poses,
         ofs_poses, num_anims, ofs_anims, num_frames, num_framechannels, ofs_frames, ofs_bounds,
         num_comment, ofs_comment, num_ext, ofs_ext) = hdr
        if version != 2:
            raise ValueError("IQM version %d" % version)
        text = data[ofs_text:ofs_text + num_text]

        def name(o):
            e = text.index(b"\0", o)
            return text[o:e].decode("latin-1")

        self.meshes = []
        for i in range(num_meshes):
            n, mat, fv, nv, ft, nt = struct.unpack_from("<6I", data, ofs_meshes + 24 * i)
            self.meshes.append(dict(name=name(n), material=name(mat), first_vertex=fv, num_vertices=nv,
                                    first_triangle=ft, num_triangles=nt))
        self.triangles = np.frombuffer(data, dtype="<u4", count=3 * num_triangles,
                                       offset=ofs_triangles).reshape(-1, 3).astype(np.int64)
        self.joints = []
        for i in range(num_joints):
            vals = struct.unpack_from("<Ii10f", data, ofs_joints + 48 * i)
            self.joints.append(dict(name=name(vals[0]), parent=vals[1], t=np.array(vals[2:5]),
                                    q=unit(vals[5:9]), s=np.array(vals[9:12])))
        self.names = [j["name"] for j in self.joints]
        self.parents = [j["parent"] for j in self.joints]
        poses = []
        for i in range(num_poses):
            vals = struct.unpack_from("<iI20f", data, ofs_poses + 88 * i)
            poses.append((vals[0], vals[1], np.array(vals[2:12]), np.array(vals[12:22])))
        self.anims = []
        for i in range(num_anims):
            n, ff, nf, fr, fl = struct.unpack_from("<3IfI", data, ofs_anims + 20 * i)
            self.anims.append(dict(name=name(n), first=ff, count=nf, rate=fr, loop=bool(fl & 1)))

        # baseframe: global bind, file space (models_iqm.cpp:201-226)
        self.base = []
        for i, j in enumerate(self.joints):
            m = trs_matrix(j["t"], j["q"], j["s"])
            self.base.append(self.base[j["parent"]] @ m if j["parent"] >= 0 else m)
        self.base = np.array(self.base)

        # frames: models_iqm.cpp:228-258
        self.num_frames = num_frames
        frames = np.frombuffer(data, dtype="<u2", count=num_frames * num_framechannels, offset=ofs_frames)
        self.frame_t = np.zeros((num_frames, num_poses, 3))
        self.frame_q = np.zeros((num_frames, num_poses, 4))
        self.frame_s = np.zeros((num_frames, num_poses, 3))
        k = 0
        for f in range(num_frames):
            for p, (par, mask, off, sc) in enumerate(poses):
                ch = off.copy()
                for c in range(10):
                    if mask & (1 << c):
                        ch[c] += frames[k] * sc[c]
                        k += 1
                self.frame_t[f, p] = ch[0:3]
                self.frame_q[f, p] = unit(ch[3:7])
                self.frame_s[f, p] = ch[7:10]

        # vertex arrays: position (0), blend indexes (4), blend weights (5)
        self.positions = None
        self.blend_idx = None
        self.blend_w = None
        for i in range(num_va):
            typ, fl, fmt, size, off = struct.unpack_from("<5I", data, ofs_va + 20 * i)
            if typ == 0 and fmt == 7:
                self.positions = np.frombuffer(data, dtype="<f4", count=num_vertices * size,
                                               offset=off).reshape(-1, size).astype(float)
            elif typ == 4 and fmt == 1:
                self.blend_idx = np.frombuffer(data, dtype="u1", count=num_vertices * size,
                                               offset=off).reshape(-1, size).astype(np.int64)
            elif typ == 5 and fmt == 1:
                self.blend_w = np.frombuffer(data, dtype="u1", count=num_vertices * size,
                                             offset=off).reshape(-1, size).astype(float) / 255.0
            elif typ == 5 and fmt == 7:
                self.blend_w = np.frombuffer(data, dtype="<f4", count=num_vertices * size,
                                             offset=off).reshape(-1, size).astype(float)

    def joint(self, name):
        return self.names.index(name)

    def globals_from_locals(self, local_t, local_q, local_s):
        """Global joint matrices (file space) from per-joint local TRS -- the chain CalculateBones builds."""
        g = []
        for i, p in enumerate(self.parents):
            m = trs_matrix(local_t[i], local_q[i], local_s[i])
            g.append(g[p] @ m if p >= 0 else m)
        return np.array(g)

    def frame_globals(self, f):
        return self.globals_from_locals(self.frame_t[f], self.frame_q[f], self.frame_s[f])

    def skin(self, globals_):
        """Skinned vertex positions (file space): sum_w (G * invBase) v."""
        pal = np.array([globals_[i] @ np.linalg.inv(self.base[i]) for i in range(len(self.joints))])
        v = np.c_[self.positions[:, :3], np.ones(len(self.positions))]
        out = np.zeros((len(v), 4))
        for k in range(self.blend_idx.shape[1]):
            w = self.blend_w[:, k]
            out += w[:, None] * np.einsum("nij,nj->ni", pal[self.blend_idx[:, k]], v)
        return out[:, :3]
