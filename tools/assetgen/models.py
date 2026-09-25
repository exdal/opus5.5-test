#!/usr/bin/env python3
"""Low-poly model generator for OxCity.

Writes binary glTF (.glb) files with no third-party dependencies. Every model is built from boxes,
cylinders and wedges with flat per-face normals and plain PBR base colours, the look is "a toy city seen
from a crane". Coordinates are glTF defaults: metres, +Y up, +Z forward.

Models are node hierarchies so the game can animate parts (character limbs swing around their
pivots, the police light bar blinks via its emissive child node, etc). Every node's local origin is
its pivot.

    python3 tools/assetgen/models.py            # writes into game/assets/Models
"""

from __future__ import annotations

import json
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "game" / "assets" / "Models"


# ---------------------------------------------------------------------------------------------------------------------
# materials


@dataclass(frozen=True)
class Mat:
    name: str
    color: tuple[float, float, float, float]
    metallic: float = 0.0
    roughness: float = 0.8
    emissive: tuple[float, float, float] = (0.0, 0.0, 0.0)
    emissive_strength: float = 1.0
    # name of a generated texture (see TEXTURES) used as the base colour map, and the glTF alpha mode
    texture: str | None = None
    alpha_mode: str = "OPAQUE"
    alpha_cutoff: float = 0.5


def srgb(hex_color: str, alpha: float = 1.0) -> tuple[float, float, float, float]:
    """#rrggbb in sRGB -> linear rgba, glTF base colour factors are linear"""
    hex_color = hex_color.lstrip("#")
    out = []
    for i in range(3):
        c = int(hex_color[i * 2 : i * 2 + 2], 16) / 255.0
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return (out[0], out[1], out[2], alpha)


def mat(name, hex_color, metallic=0.0, roughness=0.8, emissive=None, strength=1.0) -> Mat:
    em = (0.0, 0.0, 0.0)
    if emissive is not None:
        em = srgb(emissive)[:3]
    return Mat(name, srgb(hex_color), metallic, roughness, em, strength)


# shared palette
ASPHALT = mat("asphalt", "#3a3b3f", roughness=0.95)
ASPHALT_DARK = mat("asphalt_dark", "#2c2d31", roughness=0.95)
PAINT_WHITE = mat("paint_white", "#e8e8e0", roughness=0.7)
PAINT_YELLOW = mat("paint_yellow", "#e9c43a", roughness=0.7)
CONCRETE = mat("concrete", "#9a978f", roughness=0.9)
CONCRETE_DARK = mat("concrete_dark", "#7c7a74", roughness=0.9)
CURB = mat("curb", "#b8b5ab", roughness=0.9)
GRASS = mat("grass", "#4f7d3a", roughness=1.0)
DIRT = mat("dirt", "#6d5a41", roughness=1.0)
GLASS = mat("glass", "#1c2a38", metallic=0.1, roughness=0.15)
GLASS_LIT = mat("glass_lit", "#6b5a2a", roughness=0.3, emissive="#ffcf6b", strength=2.0)
TIRE = mat("tire", "#161616", roughness=0.9)
HUB = mat("hub", "#b9bcc2", metallic=0.9, roughness=0.3)
CHROME = mat("chrome", "#d6d8dc", metallic=1.0, roughness=0.2)
HEADLIGHT = mat("headlight", "#fff8dc", emissive="#fff4d0", strength=4.0)
TAILLIGHT = mat("taillight", "#7a0a0a", emissive="#ff2020", strength=3.0)
SKIN_A = mat("skin_a", "#e0ac7e")
SKIN_B = mat("skin_b", "#a8703f")
SKIN_C = mat("skin_c", "#6b4226")
HAIR_DARK = mat("hair_dark", "#2a1d14")
HAIR_BLOND = mat("hair_blond", "#c8a45a")
JEANS = mat("jeans", "#2f4a78")
SHOE = mat("shoe", "#1e1e1e")
GOLD = mat("gold", "#e0b43c", metallic=1.0, roughness=0.3)
MARBLE = mat("marble", "#e6e1d6", roughness=0.5)
MONEY = mat("money", "#3f9a4a", roughness=0.6)
MONEY_BAND = mat("money_band", "#e9d9a0")
WOOD = mat("wood", "#7a5230")
LEAVES = mat("leaves", "#2f6b2f")
LAMP_POST = mat("lamp_post", "#2b2f36", metallic=0.6, roughness=0.5)
LAMP_LIGHT = mat("lamp_light", "#fff0c0", emissive="#ffe29a", strength=6.0)
POLICE_BLUE = mat("police_blue", "#1f3f8f", roughness=0.4)
SIREN_RED = mat("siren_red", "#7a0000", emissive="#ff1010", strength=8.0)
SIREN_BLUE = mat("siren_blue", "#00167a", emissive="#1030ff", strength=8.0)
BLACK = mat("black", "#151515")
VAULT = mat("vault", "#8d9299", metallic=0.9, roughness=0.35)


# ---------------------------------------------------------------------------------------------------------------------
# textures: tiny procedural RGBA images, embedded into the glb as PNG


