#!/usr/bin/env python3
"""
Parse a Realms of the Haunting .RAW map into JSON.

Port of roth-editor's src/resources/parsers/raw.gd (+ parser.gd), which is the
verified reference for this format. Ported rather than reused because roth-editor
is a Godot 4.6 project and Godot isn't a dependency we want in this pipeline.

Reads the player's own game files. Ships nothing, bundles nothing.

Usage:
    python parse_raw.py <MAP.RAW> [-o out.json] [--summary]
"""

import argparse
import json
import struct
import sys
from pathlib import Path

U8, I8, U16, I16, CHAR = "u8", "i8", "u16", "i16", "char"


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def seek(self, pos: int) -> None:
        self.pos = pos

    def tell(self) -> int:
        return self.pos

    def eof(self) -> bool:
        return self.pos >= len(self.data)

    def u8(self) -> int:
        v = self.data[self.pos]
        self.pos += 1
        return v

    def i8(self) -> int:
        v = struct.unpack_from("<b", self.data, self.pos)[0]
        self.pos += 1
        return v

    def u16(self) -> int:
        v = struct.unpack_from("<H", self.data, self.pos)[0]
        self.pos += 2
        return v

    def i16(self) -> int:
        v = struct.unpack_from("<h", self.data, self.pos)[0]
        self.pos += 2
        return v

    def read(self, kind):
        if isinstance(kind, list):  # array of CHAR -> string
            out = ""
            for k in kind:
                c = self.read(k)
                if c != 0:
                    out += chr(c)
            return out
        return {U8: self.u8, I8: self.i8, U16: self.u16, I16: self.i16, CHAR: self.u8}[kind]()


# --- Section definitions (ordered; order IS the byte layout) -----------------

HEADER = [
    ("verticesOffset", U16), ("version", U16), ("sectorsOffset", U16),
    ("facesOffset", U16), ("faceTextureMapsOffset", U16), ("mapMetadataOffset", U16),
    ("verticesOffsetRepeat", U16), ("signature", [CHAR, CHAR]),
    ("midPlatformsSection", U16), ("section7Size", U16), ("verticesSectionSize", U16),
    ("objectsSectionsSize", U16), ("footerSize", U16), ("commandSectionSize", U16),
    ("sectorCount", U16),
]

SECTOR = [
    ("ceilingHeight", I16), ("floorHeight", I16), ("unk0x04", U16),
    ("ceilingTextureIndex", U16), ("floorTextureIndex", U16),
    ("textureFit", U8), ("lighting", U8), ("textureMapOverride", I8), ("facesCount", U8),
    ("firstFaceOffset", U16),
    ("ceilingTextureShiftX", U8), ("ceilingTextureShiftY", U8),
    ("floorTextureShiftX", U8), ("floorTextureShiftY", U8),
    ("floorTriggerID", U16), ("unk0x16", U16), ("intermediateFloorOffset", U16),
]

FACE = [
    ("vertexOffset01", U16), ("vertexOffset02", U16), ("textureMapOffset", U16),
    ("sectorOffset", U16), ("sisterFaceOffset", U16), ("addCollision", U16),
]
FACE_SIZE = 0x0C

TEXTURE_MAPPING = [
    ("unk0x00", U8), ("type", U8), ("midTextureIndex", U16),
    ("upperTextureIndex", U16), ("lowerTextureIndex", U16), ("unk0x08", U16),
]
ADDITIONAL_METADATA = [("shiftTextureX", U8), ("shiftTextureY", U8), ("unk0x0C", U16)]

MID_PLATFORM = [
    ("ceilingTextureIndex", U16), ("ceilingHeight", I16),
    ("ceilingTextureShiftX", U8), ("ceilingTextureShiftY", U8),
    ("floorTextureIndex", U16), ("floorHeight", I16),
    ("floorTextureShiftX", U8), ("floorTextureShiftY", U8),
    ("floorTextureScale", U8), ("padding", U8),
]

MAP_METADATA = [
    ("initPosX", I16), ("initPosZ", I16), ("initPosY", I16), ("rotation", I16),
    ("moveSpeed", U16), ("playerHeight", U16), ("maxClimb", U16), ("minFit", U16),
    ("unk0x10", U16), ("candleGlow", I16), ("lightAmbience", U16), ("unk0x16", U16),
    ("skyTexture", U16), ("unk0x1A", U16),
]

VERTICES_HEADER = [
    ("sectionSize", U16), ("sectionHeaderSize", U16), ("blank", U16), ("verticesCount", U16),
]
VERTEX = [
    ("unk0x00", U16), ("unk0x02", U16), ("unk0x04", U16), ("unk0x06", U16),
    ("x", I16), ("y", I16),
]

