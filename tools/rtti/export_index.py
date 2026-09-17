"""Export compact name -> rva indexes from the static RTTI and symbol dumps.

    python tools/rtti/export_index.py

Writes data/rtti/ds/index.json and data/rtti/ds/symbol_index.json. The deploy target
copies them to <game>/Bridger/types.json and <game>/Bridger/symbols.json, where
bridger.dll uses them so that find_type() and find_symbol() work before any in-process
scan has run. Symbol keys are "Group::name", matching decima::find_symbol.
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
source = os.path.join(ROOT, "data/rtti/ds/static.json")
target = os.path.join(ROOT, "data/rtti/ds/index.json")

types = json.load(open(source))["types"]
index = {name: t["rva"] for name, t in types.items() if isinstance(t.get("rva"), int)}
with open(target, "w", newline="\n") as out:
    json.dump({"image_base": json.load(open(source))["image_base"], "types": index}, out,
              separators=(",", ":"), sort_keys=True)
print(f"wrote {target}: {len(index)} types")

symbol_source = os.path.join(ROOT, "data/rtti/ds/symbols.json")
symbol_target = os.path.join(ROOT, "data/rtti/ds/symbol_index.json")
document = json.load(open(symbol_source))
symbols = {}
for group_name, group in document["groups"].items():
    for symbol in group.get("symbols", []):
        name = symbol.get("name")
        rva = (symbol.get("exported") or {}).get("rva")
        if name and isinstance(rva, int):
            symbols[f"{group_name}::{name}"] = rva
with open(symbol_target, "w", newline=chr(10)) as out:
    json.dump({"image_base": document["image_base"], "symbols": symbols}, out,
              separators=(",", ":"), sort_keys=True)
shared = {}
for key, rva in symbols.items():
    shared.setdefault(rva, []).append(key)
stubbed = sum(len(keys) for keys in shared.values() if len(keys) > 3)
print(f"wrote {symbol_target}: {len(symbols)} symbols, "
      f"{len(shared)} distinct addresses, ~{stubbed} on shared stubs")
