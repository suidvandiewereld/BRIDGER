import argparse
import gzip
import json
import os
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DB = os.path.join(ROOT, "data", "rtti", "ds", "types.json")


def load():
    path = DB if os.path.exists(DB) else DB + ".gz"
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as fh:
        return json.load(fh)


def flatten(db, name, base=0, seen=None):
    seen = seen if seen is not None else set()
    ty = db.get(name)
    if ty is None or name in seen:
        return
    seen.add(name)
    for b in ty.get("bases", []):
        yield from flatten(db, b["name"], base + b["offset"], seen)
    for m in ty.get("members", []):
        if "offset" in m:
            yield base + m["offset"], m, name


def children(db):
    out = defaultdict(list)
    for name, ty in db.items():
        for b in ty.get("bases", []):
            out[b["name"]].append(name)
    return out


def cmd_stats(db, args):
    kinds = defaultdict(int)
    for ty in db.values():
        kinds[ty.get("type")] += 1
    print(f"types      {len(db)}")
    for kind, count in sorted(kinds.items(), key=lambda kv: -kv[1]):
        print(f"  {kind:<12} {count}")
    print(f"members    {sum(len(t.get('members', [])) for t in db.values())}")
    print(f"handlers   {sum(len(t.get('messages', [])) for t in db.values())}")
    roots = [n for n, t in db.items() if t.get("type") == "class" and not t.get("bases")]
    print(f"roots      {len(roots)}")


def cmd_find(db, args):
    query = args.query.lower()
    hits = [(n, t) for n, t in db.items() if query in n.lower()]
    if args.kind:
        hits = [(n, t) for n, t in hits if t.get("type") == args.kind]
    for name, ty in sorted(hits):
        bases = ", ".join(b["name"] for b in ty.get("bases", []))
        print(f"{ty.get('type'):<10} {name}" + (f" : {bases}" if bases else ""))
    print(f"\n{len(hits)} match(es)", file=sys.stderr)


def cmd_show(db, args):
    ty = db.get(args.name)
    if ty is None:
        sys.exit(f"unknown type: {args.name}")

    kind = ty.get("type")
    print(f"{kind} {args.name}   flags={ty.get('flags')} unknownC={ty.get('unknownC')}")

    if kind in ("enum", "enum flags"):
        print(f"  size {ty.get('size')} bytes")
        for m in ty.get("members", []):
            print(f"  {m['value']:>6}  {m['name']}")
        return

    if kind == "primitive":
        print(f"  parent type: {ty.get('parent_type')}")
        return

    for b in ty.get("bases", []):
        print(f"  base   +{b['offset']:<6} {b['name']}")

    rows = sorted(flatten(db, args.name), key=lambda r: r[0])
    if rows:
        print(f"  {'offset':>8}  {'type':<44} {'name':<40} declared in")
        for offset, member, owner in rows:
            suffix = " (property)" if member.get("is_property") else ""
            print(f"  {offset:>8}  {member.get('type', ''):<44} {member['name']:<40} {owner}{suffix}")

    if ty.get("messages"):
        print("  handles: " + ", ".join(ty["messages"]))


def cmd_tree(db, args):
    if args.up:
        def walk(name, depth=0):
            print("  " * depth + name)
            for b in db.get(name, {}).get("bases", []):
                walk(b["name"], depth + 1)
        walk(args.name)
        return

    kids = children(db)
    total = 0

    def walk(name, depth=0):
        nonlocal total
        print("  " * depth + name)
        total += 1
        if depth < args.depth:
            for child in sorted(kids.get(name, [])):
                walk(child, depth + 1)

    walk(args.name)
    print(f"\n{total} type(s)", file=sys.stderr)


def cmd_msg(db, args):
    hits = sorted(n for n, t in db.items() if args.name in t.get("messages", []))
    for name in hits:
        print(name)
    print(f"\n{len(hits)} handler(s)", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Query the Death Stranding Decima RTTI database.")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("stats", help="summarise the database").set_defaults(fn=cmd_stats)

    p = sub.add_parser("find", help="search type names")
    p.add_argument("query")
    p.add_argument("--kind", choices=["class", "enum", "enum flags", "primitive"])
    p.set_defaults(fn=cmd_find)

    p = sub.add_parser("show", help="print a flattened memory layout")
    p.add_argument("name")
    p.set_defaults(fn=cmd_show)

    p = sub.add_parser("tree", help="walk subclasses, or bases with --up")
    p.add_argument("name")
    p.add_argument("--up", action="store_true")
    p.add_argument("--depth", type=int, default=2)
    p.set_defaults(fn=cmd_tree)

    p = sub.add_parser("msg", help="list types handling a message")
    p.add_argument("name")
    p.set_defaults(fn=cmd_msg)

    args = parser.parse_args()
    args.fn(load(), args)


if __name__ == "__main__":
    main()
