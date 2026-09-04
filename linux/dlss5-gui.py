#!/usr/bin/env python3
"""
dlss5-gui.py - local web GUI for dlss5-linux.sh

Scans your Steam libraries, probes each installed game for DLSS 5 Neural
Rendering compatibility, installs/uninstalls the injector, and launches the game
with the required Proton environment applied.

Python standard library only. Binds to 127.0.0.1 and requires a per-run token,
so nothing else on the machine can drive it.

    ./dlss5-gui.py            # serve on 127.0.0.1:8777 and open a browser
    ./dlss5-gui.py --port N --no-open

Launching goes through Proton directly rather than through Steam, so the
environment overrides are applied by this process. Nothing touches Steam's own
config files. The Steam launch-option string is still offered for copying if you
would rather start the game the usual way.
"""

import argparse
import hashlib
import hmac
import json
import os
import re
import secrets
import subprocess
import sys
import threading
import time
import traceback
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "dlss5-linux.sh"

# Runtimes and redistributables that live in steamapps/common but are not games.
NON_GAME_APPIDS = {
    "228980",   # Steamworks Common Redistributables
    "1070560",  # Steam Linux Runtime 1.0 (scout)
    "1391110",  # Steam Linux Runtime 2.0 (soldier)
    "1628350",  # Steam Linux Runtime 3.0 (sniper)
    "1493710",  # Proton Experimental
}
NON_GAME_PREFIXES = ("Proton", "Steam Linux Runtime", "Steamworks", "SteamVR")

# Games that are not in a Steam library - GOG, Heroic, a loose Wine install -
# are remembered here between runs, and get a Wine prefix of our own because
# they have no compatdata directory to borrow.
CONFIG_HOME = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
DATA_HOME = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
FOLDERS_FILE = CONFIG_HOME / "dlss5-linux" / "folders.json"
PREFIX_DIR = DATA_HOME / "dlss5-linux" / "prefixes"

TOKEN = secrets.token_urlsafe(24)
JOBS = {}
JOBS_LOCK = threading.Lock()

# Every request body here is a path and a couple of flags. Refusing anything
# larger stops a bogus Content-Length from parking a handler thread on a read
# that will never be satisfied.
MAX_BODY = 1 << 20


class BadRequest(Exception):
    """The caller's fault - reported as 400 rather than a 500 with a traceback."""


class Busy(Exception):
    """A job is already working in that folder - reported as 409."""


_REQUIRED = object()


def str_field(body, key, default=_REQUIRED):
    """A string field out of a request body, or the 400 the caller deserves.

    Missing required fields stay a KeyError so they keep reporting as "missing
    field"; a field of the wrong type is just as much the caller's fault as an
    absent one, and used to surface as a 500 from deep inside string formatting.
    """
    if key not in body:
        if default is _REQUIRED:
            raise KeyError(key)
        return default

    value = body[key]
    if not isinstance(value, str) or not value.strip():
        raise BadRequest("field %r must be a non-empty string" % key)
    return value


def token_ok(supplied):
    """Constant time: a plain == leaks how much of the token already matched."""
    try:
        return hmac.compare_digest(supplied or "", TOKEN)
    except TypeError:      # non-ASCII, so it cannot be the token anyway
        return False


# --------------------------------------------------------------------------
# Steam library discovery
# --------------------------------------------------------------------------

def steam_root():
    for candidate in (
        "~/.local/share/Steam",
        "~/.steam/steam",
        "~/.steam/root",
        "~/.var/app/com.valvesoftware.Steam/data/Steam",
    ):
        path = Path(candidate).expanduser()
        if (path / "steamapps").is_dir():
            return path
    return None


def library_paths(root):
    """steamapps directories across every configured Steam library."""
    libs = [root / "steamapps"]

    vdf = root / "steamapps" / "libraryfolders.vdf"
    if vdf.is_file():
        text = vdf.read_text(errors="replace")
        for match in re.finditer(r'"path"\s+"([^"]+)"', text):
            candidate = Path(match.group(1)) / "steamapps"
            if candidate.is_dir() and candidate not in libs:
                libs.append(candidate)

    return libs


def parse_manifest(acf):
    """Pull appid / name / installdir out of an appmanifest_*.acf."""
    try:
        text = acf.read_text(errors="replace")
    except OSError:
        return None

    def field(key):
        match = re.search(r'"%s"\s+"([^"]*)"' % key, text)
        return match.group(1) if match else None

    appid, name, installdir = field("appid"), field("name"), field("installdir")
    if not (appid and installdir):
        return None

    path = acf.parent / "common" / installdir
    if not path.is_dir():
        return None

    return {
        "appid": appid,
        "name": name or installdir,
        "path": str(path),
        "compatdata": str(acf.parent / "compatdata" / appid),
    }


def list_games():
    root = steam_root()
    if root is None:
        return []

    games, seen = [], set()
    for lib in library_paths(root):
        for acf in sorted(lib.glob("appmanifest_*.acf")):
            game = parse_manifest(acf)
            if not game or game["appid"] in seen:
                continue
            if game["appid"] in NON_GAME_APPIDS:
                continue
            if game["name"].startswith(NON_GAME_PREFIXES):
                continue
            seen.add(game["appid"])
            games.append(game)

    games.sort(key=lambda g: g["name"].lower())
    return games


# --------------------------------------------------------------------------
# Manually added game folders
# --------------------------------------------------------------------------