COMMAND_HEADER = [
    ("signature", [CHAR, CHAR]), ("unk0x02", U16), ("commandsOffset", U16), ("commandCount", U16),
]
ENTRY_COMMAND_COUNT = [("categoryOffset", U16), ("count", U16)]
COMMAND = [("size", U16), ("commandModifier", U8), ("commandBase", U8), ("nextCommandIndex", U16)]

SECTION_7_HEADER = [("sizeA", U16), ("count", U16)]
SOUND_EFFECT = [
    ("unk0x00", I16), ("unk0x02", I16), ("unk0x04", U16), ("unk0x06", U16),
    ("unk0x08", U8), ("zoneIndex", U8), ("unk0x0A", U16), ("unk0x0C", U16),
    ("unk0x0E", U16), ("volume", U8), ("unk0x11", U8),
]
UNKNOWN_ARRAY_2 = [("zoneCount", U16)] + [
    (f"zone{z}{f}", t)
    for z in (1, 2, 3)
    for f, t in (("Dampen", U8), ("Flags", U8), ("XBoundLower", I16), ("YBoundLower", I16),
                 ("XBoundUpper", I16), ("YBoundUpper", I16))
]

OBJECTS_HEADER = [("size", U16)]
OBJECTS_CONTAINER = [("count", U8), ("countRepeat", U8)]
OBJECT = [
    ("posX", I16), ("posY", I16), ("textureIndex", U8), ("textureSource", U8),
    ("rotation", U8), ("unk0x07", U8), ("lighting", U8), ("renderType", U8),
    ("posZ", I16), ("unk0x0C", U16), ("unk0x0E", U16),
]


