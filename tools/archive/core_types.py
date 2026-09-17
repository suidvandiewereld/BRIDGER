"""Decima RTTI type ids, as stored at the head of every object in a .core file.

A type id is MurmurHash3 x64-128 (seed 42) of a text description of the type: its name, base
chain, serialised members (by the type id of each member's type) and, for enums and containers,
their values or element type. The member order is the engine's own: fields collected base-first,
filtered, then ordered by offset with the engine's randomised quicksort, which matters because
properties share offset 0.

Built on data/rtti/ds/static.json plus the class version word read from ds.exe.

    python tools/archive/core_types.py <file.core>     name every object in a core file
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "rtti"))

from decima_archive import GAME, murmur3_x64_128

SAVE_STATE = 1 << 1
POINTERS = {"Ref", "cptr", "UUIDRef", "StreamingRef", "WeakPtr"}


def _split_template(name):
    m = re.match(r"^(\w+)<(.*)>$", name)
    return (m.group(1), m.group(2)) if m else (None, None)


def full_name(name):
    outer, inner = _split_template(name)
    return outer + "_" + full_name(inner) if outer else name


class Registry:
    def __init__(self, static_path=None):
        static = json.load(open(static_path or os.path.join(ROOT, "data", "rtti", "ds", "static.json")))
        self.types = static["types"]
        if "RegularSkinnedMeshResourceNG" in self.types:
            base = self.types.pop("RegularSkinnedMeshResource")
            derived = self.types.pop("RegularSkinnedMeshResourceNG")
            derived["bases"] = [dict(b, name="RegularSkinnedMeshResourceBase")
                                if b["name"] == "RegularSkinnedMeshResource" else b for b in derived["bases"]]
            self.types["RegularSkinnedMeshResourceBase"] = base
            self.types["RegularSkinnedMeshResource"] = derived
        self.image_base = static.get("image_base", 0x140000000)
        from pe import PE
        self.pe = PE(os.path.join(GAME, "ds.exe"))
        self.cache = {}
        self.pending = set()

    def version(self, name):
        return self.pe.u32(self.types[name]["rva"] + 0x0C)

    def ordered_fields(self, name):
        fields = []

        def collect(cls, base_offset):
            t = self.types[cls]
            for b in t.get("bases", []):
                collect(b["name"], base_offset + b["offset"])
            for m in t.get("members", []) or []:
                if m.get("category") is None:
                    continue
                fields.append((m, m["offset"] + base_offset))

        collect(name, 0)
        fields = [f for f in fields if not (f[0]["flags"] & SAVE_STATE)]
        _quick_sort(fields, lambda f: f[1])
        return fields

    def class_flags(self, name):
        t = self.types[name]
        if t.get("flags", 0) & 0xFFF:
            return t["flags"]
        for b in t.get("bases", []):
            f = self.class_flags(b["name"])
            if f:
                return f
        return 0

    def type_string(self, name):
        s = "RTTIBinaryVersion: 2, Type: %s\n" % full_name(name)
        t = self.types.get(name)
        if t and t["kind"] == "class":
            for m, _ in self.ordered_fields(name):
                cat = m["category"] or "(none)"
                s += "Attr: %s %s %s %d\n" % (self.hash_string(self.nested_id(m["type"])), cat, m["name"], m["flags"] & 0xDEB)
        s += self.base_info(name, 0)
        if t and t["kind"].startswith("enum"):
            if t["kind"] == "enum":
                s += "Enum-Size: %d\n" % t["size"]
            for v in t["values"]:
                s += "Enumeration-Value: %s %s\n" % (v["value"], v["name"])
        outer, inner = _split_template(name)
        if outer and outer not in POINTERS:
            s += "Contained-Type: %s\n" % self.hash_string(self.nested_id(inner))
        return s

    def base_info(self, name, indent):
        t = self.types.get(name)
        pad = "  " * indent
        if t and t["kind"] == "class":
            s = "%s%s %X %X\n" % (pad, full_name(name), self.version(name), self.class_flags(name))
            for b in t.get("bases", []):
                s += self.base_info(b["name"], indent + 1)
            return s
        return "%s%s 0 0\n" % (pad, full_name(name))

    def type_id(self, name):
        if name in self.cache:
            return self.cache[name]
        self.pending.add(name)
        try:
            h = murmur3_x64_128(self.type_string(name).encode())
        finally:
            self.pending.discard(name)
        self.cache[name] = h
        return h

    def nested_id(self, name):
        if name in self.pending:
            return murmur3_x64_128(("RTTIBinaryVersion: 2, Type: " + full_name(name)).encode())
        return self.type_id(name)

    @staticmethod
    def hash_string(h):
        return h.hex()

    def low(self, name):
        return struct.unpack_from("<Q", self.type_id(name))[0]

    def table(self):
        out = {}
        for n, t in self.types.items():
            if t["kind"] == "class":
                try:
                    out[self.low(n)] = n
                except Exception:
                    pass
        return out


def _quick_sort(items, key):
    state = [0]

    def partition(left, right):
        start, end = left - 1, right
        while True:
            start += 1
            while start < end and key(items[start]) < key(items[right]):
                start += 1
            end -= 1
            while end > start and key(items[right]) < key(items[end]):
                end -= 1
            if start >= end:
                break
            items[start], items[end] = items[end], items[start]
        items[start], items[right] = items[right], items[start]
        return start

    def sort(left, right):
        if left < right:
            state[0] = (0x19660D * state[0] + 0x3C6EF35F) & 0xFFFFFFFF
            pivot = (state[0] >> 8) % (right - left)
            items[left + pivot], items[right] = items[right], items[left + pivot]
            start = partition(left, right)
            sort(left, start - 1)
            sort(start + 1, right)

    sort(0, len(items) - 1)


def objects(data):
    off = 0
    while off + 12 <= len(data):
        h, size = struct.unpack_from("<QI", data, off)
        yield off, h, size
        off += 12 + size


if __name__ == "__main__":
    reg = Registry()
    table = reg.table()
    data = open(sys.argv[1], "rb").read()
    from collections import Counter
    counts = Counter()
    for off, h, size in objects(data):
        counts[table.get(h, "%016x" % h)] += 1
    for name, n in counts.most_common():
        print("%4d  %s" % (n, name))
