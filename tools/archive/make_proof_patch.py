"""Build the proof patch: the pause menu's "Options" and "Quit Game" gain a [BRIDGER] tag.

    python tools/archive/make_proof_patch.py <out.bin>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import core_codec as K
import decima_archive as D

PATH = "localized/sentences/ds_ui/ds_menu_main/simpletext"
CHANGES = {"Options": "Options [BRIDGER]", "Quit Game": "Quit Game [BRIDGER]"}


def entries(blob):
    o, out = 0, []
    while o < len(blob):
        n = struct.unpack_from("<H", blob, o)[0]
        out.append([blob[o + 2:o + 2 + n], blob[o + 2 + n:o + 5 + n]])
        o += 5 + n
    return out


def pack(items):
    return b"".join(struct.pack("<H", len(text)) + text + tail for text, tail in items)


def main(out):
    data = None
    source = None
    for archive in D.archives():
        a = D.Archive(archive)
        if D.path_hash(PATH) in a.files:
            data = a.read(D.path_hash(PATH))
            source = a
            break
    codec = K.Codec()
    objs = codec.decode(data)
    changed = 0
    for obj in objs:
        items = entries(bytes.fromhex(obj["$extra"]))
        english = items[0][0].decode("utf-8")
        if english in CHANGES:
            items[0][0] = CHANGES[english].encode("utf-8")
            obj["$extra"] = pack(items).hex()
            changed += 1
    patched = codec.encode(objs)
    assert codec.encode(codec.decode(data)) == data
    prefetch_path = "prefetch/fullgame.prefetch"
    prefetch = source.read(D.path_hash(prefetch_path))
    pobjs = codec.decode(prefetch)
    plist = pobjs[0]
    index = next(i for i, f in enumerate(plist["Files"]) if f["Path"] == PATH)
    sizes = bytearray(bytes.fromhex(plist["Sizes"]["data"]))
    old = struct.unpack_from("<i", sizes, index * 4)[0]
    struct.pack_into("<i", sizes, index * 4, len(patched))
    plist["Sizes"]["data"] = sizes.hex()
    print("prefetch size for %s: %d -> %d" % (PATH, old, len(patched)))
    size = D.write_archive(out, {PATH: patched, prefetch_path: codec.encode(pobjs)})
    print("changed %d strings, wrote %s (%d bytes)" % (changed, out, size))


if __name__ == "__main__":
    main(sys.argv[1])