def parse_section(r: Reader, definition) -> dict:
    out = {}
    for name, kind in definition:
        out[name] = r.read(kind)
    # Variable-size cases, mirroring parser.gd
    if definition is TEXTURE_MAPPING and out["type"] >= 0x80:
        out["additionalMetadata"] = {n: r.read(k) for n, k in ADDITIONAL_METADATA}
    if definition is COMMAND:
        out["args"] = [r.u16() for _ in range((out["size"] - 0x06) // 2)]
    return out


def parse(data: bytes) -> dict:
    r = Reader(data)

    header = parse_section(r, HEADER)

    sectors = [parse_section(r, SECTOR) for _ in range(header["sectorCount"])]

    face_count = r.u16()
    faces, faces_offset_map = [], {}
    for i in range(face_count):
        faces_offset_map[r.tell()] = i
        faces.append(parse_section(r, FACE))

    mapping_count = r.u16()
    mappings, mappings_offset_map = [], {}
    for i in range(mapping_count):
        mappings_offset_map[r.tell()] = i
        mappings.append(parse_section(r, TEXTURE_MAPPING))

    mid_platforms, mid_platforms_offset_map = [], {}
    if header["midPlatformsSection"] != 0x00:
        for i in range(r.u16()):
            mid_platforms_offset_map[r.tell()] = i
            mid_platforms.append(parse_section(r, MID_PLATFORM))

    map_metadata = parse_section(r, MAP_METADATA)

    r.seek(header["verticesOffset"])
    vertices_header = parse_section(r, VERTICES_HEADER)
    vertices, vertices_offset_map = [], {}
    for i in range(vertices_header["verticesCount"]):
        vertices_offset_map[r.tell() - header["verticesOffset"]] = i
        v = parse_section(r, VERTEX)
        for junk in ("unk0x00", "unk0x02", "unk0x04", "unk0x06"):
            v.pop(junk)
        vertices.append(v)

    command_header = parse_section(r, COMMAND_HEADER)
    for _ in range(15):
        parse_section(r, ENTRY_COMMAND_COUNT)

    entry_offsets = [parse_section(r, [("offset", U16)])["offset"]
                     for _ in range(command_header["commandCount"])]

    commands_base = header["verticesOffset"] + header["verticesSectionSize"]
    commands, commands_offset_map = [], {}
    for i in range(command_header["commandCount"]):
        commands_offset_map[r.tell() - commands_base] = i + 1
        cmd = parse_section(r, COMMAND)
        cmd.pop("size")
        commands.append(cmd)

    entry_command_indexes = [commands_offset_map[o] for o in entry_offsets if o != 0x0000]
    command_header.pop("commandCount")
    command_header.pop("commandsOffset")

    section_7_header = parse_section(r, SECTION_7_HEADER)
    unk_array_01 = [parse_section(r, SOUND_EFFECT) for _ in range(section_7_header["count"])]

    unk_array_02 = []
    if header["section7Size"] > section_7_header["sizeA"]:
        end = header["section7Size"] + header["commandSectionSize"] + commands_base
        while r.tell() < end:
            unk_array_02.append(parse_section(r, UNKNOWN_ARRAY_2))

    # Objects: a per-sector table of relative offsets into this section
    object_start = r.tell()
    objects_header = parse_section(r, OBJECTS_HEADER)
    cursor = r.tell()
    for i in range(header["sectorCount"]):
        r.seek(cursor)
        cursor += 0x02
        rel = r.u16()
        if rel == 0x0000:
            sectors[i]["objectInformation"] = []
            continue
        r.seek(object_start + rel)
        container = parse_section(r, OBJECTS_CONTAINER)
        sectors[i]["objectInformation"] = [
            parse_section(r, OBJECT) for _ in range(container["count"])
        ]

    r.seek(object_start + objects_header["size"])

    # Resolve file offsets into array indices
    for i, sector in enumerate(sectors):
        if sector["intermediateFloorOffset"] > 0x0000:
            sector["intermediateFloorIndex"] = mid_platforms_offset_map[sector["intermediateFloorOffset"]]
        sector["firstFaceIndex"] = faces_offset_map[sector["firstFaceOffset"]]
        for j in range(sector["facesCount"]):
            faces[faces_offset_map[sector["firstFaceOffset"] + FACE_SIZE * j]]["sectorIndex"] = i
        sector.pop("intermediateFloorOffset")
        sector.pop("firstFaceOffset")

    for face in faces:
        face["vertexIndex01"] = vertices_offset_map[face["vertexOffset01"]]
        face["vertexIndex02"] = vertices_offset_map[face["vertexOffset02"]]
        face["textureMappingIndex"] = mappings_offset_map[face["textureMapOffset"]]
        if face["sisterFaceOffset"] not in (0xFFFF, 0x0000):
            face["sisterFaceIndex"] = faces_offset_map[face["sisterFaceOffset"]]
        for junk in ("vertexOffset01", "vertexOffset02", "textureMapOffset",
                     "sisterFaceOffset", "sectorOffset"):
            face.pop(junk)

    return {
        "sectorsSection": {"sectors": sectors},
        "facesSection": {"faces": faces},
        "faceTextureMappingSection": {"mappings": mappings},
        **({"midPlatformsSection": {"platforms": mid_platforms}} if mid_platforms else {}),
        "mapMetadataSection": map_metadata,
        "verticesSection": {"vertices": vertices},
        "commandsSection": {
            "header": command_header,
            "entryCommandIndexes": entry_command_indexes,
            "allCommands": commands,
        },
        "section7": {
            "unkArray01": unk_array_01,
            **({"unkArray02": unk_array_02} if unk_array_02 else {}),
        },
    }


def summarize(m: dict, name: str) -> str:
    sectors = m["sectorsSection"]["sectors"]
    faces = m["facesSection"]["faces"]
    verts = m["verticesSection"]["vertices"]
    meta = m["mapMetadataSection"]
    objs = [o for s in sectors for o in s["objectInformation"]]
    two_sided = sum(1 for f in faces if "sisterFaceIndex" in f)
    xs = [v["x"] for v in verts]
    ys = [v["y"] for v in verts]
    floors = [s["floorHeight"] for s in sectors]
    ceils = [s["ceilingHeight"] for s in sectors]

    lines = [
        f"{name}",
        f"  sectors ............ {len(sectors)}",
        f"  faces .............. {len(faces)}  ({two_sided} two-sided / portals)",
        f"  vertices ........... {len(verts)}",
        f"  texture mappings ... {len(m['faceTextureMappingSection']['mappings'])}",
        f"  mid-platforms ...... {len(m.get('midPlatformsSection', {}).get('platforms', []))}",
        f"  objects ............ {len(objs)}",
        f"  commands ........... {len(m['commandsSection']['allCommands'])}"
        f"  (entry points: {len(m['commandsSection']['entryCommandIndexes'])})",
        f"  extent X ........... {min(xs)} .. {max(xs)}   ({max(xs) - min(xs)} units wide)",
        f"  extent Y ........... {min(ys)} .. {max(ys)}   ({max(ys) - min(ys)} units deep)",
        f"  floor heights ...... {min(floors)} .. {max(floors)}",
        f"  ceiling heights .... {min(ceils)} .. {max(ceils)}",
        f"  SCALE CLUES: playerHeight={meta['playerHeight']}  maxClimb={meta['maxClimb']}  minFit={meta['minFit']}",
        f"  start position ..... ({meta['initPosX']}, {meta['initPosY']}, {meta['initPosZ']}) rot={meta['rotation']}",
    ]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Parse a ROTH .RAW map into JSON.")
    ap.add_argument("raw", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()

    parsed = parse(args.raw.read_bytes())

    if args.out:
        args.out.write_text(json.dumps(parsed, indent=1))
        print(f"wrote {args.out}")
    if args.summary or not args.out:
        print(summarize(parsed, args.raw.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