def png_bytes(width: int, height: int, pixels: list[tuple[int, int, int, int]]) -> bytes:
    """minimal RGBA8 PNG encoder (zlib + crc32 from the standard library)"""
    import zlib

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        for x in range(width):
            raw.extend(bytes(pixels[y * width + x]))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def soft_dot_texture(size: int = 64) -> bytes:
    """white disc with a smooth falloff, particles tint it"""
    pixels = []
    for y in range(size):
        for x in range(size):
            dx = (x + 0.5) / size * 2.0 - 1.0
            dy = (y + 0.5) / size * 2.0 - 1.0
            r = math.sqrt(dx * dx + dy * dy)
            a = max(0.0, 1.0 - r)
            a = a * a * (3.0 - 2.0 * a)
            pixels.append((255, 255, 255, int(a * 255)))
    return png_bytes(size, size, pixels)


def blood_splat_texture(seed: int, size: int = 128) -> bytes:
    """hard edged splatter for alpha masking: a lumpy pool, streaks thrown out one way, droplets around it"""
    import random

    rng = random.Random(seed)
    blobs = []  # (cx, cy, radius) in [-1, 1] space
    lobes = [(rng.uniform(0, 2 * math.pi), rng.uniform(0.08, 0.2)) for _ in range(7)]
    throw = rng.uniform(0, 2 * math.pi)
    for i in range(rng.randint(5, 8)):
        # streaks: a line of shrinking blobs in the throw direction
        angle = throw + rng.uniform(-0.6, 0.6)
        length = rng.uniform(0.45, 0.85)
        steps = 6
        for k in range(steps):
            t = 0.25 + length * k / steps
            blobs.append((math.cos(angle) * t, math.sin(angle) * t, 0.07 * (1.0 - k / steps) + 0.015))
    for _ in range(rng.randint(10, 18)):
        # droplets
        angle = rng.uniform(0, 2 * math.pi)
        dist = rng.uniform(0.45, 0.95)
        blobs.append((math.cos(angle) * dist, math.sin(angle) * dist, rng.uniform(0.015, 0.045)))

    pixels = []
    for y in range(size):
        for x in range(size):
            px = (x + 0.5) / size * 2.0 - 1.0
            py = (y + 0.5) / size * 2.0 - 1.0
            angle = math.atan2(py, px)
            radius = 0.34 + sum(amp * math.cos(angle * (i + 2) + phase) for i, (phase, amp) in enumerate(lobes)) * 0.35
            inside = math.hypot(px, py) < radius
            if not inside:
                inside = any((px - bx) ** 2 + (py - by) ** 2 < br * br for bx, by, br in blobs)
            # value varies a little so the pool doesn't look flat, alpha is the mask
            shade = 205 + int(40 * (0.5 + 0.5 * math.sin(px * 9.0 + py * 7.0)))
            pixels.append((shade, shade, shade, 255 if inside else 0))
    return png_bytes(size, size, pixels)


TEXTURES = {
    "soft_dot": soft_dot_texture,
    **{f"blood_splat_{i}": (lambda i=i: blood_splat_texture(1000 + i)) for i in range(4)},
}


# ---------------------------------------------------------------------------------------------------------------------
# geometry


@dataclass
class Geo:
    positions: list = field(default_factory=list)
    normals: list = field(default_factory=list)
    uvs: list = field(default_factory=list)
    indices: list = field(default_factory=list)

    def quad(self, a, b, c, d, n):
        base = len(self.positions)
        self.positions += [a, b, c, d]
        self.normals += [n, n, n, n]
        self.uvs += [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
        self.indices += [base, base + 1, base + 2, base, base + 2, base + 3]

    def tri(self, a, b, c):
        n = normalize(cross(sub(b, a), sub(c, a)))
        base = len(self.positions)
        self.positions += [a, b, c]
        self.normals += [n, n, n]
        self.uvs += [(0.0, 0.0), (1.0, 0.0), (0.5, 1.0)]
        self.indices += [base, base + 1, base + 2]

    def extend(self, other: "Geo"):
        base = len(self.positions)
        self.positions += other.positions
        self.normals += other.normals
        self.uvs += other.uvs
        self.indices += [i + base for i in other.indices]


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(v):
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / length, v[1] / length, v[2] / length)


def box(center, size) -> Geo:
    """axis aligned box, counter-clockwise front faces"""
    cx, cy, cz = center
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    x0, x1, y0, y1, z0, z1 = cx - hx, cx + hx, cy - hy, cy + hy, cz - hz, cz + hz
    g = Geo()
    g.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (0, 0, 1))  # +z
    g.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (0, 0, -1))  # -z
    g.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (1, 0, 0))  # +x
    g.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (-1, 0, 0))  # -x
    g.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), (0, 1, 0))  # +y
    g.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0))  # -y
    return g