def read_folders():
    """The folders added by hand, as they were stored (absolute, resolved)."""
    try:
        data = json.loads(FOLDERS_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return []
    if not isinstance(data, list):
        return []
    return sorted({item for item in data if isinstance(item, str) and item})


def write_folders(paths):
    FOLDERS_FILE.parent.mkdir(parents=True, exist_ok=True)
    # Written beside the target and renamed, so an interrupted write cannot
    # leave a half-serialised list that read_folders() would then discard.
    tmp = FOLDERS_FILE.with_name(FOLDERS_FILE.name + ".part")
    tmp.write_text(json.dumps(sorted(paths), indent=2) + "\n")
    tmp.replace(FOLDERS_FILE)


def prefix_for(path):
    """A Wine prefix of our own, for a game with no Steam compatdata.

    Named after the folder so it is recognisable in a file manager, and
    suffixed with a digest of the full path so two games called "Bin" in
    different places do not end up sharing one prefix.
    """
    digest = hashlib.sha256(str(path).encode("utf-8")).hexdigest()[:12]
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", Path(path).name).strip("-.") or "game"
    return PREFIX_DIR / ("%s-%s" % (slug[:48], digest))


def manual_games():
    """Added folders in the same shape list_games() returns.

    The appid is synthetic and only keys the UI's per-game state; `manual`
    keeps it from reaching Proton as a real SteamAppId.
    """
    games = []
    for raw in read_folders():
        path = Path(raw)
        games.append({
            "appid": "manual:" + raw,
            "name": path.name or raw,
            "path": raw,
            "compatdata": str(prefix_for(raw)),
            "manual": True,
            "missing": not path.is_dir(),
        })
    return games


def add_folder(raw):
    """Register a game folder Steam knows nothing about. Returns its real path."""
    try:
        path = Path(raw).expanduser().resolve()
    except (OSError, ValueError):
        raise BadRequest("Not a usable path: %s" % raw)

    if not path.is_dir():
        raise BadRequest("Not a directory: %s" % path)

    # An added folder becomes a base for the containment checks that guard
    # xdg-open and log reading, so one that contains everything would be the
    # end of them. $HOME and anything above it are refused for that reason.
    home = Path.home()
    if path == path.parent or path == home or home.is_relative_to(path):
        raise BadRequest("Too broad to add: %s. Pick the game's own folder." % path)

    folders = read_folders()
    if str(path) in folders:
        raise BadRequest("Already added: %s" % path)
    # Covers both a game Steam already lists and a subfolder of one added
    # earlier, either of which would show up twice.
    if under(path, game_roots()):
        raise BadRequest("Already covered by a known game folder: %s" % path)

    write_folders(folders + [str(path)])
    return str(path)


def remove_folder(raw):
    """Forget an added folder. Nothing on disk is touched."""
    folders = read_folders()
    try:
        resolved = str(Path(raw).expanduser().resolve())
    except (OSError, ValueError):
        resolved = raw

    kept = [f for f in folders if f not in (raw, resolved)]
    if len(kept) == len(folders):
        raise BadRequest("Not an added folder: %s" % raw)
    write_folders(kept)


def list_protons():
    """Available Proton runtimes, newest-looking first."""
    root = steam_root()
    if root is None:
        return []

    found = []
    for base in (root / "compatibilitytools.d", root / "steamapps" / "common"):
        if not base.is_dir():
            continue
        for entry in sorted(base.iterdir()):
            if (entry / "proton").is_file():
                found.append({"name": entry.name, "path": str(entry)})

    def rank(item):
        name = item["name"]
        # GE builds first, then highest version number.
        numbers = [int(n) for n in re.findall(r"\d+", name)] or [0]
        return (0 if "GE-Proton" in name else 1, [-n for n in numbers])

    found.sort(key=rank)
    return found


# --------------------------------------------------------------------------
# dlss5-linux.sh bridge
# --------------------------------------------------------------------------

def run_script(args, timeout=120):
    proc = subprocess.run(
        [str(SCRIPT)] + args,
        capture_output=True, text=True, timeout=timeout,
    )
    return proc.returncode, proc.stdout, proc.stderr


def probe(path):
    try:
        rc, out, err = run_script(["--game-dir=" + path, "--probe", "--json"])
    except subprocess.TimeoutExpired:
        return {"verdict": "unknown", "reasons": ["Probe timed out."], "dir": path}

    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return {
            "verdict": "unknown",
            "reasons": [(err or out or "Probe produced no output.").strip()],
            "dir": path,
        }


def preflight():
    try:
        rc, out, err = run_script(["--check"], timeout=60)
    except subprocess.TimeoutExpired:
        return {"ok": False, "lines": [{"kind": "bad", "text": "Preflight timed out."}]}

    lines = []
    for raw in (out + err).splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", raw).rstrip()
        if not line or line.startswith("dlss5-linux.sh v") or line == "Preflight":
            continue
        # The script's own markers. "err" is what die() prints, and it used to
        # fall through to a neutral "info" chip with the marker still attached.
        text = line.lstrip(" ")
        for marker, kind in (("ok ", "ok"), ("warn ", "warn"), ("err ", "bad"), ("==> ", "info")):
            if text.startswith(marker):
                lines.append({"kind": kind, "text": text[len(marker):]})
                break
        else:
            lines.append({"kind": "info", "text": text})

    return {"ok": rc == 0, "lines": lines}


# --------------------------------------------------------------------------
# Background jobs (install / uninstall / launch)
# --------------------------------------------------------------------------

def busy_folder(key):
    """Folder of a running job overlapping `key`, or None. Caller holds JOBS_LOCK.

    Overlap rather than equality, because the same game is named two ways: the
    game root goes to install/uninstall, the resolved binary directory to
    configure and launch.
    """
    target = Path(key)
    for job in JOBS.values():
        if job["status"] != "running" or not job["folder"]:
            continue
        other = Path(job["folder"])
        if target.is_relative_to(other) or other.is_relative_to(target):
            return job["folder"]
    return None


def start_job(argv, env=None, cwd=None, folder=None):
    # One job per game folder: double-clicking Install used to start two
    # installers unpacking over each other in the same directory.
    key = str(Path(folder).resolve()) if folder else None

    job_id = uuid.uuid4().hex
    with JOBS_LOCK:
        if key:
            clash = busy_folder(key)
            if clash:
                raise Busy("A job is already running in %s. Wait for it to finish." % clash)
        JOBS[job_id] = {"status": "running", "output": "", "rc": None, "folder": key}

    def worker():
        try:
            proc = subprocess.Popen(
                argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1, env=env, cwd=cwd,
            )
            for chunk in proc.stdout:
                # curl's progress bar uses \r; flatten it so the log stays readable.
                clean = re.sub(r"\x1b\[[0-9;]*m", "", chunk).replace("\r", "\n")
                with JOBS_LOCK:
                    job = JOBS[job_id]
                    job["output"] = (job["output"] + clean)[-20000:]
            proc.wait()
            with JOBS_LOCK:
                JOBS[job_id]["rc"] = proc.returncode
                JOBS[job_id]["status"] = "done" if proc.returncode == 0 else "failed"
        except Exception as exc:                      # noqa: BLE001 - surfaced to the UI
            with JOBS_LOCK:
                JOBS[job_id]["output"] += "\n%s: %s" % (type(exc).__name__, exc)
                JOBS[job_id]["status"] = "failed"
                JOBS[job_id]["rc"] = -1

    threading.Thread(target=worker, daemon=True).start()
    return job_id


# --------------------------------------------------------------------------
# Game folder / log inspection
# --------------------------------------------------------------------------

# OptiScaler ships LogToFile=true at LogLevel=2, so the log exists after a run
# without the user configuring anything. ReShade writes its own alongside.
LOG_NAMES = ("OptiScaler.log", "ReShade.log")
CAPTURE_DIR = "dlssnr-capture"


def game_roots():
    """Every place a game we know about can live, and so the only places the
    open and log-reading routes will touch: each steamapps/common, plus the
    folders added by hand."""
    roots = []
    root = steam_root()
    if root is not None:
        roots += [lib / "common" for lib in library_paths(root) if (lib / "common").is_dir()]
    roots += [path for path in map(Path, read_folders()) if path.is_dir()]
    return roots


def under(path, bases):
    """Resolved `path` if it sits in one of `bases`, else None.

    Both sides are resolved first, so this is containment and not a string
    prefix test: ".." and symlinks out of the tree cannot smuggle a path past it.
    """
    try:
        target = Path(path).resolve()
    except (OSError, ValueError, TypeError):
        return None
    return target if any(target.is_relative_to(base) for base in bases) else None


def contained(path, bases, what, where):
    """under(), but raising the 400 the caller wants when nothing contains it."""
    target = under(path, bases)
    if target is None:
        raise BadRequest("%s is outside %s: %s" % (what, where, path))
    return target


def open_path(path):
    """Hand a folder or file to the desktop's default handler."""
    target = Path(path)
    if not target.exists():
        raise BadRequest("Does not exist: %s" % target)

    subprocess.Popen(
        ["xdg-open", str(target)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    return str(target)


def game_logs(game_dir, engine=None):
    """Every log we know about in the game folder, newest first."""
    directory = Path(game_dir)
    found = []

    for name in LOG_NAMES:
        path = directory / name
        if not path.is_file():
            continue
        stat = path.stat()
        found.append({
            "name": name,
            "path": str(path),
            "size": stat.st_size,
            "mtime": stat.st_mtime,
            "preferred": bool(engine) and name.lower().startswith(engine[:4].lower()),
        })

    found.sort(key=lambda item: (not item["preferred"], -item["mtime"]))
    return found


# What the injector's own log says actually happened. These are the lines
# OptiScaler writes; the neural pass dispatching is the only proof that matters,
# because everything upstream of it can succeed and still produce no effect.
NR_DISPATCH = re.compile(r"DlssNr_\w+::Dispatch")
NR_BACKEND = re.compile(r"TryCreateOptiFeature Creating (\w+) upscaler feature")
NR_NVAPI_MISS = re.compile(r"Upscaler support[^\n]*\bdlss: false")
NR_LOADED = re.compile(r"OptiScaler v[\d.]+")
NR_TIMESTAMP = re.compile(r"^\[(\d\d):(\d\d):(\d\d)")


def analyse_log(game_dir):
    """Read OptiScaler.log and say whether the neural pass is actually running.

    Returns None when there is no log to read. Everything else is a state the
    UI shows verbatim, because "installed" and "working" are very different
    things here and the difference is only ever visible in this file.
    """
    directory = Path(game_dir)
    path = directory / "OptiScaler.log"
    if not path.is_file():
        return None

    # The tail is enough and bounds the read: the interesting lines repeat every
    # frame, and a long session's log is mostly dispatches.
    try:
        size = path.stat().st_size
        with path.open("rb") as fh:
            if size > 400_000:
                fh.seek(-400_000, os.SEEK_END)
            text = fh.read().decode("utf-8", errors="replace")
    except OSError as exc:
        return {"state": "unknown", "headline": "Could not read OptiScaler.log: %s" % exc}

    dispatches = len(NR_DISPATCH.findall(text))
    backends = NR_BACKEND.findall(text)
    backend = backends[-1].lower() if backends else None
    nvapi_missed = bool(NR_NVAPI_MISS.search(text))
    loaded = bool(NR_LOADED.search(text))
    age = time.time() - path.stat().st_mtime

    info = {
        "dispatches": dispatches,
        "backend": backend,
        "nvapi_missed": nvapi_missed,
        "age": age,
        "mtime": path.stat().st_mtime,
    }

    if not loaded:
        info.update(state="bad", headline="OptiScaler never loaded",
                    detail="The log exists but has no startup banner. The proxy DLL was "
                           "probably not loaded - check WINEDLLOVERRIDES.")
    elif not dispatches:
        info.update(state="bad", headline="Injector loaded, neural pass never ran",
                    detail="OptiScaler started but DLSS-NR was never dispatched. Either "
                           "neural rendering is off, or the game gave it nothing to hook.")
    elif backend and backend != "dlss":
        info.update(
            state="warn",
            headline="Neural pass running, but on %s instead of DLSS" % backends[-1],
            detail=("OptiScaler could not see DLSS support and fell back to %s. "
                    % backends[-1]) +
                   ("NVAPI was probed before the game loaded nvapi64.dll. " if nvapi_missed else "") +
                   "Set the DX12 upscaler to 'dlss' below and relaunch.")
    else:
        info.update(state="ok", headline="DLSS 5 neural rendering is running",
                    detail="%d dispatches recorded%s." % (
                        dispatches, " on DLSS" if backend == "dlss" else ""))

    return info


# --------------------------------------------------------------------------
# Is the game running - however it was started
# --------------------------------------------------------------------------

# Processes that live in a game folder without being the game. The crash
# handler is the one that matters: Sentry's crashpad_handler.exe stays up with
# the game folder as its working directory after the game itself has exited,
# which kept Expedition 33 marked as running long after it closed.
HELPER_PROCS = (
    "crashpad_handler.exe", "crashreportclient.exe", "unitycrashhandler64.exe",
    "unitycrashhandler32.exe", "epicwebhelper.exe", "steamwebhelper.exe",
    "gameoverlayui.exe", "wineboot.exe", "winedevice.exe", "services.exe",
    "plugplay.exe", "explorer.exe", "rpcss.exe", "svchost.exe", "conhost.exe",
    "tabtip.exe", "start.exe", "winemenubuilder.exe",
)


def is_helper(cmdline):
    """Whether a command line names one of the helpers above, not a game."""
    first = cmdline.split("\0")[0].replace("\\", "/")
    return first.rsplit("/", 1)[-1].strip().lower() in HELPER_PROCS


def running_dirs(candidates):
    """Which of `candidates` (game directories) have a live process.

    Scans /proc once for all of them, so it stays cheap enough to poll. The
    game does not have to have been started from here: Steam, Heroic and a bare
    Proton call all produce a process whose command line names the executable
    and whose working directory is the folder it was launched from.
    """
    wanted = {}
    for path in candidates:
        try:
            wanted[str(Path(path).resolve())] = None
        except (OSError, ValueError):
            continue
    if not wanted:
        return {}

    live = {}
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        pid = entry.name
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as fh:
                cmdline = fh.read().decode("utf-8", errors="replace")
        except OSError:
            continue
        if not cmdline or is_helper(cmdline):
            continue

        # The working directory is the reliable half: Wine runs the game with
        # its own folder as cwd whoever started it. The command line is the
        # fallback for a launcher that sets cwd somewhere else.
        try:
            cwd = os.path.realpath("/proc/%s/cwd" % pid)
        except OSError:
            cwd = ""

        lowered = cmdline.replace("\\", "/").lower()
        for target in wanted:
            if target in live:
                continue
            if cwd == target or cwd.startswith(target + os.sep):
                live[target] = int(pid)
            elif target.lower() in lowered:
                live[target] = int(pid)

    return live


def read_log_tail(path, limit=160_000):
    """Last `limit` bytes of a log. Logs grow without bound; the tail is the part
    that matters, and shipping megabytes to the browser every second would not."""
    file = Path(path)
    if not file.is_file():
        return "", 0, False

    size = file.stat().st_size
    with file.open("rb") as handle:
        if size > limit:
            handle.seek(size - limit)
        data = handle.read()

    text = data.decode("utf-8", errors="replace")
    if size > limit:
        # Drop the partial first line so the view starts cleanly.
        text = text.split("\n", 1)[-1]

    return text, size, size > limit


def read_optiscaler_config(game_dir):
    """Current [DlssNr] Enabled and [Upscalers] Dx12Upscaler, or None if no ini."""
    ini = Path(game_dir) / "OptiScaler.ini"
    if not ini.is_file():
        return None

    current, found = None, {"nr": "auto", "upscaler": "auto"}
    for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1]
            continue
        if current == "DlssNr" and stripped.startswith("Enabled="):
            found["nr"] = stripped.split("=", 1)[1]
        elif current == "Upscalers" and stripped.startswith("Dx12Upscaler="):
            found["upscaler"] = stripped.split("=", 1)[1]

    return found


def launch_env(hook, extra=None):
    """The Proton environment the injector needs. Mirrors print_launch_options()."""
    env = {
        "WINEDLLOVERRIDES": "%s=n,b" % hook.removesuffix(".dll"),
        "PROTON_ENABLE_NVAPI": "1",
        "PROTON_ENABLE_NGX_UPDATER": "1",
        "VKD3D_CONFIG": "dxr11",
        "PROTON_HIDE_NVIDIA_GPU": "0",
    }
    if extra:
        env.update(extra)
    return env


def steam_launch_options(hook):
    env = launch_env(hook)
    parts = ['WINEDLLOVERRIDES="%s"' % env.pop("WINEDLLOVERRIDES")]
    parts += ["%s=%s" % (k, v) for k, v in env.items()]
    return " ".join(parts) + " %COMMAND%"


def start_launch(game, proton_path, hook):
    """Run the game through Proton with our overrides applied by this process."""
    root = steam_root()
    if root is None:
        raise RuntimeError("No Steam installation found.")

    if not isinstance(game, dict):
        raise BadRequest("field 'game' must be a JSON object")

    proton = Path(proton_path) / "proton"
    if not proton.is_file():
        raise BadRequest("No proton binary at %s" % proton)

    game_dir = Path(str_field(game, "path"))
    exe = game.get("executable")
    if not exe or not isinstance(exe, str):
        raise BadRequest("Probe the game first so we know which executable to run.")

    target = game_dir / exe
    if not target.is_file():
        # The probe resolves launcher folders to the real binary directory.
        matches = list(game_dir.rglob(exe))
        if not matches:
            raise BadRequest("Could not find %s under %s" % (exe, game_dir))
        target = matches[0]

    compatdata = Path(str_field(game, "compatdata"))
    compatdata.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env.update(launch_env(hook))
    env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = str(root)
    env["STEAM_COMPAT_DATA_PATH"] = str(compatdata)

    # A manual game's appid is our own synthetic key, not Steam's, so it must
    # not reach Proton - a made-up SteamAppId is worse than none.
    appid = "" if game.get("manual") else str_field(game, "appid", "")
    if appid:
        # Without these, steam_api64.dll cannot attach to the running Steam
        # client and many games exit within a second of starting - before they
        # ever create a D3D12 device, so the injector looks innocent in the log.
        env["STEAM_COMPAT_APP_ID"] = appid
        env["SteamAppId"] = appid
        env["SteamGameId"] = appid

    # The library root, so Proton maps it to a drive letter instead of leaving
    # the game to reach itself through Z:\ (the whole-filesystem fallback).
    # A manual game has no library above it and its prefix is one of ours, off
    # in ~/.local/share, so mount the folder's own parent instead.
    library = game_dir.parent if game.get("manual") else compatdata.parent.parent
    if library.is_dir():
        env["STEAM_COMPAT_MOUNTS"] = str(library)
    env["STEAM_COMPAT_INSTALL_PATH"] = str(game_dir)

    return start_job([str(proton), "run", str(target)],
                     env=env, cwd=str(target.parent), folder=str(target.parent))


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "dlss5-gui"
    _sent = False

    def log_message(self, fmt, *args):
        pass  # keep the terminal clean; job output is shown in the UI

    # -- helpers --

    def _send(self, code, body, ctype="application/json"):
        payload = body if isinstance(body, bytes) else body.encode("utf-8")
        self._sent = True
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(payload)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def _authed(self):
        if token_ok(self.headers.get("X-Token")):
            return True
        self._json({"error": "bad token"}, 403)
        return False

    def _body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise BadRequest("Content-Length is not a number")
        if length < 0:
            raise BadRequest("Content-Length is negative")
        if length > MAX_BODY:
            raise BadRequest("body too large")
        if not length:
            return {}

        try:
            body = json.loads(self.rfile.read(length))
        except json.JSONDecodeError:
            return {}
        if not isinstance(body, dict):
            raise BadRequest("body must be a JSON object")
        return body

    def _crashed(self):
        """A route that raised past its own handling. Answering something beats
        letting the thread die with the browser still waiting on a response."""
        traceback.print_exc()
        if self._sent:
            return
        try:
            self._json({"error": "internal error - see the terminal"}, 500)
        except Exception:                             # noqa: BLE001 - client gone
            pass

    # -- routes --

    def do_GET(self):
        try:
            self._get()
        except Exception:                             # noqa: BLE001 - never hang
            self._crashed()

    def do_POST(self):
        try:
            self._post()
        except Exception:                             # noqa: BLE001 - never hang
            self._crashed()

    def _get(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)

        if url.path == "/":
            if not token_ok(query.get("t", [""])[0]):
                self._send(403, "Open the URL printed in the terminal.", "text/plain")
                return
            self._send(200, PAGE.replace("__TOKEN__", TOKEN), "text/html; charset=utf-8")
            return

        if not url.path.startswith("/api/"):
            self._json({"error": "not found"}, 404)
            return

        if not self._authed():
            return

        if url.path == "/api/preflight":
            self._json(preflight())
        elif url.path == "/api/games":
            games = sorted(list_games() + manual_games(), key=lambda g: g["name"].lower())
            self._json({"games": games, "protons": list_protons(),
                        "folders": read_folders()})
        elif url.path.startswith("/api/job/"):
            job_id = url.path.rsplit("/", 1)[-1]
            with JOBS_LOCK:
                self._json(JOBS.get(job_id, {"status": "missing", "output": "", "rc": None}))
        else:
            self._json({"error": "not found"}, 404)

    def _post(self):
        url = urlparse(self.path)
        if not self._authed():
            return

        try:
            body = self._body()

            if url.path == "/api/probe":
                self._json(probe(str_field(body, "path")))

            elif url.path == "/api/install":
                # The game ROOT, never the probe's resolved binary directory:
                # the script derives its anti-cheat refusal from the path it is
                # handed, and anti-cheat sits above the binary (Halo MCC). It
                # resolves the binary directory itself.
                path = str_field(body, "path")
                args = [
                    "--game-dir=" + path,
                    "--engine=" + str_field(body, "engine", "optiscaler"),
                    "--hook=" + str_field(body, "hook", "dxgi.dll"),
                    "-y",
                ]
                self._json({"job": start_job([str(SCRIPT)] + args, folder=path)})

            elif url.path == "/api/uninstall":
                # Unlike install, this one gets the resolved directory: the
                # script's uninstall path deletes from what it is handed.
                path = str_field(body, "path")
                args = ["--game-dir=" + path, "--uninstall", "-y"]
                self._json({"job": start_job([str(SCRIPT)] + args, folder=path)})

            elif url.path == "/api/launch":
                job = start_launch(body["game"], str_field(body, "proton"),
                                   str_field(body, "hook", "dxgi.dll"))
                self._json({"job": job})

            elif url.path == "/api/launch-options":
                self._json({"options": steam_launch_options(str_field(body, "hook", "dxgi.dll"))})

            elif url.path == "/api/activity":
                # One endpoint for the whole poll: which games are alive, and
                # what the selected one's log says it is doing. Both are cheap
                # and both change while the game runs, so they belong together
                # rather than as two timers drifting apart.
                paths = body.get("paths") or []
                if not isinstance(paths, list):
                    raise BadRequest("field 'paths' must be a list")
                paths = [p for p in paths[:400] if isinstance(p, str) and p]

                detail = str_field(body, "detail", "")
                status = analyse_log(detail) if detail else None

                self._json({"running": running_dirs(paths), "status": status})

            elif url.path == "/api/folders/add":
                self._json({"added": add_folder(str_field(body, "path")),
                            "folders": read_folders()})

            elif url.path == "/api/folders/remove":
                remove_folder(str_field(body, "path"))
                self._json({"folders": read_folders()})

            elif url.path == "/api/open":
                # Token-gated, but nothing here has any business handing an
                # arbitrary path to the desktop's handler.
                target = contained(str_field(body, "path"), game_roots(),
                                    "Path", "the known game folders")
                self._json({"opened": open_path(target)})

            elif url.path == "/api/config":
                self._json({"config": read_optiscaler_config(str_field(body, "path"))})

            elif url.path == "/api/configure":
                path = str_field(body, "path")
                args = ["--game-dir=" + path, "--configure"]
                if str_field(body, "nr", ""):
                    args.append("--nr=" + str_field(body, "nr"))
                if str_field(body, "upscaler", ""):
                    args.append("--upscaler=" + str_field(body, "upscaler"))
                self._json({"job": start_job([str(SCRIPT)] + args, folder=path)})

            elif url.path == "/api/gamelog":
                directory = contained(str_field(body, "path"), game_roots(),
                                       "Game folder", "the known game folders")
                logs = game_logs(directory, str_field(body, "engine", ""))
                wanted = str_field(body, "log", "") or (logs[0]["path"] if logs else None)

                content, size, truncated = ("", 0, False)
                if wanted:
                    # Only logs belonging to the folder they were claimed for.
                    wanted = str(contained(wanted, [directory], "Log file", "the game folder"))
                    content, size, truncated = read_log_tail(wanted)

                self._json({
                    "logs": logs,
                    "chosen": wanted,
                    "content": content,
                    "size": size,
                    "truncated": truncated,
                    "captures": (directory / CAPTURE_DIR).is_dir(),
                })

            else:
                self._json({"error": "not found"}, 404)

        except Busy as exc:
            self._json({"error": str(exc)}, 409)
        except BadRequest as exc:
            self._json({"error": str(exc)}, 400)
        except KeyError as exc:
            self._json({"error": "missing field: %s" % exc}, 400)
        except Exception as exc:                      # noqa: BLE001 - surfaced to the UI
            traceback.print_exc()
            self._json({"error": str(exc)}, 500)


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DLSS 5 on Linux</title>
<style>
  :root {
    --bg:#f6f7f9; --panel:#ffffff; --line:#dfe3e8; --ink:#14171a; --muted:#666e78;
    --accent:#2f6fed; --ok:#1a7f45; --warn:#9a6400; --bad:#b3261e;
    --ok-bg:#e6f4ec; --warn-bg:#fdf3e0; --bad-bg:#fdeceb; --info-bg:#eef2fb;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg:#14171a; --panel:#1c2024; --line:#2c3238; --ink:#e8eaed; --muted:#9aa3ad;
      --accent:#79a5ff; --ok:#5fd08a; --warn:#e0b25a; --bad:#f0857c;
      --ok-bg:#16301f; --warn-bg:#302614; --bad-bg:#331b19; --info-bg:#1b2333;
    }
  }
  * { box-sizing:border-box; }
  body {
    margin:0; background:var(--bg); color:var(--ink);
    font:14px/1.5 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  }
  code, pre, .mono { font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }

  header {
    padding:14px 20px; border-bottom:1px solid var(--line); background:var(--panel);
    display:flex; align-items:center; gap:16px; flex-wrap:wrap;
  }
  header h1 { margin:0; font-size:16px; font-weight:650; letter-spacing:-.01em; }
  header .sub { color:var(--muted); font-size:12px; }
  #preflight { display:flex; gap:6px; flex-wrap:wrap; margin-left:auto; }
  .chip {
    font-size:11.5px; padding:3px 9px; border-radius:999px;
    background:var(--info-bg); color:var(--muted); white-space:nowrap;
  }
  .chip.ok   { background:var(--ok-bg);   color:var(--ok); }
  .chip.warn { background:var(--warn-bg); color:var(--warn); }
  .chip.bad  { background:var(--bad-bg);  color:var(--bad); font-weight:650; }

  main { display:grid; grid-template-columns:minmax(260px,340px) 1fr; gap:0; align-items:start; }
  @media (max-width:820px) { main { grid-template-columns:1fr; } }

  #list { border-right:1px solid var(--line); min-height:calc(100vh - 60px); background:var(--panel); }
  .listbar { padding:10px 12px; border-bottom:1px solid var(--line); display:flex; gap:8px; }
  .listbar input {
    flex:1; min-width:0; padding:7px 10px; border:1px solid var(--line);
    border-radius:7px; background:var(--bg); color:var(--ink); font:inherit;
  }
  #games { list-style:none; margin:0; padding:6px; max-height:calc(100vh - 118px); overflow:auto; }
  #games li {
    padding:9px 11px; border-radius:8px; cursor:pointer;
    display:flex; align-items:center; gap:9px; justify-content:space-between;
  }
  #games li:hover { background:var(--info-bg); }
  #games li[aria-selected="true"] { background:var(--info-bg); outline:1px solid var(--accent); }
  #games .nm { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  #games .rm {
    padding:0; width:20px; height:20px; line-height:1; flex:none; font-size:15px;
    border:none; background:none; color:var(--muted); opacity:0; border-radius:5px;
  }
  #games li:hover .rm, #games li:focus-within .rm { opacity:1; }
  #games .rm:hover { background:var(--bad-bg); color:var(--bad); opacity:1; }

  /* Live status. The dot pulses only while a process is actually alive, so a
     glance at the list says which game is running without reading anything. */
  .live {
    flex:none; width:8px; height:8px; border-radius:50%; background:var(--ok);
    animation:pulse 1.6s ease-in-out infinite;
  }
  @keyframes pulse { 0%,100% { opacity:1 } 50% { opacity:.25 } }
  @media (prefers-reduced-motion: reduce) { .live { animation:none } }

  .status {
    display:flex; gap:12px; align-items:flex-start; padding:13px 15px;
    border-radius:10px; margin:14px 0; border:1px solid var(--line);
    background:var(--info-bg);
  }
  .status.ok   { background:var(--ok-bg);   border-color:var(--ok);   }
  .status.warn { background:var(--warn-bg); border-color:var(--warn); }
  .status.bad  { background:var(--bad-bg);  border-color:var(--bad);  }
  .status .mark { font-size:17px; line-height:1.3; flex:none; }
  .status .hd { font-weight:650; }
  .status.ok   .hd { color:var(--ok);   }
  .status.warn .hd { color:var(--warn); }
  .status.bad  .hd { color:var(--bad);  }
  .status .dt { color:var(--muted); font-size:12.5px; margin-top:3px; }

  .addbar { align-items:center; }
  .addbar input {
    font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; font-size:12px;
  }
  .adderr {
    padding:0 12px 10px; font-size:12px; color:var(--bad); border-bottom:1px solid var(--line);
  }

  .badge {
    font-size:10.5px; font-weight:650; letter-spacing:.03em; text-transform:uppercase;
    padding:2.5px 7px; border-radius:5px; background:var(--info-bg); color:var(--muted); flex:none;
  }
  .badge.supported   { background:var(--ok-bg);   color:var(--ok); }
  .badge.partial     { background:var(--warn-bg); color:var(--warn); }
  .badge.blocked,
  .badge.missing,
  .badge.unsupported { background:var(--bad-bg);  color:var(--bad); }

  #detail { padding:20px 24px; }

  /* The pane was capped at 900px, which left a third of a wide window empty
     while the facts wrapped to a second row and every control stacked. Above
     1200px the read-only half (status, facts, reasons) and the half you act on
     (controls, config, launch string) sit side by side instead; below it they
     stack exactly as before. The log stays full width under both. */
  .panes { display:grid; gap:8px 26px; align-items:start; }
  @media (min-width:1200px) {
    /* min-content / 1fr, not auto / auto: the controls column spans both rows,
       and with two auto rows the browser spreads its extra height over both,
       leaving a gap between the reasons and the log. Pinning row 1 to the
       left column's own height sends all of the slack to row 2. */
    .panes { grid-template-columns:minmax(0,1fr) minmax(360px,440px);
             grid-template-rows:min-content 1fr; }
    /* The controls column is much taller than facts+reasons, so the log goes
       under the left one rather than below both - otherwise the left column is
       a blank 400px while the log is pushed past the fold. Stacked, DOM order
       already gives facts, controls, log. */
    .panes > .col-read  { grid-column:1; grid-row:1; }
    .panes > .col-act   { grid-column:2; grid-row:1 / span 2; }
    .panes > .logwrap   { grid-column:1; grid-row:2; }
  }
  .panes > .logwrap { margin-top:14px; min-width:0; }
  .empty { color:var(--muted); padding:40px 0; }
  h2 { margin:0 0 2px; font-size:19px; letter-spacing:-.01em; }
  .path { color:var(--muted); font-size:12px; word-break:break-all; margin-bottom:16px; }

  /* auto-fit stretched six tiles across the full width and left "Installed"
     alone on a second row. Three columns divides six evenly at any width the
     left pane actually takes. */
  .facts { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:10px; margin:14px 0; }
  @media (max-width:560px) { .facts { grid-template-columns:repeat(2,minmax(0,1fr)); } }
  .fact { background:var(--panel); border:1px solid var(--line); border-radius:9px; padding:9px 11px; }
  .fact .k { font-size:11px; color:var(--muted); text-transform:uppercase; letter-spacing:.04em; }
  .fact .v { font-size:13.5px; margin-top:2px; word-break:break-word; }

  .reasons { margin:14px 0; padding:0; list-style:none; }
  .reasons li {
    padding:9px 12px; border-radius:8px; background:var(--info-bg);
    margin-bottom:6px; font-size:13px;
  }
  .reasons li.bad  { background:var(--bad-bg);  color:var(--bad); }
  .reasons li.warn { background:var(--warn-bg); color:var(--warn); }

  .controls {
    display:flex; gap:10px; flex-wrap:wrap; align-items:center;
    padding:14px 0; border-top:1px solid var(--line); margin-top:16px;
  }
  label.f { display:flex; flex-direction:column; gap:3px; font-size:11px; color:var(--muted); }
  select, button {
    font:inherit; padding:7px 12px; border-radius:7px;
    border:1px solid var(--line); background:var(--panel); color:var(--ink); cursor:pointer;
  }
  button.primary { background:var(--accent); border-color:var(--accent); color:#fff; font-weight:600; }
  button.danger  { color:var(--bad); }
  button:disabled { opacity:.45; cursor:not-allowed; }

  .opts { margin-top:16px; }
  .opts textarea {
    width:100%; min-height:70px; padding:10px; border-radius:8px; resize:vertical;
    border:1px solid var(--line); background:var(--panel); color:var(--ink);
    font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; font-size:12px;
  }
  .logwrap { margin-top:18px; border-top:1px solid var(--line); padding-top:14px; }
  .logtabs { display:flex; gap:8px; align-items:center; flex-wrap:wrap; margin-bottom:8px; }
  .logtabs button.tab { border-radius:7px 7px 0 0; border-bottom-color:transparent; }
  .logtabs button.tab[aria-selected="true"] {
    background:var(--info-bg); border-color:var(--accent); color:var(--accent); font-weight:600;
  }
  .logmeta { color:var(--muted); font-size:11.5px; margin-left:auto; }
  .live { color:var(--ok); font-weight:600; }
  #log {
    background:#0d1013; color:#cfd6dd; border-radius:9px;
    padding:12px 14px; font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
    font-size:12px; white-space:pre-wrap; max-height:340px; overflow:auto; margin:0;
  }
  .note { color:var(--muted); font-size:12px; margin-top:10px; }
