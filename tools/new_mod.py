import argparse
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]*$")


def substitute(text, values):
    for key, value in values.items():
        text = text.replace(key, value)
    return text


def game_directory():
    if os.environ.get("BRIDGER_GAME_DIR"):
        return os.environ["BRIDGER_GAME_DIR"]
    cache = os.path.join(ROOT, "build", "CMakeCache.txt")
    if os.path.exists(cache):
        with open(cache, encoding="utf-8", errors="replace") as fh:
            match = re.search(r"^BRIDGER_GAME_DIR:PATH=(.+)$", fh.read(), re.M)
        if match and match.group(1).strip():
            return match.group(1).strip()
    return None


def main():
    parser = argparse.ArgumentParser(description="Create a new Bridger mod from a template.")
    parser.add_argument("id", help="lowercase mod id, used for the folder and dll name")
    parser.add_argument("--name", help="display name")
    parser.add_argument("--author", default="")
    parser.add_argument("--description", default="")
    parser.add_argument("--lua", action="store_true",
                        help="a script mod: main.lua instead of a C++ project, created straight in the "
                             "game's Bridger/mods so it runs as soon as it is saved")
    parser.add_argument("--into", help="directory to create the mod in")
    args = parser.parse_args()

    if not IDENTIFIER.match(args.id):
        sys.exit("id must be lowercase letters, digits and underscores, starting with a letter")

    into = args.into
    if into is None and args.lua:
        game = game_directory()
        into = os.path.join(game, "Bridger", "mods") if game else os.path.join(ROOT, "mods")
    into = into or os.path.join(ROOT, "mods")

    destination = os.path.join(into, args.id)
    if os.path.exists(destination):
        sys.exit(f"{destination} already exists")

    values = {
        "__MOD_ID__": args.id,
        "__MOD_NAME__": args.name or args.id.replace("_", " ").title(),
        "__MOD_AUTHOR__": args.author,
        "__MOD_DESCRIPTION__": args.description or "A Bridger mod.",
        "__BRIDGER_LUA__": os.path.join(ROOT, "include", "bridger", "lua").replace("\\", "/"),
    }

    template = os.path.join(ROOT, "templates", "script" if args.lua else "mod")
    shutil.copytree(template, destination)
    for folder, _, files in os.walk(destination):
        for name in files:
            path = os.path.join(folder, name)
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(substitute(text, values))

    print(f"created {destination}")
    print()
    if args.lua:
        print("nothing to build. If the game is running it loads the mod within two seconds;")
        print("edit main.lua and save to reload. Open the folder in VS Code with the Lua extension")
        print("for completion across the engine.")
        return
    print("build and install it with:")
    print(f'  cmake -S "{destination}" -B "{destination}/build" -A x64 \\')
    print(f'        -DBRIDGER_DIR="{ROOT}" \\')
    print('        -DBRIDGER_GAME_DIR="<your Death Stranding folder>"')
    print(f'  cmake --build "{destination}/build" --config Release --target install_mod')


if __name__ == "__main__":
    main()
