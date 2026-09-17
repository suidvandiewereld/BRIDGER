"""Render a Decima skinned mesh .core (plus its .core.stream files) to PNG views.

    python tools/archive/mesh_preview.py <mesh.core> <stream dir> <out.png>
"""
import glob
import os
import re
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import core_types as C

STORAGE = {1: ("h", 2, True), 2: ("f", 4, False), 3: ("e", 2, False), 4: ("B", 1, True), 5: ("h", 2, False),
           7: ("B", 1, False), 8: ("H", 2, False), 9: ("H", 2, True)}


def align(n, a=256):
    return (n + a - 1) // a * a


def parse_vertex_array(p):
    o = 16
    count, nstreams, streaming = struct.unpack_from("<IIB", p, o); o += 9
    streams = []
    for _ in range(nstreams):
        flags, stride, nel = struct.unpack_from("<III", p, o); o += 12
        els = []
        for _ in range(nel):
            off, storage, slots, kind = struct.unpack_from("<BBBB", p, o); o += 4
            els.append((off, storage, slots, kind))
        o += 16
        if not streaming:
            o += stride * count
        streams.append((stride, els))
    return count, streams


def parse_index_array(p):
    count, flags, fmt, streaming = struct.unpack_from("<IIII", p, 16)
    return count, 4 if fmt == 1 else 2


def main(core, stream_dir, out):
    reg = C.Registry()
    table = reg.table()
    data = open(core, "rb").read()
    objs = [(table.get(h, "?"), data[off + 12:off + 12 + size]) for off, h, size in C.objects(data)]
    vas = [parse_vertex_array(p) for n, p in objs if n == "VertexArrayResource"]
    ias = [parse_index_array(p) for n, p in objs if n == "IndexArrayResource"]
    locations = []
    for n, p in objs:
        if n == "RegularSkinnedMeshResource":
            m = re.search(rb"([a-z0-9_/]+/msh_[a-z0-9_]+)", p)
            locations.append(m.group(1).decode() if m else None)
    per = len(vas) // max(1, len(locations))
    print("meshes", len(locations), "vertex arrays", len(vas), "index arrays", len(ias), "per mesh", per)

    views = []
    for mi, loc in enumerate(locations[:1]):
        name = loc.split("_")[-2]
        stream = open(glob.glob(os.path.join(stream_dir, "*%s*.core.stream" % name))[0], "rb").read()
        offset = 0
        tris_all, pos_all = [], []
        base = 0
        for pi in range(per):
            count, streams = vas[mi * per + pi]
            positions = None
            for stride, els in streams:
                block = stream[offset:offset + stride * count]
                for off, storage, slots, kind in els:
                    if kind == 0:
                        fmt, size, norm = STORAGE[storage]
                        arr = np.frombuffer(block, dtype=np.uint8).reshape(count, stride)
                        raw = arr[:, off:off + size * 3].copy().view({"f": np.float32, "h": np.int16, "e": np.float16}[fmt])
                        positions = raw.astype(np.float32).reshape(count, 3)
                        if norm:
                            positions /= 32767.0
                offset += align(stride * count)
            icount, isize = ias[mi * per + pi]
            idx = np.frombuffer(stream[offset:offset + icount * isize], dtype=np.uint32 if isize == 4 else np.uint16).astype(np.int64)
            offset += align(icount * isize)
            if positions is None:
                continue
            print("primitive", pi, "vertices", count, "indices", icount)
            tris_all.append(idx.reshape(-1, 3) + base)
            pos_all.append(positions)
            base += count
        pos = np.concatenate(pos_all)
        tris = np.concatenate(tris_all)
        views = [("front", (0, 2), 1), ("side", (1, 2), 0)]
        size = 900
        img = Image.new("RGB", (size * len(views), size), (18, 20, 24))
        lo, hi = np.percentile(pos, 0.5, axis=0), np.percentile(pos, 99.5, axis=0)
        scale = 0.9 * size / max(hi - lo)
        for vi, (label, (ax, ay), depth_axis) in enumerate(views):
            layer = Image.new("RGB", (size, size), (18, 20, 24))
            d = ImageDraw.Draw(layer)
            order = np.argsort(pos[tris].mean(1)[:, depth_axis])
            light = np.array([0.4, 0.8, 0.45]) / np.linalg.norm([0.4, 0.8, 0.45])
            for t in tris[order]:
                a, b, c = pos[t[0]], pos[t[1]], pos[t[2]]
                n = np.cross(b - a, c - a)
                ln = np.linalg.norm(n)
                if ln == 0:
                    continue
                shade = abs(float(np.dot(n / ln, light)))
                g = int(40 + 180 * shade)
                pts = [((v[ax] - (lo[ax] + hi[ax]) / 2) * scale + size / 2, size / 2 - (v[ay] - (lo[ay] + hi[ay]) / 2) * scale) for v in (a, b, c)]
                d.polygon(pts, fill=(g, g, g + 8))
            d.text((10, 10), label, fill=(200, 200, 200))
            img.paste(layer, (vi * size, 0))
        img.save(out)
        print("wrote", out, "bounds", lo, hi)


if __name__ == "__main__":
    main(*sys.argv[1:4])