</style>
</head>
<body>

<header>
  <div>
    <h1>DLSS 5 Neural Rendering</h1>
    <div class="sub">Probe, install and launch &mdash; Fedora / Proton</div>
  </div>
  <div id="preflight"></div>
</header>

<main>
  <div id="list">
    <div class="listbar">
      <input id="filter" type="search" placeholder="Filter games" autocomplete="off">
      <button id="probeall" title="Probe every game (slow)">Probe all</button>
    </div>
    <div class="listbar addbar">
      <input id="addpath" type="text" spellcheck="false" autocomplete="off"
             placeholder="/path/to/a/game not in Steam">
      <button id="addfolder" title="Remember a game folder Steam does not list">Add</button>
    </div>
    <div id="adderr" class="adderr" hidden></div>
    <ul id="games"><li class="empty" style="padding:20px">Loading&hellip;</li></ul>
  </div>

  <div id="detail"><div class="empty">Select a game on the left.</div></div>
</main>

<script>
const TOKEN = "__TOKEN__";
let GAMES = [], PROTONS = [], SELECTED = null;
const ACTIVITY = { running: {}, status: null };   // refreshed by pollActivity()
const RESULTS = {};   // appid -> probe result
const CONFIG  = {};   // appid -> OptiScaler.ini values

// Log panel state lives outside the DOM so it survives the re-render that
// follows every job (a job finishing triggers a re-probe, which repaints).
const LOGS = { job: "", game: "", tab: "job", meta: "", live: false, file: null, files: [] };
let gameLogTimer = null;

