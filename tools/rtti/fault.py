"""Turn a crash dialog into a diagnosis.

    python tools/rtti/fault.py 0x00007FF7DD7260B5 0x00007FF7DAE10000
    python tools/rtti/fault.py 0x29160b5                      (already an RVA)

Takes the "instruction location" and "Base" straight off the game's error dialog and
prints the containing function, the faulting instruction, and the lines around it with
RTTI and symbol names resolved. Pass extra addresses to walk a whole call stack:

    python tools/rtti/fault.py --base 0x7FF7DAE10000 0x7FF7DD7260B5 0x7FF7DD9E51C1

Set BRIDGER_GAME_DIR to override the game folder. Needs `pip install capstone`.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import disasm as D

CONTEXT = 6


def to_rva(value, base):
    """Dialog addresses are absolute; anything below the image size is already an RVA."""
    return value - base if base and value >= base else value


def report(rva):
    start = D.function_of(rva)
    if start is None:
        print(f"{rva:#x}  no .pdata entry — mid-function chunk, or not code")
        return
    name = D.name_of(start) or ""
    print(f"\n=== {rva:#x} in {start:#x} {name}".rstrip())
    lines = D.disasm(start, max_insns=4000).splitlines()
    hit = next((i for i, line in enumerate(lines) if line.startswith(f"{rva:08x}")), None)
    if hit is None:
        print(f"    ({len(lines)} instructions decoded from {start:#x}, {rva:#x} not among them)")
        return
    for i in range(max(0, hit - CONTEXT), min(len(lines), hit + CONTEXT + 1)):
        print(("  >>" if i == hit else "    ") + lines[i])


def main(argv):
    base = 0
    if "--base" in argv:
        at = argv.index("--base")
        base = int(argv[at + 1], 0)
        del argv[at:at + 2]
    values = [int(a, 0) for a in argv]
    if not values:
        sys.exit(__doc__)
    if base == 0 and len(values) == 2 and values[1] < values[0]:
        values, base = values[:1], values[1]
    for value in values:
        report(to_rva(value, base))


if __name__ == "__main__":
    main(sys.argv[1:])
