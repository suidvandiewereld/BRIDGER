"""Line up Decima program binding layouts with their shaders' DXBC reflection.

    python tools/rtti/layout_study.py captures/layouts
"""
import glob
import os
import struct
import sys

TYPES = {0: "cbuffer", 1: "tbuffer", 2: "texture", 3: "sampler", 4: "uav_rw", 5: "structured",
         6: "uav_structured", 7: "structured_rw"}


def rdef_bindings(blob):
    count = struct.unpack_from("<I", blob, 28)[0]
    for off in struct.unpack_from("<%dI" % count, blob, 32):
        if blob[off:off + 4] != b"RDEF":
            continue
        base = off + 8
        _cb_count, _cb_off, bind_count, bind_off = struct.unpack_from("<IIII", blob, base)
        minor, major = blob[base + 16], blob[base + 17]
        size = 40 if (major, minor) >= (5, 1) else 32
        out = []
        for i in range(bind_count):
            e = base + bind_off + i * size
            name_off, kind, ret, dim, samples, point, bind_count_, flags = struct.unpack_from("<8I", blob, e)
            space = struct.unpack_from("<I", blob, e + 32)[0] if size == 40 else 0
            name = blob[base + name_off:blob.index(b"\0", base + name_off)].decode()
            out.append((TYPES.get(kind, kind), point, space, bind_count_, dim, name))
        return out
    return []


def words(data, n=48):
    return struct.unpack_from("<%dI" % n, data, 0)


root = sys.argv[1]
for layout_path in sorted(glob.glob(os.path.join(root, "*.layout"))):
    tag = os.path.basename(layout_path)[:-len(".layout")]
    data = open(layout_path, "rb").read()
    print("=" * 100)
    print(tag)
    w = words(data)
    for i in range(0, 48, 8):
        print("  %03x: %s" % (i * 4, " ".join("%08x" % x for x in w[i:i + 8])))
    for stage in (2, 3):
        path = os.path.join(root, "%s.stage%d.dxbc" % (tag, stage))
        if os.path.exists(path):
            b = rdef_bindings(open(path, "rb").read())
            print("  stage%d: %d bindings" % (stage, len(b)))
            for kind, point, space, cnt, dim, name in b:
                print("     %-10s reg %3d space %d count %2d dim %2d %s" % (kind, point, space, cnt, dim, name))
