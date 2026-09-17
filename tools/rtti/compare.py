import argparse
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATIC = os.path.join(ROOT, "data", "rtti", "ds", "static.json")
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import gamedir
RUNTIME = os.path.join(gamedir.find() or ".", "Bridger", "dumps", "rtti.json")


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)["types"]


def signature(entry):
    if entry["kind"] == "class":
        return (
            "class",
            entry["size"],
            tuple((b["name"], b["offset"]) for b in entry["bases"]),
            tuple((m["name"], m["type"], m["offset"], m["flags"], m["category"])
                  for m in entry["members"]),
        )
    return (
        "enum",
        entry["size"],
        tuple((v["name"], v["value"]) for v in entry["values"]),
    )


def main():
    parser = argparse.ArgumentParser(description="Diff a runtime RTTI dump against the static one.")
    parser.add_argument("--static", default=STATIC)
    parser.add_argument("--runtime", default=RUNTIME)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    static = load(args.static)
    runtime = load(args.runtime)

    print(f"static  {len(static)} types")
    print(f"runtime {len(runtime)} types")

    only_runtime = sorted(set(runtime) - set(static))
    only_static = sorted(set(static) - set(runtime))
    shared = sorted(set(static) & set(runtime))

    print(f"\nonly in runtime: {len(only_runtime)}")
    print(f"only in static:  {len(only_static)}")

    differing = [n for n in shared if signature(static[n]) != signature(runtime[n])]
    print(f"shared but differing: {len(differing)} of {len(shared)}")

    registered = [n for n in shared if runtime[n].get("id", 0xFFFFFFFF) != 0xFFFFFFFF]
    print(f"shared with a runtime type id: {len(registered)}")

    if args.list:
        print("\nonly in runtime:")
        for name in only_runtime:
            print(f"  {name}")
        print("\nonly in static:")
        for name in only_static:
            print(f"  {name}")
        print("\ndiffering:")
        for name in differing[:40]:
            print(f"  {name}")


if __name__ == "__main__":
    main()
