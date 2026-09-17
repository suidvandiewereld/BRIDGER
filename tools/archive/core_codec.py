"""Decode and encode Decima .core files against the RTTI schema.

A .core file is a flat list of objects: [u64 type id][u32 size][payload]. A payload is the class's
serialised fields in engine order (core_types.Registry.ordered_fields), followed, for classes with a
MsgReadBinary handler, by that handler's own binary data, kept here as `$extra` bytes.

Values decode to plain Python: numbers, bools, str, bytes, lists, dicts ({"$type": name, field: v}),
and references ({"$ref": kind, "uuid": hex, "path": str}). encode(decode(x)) == x, byte for byte,
which is how the codec is checked (see `roundtrip`).

    python tools/archive/core_codec.py dump <file.core> [--max-array N]
    python tools/archive/core_codec.py roundtrip <file.core>...
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import core_types as C

NUMBERS = {
    "bool": "?", "int8": "b", "uint8": "B", "int16": "h", "uint16": "H", "int": "i", "int32": "i",
    "uint": "I", "uint32": "I", "int64": "q", "uint64": "Q", "float": "f", "double": "d",
    "HalfFloat": "e", "wchar": "H", "ucs4": "I", "tchar": "b",
}
ALIASES = {
    "AnimationEventID": "int", "AnimationMessageID": "int", "AnimationMessagePresetID": "int",
    "AnimationNodeID": "uint16", "AnimationSet": "int", "AnimationTagID": "int", "AnimationVariableID": "int",
    "CommandLine": "String", "EntitySoundID": "int", "EntitySoundParamID": "int", "Filename": "String",
    "LinearGainFloat": "float", "MaterialType": "uint16", "MusicTime": "int64",
    "PhysicsCollisionFilterInfo": "uint32", "ProgramParameterHandle": "uint32", "SlowMotionHandle": "int",
    "SoundGroupIndex": "int8", "intptr": "int64", "uintptr": "uint64",
}
REF_KINDS = {0: None, 1: "internal", 2: "external", 5: "internal-ref", 3: "external-ref"}
REF_CODES = {v: k for k, v in REF_KINDS.items()}
POINTERS = C.POINTERS


def crc32c(data):
    table = crc32c.table
    if table is None:
        table = crc32c.table = []
        for i in range(256):
            c = i
            for _ in range(8):
                c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
            table.append(c)
    crc = 0
    for b in data:
        crc = (crc >> 8) ^ table[(crc ^ b) & 0xFF]
    return crc & 0x7FFFFFFF


crc32c.table = None


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise EOFError("read past end (%d + %d > %d)" % (self.pos, n, len(self.data)))
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def unpack(self, fmt):
        size = struct.calcsize("<" + fmt)
        return struct.unpack("<" + fmt, self.take(size))


class Codec:
    def __init__(self, registry=None):
        self.reg = registry or C.Registry()
        self.by_id = self.reg.table()
        self.unknown = set()

    def read_value(self, r, type_name):
        type_name = ALIASES.get(type_name, type_name)
        if type_name == "uint128":
            return r.take(16).hex()
        if type_name in NUMBERS:
            return r.unpack(NUMBERS[type_name])[0]
        if type_name == "String":
            n = r.unpack("I")[0]
            if n == 0:
                return ""
            r.unpack("I")
            return r.take(n).decode("utf-8", "surrogateescape")
        if type_name == "WString":
            n = r.unpack("I")[0]
            return r.take(n * 2).decode("utf-16-le", "surrogatepass")
        outer, inner = C._split_template(type_name)
        if outer in POINTERS:
            code = r.unpack("B")[0]
            kind = REF_KINDS.get(code, code)
            if kind is None:
                return None
            ref = {"$ref": kind, "uuid": r.take(16).hex()}
            if code in (2, 3):
                ref["path"] = self.read_value(r, "String")
            return ref
        if outer == "Array":
            n = r.unpack("I")[0]
            if inner in NUMBERS and NUMBERS[inner] != "?":
                fmt = NUMBERS[inner]
                return {"$packed": inner, "data": r.take(n * struct.calcsize(fmt)).hex()}
            return [self.read_value(r, inner) for _ in range(n)]
        if outer in ("HashMap", "HashSet"):
            n = r.unpack("I")[0]
            return {"$hashed": [(r.unpack("I")[0], self.read_value(r, inner)) for _ in range(n)]}
        t = self.reg.types.get(type_name)
        if t is None:
            self.unknown.add(type_name)
            raise KeyError("unknown type %s" % type_name)
        if t["kind"].startswith("enum"):
            fmt = {1: "B", 2: "H", 4: "I"}[t["size"]]
            return r.unpack(fmt)[0]
        return self.read_class(r, type_name)

    def read_class(self, r, type_name):
        out = {"$type": type_name}
        for m, _ in self.reg.ordered_fields(type_name):
            out[m["name"]] = self.read_value(r, m["type"])
        return out

    def decode(self, data):
        objects = []
        for off, h, size in C.objects(data):
            name = self.by_id.get(h)
            payload = data[off + 12:off + 12 + size]
            if name is None:
                objects.append({"$type": None, "$id": "%016x" % h, "$raw": payload.hex()})
                continue
            r = Reader(payload)
            obj = self.read_class(r, name)
            if r.pos < len(payload):
                obj["$extra"] = payload[r.pos:].hex()
            objects.append(obj)
        return objects

    def write_value(self, out, type_name, v):
        type_name = ALIASES.get(type_name, type_name)
        if type_name == "uint128":
            out += bytes.fromhex(v)
            return
        if type_name in NUMBERS:
            out += struct.pack("<" + NUMBERS[type_name], v)
            return
        if type_name == "String":
            b = v.encode("utf-8", "surrogateescape")
            out += struct.pack("<I", len(b))
            if b:
                out += struct.pack("<I", crc32c(b)) + b
            return
        if type_name == "WString":
            b = v.encode("utf-16-le", "surrogatepass")
            out += struct.pack("<I", len(b) // 2) + b
            return
        outer, inner = C._split_template(type_name)
        if outer in POINTERS:
            if v is None:
                out += b"\0"
                return
            code = REF_CODES.get(v["$ref"], v["$ref"])
            out += struct.pack("<B", code) + bytes.fromhex(v["uuid"])
            if code in (2, 3):
                self.write_value(out, "String", v["path"])
            return
        if outer == "Array":
            if isinstance(v, dict) and "$packed" in v:
                raw = bytes.fromhex(v["data"])
                out += struct.pack("<I", len(raw) // struct.calcsize(NUMBERS[inner])) + raw
                return
            out += struct.pack("<I", len(v))
            for item in v:
                self.write_value(out, inner, item)
            return
        if outer in ("HashMap", "HashSet"):
            entries = v["$hashed"]
            out += struct.pack("<I", len(entries))
            for checksum, item in entries:
                out += struct.pack("<I", checksum)
                self.write_value(out, inner, item)
            return
        t = self.reg.types[type_name]
        if t["kind"].startswith("enum"):
            out += struct.pack("<" + {1: "B", 2: "H", 4: "I"}[t["size"]], v)
            return
        self.write_class(out, type_name, v)

    def write_class(self, out, type_name, obj):
        for m, _ in self.reg.ordered_fields(type_name):
            self.write_value(out, m["type"], obj[m["name"]])

    def encode(self, objects):
        out = bytearray()
        for obj in objects:
            if obj["$type"] is None:
                payload = bytes.fromhex(obj["$raw"])
                h = int(obj["$id"], 16)
            else:
                body = bytearray()
                self.write_class(body, obj["$type"], obj)
                body += bytes.fromhex(obj.get("$extra", ""))
                payload = bytes(body)
                h = self.reg.low(obj["$type"])
            out += struct.pack("<QI", h, len(payload)) + payload
        return bytes(out)


def _trim(v, max_array):
    if isinstance(v, list):
        items = [_trim(x, max_array) for x in v[:max_array]]
        return items + (["... %d more" % (len(v) - max_array)] if len(v) > max_array else [])
    if isinstance(v, dict):
        return {k: (x[:64] + "..." if isinstance(x, str) and k in ("data", "$extra", "$raw") and len(x) > 64 else _trim(x, max_array))
                for k, x in v.items()}
    return v


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    codec = Codec()
    if argv[1] == "dump":
        max_array = int(argv[argv.index("--max-array") + 1]) if "--max-array" in argv else 8
        objs = codec.decode(open(argv[2], "rb").read())
        print(json.dumps([_trim(o, max_array) for o in objs], indent=1))
    elif argv[1] == "roundtrip":
        ok = True
        for path in argv[2:]:
            data = open(path, "rb").read()
            try:
                objs = codec.decode(data)
                again = codec.encode(objs)
                same = again == data
                extras = sum(1 for o in objs if "$extra" in o)
                raw = sum(1 for o in objs if o["$type"] is None)
                print("%-60s %s  objects %d  with extra %d  undecoded %d" % (
                    os.path.basename(path), "OK  " if same else "DIFF", len(objs), extras, raw))
                ok &= same
            except Exception as e:
                print("%-60s FAIL %r" % (os.path.basename(path), e))
                ok = False
        if codec.unknown:
            print("unknown types:", sorted(codec.unknown))
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main(sys.argv)