def wedge(center, size, slope_front=True) -> Geo:
    """box whose top slopes down towards +z (a windscreen), or towards -z when slope_front is False"""
    cx, cy, cz = center
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    x0, x1, y0, y1, z0, z1 = cx - hx, cx + hx, cy - hy, cy + hy, cz - hz, cz + hz
    g = Geo()
    if slope_front:
        # tall at -z, flush with the bottom at +z
        g.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (0, 0, -1))
        g.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0))
        g.quad((x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z0), normalize((0, hz * 2, hy * 2)))
        g.tri((x1, y0, z1), (x1, y0, z0), (x1, y1, z0))
        g.tri((x0, y0, z0), (x0, y0, z1), (x0, y1, z0))
    else:
        g.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (0, 0, 1))
        g.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0))
        g.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z1), (x1, y1, z1), normalize((0, hz * 2, -hy * 2)))
        g.tri((x1, y0, z0), (x1, y1, z1), (x1, y0, z1))
        g.tri((x0, y0, z1), (x0, y1, z1), (x0, y0, z0))
    return g


def cylinder(center, radius, length, axis="y", segments=12) -> Geo:
    g = Geo()
    cx, cy, cz = center

    def point(angle, t):
        a, b = math.cos(angle) * radius, math.sin(angle) * radius
        if axis == "y":
            return (cx + a, cy + t, cz + b)
        if axis == "x":
            return (cx + t, cy + a, cz + b)
        return (cx + a, cy + b, cz + t)

    def radial(angle):
        a, b = math.cos(angle), math.sin(angle)
        if axis == "y":
            return (a, 0.0, b)
        if axis == "x":
            return (0.0, a, b)
        return (a, b, 0.0)

    h = length / 2
    cap_n = {"y": (0, 1, 0), "x": (1, 0, 0), "z": (0, 0, 1)}[axis]
    for i in range(segments):
        a0 = 2 * math.pi * i / segments
        a1 = 2 * math.pi * (i + 1) / segments
        am = (a0 + a1) / 2
        p00, p01, p10, p11 = point(a0, -h), point(a0, h), point(a1, -h), point(a1, h)
        side = Geo()
        side.quad(p00, p10, p11, p01, radial(am))
        # make sure the side faces outwards whatever the axis handedness is
        if dot(cross(sub(p10, p00), sub(p01, p00)), radial(am)) < 0:
            side.indices = [side.indices[i] for i in (0, 2, 1, 3, 5, 4)]
        g.extend(side)
        for sign in (-1, 1):
            centre = {"y": (cx, cy + sign * h, cz), "x": (cx + sign * h, cy, cz), "z": (cx, cy, cz + sign * h)}[axis]
            a, b = point(a0, sign * h), point(a1, sign * h)
            n = tuple(sign * v for v in cap_n)
            cap = Geo()
            cap.positions = [centre, a, b]
            cap.normals = [n, n, n]
            cap.uvs = [(0.5, 0.5), (0.0, 0.0), (1.0, 0.0)]
            if dot(cross(sub(a, centre), sub(b, centre)), n) > 0:
                cap.indices = [0, 1, 2]
            else:
                cap.indices = [0, 2, 1]
            g.extend(cap)
    return g


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


# ---------------------------------------------------------------------------------------------------------------------
# model description


@dataclass
class Node:
    name: str
    translation: tuple = (0.0, 0.0, 0.0)
    parts: list = field(default_factory=list)  # (Geo, Mat)
    children: list = field(default_factory=list)

    def add(self, geo: Geo, material: Mat) -> "Node":
        self.parts.append((geo, material))
        return self

    def child(self, name, translation=(0.0, 0.0, 0.0)) -> "Node":
        node = Node(name, translation)
        self.children.append(node)
        return node


def write_glb(path: Path, root: Node):
    materials: list[Mat] = []
    images: list[str] = []  # texture names, one glTF image + texture each
    material_index: dict[Mat, int] = {}
    buffer = bytearray()
    buffer_views = []
    accessors = []
    meshes = []
    nodes = []

    def push_view(data: bytes, target: int | None) -> int:
        while len(buffer) % 4:
            buffer.append(0)
        offset = len(buffer)
        buffer.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        buffer_views.append(view)
        return len(buffer_views) - 1

    def push_accessor(values, kind: str) -> int:
        if kind == "SCALAR":
            data = struct.pack(f"<{len(values)}I", *values)
            view = push_view(data, 34963)
            accessors.append({"bufferView": view, "componentType": 5125, "count": len(values), "type": "SCALAR"})
        else:
            width = 3 if kind == "VEC3" else 2
            flat = [c for v in values for c in v[:width]]
            data = struct.pack(f"<{len(flat)}f", *flat)
            view = push_view(data, 34962)
            acc = {"bufferView": view, "componentType": 5126, "count": len(values), "type": kind}
            if kind == "VEC3":
                acc["min"] = [min(v[i] for v in values) for i in range(3)]
                acc["max"] = [max(v[i] for v in values) for i in range(3)]
            accessors.append(acc)
        return len(accessors) - 1

    def emit(node: Node) -> int:
        entry = {"name": node.name}
        if any(node.translation):
            entry["translation"] = list(node.translation)
        if node.parts:
            # one primitive per material, merged
            by_mat: dict[Mat, Geo] = {}
            for geo, material in node.parts:
                by_mat.setdefault(material, Geo()).extend(geo)
            primitives = []
            for material, geo in by_mat.items():
                if material not in material_index:
                    material_index[material] = len(materials)
                    materials.append(material)
                primitives.append(
                    {
                        "attributes": {
                            "POSITION": push_accessor(geo.positions, "VEC3"),
                            "NORMAL": push_accessor(geo.normals, "VEC3"),
                            "TEXCOORD_0": push_accessor(geo.uvs, "VEC2"),
                        },
                        "indices": push_accessor(geo.indices, "SCALAR"),
                        "material": material_index[material],
                    }
                )
            meshes.append({"name": node.name, "primitives": primitives})
            entry["mesh"] = len(meshes) - 1
        nodes.append(entry)
        index = len(nodes) - 1
        child_indices = [emit(c) for c in node.children]
        if child_indices:
            nodes[index]["children"] = child_indices
        return index

    root_index = emit(root)

    for m in materials:
        if m.texture and m.texture not in images:
            images.append(m.texture)
    image_json = [{"name": name, "mimeType": "image/png", "bufferView": push_view(TEXTURES[name](), None)} for name in images]

    gltf = {
        "asset": {"version": "2.0", "generator": "oxcity assetgen"},
        "scene": 0,
        "scenes": [{"name": root.name, "nodes": [root_index]}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": [material_json(m, images) for m in materials],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(buffer)}],
    }
    if images:
        gltf["images"] = image_json
        # linear filtering, clamped: these are decals and sprites, not tiling surfaces
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
        gltf["textures"] = [{"source": i, "sampler": 0} for i in range(len(images))]
    if any(m.emissive_strength != 1.0 for m in materials):
        gltf["extensionsUsed"] = ["KHR_materials_emissive_strength"]

    json_bytes = json.dumps(gltf, separators=(",", ":")).encode()
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    while len(buffer) % 4:
        buffer.append(0)
    total = 12 + 8 + len(json_bytes) + 8 + len(buffer)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
    out += struct.pack("<II", len(buffer), 0x004E4942) + buffer
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)


