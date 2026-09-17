"""Resolve a crash address inside bridger.dll to a function name.

The game's crash handler reports faults in our own DLL by raw address, which is
unresolvable on its own. bridger.dll logs where it loaded ("bridger.dll base 0x...") and
the linker writes build/Release/bridger.map, so the two together name the function.

    python tools/rtti/bridger_fault.py --base 0x7FFFE1D00000 0x7FFFE1D76FD2 0x7FFFE1D89EB9

Only the low 32 bits of each address are shown in the dialog, so passing those works too
as long as the base is given the same way:

    python tools/rtti/bridger_fault.py --base 0xE1D00000 0xE1D76FD2

Find the base in Bridger/bridger.log. Pass a whole call stack to see the path in.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAP = os.path.join(ROOT, "build/Release/bridger.map")

ENTRY = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")


def load_symbols():
    if not os.path.exists(MAP):
        sys.exit(f"no linker map at {MAP} — build bridger_core first")
    preferred = 0
    entries = []
    with open(MAP, "r", errors="replace") as handle:
        for line in handle:
            if "Preferred load address is" in line:
                preferred = int(line.strip().split()[-1], 16)
                continue
            found = ENTRY.match(line)
            if found:
                entries.append((int(found.group(2), 16) - preferred, found.group(1)))
    entries.sort()
    return entries


def resolve(entries, rva):
    """The map lists symbol starts, so the owner is the last one at or below the address."""
    low, high, best = 0, len(entries) - 1, None
    while low <= high:
        mid = (low + high) // 2
        if entries[mid][0] <= rva:
            best = entries[mid]
            low = mid + 1
        else:
            high = mid - 1
    return best


def main(argv):
    if "--base" not in argv:
        sys.exit(__doc__)
    at = argv.index("--base")
    base = int(argv[at + 1], 0)
    del argv[at:at + 2]
    if not argv:
        sys.exit(__doc__)

    entries = load_symbols()
    mask = 0xFFFFFFFF if base <= 0xFFFFFFFF else None
    for text in argv:
        value = int(text, 0)
        if mask is not None:
            value &= mask
        rva = value - base
        if rva < 0:
            print(f"{text}: below the given base")
            continue
        found = resolve(entries, rva)
        if found is None:
            print(f"{text}  rva {rva:#x}  (no symbol)")
        else:
            print(f"{text}  rva {rva:#x}  {found[1]}  +{rva - found[0]:#x}")


if __name__ == "__main__":
    main(sys.argv[1:])
