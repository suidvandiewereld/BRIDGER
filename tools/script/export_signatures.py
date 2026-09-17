"""Writes data/rtti/ds/signatures.json from the runtime symbol dump.

Scripts call and hook engine functions by name, so the runtime needs every exported function's
parameter types to marshal arguments. symbols.json carries them as token lists; this keeps only
functions with an address and flattens each token to the type text the FFI classifier reads:

    {"EntitySymbols::Entity_ExportedGetPosition": ["WorldPosition", "Entity const *"], ...}

Token 0 is the result. Deployed beside bridger.dll as signatures.json.
"""

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--symbols", default=ROOT / "data/rtti/ds/symbols.json", type=Path)
    parser.add_argument("--out", default=ROOT / "data/rtti/ds/signatures.json", type=Path)
    args = parser.parse_args()

    dump = json.loads(args.symbols.read_text(encoding="utf-8"))
    signatures = {}
    for group, body in dump["groups"].items():
        for symbol in body["symbols"]:
            if symbol["kind"] != "function":
                continue
            definition = symbol["exported"]
            if not definition.get("rva") or not definition["tokens"]:
                continue
            tokens = [(t["type"] + t["modifiers"]).strip() for t in definition["tokens"]]
            signatures[f"{group}::{symbol['name']}"] = tokens

    args.out.write_text(json.dumps({"signatures": signatures}, separators=(",", ":"), sort_keys=True),
                        encoding="utf-8")
    print(f"wrote {args.out.relative_to(ROOT)}: {len(signatures)} signatures")


if __name__ == "__main__":
    main()
