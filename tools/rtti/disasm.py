"""Disassemble ds.exe with capstone, annotated with RTTI and ExportedSymbol names.

    python tools/rtti/disasm.py fn 0x2123ed0 [0x...]      disassemble functions
    python tools/rtti/disasm.py handlers CameraEntity     message handler table of a class
    python tools/rtti/disasm.py callers 0x2123ed0         every direct call site in .text
    python tools/rtti/disasm.py calls 0x336b300           call targets of a function
    python tools/rtti/disasm.py walk 0x218a9d0 [depth]    caller tree, functions resolved via .pdata

Set BRIDGER_GAME_DIR to override the game folder. Needs `pip install capstone`.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from pe import PE

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
except ImportError:
    sys.exit("capstone is required: pip install capstone")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import gamedir
GAME = gamedir.require()

pe = PE(os.path.join(GAME, "ds.exe"))
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

names = {}
static = json.load(open(os.path.join(ROOT, "data/rtti/ds/static.json")))["types"]
for type_name, t in static.items():
    names.setdefault(t["rva"], f"RTTI<{type_name}>")
    if t.get("constructor"):
        names.setdefault(t["constructor"], f"{type_name}::rtti_ctor")

symbols_path = os.path.join(GAME, "Bridger/dumps/symbols.json")
if os.path.exists(symbols_path):
    dump = json.load(open(symbols_path))
    base = dump.get("image_base", 0)

    def walk(node):
        if isinstance(node, dict):
            address = node.get("address")
            if isinstance(address, int) and isinstance(node.get("name"), str):
                names.setdefault(address - base if address >= base else address, node["name"])
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(dump)

BRANCHES = {"call", "jmp", "je", "jne", "jz", "jnz", "ja", "jb", "jl", "jg", "jle", "jge", "jbe",
            "jae", "js", "jns"}


def name_of(rva):
    return names.get(rva)


def _ends_function(insn):
    if insn.mnemonic == "int3":
        return True
    if insn.mnemonic == "ret" or (insn.mnemonic == "jmp" and insn.operands
                                  and insn.operands[0].type == X86_OP_IMM):
        return pe.u8(insn.address + insn.size) in (0xCC, 0x00, None)
    return False


def disasm(rva, max_insns=600):
    code = pe.blob[pe.offset(rva):pe.offset(rva) + max_insns * 15]
    out = []
    for insn in md.disasm(code, rva):
        line = f"{insn.address:08x}  {insn.mnemonic:8s} {insn.op_str}"
        notes = []
        for op in insn.operands:
            if op.type == X86_OP_IMM and insn.mnemonic in BRANCHES and name_of(op.imm):
                notes.append(name_of(op.imm))
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                target = insn.address + insn.size + op.mem.disp
                label = name_of(target)
                notes.append(f"[{target:#x} {pe.section_of(target)}{' ' + label if label else ''}]")
                if insn.mnemonic in ("call", "jmp"):
                    pointer = pe.pointer(target)
                    if pointer:
                        notes.append(f"-> {pointer:#x} {name_of(pointer) or ''}")
        if notes:
            line += "   ; " + " ".join(notes)
        out.append(line)
        if _ends_function(insn):
            break
    return "\n".join(out)


def calls(rva, max_insns=4000):
    code = pe.blob[pe.offset(rva):pe.offset(rva) + max_insns * 15]
    seen = {}
    for insn in md.disasm(code, rva):
        if insn.mnemonic == "call" and insn.operands and insn.operands[0].type == X86_OP_IMM:
            seen.setdefault(insn.operands[0].imm, []).append(insn.address)
        if _ends_function(insn):
            break
    return seen


def handlers(class_name):
    t = static[class_name]
    count = pe.u8(t["rva"] + 8)
    table = pe.pointer(t["rva"] + 112)
    result = []
    for i in range(count):
        entry = table + i * 16
        message = pe.pointer(entry)
        handler = pe.pointer(entry + 8)
        message_name = pe.cstring(pe.pointer(message + 56)) if message else None
        result.append((message_name, handler))
    return result


def callers(target):
    vaddr, _, raddr, rsize = pe.range_of(".text")
    blob = pe.blob
    found = []
    i = blob.find(b"\xe8", raddr)
    while i != -1 and i < raddr + rsize:
        rva = vaddr + (i - raddr)
        if rva + 5 + struct.unpack_from("<i", blob, i + 1)[0] == target:
            found.append(rva)
        i = blob.find(b"\xe8", i + 1)
    return found


_pdata = None


def _load_pdata():
    global _pdata
    if _pdata is None:
        rva, size = struct.unpack_from("<II", pe.blob, pe.opt + 112 + 3 * 8)
        base = pe.offset(rva)
        entries = [struct.unpack_from("<III", pe.blob, base + i * 12) for i in range(size // 12)]
        _pdata = ([e[0] for e in entries], entries)
    return _pdata


def function_of(rva):
    """Start of the function containing rva, following chained unwind info to the root."""
    import bisect
    starts, entries = _load_pdata()
    i = bisect.bisect_right(starts, rva) - 1
    for _ in range(16):
        if i < 0 or not (entries[i][0] <= rva < entries[i][1]):
            return None
        begin, _, unwind = entries[i]
        off = pe.offset(unwind)
        if not (pe.blob[off] >> 3) & 4:
            return begin
        count = pe.blob[off + 2]
        rva = struct.unpack_from("<I", pe.blob, off + 4 + ((count + 1) & ~1) * 2)[0]
        i = bisect.bisect_right(starts, rva) - 1
    return None


def walk(rva, depth=3, indent=0, seen=None):
    seen = seen if seen is not None else set()
    parents = sorted({function_of(site) for site in callers(rva)} - {None})
    label = name_of(rva) or ""
    print("  " * indent + f"{rva:#x} {label} <- {len(parents)} caller function(s)")
    if rva in seen or indent >= depth:
        return
    seen.add(rva)
    for parent in parents[:8]:
        walk(parent, depth, indent + 1, seen)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    command, args = argv[1], argv[2:]
    if command == "fn":
        for arg in args:
            rva = int(arg, 16)
            print(f"==== {rva:#x} {name_of(rva) or ''}")
            print(disasm(rva, int(os.environ.get("MAXI", "600"))))
            print()
    elif command == "handlers":
        for class_name in args:
            print(f"### {class_name}")
            for message, handler in handlers(class_name):
                print(f"   {handler:#x}  {message}")
    elif command == "callers":
        for arg in args:
            sites = callers(int(arg, 16))
            print(f"{arg}: {len(sites)} call sites")
            for site in sites:
                print(f"   {site:#x}")
    elif command == "walk":
        walk(int(args[0], 16), int(args[1]) if len(args) > 1 else 3)
    elif command == "calls":
        for arg in args:
            for target, sites in sorted(calls(int(arg, 16)).items()):
                print(f"   {target:#x} x{len(sites)}  {name_of(target) or ''}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
