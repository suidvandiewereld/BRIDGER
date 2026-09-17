"""Check the vtable resolution the content layer relies on, without launching the game.

`src/content/content.cpp` finds a class's vtable by reading it out of the class's own
constructor: the engine loads it with `lea rax, [rip + disp32]` and stores it with
`mov [this], rax`. That is the one place in the content layer where a byte pattern stands in for
recorded data, so it is worth being able to say exactly how well it does.

A candidate is confirmed rather than guessed. Slot 0 of a Decima vtable is `GetRTTI`, emitted as
`lea rax, [rip + descriptor]; ret`, so reading it back says which class the vtable belongs to; a
candidate is accepted when that is this very class. Where GetRTTI has some other shape the check
falls back to "slot 0 is a code pointer".

    python tools/rtti/vtables.py
    python tools/rtti/vtables.py --exe "D:/games/Death Stranding/ds.exe" --list AIAirMover
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import gamedir
DEFAULT_EXE = os.path.join(gamedir.find() or ".", "ds.exe")
DEFAULT_DUMP = os.path.join(ROOT, "data", "rtti", "ds", "static.json")
EXECUTABLE = 0x20000000


class Image:
    """The parts of the PE the scan needs: section kinds, and reads by RVA.

    static.json records addresses as RVAs, so everything here works in RVA space. Vtable slots
    on disk are unrelocated VAs against the preferred image base, which is the one place the
    base has to be subtracted.
    """

    def __init__(self, path, image_base):
        self.pe = PE(path)
        self.image_base = image_base
        blob = self.pe.blob
        count = struct.unpack_from("<H", blob, self.pe.pe + 6)[0]
        table = self.pe.opt + struct.unpack_from("<H", blob, self.pe.pe + 20)[0]
        self.characteristics = {}
        for i in range(count):
            entry = table + i * 40
            name = blob[entry:entry + 8].rstrip(b"\0").decode()
            self.characteristics[name] = struct.unpack_from("<I", blob, entry + 36)[0]

    def read(self, rva, size):
        offset = self.pe.offset(rva)
        return b"" if offset is None else self.pe.blob[offset:offset + size]

    def kind(self, rva):
        name = self.pe.section_of(rva)
        if name is None:
            return None
        return "code" if self.characteristics.get(name, 0) & EXECUTABLE else "data"

    def section(self, rva):
        return self.pe.section_of(rva)


def rtti_from_vtable(image, vtable):
    """The descriptor slot 0's GetRTTI returns, or None when it is not in the usual shape."""
    slot = image.read(vtable, 8)
    if len(slot) != 8:
        return None
    getter = struct.unpack("<Q", slot)[0] - image.image_base
    if image.kind(getter) != "code":
        return None
    code = image.read(getter, 8)
    if len(code) < 8 or code[0:3] != b"\x48\x8d\x05" or code[7] != 0xC3:
        return None
    return getter + 7 + struct.unpack_from("<i", code, 3)[0]


def resolve(image, constructor, descriptor):
    """Returns (vtable, 'exact' | 'weak' | 'none'), mirroring content.cpp exactly."""
    if image.kind(constructor) != "code":
        return None, "none"
    code = image.read(constructor, 288)
    if not code:
        return None, "none"
    base = constructor

    for i in range(0, min(len(code) - 5, 24)):
        if code[i] != 0xE9:
            continue
        target = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        if image.kind(target) != "code":
            break
        body = image.read(target, 288)
        if body:
            code, base = body, target
        break
    if len(code) < 11:
        return None, "none"

    candidate = None
    weak = None
    for i in range(0, len(code) - 10):
        if code[i:i + 3] == b"\x48\x8d\x05":
            loaded = base + i + 7 + struct.unpack_from("<i", code, i + 3)[0]
            candidate = loaded if image.kind(loaded) == "data" else None
            continue
        if candidate is None or code[i] != 0x48 or code[i + 1] != 0x89:
            continue
        modrm = code[i + 2]
        if (modrm >> 6) != 0 or ((modrm >> 3) & 7) != 0 or (modrm & 7) in (4, 5):
            continue
        if rtti_from_vtable(image, candidate) == descriptor:
            return candidate, "exact"
        if weak is None:
            slot = image.read(candidate, 8)
            if len(slot) == 8 and image.kind(struct.unpack("<Q", slot)[0] - image.image_base) == "code":
                weak = candidate
    return (weak, "weak") if weak is not None else (None, "none")


def polymorphic(types, name, depth=0):
    """True when the class reaches RTTIObject, so it is expected to have a vtable at all."""
    if name == "RTTIObject":
        return True
    entry = types.get(name)
    if entry is None or depth > 12:
        return False
    return any(polymorphic(types, base["name"], depth + 1) for base in entry.get("bases", []))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--dump", default=DEFAULT_DUMP)
    parser.add_argument("--list", nargs="*", metavar="CLASS",
                        help="print the vtable for these classes instead of the summary")
    args = parser.parse_args()

    dump = json.load(open(args.dump, encoding="utf-8"))
    types = dump["types"]
    image = Image(args.exe, dump["image_base"])

    if args.list:
        for name in args.list:
            entry = types.get(name)
            if entry is None or not entry.get("constructor"):
                print(f"{name}: no class, or no constructor")
                continue
            vtable, how = resolve(image, entry["constructor"], entry["rva"])
            if vtable is None:
                print(f"{name}: unresolved")
                continue
            print(f"{name}: vtable at {vtable:#x} ({image.section(vtable)}), {how}, "
                  f"ctor {entry['constructor']:#x}")
        return 0

    tally = {"exact": 0, "weak": 0, "none": 0}
    poly = poly_missed = 0
    for name, entry in types.items():
        if entry.get("kind") != "class" or not entry.get("constructor"):
            continue
        _, how = resolve(image, entry["constructor"], entry["rva"])
        tally[how] += 1
        if polymorphic(types, name):
            poly += 1
            poly_missed += how == "none"

    total = sum(tally.values())
    print(f"classes with a constructor        {total}")
    print(f"  confirmed by GetRTTI            {tally['exact']}")
    print(f"  slot 0 is a code pointer only   {tally['weak']}")
    print(f"  unresolved                      {tally['none']}")
    print()
    print(f"polymorphic classes               {poly}")
    print(f"  unresolved                      {poly_missed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
