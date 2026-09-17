"""Locates the Death Stranding install.

BRIDGER_GAME_DIR wins when set. Otherwise Steam's library folders are searched for app 1190460.
"""
import os
import re
import sys

APP_ID = "1190460"


def _steam_root():
    try:
        import winreg
    except ImportError:
        return None
    for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                      (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
        try:
            with winreg.OpenKey(hive, key) as handle:
                for name in ("SteamPath", "InstallPath"):
                    try:
                        return winreg.QueryValueEx(handle, name)[0]
                    except OSError:
                        pass
        except OSError:
            pass
    return None


def find():
    override = os.environ.get("BRIDGER_GAME_DIR")
    if override:
        return override
    root = _steam_root()
    if not root:
        return None
    libraries = [root]
    vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
    if os.path.exists(vdf):
        text = open(vdf, encoding="utf-8", errors="replace").read()
        libraries += [path.replace("\\\\", "\\") for path in re.findall(r'"path"\s+"([^"]+)"', text)]
    for library in libraries:
        manifest = os.path.join(library, "steamapps", f"appmanifest_{APP_ID}.acf")
        if os.path.exists(manifest):
            text = open(manifest, encoding="utf-8", errors="replace").read()
            match = re.search(r'"installdir"\s+"([^"]+)"', text)
            if match:
                return os.path.join(library, "steamapps", "common", match.group(1))
    return None


def require():
    game = find()
    if not game or not os.path.isdir(game):
        sys.exit("Death Stranding not found; set BRIDGER_GAME_DIR to the install folder")
    return game