def material_json(m: Mat, images: list[str]) -> dict:
    out = {
        "name": m.name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(m.color),
            "metallicFactor": m.metallic,
            "roughnessFactor": m.roughness,
        },
    }
    if m.texture:
        out["pbrMetallicRoughness"]["baseColorTexture"] = {"index": images.index(m.texture)}
    if m.alpha_mode != "OPAQUE":
        out["alphaMode"] = m.alpha_mode
        if m.alpha_mode == "MASK":
            out["alphaCutoff"] = m.alpha_cutoff
    if any(m.emissive):
        out["emissiveFactor"] = list(m.emissive)
        if m.emissive_strength != 1.0:
            out["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": m.emissive_strength}}
    return out


# ---------------------------------------------------------------------------------------------------------------------
# characters

CHARACTER_STYLES = {
    # name: (shirt, pants, skin, hair)
    "player": (mat("jacket_yellow", "#e6b62a"), JEANS, SKIN_A, HAIR_DARK),
    "ped_0": (mat("shirt_red", "#b0302a"), JEANS, SKIN_A, HAIR_BLOND),
    "ped_1": (mat("shirt_green", "#3d7a4a"), mat("pants_khaki", "#a08a5c"), SKIN_B, HAIR_DARK),
    "ped_2": (mat("shirt_purple", "#6b3f8f"), BLACK, SKIN_C, HAIR_DARK),
    "ped_3": (mat("shirt_white", "#d9d9d2"), mat("pants_grey", "#55585e"), SKIN_B, HAIR_BLOND),
    "ped_4": (mat("shirt_orange", "#d4702a"), JEANS, SKIN_C, BLACK),
    "cop": (POLICE_BLUE, mat("cop_pants", "#1a2340"), SKIN_A, POLICE_BLUE),
    "guard": (mat("guard_grey", "#4a4d52"), BLACK, SKIN_B, BLACK),
}


def character(name: str) -> Node:
    """~1.8m person, pivots: legs at the hip, arms at the shoulder. Origin at the feet"""
    shirt, pants, skin, hair = CHARACTER_STYLES[name]
    root = Node(name)
    torso = root.child("torso", (0.0, 0.95, 0.0))
    torso.add(box((0.0, 0.3, 0.0), (0.46, 0.6, 0.26)), shirt)
    head = torso.child("head", (0.0, 0.62, 0.0))
    head.add(box((0.0, 0.14, 0.0), (0.24, 0.26, 0.24)), skin)
    head.add(box((0.0, 0.29, -0.01), (0.26, 0.06, 0.26)), hair)
    head.add(box((0.0, 0.18, 0.121), (0.14, 0.03, 0.01)), BLACK)  # eyes, so facing reads from above
    if name == "cop":
        head.add(box((0.0, 0.3, 0.05), (0.28, 0.05, 0.34)), POLICE_BLUE)
        head.add(box((0.0, 0.33, 0.0), (0.02, 0.02, 0.02)), GOLD)
    for side, x in (("l", 0.3), ("r", -0.3)):
        arm = torso.child(f"arm_{side}", (x, 0.55, 0.0))
        arm.add(box((0.0, -0.25, 0.0), (0.13, 0.5, 0.14)), shirt)
        arm.add(box((0.0, -0.55, 0.0), (0.11, 0.12, 0.12)), skin)
    for side, x in (("l", 0.12), ("r", -0.12)):
        leg = root.child(f"leg_{side}", (x, 0.95, 0.0))
        leg.add(box((0.0, -0.42, 0.0), (0.18, 0.84, 0.2)), pants)
        leg.add(box((0.0, -0.9, 0.05), (0.19, 0.1, 0.3)), SHOE)
    return root


