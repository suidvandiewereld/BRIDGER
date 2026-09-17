"""Drive the running game from a terminal: a Lua console inside Death Stranding.

    python tools/bridger.py                          interactive console (Ctrl+C or exit to leave)
    python tools/bridger.py --target my_mod          the console, inside a script mod's own state
    python tools/bridger.py eval "game.player_entity()"
    python tools/bridger.py run sketch.lua           run a file once, in the console state
    python tools/bridger.py scripts                  every mod, and each script's state
    python tools/bridger.py reload my_mod
    python tools/bridger.py log -f                   follow bridger.log
    python tools/bridger.py console -f               follow script output and errors
    python tools/bridger.py new my_script            scaffold Bridger/scripts/my_script.lua

The game side is the named pipe \\\\.\\pipe\\bridger that bridger.dll opens at startup: one JSON
request per line, one JSON response per line. Nothing here needs anything beyond the standard library.
"""

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

PIPE = r"\\.\pipe\bridger"
ROOT = Path(__file__).resolve().parents[1]

RESET, DIM, RED, GREEN, YELLOW, CYAN = "\033[0m", "\033[2m", "\033[31m", "\033[32m", "\033[33m", "\033[36m"


class GameNotRunning(Exception):
    pass


class Connection:
    def __init__(self):
        try:
            self.pipe = open(PIPE, "r+b", buffering=0)
        except OSError as error:
            raise GameNotRunning(
                "cannot reach the game's console pipe. Is Death Stranding running with Bridger "
                f"deployed? ({error})") from error
        self.pending = b""

    def request(self, **fields):
        self.pipe.write((json.dumps(fields) + "\n").encode("utf-8"))
        while b"\n" not in self.pending:
            chunk = self.pipe.read(65536)
            if not chunk:
                raise GameNotRunning("the game closed the console pipe")
            self.pending += chunk
        line, self.pending = self.pending.split(b"\n", 1)
        return json.loads(line.decode("utf-8", errors="replace"))

    def close(self):
        self.pipe.close()


def colour(enabled):
    if not enabled:
        global RESET, DIM, RED, GREEN, YELLOW, CYAN
        RESET = DIM = RED = GREEN = YELLOW = CYAN = ""
    elif os.name == "nt":
        os.system("")


def show_evaluation(response):
    output = response.get("output", "")
    if output:
        sys.stdout.write(output if output.endswith("\n") else output + "\n")
    result = response.get("result", "")
    if response.get("ok"):
        if result:
            print(f"{GREEN}{result}{RESET}")
    else:
        print(f"{RED}{result}{RESET}")


def incomplete(response):
    return not response.get("ok") and "<eof>" in response.get("result", "")


def game_directory():
    if os.environ.get("BRIDGER_GAME_DIR"):
        return Path(os.environ["BRIDGER_GAME_DIR"])
    cache = ROOT / "build" / "CMakeCache.txt"
    if cache.exists():
        match = re.search(r"^BRIDGER_GAME_DIR:PATH=(.+)$", cache.read_text(errors="replace"), re.M)
        if match and match.group(1).strip():
            return Path(match.group(1).strip())
    return None


def repl(target):
    connection = Connection()
    ping = connection.request(op="ping")
    ticking = "" if ping.get("game_thread") else f"  {YELLOW}(game thread not ticking: runs on the console thread){RESET}"
    print(f"{CYAN}Bridger {ping.get('version')} - {ping.get('lua')} - target {target}{RESET}{ticking}")
    print(f"{DIM}help() lists the essentials. :target <id> switches script, :scripts lists them, exit leaves.{RESET}")

    try:
        import readline

        def completer(text, state):
            line = readline.get_line_buffer()
            if state == 0:
                completer.items = connection.request(op="complete", target=target, prefix=line).get("items", [])
            items = completer.items
            if state < len(items):
                start = len(line) - len(text)
                return items[state][start:]
            return None

        readline.set_completer_delims(" \t\n()[]{},=+-*/%<>~#;")
        readline.set_completer(completer)
        readline.parse_and_bind("tab: complete")
    except ImportError:
        pass

    buffer = []
    while True:
        try:
            line = input(f"{target}> " if not buffer else f"{target}>> ")
        except (EOFError, KeyboardInterrupt):
            print()
            break
        line = line.lstrip("﻿")
        if not buffer:
            stripped = line.strip()
            if stripped in ("exit", "quit", ":q"):
                break
            if stripped.startswith(":target"):
                parts = stripped.split(maxsplit=1)
                target = parts[1] if len(parts) > 1 else "console"
                print(f"{DIM}target is now {target}{RESET}")
                continue
            if stripped == ":scripts":
                list_scripts(connection)
                continue
            if stripped.startswith(":reload"):
                parts = stripped.split(maxsplit=1)
                show_evaluation(connection.request(op="reload", id=parts[1] if len(parts) > 1 else target))
                continue
            if not stripped:
                continue
        buffer.append(line)
        response = connection.request(op="eval", target=target, code="\n".join(buffer))
        if incomplete(response):
            continue
        buffer = []
        show_evaluation(response)
    connection.close()