const api = async (path, body) => {
  const res = await fetch(path, {
    method: body ? "POST" : "GET",
    headers: { "X-Token": TOKEN, "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  return res.json();
};

// navigator.clipboard needs a secure context; http://127.0.0.1 counts as one,
// but it still rejects when the document is not focused, so keep a fallback.
async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (e) {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.cssText = "position:fixed;top:-9999px;opacity:0";
    document.body.appendChild(ta);
    ta.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch (_) { ok = false; }
    ta.remove();
    return ok;
  }
}

const fmtBytes = n => n < 1024 ? n + " B" : (n / 1024).toFixed(1) + " KB";
const fmtAge = s => s < 90 ? Math.round(s) + "s"
                  : s < 5400 ? Math.round(s / 60) + " min"
                  : s < 172800 ? Math.round(s / 3600) + " h"
                  : Math.round(s / 86400) + " days";

function flash(btn, msg) {
  const original = btn.dataset.label || btn.textContent;
  btn.dataset.label = original;
  btn.textContent = msg;
  setTimeout(() => { btn.textContent = btn.dataset.label; }, 1400);
}

const el = (tag, props = {}, kids = []) => {
  const node = Object.assign(document.createElement(tag), props);
  for (const k of [].concat(kids)) node.append(k);
  return node;
};

// ---- preflight -----------------------------------------------------------

async function loadPreflight() {
  const data = await api("/api/preflight");
  const box = document.getElementById("preflight");
  const chips = (data.lines || []).map(l =>
    el("span", { className: "chip " + l.kind, textContent: l.text, title: l.text }));

  // A non-zero exit means nothing downstream will work. Saying so up front beats
  // leaving the reason to be read out of one grey chip among a dozen.
  if (!data.ok) chips.unshift(el("span", { className: "chip bad",
    textContent: "Preflight failed", title: "dlss5-linux.sh --check exited non-zero" }));

  box.replaceChildren(...chips);
}

// ---- game list -----------------------------------------------------------

// The resolved binary directory when we have probed, the folder itself when we
// have not. Everything runtime - the log, the process cwd - is in the former.
const gameDir = g => (RESULTS[g.appid] || {}).dir || g.path;

function renderList() {
  const needle = document.getElementById("filter").value.toLowerCase();
  const ul = document.getElementById("games");
  const shown = GAMES.filter(g => g.name.toLowerCase().includes(needle));

  if (!shown.length) {
    ul.replaceChildren(el("li", { className: "empty", style: "padding:20px",
      textContent: GAMES.length ? "No match."
                                : "No games found. Add a folder above." }));
    return;
  }

  ul.replaceChildren(...shown.map(g => {
    // A folder that has since been deleted or unmounted is said so outright,
    // rather than being probed so the script can fail on it.
    const verdict = g.missing ? "missing" : RESULTS[g.appid]?.verdict;
    const live = !!ACTIVITY.running[gameDir(g)];
    const li = el("li", { tabIndex: 0 }, [
      el("span", { className: "nm", textContent: g.name, title: g.path }),
      ...(live ? [el("span", { className: "live", title: "Running now" })] : []),
      el("span", { className: "badge " + (verdict || ""), textContent: verdict || "?" }),
    ]);
    if (g.manual) {
      const rm = el("button", { className: "rm", textContent: "\u00d7",
                                title: "Forget " + g.path + " (nothing is deleted)" });
      rm.onclick = e => { e.stopPropagation(); removeFolder(g); };
      li.append(rm);
    }
    li.setAttribute("aria-selected", SELECTED?.appid === g.appid);
    li.onclick = () => select(g);
    li.onkeydown = e => { if (e.key === "Enter") select(g); };
    return li;
  }));
}

async function select(game) {
  SELECTED = game;
  stopFollowing();
  if (game.missing) {
    renderList();
    renderDetail();
    return;
  }
  Object.assign(LOGS, { job: "", game: "", tab: "job", meta: "", file: null, files: [], captures: false });

  renderList();
  renderDetail();
  pollActivity();
  if (!RESULTS[game.appid]) await probeOne(game);

  // Learn which logs exist so "Open log file" / "Open captures" reflect reality.
  const r = RESULTS[game.appid] || {};
  const data = await api("/api/gamelog", { path: r.dir || game.path, engine: r.installed?.engine });
  LOGS.files = data.logs || [];
  LOGS.file = data.chosen;
  LOGS.captures = data.captures;

  const cfgRes = await api("/api/config", { path: r.dir || game.path });
  if (cfgRes.config) CONFIG[game.appid] = cfgRes.config; else delete CONFIG[game.appid];

  renderDetail();
}

async function probeOne(game) {
  RESULTS[game.appid] = { verdict: "…", reasons: ["Probing…"] };
  renderList();
  if (SELECTED?.appid === game.appid) renderDetail();

  RESULTS[game.appid] = await api("/api/probe", { path: game.path });
  renderList();
  if (SELECTED?.appid === game.appid) renderDetail();
}

// ---- live activity -------------------------------------------------------

// One poll answers both questions: which games have a live process, and what
// the selected game's log says it is doing. It runs whether or not the launch
// came from here - Steam and Heroic produce the same process, so a game started
// outside this page still lights up.
let activityTimer = null;

async function pollActivity() {
  const paths = GAMES.filter(g => !g.missing).map(gameDir);
  const detail = SELECTED && !SELECTED.missing ? gameDir(SELECTED) : "";

  let data;
  try {
    data = await api("/api/activity", { paths, detail });
  } catch (e) {
    return;  // a poll that fails is not worth reporting; the next one will do
  }
  if (data.error) return;

  const before = JSON.stringify(ACTIVITY);
  ACTIVITY.running = data.running || {};
  ACTIVITY.status = data.status || null;
  if (JSON.stringify(ACTIVITY) === before) return;   // nothing moved, no repaint

  renderList();
  renderDetail();
}

function startActivity() {
  if (activityTimer) clearInterval(activityTimer);
  pollActivity();
  // 3s: fast enough that starting a game from Steam shows up while you are
  // still looking at the page, slow enough that a /proc scan costs nothing.
  activityTimer = setInterval(pollActivity, 3000);
}

// ---- added folders -------------------------------------------------------

function addError(message) {
  const box = document.getElementById("adderr");
  box.textContent = message || "";
  box.hidden = !message;
}

async function loadGames() {
  const data = await api("/api/games");
  GAMES = data.games || [];
  PROTONS = data.protons || [];
  // The selected game object is replaced wholesale by a reload, so re-point
  // SELECTED at the new one or the detail pane keeps rendering a stale copy.
  if (SELECTED) SELECTED = GAMES.find(g => g.appid === SELECTED.appid) || null;
  renderList();
}

async function addFolder() {
  const input = document.getElementById("addpath");
  const btn = document.getElementById("addfolder");
  const path = input.value.trim();
  if (!path) return;

  btn.disabled = true;
  const res = await api("/api/folders/add", { path });
  btn.disabled = false;

  if (res.error) { addError(res.error); input.focus(); return; }

  addError("");
  input.value = "";
  await loadGames();

  // Jump straight to it: adding a folder is only ever a prelude to probing it.
  const added = GAMES.find(g => g.path === res.added);
  if (added) {
    document.getElementById("filter").value = "";
    renderList();
    select(added);
  }
}

async function removeFolder(game) {
  const res = await api("/api/folders/remove", { path: game.path });
  if (res.error) { addError(res.error); return; }

  delete RESULTS[game.appid];
  delete CONFIG[game.appid];
  if (SELECTED?.appid === game.appid) { SELECTED = null; stopFollowing(); }
  addError("");
  await loadGames();
  renderDetail();
}

// Probe everything, a few at a time so the box stays responsive.
async function probeAll() {
  const btn = document.getElementById("probeall");
  btn.disabled = true;
  const queue = GAMES.filter(g => !g.missing && !RESULTS[g.appid]);
  const workers = Array.from({ length: 3 }, async () => {
    while (queue.length) await probeOne(queue.shift());
  });
  await Promise.all(workers);
  btn.disabled = false;
}

// ---- detail pane ---------------------------------------------------------

function renderDetail() {
  const box = document.getElementById("detail");
  if (!SELECTED) {
    box.replaceChildren(el("div", { className: "empty", textContent: "Select a game on the left." }));
    return;
  }

  const g = SELECTED;
  if (g.missing) {
    box.replaceChildren(el("div", { className: "empty" }, [
      el("div", { textContent: "This folder is gone: " + g.path }),
      el("div", { style: "margin-top:8px", textContent:
        "It was added by hand and has since been moved, deleted or unmounted. " +
        "Remove it with the \u00d7 in the list, or restore the folder and reload." }),
    ]));
    return;
  }

  const r = RESULTS[g.appid] || {};
  const installed = r.installed;
  const blocked = r.verdict === "blocked";
  const hook = installed?.hook || r.hook || "dxgi.dll";

  const fact = (k, v) => el("div", { className: "fact" }, [
    el("div", { className: "k", textContent: k }),
    el("div", { className: "v", textContent: v }),
  ]);

  // "Installed" and "working" are different things, and only the injector's own
  // log can tell them apart: it can load, hook and still never dispatch the
  // neural pass. This says which of those actually happened.
  const statusBanner = () => {
    const live = !!ACTIVITY.running[gameDir(g)];
    const st = ACTIVITY.status;

    if (!installed && !live) return null;
    if (!st) {
      return el("div", { className: "status" }, [
        el("span", { className: "mark", textContent: live ? "\u25cf" : "\u25cb" }),
        el("div", {}, [
          el("div", { className: "hd", textContent: live
            ? "Running - waiting for the injector to write its log"
            : "Installed, never run" }),
          el("div", { className: "dt", textContent: live
            ? "The game is up. OptiScaler.log appears once it loads."
            : "Launch the game and this will say whether the neural pass ran." }),
        ]),
      ]);
    }

    // A log older than a couple of minutes with nothing running describes the
    // last session, not this moment, and saying so stops it reading as live.
    const stale = !live && st.age > 120;
    const when = stale ? " (last session, " + fmtAge(st.age) + " ago)" : "";

    return el("div", { className: "status " + (stale ? "" : st.state) }, [
      el("span", { className: "mark", textContent:
        st.state === "ok" ? "\u2713" : st.state === "warn" ? "!" : "\u2717" }),
      el("div", {}, [
        el("div", { className: "hd" }, [
          el("span", { textContent: st.headline + when }),
          ...(live ? [el("span", { className: "live", style: "margin-left:8px" },
                              [])] : []),
        ]),
        el("div", { className: "dt", textContent: st.detail || "" }),
      ]),
    ]);
  };

  const engineSel = el("select", { id: "engine" }, [
    el("option", { value: "optiscaler", textContent: "OptiScaler (recommended)" }),
    el("option", { value: "reshade", textContent: "ReShade + RenoDX" }),
  ]);
  if (installed) engineSel.value = installed.engine;

  const hookSel = el("select", { id: "hook" },
    ["dxgi.dll", "d3d12.dll", "version.dll", "winmm.dll", "dbghelp.dll", "wininet.dll", "winhttp.dll"]
      .map(n => el("option", { value: n, textContent: n })));
  hookSel.value = hook;

  const protonSel = el("select", { id: "proton" },
    PROTONS.map(p => el("option", { value: p.path, textContent: p.name })));

  const bInstall = el("button", { className: "primary", textContent: installed ? "Reinstall" : "Install" });
  const bRemove  = el("button", { className: "danger", textContent: "Uninstall", disabled: !installed });
  const bLaunch  = el("button", { textContent: "Launch with overrides", disabled: !installed || !PROTONS.length });
  const bProbe   = el("button", { textContent: "Re-probe" });

  bInstall.disabled = blocked;
  bInstall.title = blocked ? "Anti-cheat detected - refusing to inject." : "";

  bProbe.onclick   = () => probeOne(g);
  // Install gets the game ROOT, not r.dir. dlss5-linux.sh resolves the binary
  // directory itself, and it scans for anti-cheat from the path we hand it -
  // anti-cheat lives above the binary (Halo MCC), so sending the resolved
  // directory would walk straight past the refusal.
  bInstall.onclick = () => job("/api/install", {
    path: g.path, engine: engineSel.value, hook: hookSel.value });
  // Uninstall stays on r.dir: it has no anti-cheat refusal to protect, and
  // dlss5-linux.sh --uninstall deletes from the path it is handed without
  // resolving it, so the root would remove nothing at all.
  bRemove.onclick  = () => job("/api/uninstall", { path: r.dir || g.path });
  // Launch never goes through the script, so there is no refusal to defeat, and
  // the resolved directory is both exact and where the log follow expects to look.
  bLaunch.onclick  = () => job("/api/launch", {
    game: { ...g, path: r.dir || g.path, executable: r.executable },
    proton: protonSel.value, hook: hookSel.value });

  const opts = el("textarea", { readOnly: true, id: "optsbox", value: "" });
  api("/api/launch-options", { hook: hookSel.value }).then(d => { opts.value = d.options; });
  hookSel.onchange = () =>
    api("/api/launch-options", { hook: hookSel.value }).then(d => { opts.value = d.options; });

  const bCopy = el("button", { textContent: "Copy" });
  bCopy.onclick = async () => flash(bCopy, (await copyText(opts.value)) ? "Copied" : "Copy failed");

  const dir = r.dir || g.path;
  const bFolder = el("button", { textContent: "Open folder" });
  bFolder.onclick = () => api("/api/open", { path: dir });

  const bOpenLog = el("button", { textContent: "Open log file", disabled: !LOGS.file });
  bOpenLog.onclick = () => LOGS.file && api("/api/open", { path: LOGS.file });

  const bCaptures = el("button", { textContent: "Open captures" });
  bCaptures.style.display = LOGS.captures ? "" : "none";
  bCaptures.title = "Before/after frames the neural pass writes each session";
  bCaptures.onclick = () => api("/api/open", { path: dir + "/dlssnr-capture" });

  // --- log panel ---
  const tabJob  = el("button", { className: "tab", textContent: "Output" });
  const tabGame = el("button", { className: "tab", textContent: "Game log" });
  tabJob.setAttribute("aria-selected", LOGS.tab === "job");
  tabGame.setAttribute("aria-selected", LOGS.tab === "game");
  tabJob.onclick  = () => { LOGS.tab = "job"; paintLog(); };
  tabGame.onclick = () => { LOGS.tab = "game"; refreshGameLog(dir, engineSel.value); };

  const fileSel = el("select", { id: "logfile" },
    LOGS.files.map(f => el("option", { value: f.path, textContent: f.name })));
  fileSel.style.display = LOGS.files.length > 1 ? "" : "none";
  if (LOGS.file) fileSel.value = LOGS.file;
  fileSel.onchange = () => { LOGS.file = fileSel.value; refreshGameLog(dir, engineSel.value); };

  const bRefresh = el("button", { textContent: "Refresh" });
  bRefresh.onclick = () => refreshGameLog(dir, engineSel.value);

  const bCopyLog = el("button", { textContent: "Copy log" });
  bCopyLog.onclick = async () => {
    const text = (LOGS.tab === "job" ? LOGS.job : LOGS.game) || "";
    if (!text) { flash(bCopyLog, "Nothing to copy"); return; }
    const ok = await copyText(text);
    flash(bCopyLog, ok ? "Copied " + fmtBytes(text.length) : "Copy failed");
  };

  // --- OptiScaler.ini settings (only meaningful once installed) ---
  const cfg = CONFIG[g.appid];
  const nrOn = cfg && cfg.nr === "true";

  const overlayKey = (installed?.engine === "reshade") ? "Home" : "Insert";
  const nrBox = el("input", { type: "checkbox", id: "nrbox", checked: !!nrOn, disabled: !cfg });
  nrBox.onchange = () => job("/api/configure", { path: dir, nr: nrBox.checked ? "on" : "off" });

  const upSel = el("select", { id: "upscaler", disabled: !cfg },
    ["auto", "dlss", "xess", "fsr21", "fsr22", "ffx"]
      .map(v => el("option", { value: v, textContent: v })));
  if (cfg) upSel.value = cfg.upscaler;
  upSel.onchange = () => job("/api/configure", { path: dir, upscaler: upSel.value });

  box.replaceChildren(
    el("h2", { textContent: g.name }),
    el("div", { className: "path", textContent: r.dir || g.path }),

    statusBanner() || "",

    el("div", { className: "panes" }, [
     el("div", { className: "col-read" }, [
      el("div", { className: "facts" }, [
        fact("Verdict", r.verdict || "not probed"),
        fact("Executable", r.executable || "—"),
        fact("Graphics API", r.api || "—"),
        fact("Upscalers", (r.upscalers || []).join(", ") || "none detected"),
        fact("Anti-cheat", (r.anticheat || []).join(", ") || "none detected"),
        fact("Installed", installed ? installed.engine + " as " + installed.hook : "no"),
      ]),

      el("ul", { className: "reasons" }, (r.reasons || []).map(text =>
        el("li", { className: blocked ? "bad" : r.verdict === "partial" ? "warn" : "", textContent: text }))),
     ]),

     el("div", { className: "col-act" }, [
      el("div", { className: "controls", style: "border-top:0;padding-top:0" }, [
      el("label", { className: "f" }, ["Engine", engineSel]),
      el("label", { className: "f" }, ["Proxy DLL", hookSel]),
      el("label", { className: "f" }, ["Proton", protonSel]),
      bProbe, bInstall, bRemove, bLaunch,
    ]),

    el("div", { className: "controls", style: "border-top:0;padding-top:0" },
      [bFolder, bOpenLog, bCaptures]),

    el("div", { className: "controls" }, [
      el("label", { className: "f", style: "flex-direction:row;align-items:center;gap:6px;font-size:13px;color:var(--ink)" },
        [nrBox, "Enable Neural Rendering"]),
      el("label", { className: "f" }, ["DX12 upscaler", upSel]),
      el("span", { className: "note", style: "margin:0;flex-basis:100%" }, [
        !cfg ? "Install first to configure OptiScaler.ini."
        : !!ACTIVITY.running[gameDir(g)]
          // OptiScaler reads the ini once, at startup - it imports no file-watch
          // API and has no reload action - so a change made now is real on disk
          // and simply will not reach the session that is already running.
          ? "The game is running. OptiScaler reads OptiScaler.ini only at startup, so a change "
            + "here is saved but does not reach this session \u2014 it applies the next time the "
            + "game starts. To change it now, press " + overlayKey + " in the game for the "
            + "OptiScaler overlay; everything under DLSS Neural Rendering is live there except "
            + "Preset, which is baked in when the model is created."
          : "Written straight into OptiScaler.ini, so it is the state the game starts in \u2014 no "
            + "overlay needed. Force 'dlss' if OptiScaler mis-detects the GPU under Proton and "
            + "falls back to XeSS."
      ]),
    ]),

    el("div", { className: "opts" }, [
      el("div", { className: "k", style: "font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;margin-bottom:5px" },
        "Steam launch options"),
      opts,
      el("div", { style: "margin-top:8px" }, bCopy),
      el("div", { className: "note", textContent:
        "\"Launch with overrides\" runs the game through Proton directly and applies these itself. " +
        "Use the copied string instead if you would rather start the game from Steam." }),
      ]),
     ]),

     el("div", { className: "logwrap" }, [
      el("div", { className: "logtabs" }, [
        tabJob, tabGame, fileSel, bRefresh, bCopyLog,
        el("span", { className: "logmeta", id: "logmeta" }),
      ]),
      el("pre", { id: "log" }),
     ]),
    ]),
  );

  paintLog();
  if (LOGS.tab === "game" && !LOGS.game) refreshGameLog(dir, engineSel.value);
}

// ---- log panel -----------------------------------------------------------

function paintLog() {
  const pre = document.getElementById("log");
  if (!pre) return;

  const atBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 40;
  pre.textContent = (LOGS.tab === "job" ? LOGS.job : LOGS.game) || "(empty)";
  if (atBottom) pre.scrollTop = pre.scrollHeight;

  const meta = document.getElementById("logmeta");
  if (meta) {
    meta.textContent = LOGS.tab === "game" ? LOGS.meta : "";
    meta.className = "logmeta" + (LOGS.live ? " live" : "");
  }

  const tabs = document.querySelectorAll(".logtabs .tab");
  if (tabs.length === 2) {
    tabs[0].setAttribute("aria-selected", LOGS.tab === "job");
    tabs[1].setAttribute("aria-selected", LOGS.tab === "game");
  }
}

async function refreshGameLog(dir, engine) {
  LOGS.tab = "game";
  const data = await api("/api/gamelog", { path: dir, engine, log: LOGS.file });

  LOGS.files = data.logs || [];
  LOGS.captures = data.captures;

  if (!LOGS.files.length) {
    LOGS.game = "No log file yet.\n\n" +
      "OptiScaler writes OptiScaler.log beside the proxy DLL once the game has run.\n" +
      "ReShade writes ReShade.log. Launch the game first.";
    LOGS.meta = "";
    paintLog();
    return;
  }

  LOGS.file = data.chosen;
  LOGS.game = data.content || "(log is empty)";
  const kb = (data.size / 1024).toFixed(1);
  LOGS.meta = (LOGS.live ? "live · " : "") + kb + " KB" + (data.truncated ? " (tail)" : "");
  paintLog();
}

// While a launch job runs, poll the game's own log so the overlay's verdict
// ("the model would not initialise") shows up here without hunting for a file.
function followGameLog(dir, engine) {
  stopFollowing();
  LOGS.live = true;
  LOGS.tab = "game";
  gameLogTimer = setInterval(() => refreshGameLog(dir, engine), 1500);
  refreshGameLog(dir, engine);
}

function stopFollowing() {
  if (gameLogTimer) clearInterval(gameLogTimer);
  gameLogTimer = null;
  LOGS.live = false;
}

// ---- jobs ----------------------------------------------------------------

async function job(path, body) {
  const isLaunch = path.endsWith("/launch");
  const started = Date.now();
  const dir = body.path || body.game?.path;
  const engine = document.getElementById("engine")?.value;

  LOGS.tab = "job";
  LOGS.job = "Starting…\n";
  paintLog();

  const res = await api(path, body);
  if (res.error) { LOGS.job = "Error: " + res.error; paintLog(); return; }

  // A launch job runs for as long as the game does, so switch straight to the
  // game's own log and follow it live.
  if (isLaunch && dir) followGameLog(dir, engine);

  const poll = setInterval(async () => {
    const j = await api("/api/job/" + res.job);
    LOGS.job = j.output || "…";
    paintLog();
    if (j.status === "running") return;

    clearInterval(poll);
    // A game exits nonzero all the time - alt-F4, a quit button, a shutdown
    // path that never calls exit(0) - so "failed" is the wrong word for a
    // launch, and it sent us hunting for a launch failure that never happened
    // after Expedition 33 ran for 39 seconds and rendered the whole time.
    // Whether it worked is a question for the game log, not the exit code.
    LOGS.job += isLaunch
      ? "\n[game exited, code " + j.rc + "]"
      : "\n[" + j.status + ", exit " + j.rc + "]";

    if (isLaunch) {
      // A game that dies this fast never reached D3D12, so the injector is not
      // the suspect - it is almost always Steam API / DRM refusing a launch
      // that did not come from Steam.
      const secs = (Date.now() - started) / 1000;
      if (secs < 15) {
        LOGS.job += "\n\nThe game exited after " + secs.toFixed(1) + "s.\n" +
          "That is too fast to have created a graphics device, so this is not the\n" +
          "injector failing. Games with Steam DRM or Denuvo usually refuse to start\n" +
          "unless Steam launched them. Copy the launch options above into Steam\n" +
          "(Properties > General > Launch Options) and start it from Steam instead.";
      } else {
        LOGS.job += "\n\nThe game ran for " + secs.toFixed(0) + "s, so it started fine - " +
          "a nonzero exit code on quit is normal.\nSwitch to the Game log tab to see what " +
          "the injector actually did.";
      }
      stopFollowing();
      if (dir) refreshGameLog(dir, engine);
    } else {
      paintLog();
      if (SELECTED) {
        const d = (RESULTS[SELECTED.appid] || {}).dir || SELECTED.path;
        const c = await api("/api/config", { path: d });
        if (c.config) CONFIG[SELECTED.appid] = c.config; else delete CONFIG[SELECTED.appid];
        probeOne(SELECTED);
      }
    }
  }, 700);
}

// ---- boot ----------------------------------------------------------------

(async () => {
  loadPreflight();
  await loadGames();
  document.getElementById("filter").oninput = renderList;
  document.getElementById("probeall").onclick = probeAll;
  document.getElementById("addfolder").onclick = addFolder;
  document.getElementById("addpath").onkeydown = e => {
    if (e.key === "Enter") addFolder();
  };
  startActivity();
})();
</script>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description="Local web GUI for dlss5-linux.sh")
    parser.add_argument("--port", type=int, default=8777)
    parser.add_argument("--no-open", action="store_true", help="do not open a browser")
    args = parser.parse_args()

    if not SCRIPT.is_file():
        sys.exit("dlss5-linux.sh not found next to this script (%s)" % SCRIPT)
    if not os.access(SCRIPT, os.X_OK):
        sys.exit("dlss5-linux.sh is not executable: chmod +x %s" % SCRIPT)

    url = "http://127.0.0.1:%d/?t=%s" % (args.port, TOKEN)
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError as exc:
        sys.exit("cannot listen on 127.0.0.1:%d - %s\nSomething else is using that port; try --port %d"
                 % (args.port, exc.strerror or exc, args.port + 1))

    # flush explicitly: stdout is block-buffered when piped to a log, and the
    # URL below is the only way in - it must appear immediately.
    print("dlss5-gui  ->  %s" % url, flush=True)
    print("(localhost only, token required; Ctrl-C to stop)", flush=True)

    if not args.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