# ---------------------------------------------------------------------------------------------------------------------
# vehicles

VEHICLE_STYLES = {
    # name: (paint, length, width, body height, cabin scale, sporty)
    "sedan": (mat("paint_sedan", "#8f1d1d", metallic=0.4, roughness=0.35), 4.4, 1.9, 0.62, 1.0, False),
    "sports": (mat("paint_sports", "#e0dc32", metallic=0.5, roughness=0.25), 4.3, 1.95, 0.5, 0.8, True),
    "taxi": (mat("paint_taxi", "#f0c419", metallic=0.2, roughness=0.4), 4.5, 1.9, 0.62, 1.0, False),
    "police": (mat("paint_police", "#f2f2f2", metallic=0.3, roughness=0.35), 4.6, 1.95, 0.64, 1.0, False),
    "van": (mat("paint_van", "#3a6ea8", metallic=0.2, roughness=0.5), 4.9, 2.05, 0.9, 1.0, False),
}


def vehicle(name: str) -> Node:
    """chassis only, wheels are separate entities driven by the physics constraint. Origin at the centre
    of the body's bottom face, +z is forward"""
    paint, length, width, body_h, cabin_scale, sporty = VEHICLE_STYLES[name]
    root = Node(name)
    body = root.child("body")
    ground = 0.3  # bottom of the body above the chassis origin
    body.add(box((0.0, ground + body_h / 2, 0.0), (width, body_h, length)), paint)
    # bumpers
    body.add(box((0.0, ground + 0.12, length / 2 + 0.05), (width * 0.95, 0.2, 0.12)), CHROME if not sporty else BLACK)
    body.add(box((0.0, ground + 0.12, -length / 2 - 0.05), (width * 0.95, 0.2, 0.12)), CHROME if not sporty else BLACK)
    # lights
    for x in (-width / 2 + 0.3, width / 2 - 0.3):
        body.add(box((x, ground + body_h - 0.15, length / 2 + 0.005), (0.38, 0.14, 0.02)), HEADLIGHT)
        body.add(box((x, ground + body_h - 0.15, -length / 2 - 0.005), (0.38, 0.12, 0.02)), TAILLIGHT)

    top = ground + body_h
    if name == "van":
        cabin_len = length * 0.78
        cabin_z = -length * 0.08
        body.add(box((0.0, top + 0.45, cabin_z), (width * 0.98, 0.9, cabin_len)), paint)
        body.add(wedge((0.0, top + 0.35, cabin_z + cabin_len / 2 + 0.25), (width * 0.94, 0.7, 0.5)), GLASS)
        body.add(box((width / 2 + 0.005, top + 0.55, cabin_z + cabin_len / 2 - 0.4), (0.01, 0.4, 0.6)), GLASS)
        body.add(box((-width / 2 - 0.005, top + 0.55, cabin_z + cabin_len / 2 - 0.4), (0.01, 0.4, 0.6)), GLASS)
    else:
        cabin_h = 0.5 * cabin_scale
        cabin_len = length * (0.42 if sporty else 0.48)
        cabin_z = -length * (0.1 if sporty else 0.05)
        cw = width * 0.84
        body.add(box((0.0, top + cabin_h / 2, cabin_z), (cw, cabin_h, cabin_len)), GLASS)
        # roof over the glass so it reads as windows from above
        body.add(box((0.0, top + cabin_h + 0.03, cabin_z), (cw + 0.02, 0.06, cabin_len * 0.8)), paint)
        body.add(wedge((0.0, top + cabin_h / 2, cabin_z + cabin_len / 2 + 0.3), (cw, cabin_h, 0.6)), GLASS)
        body.add(wedge((0.0, top + cabin_h / 2, cabin_z - cabin_len / 2 - 0.25), (cw, cabin_h, 0.5), False), GLASS)
        if sporty:
            body.add(box((0.0, top + 0.28, -length / 2 + 0.15), (width * 0.9, 0.05, 0.3)), BLACK)  # spoiler
            body.add(box((width * 0.3, top + 0.12, -length / 2 + 0.15), (0.05, 0.2, 0.05)), BLACK)
            body.add(box((-width * 0.3, top + 0.12, -length / 2 + 0.15), (0.05, 0.2, 0.05)), BLACK)
            body.add(box((0.0, top + 0.005, length * 0.25), (0.3, 0.01, length * 0.4)), BLACK)  # stripe
        if name == "taxi":
            body.add(box((0.0, top + cabin_h + 0.14, cabin_z), (0.6, 0.18, 0.25)), mat("taxi_sign", "#fff3b0", emissive="#ffe070", strength=3.0))
            for i in range(6):
                body.add(box((width / 2 + 0.005, ground + body_h * 0.5, -1.2 + i * 0.4), (0.01, 0.1, 0.2)), BLACK if i % 2 else paint)
                body.add(box((-width / 2 - 0.005, ground + body_h * 0.5, -1.2 + i * 0.4), (0.01, 0.1, 0.2)), BLACK if i % 2 else paint)
        if name == "police":
            body.add(box((0.0, top + 0.01, length * 0.3), (width * 0.98, 0.02, length * 0.3)), BLACK)
            body.add(box((0.0, top + 0.01, -length * 0.38), (width * 0.98, 0.02, length * 0.2)), BLACK)
            bar = root.child("lightbar", (0.0, top + cabin_h + 0.06, cabin_z))
            bar.add(box((0.0, 0.03, 0.0), (1.2, 0.06, 0.25)), BLACK)
            red = bar.child("siren_red", (0.33, 0.1, 0.0))
            red.add(box((0.0, 0.0, 0.0), (0.5, 0.1, 0.22)), SIREN_RED)
            blue = bar.child("siren_blue", (-0.33, 0.1, 0.0))
            blue.add(box((0.0, 0.0, 0.0), (0.5, 0.1, 0.22)), SIREN_BLUE)
    return root


