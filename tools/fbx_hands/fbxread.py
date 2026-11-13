#!/usr/bin/env python3
"""Minimal binary FBX reader (standard library only).

Binary FBX layout (Autodesk FBX 7.x, as documented by the Blender FBX importer
and https://code.blender.org/2013/08/fbx-binary-file-format-specification/):
  header: b"Kaydara FBX Binary  \\x00\\x1a\\x00", uint32 version
  node record: endOffset, numProperties, propertyListLen (uint32, or uint64 when
               version >= 7500), uint8 nameLen, name, properties, nested records,
               then a null record (13 or 25 zero bytes) if there are nested records.
  property: 1-byte type code
    Y i16, C bool(u8), I i32, F f32, D f64, L i64
    f d l i b : arrays -> uint32 arrayLength, uint32 encoding (1 = zlib), uint32 compressedLength
    S R : uint32 length, raw bytes

Usage as a library: root = read_fbx(path); nodes are Node(name, props, children).
"""
import struct
import sys
import zlib
from array import array

MAGIC = b"Kaydara FBX Binary  \x00\x1a\x00"

_ARR = {"f": ("f", 4), "d": ("d", 8), "l": ("q", 8), "i": ("i", 4), "b": ("b", 1)}


class Node:
    __slots__ = ("name", "props", "children")

    def __init__(self, name, props, children):
        self.name = name
        self.props = props
        self.children = children

    def find(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def findall(self, name):
        return [c for c in self.children if c.name == name]

    def __repr__(self):
        return "Node(%r, %r, %d children)" % (self.name, [p if not isinstance(p, array) else "array[%d]" % len(p) for p in self.props][:6], len(self.children))


def _read_prop(buf, off):
    t = chr(buf[off]); off += 1
    if t == "Y":
        return struct.unpack_from("<h", buf, off)[0], off + 2
    if t == "C":
        return bool(buf[off]), off + 1
    if t == "I":
        return struct.unpack_from("<i", buf, off)[0], off + 4
    if t == "F":
        return struct.unpack_from("<f", buf, off)[0], off + 4
    if t == "D":
        return struct.unpack_from("<d", buf, off)[0], off + 8
    if t == "L":
        return struct.unpack_from("<q", buf, off)[0], off + 8
    if t in _ARR:
        n, enc, clen = struct.unpack_from("<III", buf, off); off += 12
        raw = bytes(buf[off:off + clen]); off += clen
        if enc == 1:
            raw = zlib.decompress(raw)
        code, size = _ARR[t]
        if len(raw) != n * size:
            raise ValueError("array size mismatch: %d elems of %d bytes vs %d bytes" % (n, size, len(raw)))
        a = array(code)
        a.frombytes(raw)
        if sys.byteorder != "little":
            a.byteswap()
        return a, off
    if t in ("S", "R"):
        n = struct.unpack_from("<I", buf, off)[0]; off += 4
        raw = bytes(buf[off:off + n]); off += n
        if t == "S":
            # FBX joins "Name\x00\x01Class" for object names
            return raw.decode("utf-8", "replace"), off
        return raw, off
    raise ValueError("unknown property type %r at %d" % (t, off - 1))


def _read_node(buf, off, wide):
    if wide:
        end, nprops, plen = struct.unpack_from("<QQQ", buf, off); off += 24
    else:
        end, nprops, plen = struct.unpack_from("<III", buf, off); off += 12
    nlen = buf[off]; off += 1
    if end == 0:
        return None, off
    name = bytes(buf[off:off + nlen]).decode("ascii"); off += nlen
    props = []
    pend = off + plen
    for _ in range(nprops):
        p, off = _read_prop(buf, off)
        props.append(p)
    if off != pend:
        raise ValueError("property list length mismatch in %s" % name)
    children = []
    sentinel = 25 if wide else 13
    while off < end:
        if end - off == sentinel and not any(buf[off:end]):
            off = end
            break
        child, off = _read_node(buf, off, wide)
        if child is None:
            break
        children.append(child)
    if off != end:
        raise ValueError("node %s end mismatch: %d vs %d" % (name, off, end))
    return Node(name, props, children), end


def read_fbx(path):
    with open(path, "rb") as fh:
        buf = memoryview(fh.read())
    if bytes(buf[:23]) != MAGIC:
        raise ValueError("not a binary FBX (ASCII FBX not supported)")
    version = struct.unpack_from("<I", buf, 23)[0]
    wide = version >= 7500
    off = 27
    top = []
    while off < len(buf):
        node, off = _read_node(buf, off, wide)
        if node is None:
            break
        top.append(node)
    return version, Node("", [], top)


def props70(node):
    """Return {name: (type, flags, values...)} from a node's Properties70 child."""
    out = {}
    p70 = node.find("Properties70") if node is not None else None
    if p70 is None:
        return out
    for p in p70.findall("P"):
        out[p.props[0]] = tuple(p.props[1:])
    return out


def split_name(s):
    """FBX object names are 'Name\\x00\\x01Class'."""
    if "\x00\x01" in s:
        a, b = s.split("\x00\x01", 1)
        return a, b
    return s, ""


if __name__ == "__main__":
    ver, root = read_fbx(sys.argv[1])
    print("FBX version", ver)

    def dump(n, d, maxd):
        if d > maxd:
            return
        ps = []
        for p in n.props[:5]:
            if isinstance(p, array):
                ps.append("array[%d]" % len(p))
            elif isinstance(p, bytes):
                ps.append("raw[%d]" % len(p))
            else:
                ps.append(repr(p).replace("\\x00\\x01", "::"))
        print("  " * d + n.name, " ".join(ps))
        for c in n.children:
            dump(c, d + 1, maxd)

    maxd = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    for n in root.children:
        dump(n, 0, maxd)