def list_scripts(connection):
    response = connection.request(op="scripts")
    states = {s["id"]: s for s in response.get("scripts", [])}
    print(f"{DIM}{'id':24} {'state':9} {'kind':7} {'hooks':>5} {'memory':>9}  error{RESET}")
    console = states.get("console")
    if console:
        print(f"{'console':24} {'running':9} {'repl':7} {console['hooks']:>5} {console['memory_kb']:>7}KB")
    for mod in response.get("mods", []):
        state = states.get(mod["id"], {})
        tone = GREEN if mod["state"] == "loaded" else RED if mod["state"] == "failed" else DIM
        kind = "lua" if mod["script"] else "dll"
        error = mod.get("error") or state.get("error") or ""
        error = error.splitlines()[0] if error else ""
        hooks = state.get("hooks", "")
        memory = f"{state['memory_kb']}KB" if "memory_kb" in state else ""
        print(f"{mod['id']:24} {tone}{mod['state']:9}{RESET} {kind:7} {hooks!s:>5} {memory:>9}  {RED}{error}{RESET}")


def follow(connection, op, follow_mode):
    since = 0
    first = True
    while True:
        response = connection.request(op=op, since=since)
        lines = response.get("lines", [])
        if first and not follow_mode:
            lines = lines[-200:]
        for line in lines:
            if op == "log":
                tone = RED if "[error]" in line else YELLOW if "[warn ]" in line else ""
                print(f"{tone}{line}{RESET}")
            else:
                kind = line["kind"]
                tone = RED if kind == "error" else YELLOW if kind == "warn" else GREEN if kind == "result" else ""
                source = line["source"]
                prefix = f"{DIM}{source}{RESET} " if not source.startswith("> ") else f"{DIM}{source}{RESET} "
                print(f"{prefix}{tone}{line['text']}{RESET}")
        since = response.get("next", since)
        first = False
        if not follow_mode:
            return
        time.sleep(0.25)


TEMPLATE = '''-- {name}: a Bridger script. Save this file and the running game reloads it.
-- The guide is docs/scripting.md; help() in the console lists the essentials.

local config = settings {{
    enabled = true,
    strength = {{ 1.0, min = 0, max = 10, help = "How much of it." }},
}}

local toggle = setting("toggle", "F6", {{ key = true, label = "Toggle" }})

bind(toggle, function()
    config.enabled = not config.enabled
    print("{name} " .. (config.enabled and "on" or "off"))
end)

every_frame(function(dt)
    if not config.enabled then return end
    -- local sam = game.player_entity()
end)
'''


def scaffold(name):
    game = game_directory()
    if game is None:
        raise SystemExit("set BRIDGER_GAME_DIR, or configure the build with -DBRIDGER_GAME_DIR, so I know where the game is")
    target = game / "Bridger" / "scripts" / f"{name}.lua"
    if target.exists():
        raise SystemExit(f"{target} already exists")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(TEMPLATE.format(name=name), encoding="utf-8")
    types = ROOT / "include" / "bridger" / "lua"
    luarc = target.parent / ".luarc.json"
    if not luarc.exists() and types.exists():
        luarc.write_text(json.dumps({
            "runtime.version": "Lua 5.4",
            "workspace.library": [str(types)],
            "diagnostics.globals": [],
            "workspace.checkThirdParty": False,
        }, indent=2), encoding="utf-8")
    print(f"created {target}")
    print("the game loads it within two seconds; edit and save to reload")


def main():
    parser = argparse.ArgumentParser(description="A Lua console inside the running game.")
    parser.add_argument("--target", default="console", help="script whose state to run in (default: console)")
    parser.add_argument("--no-color", action="store_true")
    sub = parser.add_subparsers(dest="command")
    p = sub.add_parser("eval", help="evaluate Lua and print the result")
    p.add_argument("code", nargs="+")
    p = sub.add_parser("run", help="run a Lua file once")
    p.add_argument("file", type=Path)
    sub.add_parser("scripts", help="list mods and script states")
    p = sub.add_parser("reload", help="reload a mod")
    p.add_argument("id")
    p = sub.add_parser("log", help="print bridger.log")
    p.add_argument("-f", "--follow", action="store_true")
    p = sub.add_parser("console", help="print script output")
    p.add_argument("-f", "--follow", action="store_true")
    p = sub.add_parser("new", help="create a script in Bridger/scripts")
    p.add_argument("name")
    args = parser.parse_args()
    colour(not args.no_color and sys.stdout.isatty())

    try:
        if args.command is None:
            repl(args.target)
        elif args.command == "new":
            scaffold(args.name)
        else:
            connection = Connection()
            if args.command == "eval":
                response = connection.request(op="eval", target=args.target, code=" ".join(args.code))
                show_evaluation(response)
                sys.exit(0 if response.get("ok") else 1)
            elif args.command == "run":
                code = args.file.read_text(encoding="utf-8-sig")
                response = connection.request(op="eval", target=args.target, code=code, timeout_ms=60000)
                show_evaluation(response)
                sys.exit(0 if response.get("ok") else 1)
            elif args.command == "scripts":
                list_scripts(connection)
            elif args.command == "reload":
                show_evaluation(connection.request(op="reload", id=args.id))
            elif args.command in ("log", "console"):
                follow(connection, args.command, args.follow)
    except GameNotRunning as error:
        print(f"{RED}{error}{RESET}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
