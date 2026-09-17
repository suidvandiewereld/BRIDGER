import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import gamedir
DEFAULT_EXE = os.path.join(gamedir.find() or ".", "ds.exe")
DEFAULT_OUT = os.path.join(ROOT, "data", "rtti", "ds", "static.json")
REFERENCE = os.path.join(ROOT, "data", "rtti", "ds", "types.json")

KIND_ATOM = 0
KIND_POINTER = 1
KIND_CONTAINER = 2
KIND_ENUM = 3
KIND_CLASS = 4
KIND_ENUM_FLAGS = 5

CLASS_NAME = 56
CLASS_BASES = 88
CLASS_MEMBERS = 96
MEMBER_STRIDE = 56
MEMBER_GETTER = 24
MEMBER_SETTER = 32
ATOM_NAME = 16
ENUM_NAME = 16
ENUM_VALUES = 24
ENUM_STRIDE = 40

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
UNASSIGNED = 0xFFFFFFFF


class Dumper:
    def __init__(self, exe):
        self.pe = PE(exe)
        self.classes = {}
        self.enums = {}
        self.names = {}
        self.text = self.text_range()

    def text_range(self):
        for name, vaddr, vsize, raddr, rsize in self.pe.sections:
            if name == ".text":
                return (vaddr, vaddr + vsize)
        return (0, 0)

    def code_pointer(self, rva):
        target = self.pe.pointer(rva)
        if target is None:
            return None
        return target if self.text[0] <= target < self.text[1] else None

    def scan(self):
        pe = self.pe
        vaddr, vsize, raddr, rsize = pe.range_of(".data")
        for rva in range(vaddr, vaddr + rsize - 128, 8):
            if pe.u32(rva) != UNASSIGNED:
                continue
            kind = pe.u8(rva + 4)
            if kind == KIND_CLASS:
                if pe.u8(rva + 10) != 0xFF or pe.u8(rva + 11) != 0:
                    continue
                name = self.read_name(rva + CLASS_NAME)
                if name:
                    self.classes[rva] = name
                    self.names[rva] = name
            elif kind in (KIND_ENUM, KIND_ENUM_FLAGS):
                name = self.read_name(rva + ENUM_NAME)
                if name:
                    self.enums[rva] = name
                    self.names[rva] = name

    def read_name(self, slot):
        target = self.pe.pointer(slot)
        if target is None or self.pe.section_of(target) != ".rdata":
            return None
        text = self.pe.cstring(target)
        return text if text and IDENTIFIER.match(text) else None

    def type_name(self, rva, depth=0):
        if rva is None or depth > 8:
            return None
        if rva in self.names:
            return self.names[rva]
        kind = self.pe.u8(rva + 4)
        if kind in (KIND_POINTER, KIND_CONTAINER):
            item = self.type_name(self.pe.pointer(rva + 8), depth + 1)
            data = self.pe.pointer(rva + 16)
            label = self.read_name(data) if data is not None else None
            if label and item:
                return f"{label}<{item}>"
            return None
        if kind == KIND_ATOM:
            return self.read_name(rva + ATOM_NAME)
        return None

    def bases(self, rva):
        count = self.pe.u8(rva + 5)
        array = self.pe.pointer(rva + CLASS_BASES)
        if not count or array is None:
            return []
        out = []
        for i in range(count):
            entry = array + i * 16
            out.append({
                "name": self.type_name(self.pe.pointer(entry)),
                "offset": self.pe.u32(entry + 8),
            })
        return out

    def members(self, rva):
        count = self.pe.u8(rva + 6)
        array = self.pe.pointer(rva + CLASS_MEMBERS)
        if not count or array is None:
            return []
        out = []
        category = ""
        for i in range(count):
            entry = array + i * MEMBER_STRIDE
            offset = self.pe.offset(entry)
            if offset is None:
                break
            type_ptr, field_offset, flags = struct.unpack_from("<QHH", self.pe.blob, offset)
            name_ptr = self.pe.pointer(entry + 16)
            name = self.pe.cstring(name_ptr) if name_ptr is not None else None
            if type_ptr == 0:
                category = name or ""
                continue
            getter = self.code_pointer(entry + MEMBER_GETTER)
            setter = self.code_pointer(entry + MEMBER_SETTER)
            out.append({
                "name": name,
                "type": self.type_name(type_ptr - self.pe.image_base),
                "offset": field_offset,
                "flags": flags,
                "category": category,
                "property": getter is not None,
                "getter": getter,
                "setter": setter,
            })
        return out

    def values(self, rva):
        count = self.pe.u8(rva + 6)
        array = self.pe.pointer(rva + ENUM_VALUES)
        if not count or array is None:
            return []
        out = []
        for i in range(count):
            entry = array + i * ENUM_STRIDE
            name_ptr = self.pe.pointer(entry + 8)
            out.append({
                "name": self.pe.cstring(name_ptr) if name_ptr is not None else None,
                "value": self.pe.i32(entry),
            })
        return out

    def build(self):
        types = {}
        for rva, name in self.classes.items():
            types[name] = {
                "kind": "class",
                "rva": rva,
                "size": self.pe.u32(rva + 16),
                "alignment": self.pe.u16(rva + 20),
                "flags": self.pe.u16(rva + 22),
                "message_handlers": self.pe.u8(rva + 8),
                "constructor": self.pe.pointer(rva + 24),
                "destructor": self.pe.pointer(rva + 32),
                "bases": self.bases(rva),
                "members": self.members(rva),
            }
        for rva, name in self.enums.items():
            types[name] = {
                "kind": "enum flags" if self.pe.u8(rva + 4) == KIND_ENUM_FLAGS else "enum",
                "rva": rva,
                "size": self.pe.u8(rva + 5),
                "values": self.values(rva),
            }
        return types


def validate(types, reference):
    ref = json.load(open(reference, encoding="utf-8"))
    report = {"missing": [], "member_mismatch": [], "value_mismatch": [], "extra": []}

    for name, want in ref.items():
        if want["type"] == "primitive":
            continue
        got = types.get(name)
        if got is None:
            report["missing"].append(name)
            continue
        if want["type"] == "class":
            expected = [m for m in want.get("members", []) if "offset" in m]
            actual = got["members"]
            if len(expected) != len(actual) or any(
                a["name"] != b["name"] or a["offset"] != b["offset"]
                or a["flags"] != b["flags"] or a["category"] != b["category"]
                or a["type"] != b["type"]
                for a, b in zip(expected, actual)
            ):
                report["member_mismatch"].append(name)
        else:
            expected = want.get("members", [])
            actual = got["values"]
            if len(expected) != len(actual) or any(
                a["name"] != b["name"] or a["value"] != b["value"]
                for a, b in zip(expected, actual)
            ):
                report["value_mismatch"].append(name)

    report["extra"] = [n for n in types if n not in ref]
    return report


def main():
    parser = argparse.ArgumentParser(description="Statically dump Decima RTTI from ds.exe.")
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--validate", action="store_true")
    args = parser.parse_args()

    dumper = Dumper(args.exe)
    dumper.scan()
    print(f"located {len(dumper.classes)} classes and {len(dumper.enums)} enums")

    types = dumper.build()
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump({"image_base": dumper.pe.image_base, "types": types}, fh, indent=1, sort_keys=True)
    print(f"wrote {args.out} ({len(types)} types)")

    if args.validate:
        report = validate(types, REFERENCE)
        for key, values in report.items():
            print(f"{key}: {len(values)}" + (f"  e.g. {values[:4]}" if values else ""))


if __name__ == "__main__":
    main()