def wheel() -> Node:
    root = Node("wheel")
    root.add(cylinder((0.0, 0.0, 0.0), 0.36, 0.26, axis="x", segments=14), TIRE)
    root.add(cylinder((0.0, 0.0, 0.0), 0.22, 0.28, axis="x", segments=10), HUB)
    return root


# ---------------------------------------------------------------------------------------------------------------------
# city

TILE = 12.0  # metres per road / block tile, must match game/src/City.cpp


def road(kind: str) -> Node:
    """kind: straight (runs along z), cross (4 way intersection)"""
    root = Node(f"road_{kind}")
    root.add(box((0.0, -0.05, 0.0), (TILE, 0.1, TILE)), ASPHALT)
    if kind == "straight":
        for i in range(4):
            z = -TILE / 2 + 1.5 + i * 3.0
            root.add(box((0.0, 0.002, z), (0.18, 0.01, 1.6)), PAINT_YELLOW)
        for x in (-TILE / 2 + 0.4, TILE / 2 - 0.4):
            root.add(box((x, 0.002, 0.0), (0.14, 0.01, TILE)), PAINT_WHITE)
    elif kind == "cross":
        root.add(box((0.0, 0.001, 0.0), (TILE * 0.6, 0.004, TILE * 0.6)), ASPHALT_DARK)
        for side in (-1, 1):
            for i in range(7):
                o = -TILE / 2 + 1.2 + i * (TILE - 2.4) / 6
                root.add(box((o, 0.002, side * (TILE / 2 - 0.9)), (0.5, 0.01, 1.4)), PAINT_WHITE)
                root.add(box((side * (TILE / 2 - 0.9), 0.002, o), (1.4, 0.01, 0.5)), PAINT_WHITE)
    return root


def sidewalk_block() -> Node:
    """a city block: raised concrete slab with curbs, buildings sit on top of it"""
    root = Node("block")
    root.add(box((0.0, 0.075, 0.0), (TILE, 0.15, TILE)), CONCRETE)
    for i in range(4):
        o = -TILE / 2 + TILE / 8 + i * TILE / 4
        root.add(box((o, 0.151, 0.0), (0.03, 0.002, TILE)), CONCRETE_DARK)
        root.add(box((0.0, 0.151, o), (TILE, 0.002, 0.03)), CONCRETE_DARK)
    for side in (-1, 1):
        root.add(box((side * (TILE / 2 - 0.1), 0.09, 0.0), (0.2, 0.18, TILE)), CURB)
        root.add(box((0.0, 0.09, side * (TILE / 2 - 0.1)), (TILE, 0.18, 0.2)), CURB)
    return root


def park_block() -> Node:
    root = Node("park")
    root.add(box((0.0, 0.075, 0.0), (TILE, 0.15, TILE)), GRASS)
    root.add(box((0.0, 0.151, 0.0), (1.4, 0.004, TILE)), DIRT)
    root.add(box((0.0, 0.151, 0.0), (TILE, 0.004, 1.4)), DIRT)
    for side in (-1, 1):
        root.add(box((side * (TILE / 2 - 0.1), 0.09, 0.0), (0.2, 0.18, TILE)), CURB)
        root.add(box((0.0, 0.09, side * (TILE / 2 - 0.1)), (TILE, 0.18, 0.2)), CURB)
    return root


BUILDING_STYLES = [
    # wall, trim, floors
    (mat("brick_red", "#8a3b2c"), mat("trim_light", "#c9bfa9"), 3),
    (mat("concrete_blue", "#56657a"), mat("trim_dark", "#3a3f47"), 6),
    (mat("stucco_tan", "#b99a6a"), mat("trim_brown", "#6a4c30"), 2),
    (mat("glass_tower", "#2d4660", metallic=0.4, roughness=0.2), mat("steel", "#8b9098", metallic=0.8, roughness=0.3), 9),
    (mat("brick_brown", "#6e4a36"), mat("trim_cream", "#e0d6bb"), 4),
    (mat("panel_green", "#4e6a58"), mat("trim_grey", "#7a7d80"), 5),
]


