import argparse
import json
import os
from collections import Counter

import sys as _sys, os as _os
_sys.path.insert(0, _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import gamedir
DEFAULT = os.path.join(gamedir.find() or ".", "Bridger", "dumps", "symbols.json")


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def each(groups):
    for name, group in groups.items():
        for symbol in group["symbols"]:
            yield name, group, symbol


def location(symbol):
    exported = symbol["exported"]
    if exported.get("rva"):
        return f'rva={exported["rva"]:#x}'
    internal = symbol["internal"]
    if internal.get("rva"):
        return f'rva={internal["rva"]:#x} (internal)'
    return "no address"


def render(symbol):
    tokens = symbol["exported"]["tokens"] or symbol["internal"]["tokens"]
    if not tokens:
        return ""
    def spell(token):
        return (token["type"] + token["modifiers"]).strip()
    returns = spell(tokens[0])
    params = ", ".join(spell(t) for t in tokens[1:]) or "void"
    return f'{returns} {symbol["name"]}({params})'


def cmd_stats(data, args):
    groups = data["groups"]
    kinds = Counter(s["kind"] for _, _, s in each(groups))
    print(f"groups   {len(groups)}")
    print(f"symbols  {sum(len(g['symbols']) for g in groups.values())}")
    for kind, count in kinds.most_common():
        print(f"  {kind:<12} {count}")
    addressed = sum(1 for _, _, s in each(groups) if s["exported"].get("rva"))
    print(f"with an image address {addressed}")


def cmd_find(data, args):
    query = args.query.lower()
    hits = [
        (g, s) for g, _, s in each(data["groups"])
        if query in s["name"].lower() or query in g.lower()
    ]
    if args.kind:
        hits = [(g, s) for g, s in hits if s["kind"] == args.kind]
    for group, symbol in sorted(hits, key=lambda h: (h[0], h[1]["name"])):
        print(f'{symbol["kind"]:<10} {group}::{symbol["name"]}  {location(symbol)}')
        signature = render(symbol)
        if signature:
            print(f"           {signature}")
    print(f"\n{len(hits)} match(es)")


def cmd_functions(data, args):
    hits = [
        (g, s) for g, _, s in each(data["groups"])
        if s["kind"] in ("function", "variable")
    ]
    for group, symbol in sorted(hits, key=lambda h: (h[0], h[1]["name"])):
        print(f'{symbol["kind"]:<9} {group}::{symbol["name"]}  {location(symbol)}')
        signature = render(symbol)
        if signature:
            print(f"          {signature}")
    print(f"\n{len(hits)} symbol(s)")


def cmd_group(data, args):
    group = data["groups"].get(args.name)
    if group is None:
        raise SystemExit(f"unknown group: {args.name}")
    print(f'{args.name}  address={group["address"]:#x} mask={group["export_mask"]} '
          f'symbols={len(group["symbols"])}')
    for symbol in group["symbols"]:
        print(f'  {symbol["kind"]:<10} {symbol["name"]:<50} {location(symbol)}')


def main():
    parser = argparse.ArgumentParser(description="Query the Decima ExportedSymbols dump.")
    parser.add_argument("--dump", default=DEFAULT)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("stats").set_defaults(fn=cmd_stats)

    p = sub.add_parser("find", help="search symbol and group names")
    p.add_argument("query")
    p.add_argument("--kind")
    p.set_defaults(fn=cmd_find)

    sub.add_parser("functions", help="list callable functions and variables").set_defaults(fn=cmd_functions)

    p = sub.add_parser("group", help="list one group")
    p.add_argument("name")
    p.set_defaults(fn=cmd_group)

    args = parser.parse_args()
    args.fn(load(args.dump), args)


if __name__ == "__main__":
    main()
