"""Rebuild everything derived from the game that the repository does not ship.

    python tools/bootstrap.py [--force]

A fresh clone carries no type database, no engine headers and no Lua types. This rebuilds all of
them from your own install, in two runs:

  Run 1  reads ds.exe and writes data/rtti/ds/static.json. Then:
           cmake --build build --config Release --target deploy
         launch the game once and quit. The core writes <game>/Bridger/dumps/symbols.json on that
         launch (this script turns "dump_on_start" on in <game>/Bridger/config.json for you).

  Run 2  copies that dump in, then writes index.json, symbol_index.json, signatures.json,
         include/bridger/lua/engine.d.lua and include/decima/. Deploy once more and you are done.

Steps whose output already exists are skipped. --force redoes them all.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gamedir  # noqa: E402

DATA = ROOT / "data" / "rtti" / "ds"
STATIC = DATA / "static.json"
SYMBOLS = DATA / "symbols.json"
INDEX = DATA / "index.json"
SYMBOL_INDEX = DATA / "symbol_index.json"
SIGNATURES = DATA / "signatures.json"
LUA_TYPES = ROOT / "include" / "bridger" / "lua" / "engine.d.lua"
HEADERS = ROOT / "include" / "decima" / "decima.h"

PYTHON = sys.executable


def run(script, *args):
    print(f"> {script} {' '.join(args)}")
    subprocess.run([PYTHON, str(ROOT / script), *args], check=True, cwd=ROOT)


def set_dump_on_start(bridger_dir: Path, value: bool) -> bool:
    """Flip dump_on_start in <game>/Bridger/config.json. Returns False if the file is unreadable."""
    config = bridger_dir / "config.json"
    document = {}
    if config.exists():
        try:
            document = json.loads(config.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return False
    if document.get("dump_on_start", False) == value:
        return True
    document["dump_on_start"] = value
    bridger_dir.mkdir(parents=True, exist_ok=True)
    config.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true", help="redo steps whose output exists")
    args = parser.parse_args()

    game = gamedir.find()
    if not game:
        sys.exit("Death Stranding not found; set BRIDGER_GAME_DIR to the install folder")
    game = Path(game)
    exe = game / "ds.exe"
    if not exe.exists():
        sys.exit(f"no ds.exe in {game}")
    bridger_dir = game / "Bridger"
    DATA.mkdir(parents=True, exist_ok=True)

    # 1. The static dump needs only the executable. Validation against the Decima Workshop
    #    reference is skipped because that reference is not shipped either.
    if args.force or not STATIC.exists():
        run("tools/rtti/dump_static.py", "--exe", str(exe), "--out", str(STATIC))
    else:
        print(f"have {STATIC.relative_to(ROOT)}")

    # 2. The export table comes from the running game.
    if args.force or not SYMBOLS.exists():
        dumped = bridger_dir / "dumps" / "symbols.json"
        if dumped.exists():
            shutil.copyfile(dumped, SYMBOLS)
            print(f"copied {dumped} -> {SYMBOLS.relative_to(ROOT)}")
            set_dump_on_start(bridger_dir, False)
        else:
            if set_dump_on_start(bridger_dir, True):
                print(f'set "dump_on_start": true in {bridger_dir / "config.json"}')
            else:
                print(f'could not edit {bridger_dir / "config.json"}; add "dump_on_start": true by hand')
            print()
            print("Next, deploy and launch the game once:")
            print("  cmake --build build --config Release --target deploy")
            print("  (launch Death Stranding, wait for the main menu, quit)")
            print(f"The core writes {dumped} on that launch. Then run this script again.")
            return
    else:
        print(f"have {SYMBOLS.relative_to(ROOT)}")

    # 3. Everything else derives from those two files.
    if args.force or not (INDEX.exists() and SYMBOL_INDEX.exists()):
        run("tools/rtti/export_index.py")
    if args.force or not SIGNATURES.exists():
        run("tools/script/export_signatures.py")
    if args.force or not LUA_TYPES.exists():
        run("tools/script/gen_luals.py")
    if args.force or not HEADERS.exists():
        run("tools/codegen/gen_headers.py")

    print()
    print("Database, Lua types and engine headers are in place. Deploy them:")
    print("  cmake --build build --config Release --target deploy")


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