def building(index: int) -> Node:
    wall, trim, floors = BUILDING_STYLES[index]
    root = Node(f"building_{index}")
    size = 9.0
    floor_h = 3.2
    h = floors * floor_h
    base = 0.15
    root.add(box((0.0, base + h / 2, 0.0), (size, h, size)), wall)
    root.add(box((0.0, base + h + 0.2, 0.0), (size + 0.3, 0.4, size + 0.3)), trim)
    # windows on all four sides, a few lit
    lit_seed = index * 7 + 3
    for f in range(floors):
        y = base + f * floor_h + floor_h * 0.55
        for w in range(4):
            o = -size / 2 + size / 8 + w * size / 4
            lit = ((f * 5 + w * 3 + lit_seed) % 7) == 0
            glass = GLASS_LIT if lit else GLASS
            root.add(box((o, y, size / 2 + 0.02), (1.1, 1.4, 0.04)), glass)
            root.add(box((o, y, -size / 2 - 0.02), (1.1, 1.4, 0.04)), glass)
            root.add(box((size / 2 + 0.02, y, o), (0.04, 1.4, 1.1)), glass)
            root.add(box((-size / 2 - 0.02, y, o), (0.04, 1.4, 1.1)), glass)
        root.add(box((0.0, base + (f + 1) * floor_h, 0.0), (size + 0.1, 0.12, size + 0.1)), trim)
    # rooftop clutter reads well from a top down camera
    root.add(box((2.0, base + h + 0.9, -1.5), (1.6, 1.0, 1.2)), CONCRETE_DARK)
    root.add(box((-2.2, base + h + 0.7, 2.0), (1.0, 0.6, 1.0)), CONCRETE_DARK)
    root.add(cylinder((-2.0, base + h + 1.2, -2.5), 0.6, 1.6, segments=10), CONCRETE)
    root.add(box((0.0, base + 1.3, size / 2 + 0.05), (1.6, 2.6, 0.1)), mat("door", "#3b2a1c"))
    return root


def bank() -> Node:
    """two tiles wide (along x), vault room at the back, origin at the centre"""
    root = Node("bank")
    w, d, h = TILE * 2 - 3.0, 9.0, 7.0
    base = 0.15
    root.add(box((0.0, base + h / 2, -0.5), (w, h, d)), MARBLE)
    root.add(box((0.0, base + h + 0.35, -0.5), (w + 0.6, 0.7, d + 0.6)), mat("roof_green", "#3e6b5a", metallic=0.5, roughness=0.5))
    # pediment and columns facing +z
    root.add(box((0.0, base + h - 0.6, d / 2 + 1.2), (w * 0.7, 1.2, 2.4)), MARBLE)
    for i in range(6):
        x = -w * 0.3 + i * (w * 0.6) / 5
        root.add(cylinder((x, base + (h - 1.2) / 2, d / 2 + 1.6), 0.35, h - 1.2, segments=10), MARBLE)
    root.add(box((0.0, base + 0.1, d / 2 + 1.2), (w * 0.75, 0.2, 2.8)), MARBLE)
    root.add(box((0.0, base + h - 0.6, d / 2 + 2.45), (4.2, 0.8, 0.1)), GOLD)  # sign plate
    root.add(box((0.0, base + 1.5, d / 2 + 0.01), (2.4, 3.0, 0.05)), mat("bank_door", "#2d2419"))
    # a big $ on the roof so it reads from the sky
    dollar = mat("dollar", "#e0b43c", metallic=1.0, roughness=0.3, emissive="#ffc84a", strength=1.5)
    y = base + h + 0.75
    for (cx, cz, sx, sz) in [
        (0.0, -2.0, 3.0, 0.5), (0.0, -0.5, 3.0, 0.5), (0.0, 1.0, 3.0, 0.5),
        (-1.25, -1.25, 0.5, 1.5), (1.25, 0.25, 0.5, 1.5), (0.0, -0.5, 0.35, 4.6),
    ]:
        root.add(box((cx, y, cz), (sx, 0.1, sz)), dollar)
    return root


def vault_door() -> Node:
    root = Node("vault_door")
    root.add(cylinder((0.0, 1.2, 0.0), 1.1, 0.4, axis="z", segments=16), VAULT)
    root.add(cylinder((0.0, 1.2, 0.25), 0.25, 0.2, axis="z", segments=8), CHROME)
    for a in range(3):
        root.add(rotated_bar(a * math.pi / 3), CHROME)
    return root


def rotated_bar(angle) -> Geo:
    g = box((0.0, 0.0, 0.0), (0.08, 1.0, 0.06))
    c, s = math.cos(angle), math.sin(angle)
    g.positions = [(p[0] * c - p[1] * s, 1.2 + p[0] * s + p[1] * c, 0.33 + p[2]) for p in g.positions]
    g.normals = [(n[0] * c - n[1] * s, n[0] * s + n[1] * c, n[2]) for n in g.normals]
    return g


def street_lamp() -> Node:
    root = Node("street_lamp")
    root.add(cylinder((0.0, 2.5, 0.0), 0.08, 5.0, segments=8), LAMP_POST)
    root.add(box((0.0, 4.95, 0.6), (0.12, 0.1, 1.3)), LAMP_POST)
    root.add(box((0.0, 4.85, 1.15), (0.35, 0.12, 0.5)), LAMP_LIGHT)
    return root


def tree() -> Node:
    root = Node("tree")
    root.add(cylinder((0.0, 1.0, 0.0), 0.15, 2.0, segments=7), WOOD)
    root.add(box((0.0, 2.6, 0.0), (2.2, 1.6, 2.2)), LEAVES)
    root.add(box((0.3, 3.5, -0.2), (1.4, 0.9, 1.4)), LEAVES)
    return root


def cash() -> Node:
    root = Node("cash")
    root.add(box((0.0, 0.08, 0.0), (0.5, 0.16, 0.26)), MONEY)
    root.add(box((0.0, 0.08, 0.0), (0.12, 0.17, 0.27)), MONEY_BAND)
    root.add(box((0.0, 0.2, 0.0), (0.12, 0.06, 0.12)), mat("cash_glow", "#3fbf4a", emissive="#50ff60", strength=4.0))
    return root


def tracer() -> Node:
    """bullet streak, 1m long along +z, scaled by the game"""
    root = Node("tracer")
    root.add(box((0.0, 0.0, 0.5), (0.05, 0.05, 1.0)), mat("tracer", "#ffd080", emissive="#ffc060", strength=12.0))
    return root


def ground() -> Node:
    root = Node("ground")
    root.add(box((0.0, -0.3, 0.0), (1.0, 0.4, 1.0)), DIRT)
    return root


def marker() -> Node:
    """mission / objective arrow, bobs above things"""
    root = Node("marker")
    m = mat("marker", "#40ff60", emissive="#40ff60", strength=6.0)
    root.add(box((0.0, 0.6, 0.0), (0.25, 0.8, 0.25)), m)
    g = Geo()
    tip = (0.0, 0.0, 0.0)
    corners = [(-0.45, 0.25, -0.45), (0.45, 0.25, -0.45), (0.45, 0.25, 0.45), (-0.45, 0.25, 0.45)]
    for i in range(4):
        g.tri(tip, corners[(i + 1) % 4], corners[i])
    g.quad(corners[0], corners[1], corners[2], corners[3], (0, 1, 0))
    root.add(g, m)
    return root


def fx() -> Node:
    """holds the materials particle systems render with (.oxparticle files reference them by uuid). The quad
    itself is never spawned"""
    root = Node("fx")
    dot = Mat("fx_soft_dot", (1.0, 1.0, 1.0, 1.0), roughness=1.0, texture="soft_dot", alpha_mode="BLEND")
    g = Geo()
    g.quad((-0.5, 0.0, 0.5), (0.5, 0.0, 0.5), (0.5, 0.0, -0.5), (-0.5, 0.0, -0.5), (0, 1, 0))
    root.add(g, dot)
    return root


def blood(variant: int) -> Node:
    """a flat 1x1 m splat lying on the ground, alpha masked. Glossy, it's wet"""
    root = Node(f"blood_{variant}")
    m = Mat(f"blood_{variant}", srgb("#5a0404"), roughness=0.25, texture=f"blood_splat_{variant}", alpha_mode="MASK")
    g = Geo()
    g.quad((-0.5, 0.0, 0.5), (0.5, 0.0, 0.5), (0.5, 0.0, -0.5), (-0.5, 0.0, -0.5), (0, 1, 0))
    root.add(g, m)
    return root


def knife() -> Node:
    """combat knife held in the fist: origin at the grip, blade along -y (down the arm) and forward. The game parents it
    to the right arm's hand"""
    root = Node("knife")
    steel = mat("knife_steel", "#dfe3e8", metallic=1.0, roughness=0.2)
    grip = mat("knife_grip", "#1b1b1b", roughness=0.6)
    root.add(box((0.0, 0.0, 0.0), (0.05, 0.05, 0.14)), grip)
    root.add(box((0.0, 0.0, 0.09), (0.12, 0.05, 0.03)), mat("knife_guard", "#8a8f96", metallic=0.8, roughness=0.3))
    blade = Geo()
    blade.extend(box((0.0, 0.0, 0.24), (0.018, 0.07, 0.28)))
    root.add(blade, steel)
    return root


def all_models() -> dict[str, Node]:
    models = {}
    for name in CHARACTER_STYLES:
        models[f"Characters/{name}"] = character(name)
    for name in VEHICLE_STYLES:
        models[f"Vehicles/{name}"] = vehicle(name)
    models["Vehicles/wheel"] = wheel()
    models["City/road_straight"] = road("straight")
    models["City/road_cross"] = road("cross")
    models["City/block"] = sidewalk_block()
    models["City/park"] = park_block()
    for i in range(len(BUILDING_STYLES)):
        models[f"City/building_{i}"] = building(i)
    models["City/bank"] = bank()
    models["City/vault_door"] = vault_door()
    models["City/street_lamp"] = street_lamp()
    models["City/tree"] = tree()
    models["City/ground"] = ground()
    models["Props/cash"] = cash()
    models["Props/tracer"] = tracer()
    models["Props/marker"] = marker()
    models["Props/fx"] = fx()
    models["Props/knife"] = knife()
    for i in range(4):
        models[f"Props/blood_{i}"] = blood(i)
    return models


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT_DIR
    for rel, root in all_models().items():
        path = out / f"{rel}.glb"
        write_glb(path, root)
        print(f"wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
