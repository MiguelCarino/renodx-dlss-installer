#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
dlss5-app.py - "DLSS 5 for Linux" as a desktop application

The same interface as dlss5-gui.py, in a window of its own: the GUI's HTTP
server runs in-process on 127.0.0.1 (an ephemeral port, so it never fights a
standalone dlss5-gui.py on 8777) and a WebKitGTK view inside a libadwaita
window shows it. The view adds what a browser tab cannot: a native folder
chooser for "Add", links that open in the real browser instead of inside the
app, and the usual desktop keys.

    ./dlss5-app.py              # native window (falls back to the browser)
    ./dlss5-app.py --browser    # skip the window, open the system browser
    ./dlss5-app.py --self-test  # headless smoke test, exit 0/1

Needs PyGObject with Gtk 4.0, Adw 1 and WebKit 6.0 for the window. Without any
of them it says which is missing and behaves like dlss5-gui.py instead, so the
packaged launcher always gets the user somewhere.

Python standard library plus gi only. Nothing is written next to this file, so
it runs unchanged from /usr/share or a read-only AppImage mount.
"""

import argparse
import importlib.util
import json
import os
import signal
import sys
import time
import threading
import webbrowser
from pathlib import Path
from urllib.parse import urlparse

APP_ID = "io.github.MiguelCarino.DLSS5Linux"
APP_NAME = "DLSS 5 for Linux"
HERE = Path(__file__).resolve().parent

# Self-test budget. The page's own preflight call has a 60 s timeout inside the
# server, but on a healthy machine the whole page settles in a second or two.
SELFTEST_TIMEOUT = 60

# A renderer that crashes this many times inside CRASH_WINDOW seconds is not
# going to recover on the next reload (a GPU driver fault, an out-of-memory
# kill); past that the app stops reloading instead of spinning forever.
CRASH_LIMIT = 3
CRASH_WINDOW = 60

# The install dir may be read-only (/usr/share, a squashfs AppImage mount):
# importing dlss5-gui.py must not try to drop a __pycache__ beside it.
sys.dont_write_bytecode = True


def load_gui():
    """Import dlss5-gui.py. The hyphen rules out a plain import statement."""
    path = HERE / "dlss5-gui.py"
    spec = importlib.util.spec_from_file_location("dlss5_gui", path)
    if spec is None or not path.is_file():
        sys.exit("dlss5-gui.py not found next to this script (%s)" % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def start_server(gui, port):
    """Bind and serve on a daemon thread; returns (server, url)."""
    gui.check_script()
    try:
        server, url = gui.make_server(port)
    except OSError as exc:
        sys.exit("cannot listen on 127.0.0.1:%d - %s" % (port, exc.strerror or exc))
    threading.Thread(target=server.serve_forever, name="dlss5-http", daemon=True).start()
    return server, url


def stop_server(server):
    if server is None:
        return
    server.shutdown()       # returns once serve_forever() has left its loop
    server.server_close()


def running_jobs(gui):
    """How many install/uninstall/launch jobs are still going.

    The jobs run on daemon threads that read the child's stdout through a pipe:
    if this process exits, the child dies of SIGPIPE on its next line of output,
    which can leave a game folder half-patched. Every way out of the app checks
    this first."""
    with gui.JOBS_LOCK:
        return sum(1 for job in gui.JOBS.values() if job["status"] == "running")


# --------------------------------------------------------------------------
# Browser fallback
# --------------------------------------------------------------------------

def run_browser(gui, port):
    """What dlss5-gui.py does, on our port: print the URL, open it, serve."""
    server, url = start_server(gui, port)
    print("dlss5-app  ->  %s" % url, flush=True)
    print("(localhost only, token required; Ctrl-C to stop)", flush=True)
    threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        threading.Event().wait()    # the server lives on its own thread
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        stop_server(server)
    return 0


def load_gi():
    """Import the toolkit, or return (None, reason) for the fallback message."""
    try:
        import gi
    except ImportError:
        return None, "PyGObject (python3-gobject) is not installed"
    for name, version in (("Gtk", "4.0"), ("Adw", "1"), ("WebKit", "6.0")):
        try:
            gi.require_version(name, version)
        except ValueError:
            return None, "%s %s typelib is not available" % (name, version)
    try:
        from gi.repository import Adw, Gdk, Gio, GLib, Gtk, WebKit
    except ImportError as exc:
        return None, "cannot load the GTK stack (%s)" % exc
    return (Adw, Gdk, Gio, GLib, Gtk, WebKit), None


# --------------------------------------------------------------------------
# Native window
# --------------------------------------------------------------------------

# Run once the page reports ready, by the self-test. Every field is something
# a broken page would get wrong: the list and the preflight chips are only
# painted after their API calls answer, and the bridge flag needs a message to
# make it from the page to Python and back.
READY_JS = r"""
(() => {
  const items = [...document.querySelectorAll('#games li')];
  const loading = items.length === 1 && /Loading/.test(items[0].textContent);
  return JSON.stringify({
    games: items.length,
    loading: loading,
    chips: document.querySelectorAll('#preflight .chip').length,
    browse: !document.getElementById('browsefolder').hidden,
    bridge: !!window.dlss5BridgeOk,
    errors: window.__dlss5Errors || [],
  });
})()
"""

# Injected only for the self-test, before the page's own script: an exception
# in the page is the failure most worth catching, and it is otherwise silent.
ERROR_TRAP_JS = r"""
window.__dlss5Errors = [];
window.addEventListener('error', e => window.__dlss5Errors.push(String(e.message)));
window.addEventListener('unhandledrejection', e =>
  window.__dlss5Errors.push('unhandled rejection: ' + String(e.reason)));
"""


def run_native(gui, mods, port, self_test):
    Adw, Gdk, Gio, GLib, Gtk, WebKit = mods

    # Window managers match the window to the .desktop file by this name
    # (StartupWMClass in the desktop entry), which is what gets the icon and
    # the "already running" state right in docks.
    GLib.set_prgname(APP_ID)
    GLib.set_application_name(APP_NAME)

    # init_check() alone is not enough: PyGObject already initialised GTK when
    # Gtk was imported, and from then on init_check() answers True even when
    # no display could be opened. Building a window on that NULL display
    # segfaults, so ask for the display itself.
    if not Gtk.init_check() or Gdk.Display.get_default() is None:
        if self_test:
            print("SELFTEST FAIL: no display to open a window on "
                  "(DISPLAY / WAYLAND_DISPLAY unset or unreachable)", flush=True)
            return 1
        print("dlss5-app: no display to open a window on - using the browser instead",
              file=sys.stderr, flush=True)
        return run_browser(gui, port)

    state = {"server": None, "url": None, "origin": None, "window": None,
             "view": None, "title": None, "rc": 0, "done": False,
             "crashes": [], "quit_when_idle": False, "signalled": False}

    # The self-test must not hand itself off to an instance the user has open.
    flags = Gio.ApplicationFlags.NON_UNIQUE if self_test else Gio.ApplicationFlags.DEFAULT_FLAGS
    app = Adw.Application(application_id=APP_ID, flags=flags)

    # -- helpers --

    def open_external(uri):
        """Hand a URI to the desktop's default handler, never to the app view."""
        window = state["window"]
        if hasattr(Gtk, "UriLauncher"):            # GTK 4.10+
            def done(launcher, result):
                try:
                    launcher.launch_finish(result)
                except GLib.Error as exc:
                    print("dlss5-app: cannot open %s - %s" % (uri, exc.message),
                          file=sys.stderr)
            Gtk.UriLauncher.new(uri).launch(window, None, done)
            return
        try:
            Gio.AppInfo.launch_default_for_uri(uri, None)
        except GLib.Error as exc:
            print("dlss5-app: cannot open %s - %s" % (uri, exc.message), file=sys.stderr)

    def run_js(script):
        state["view"].evaluate_javascript(script, -1, None, None, None, None)

    def browse_folder():
        dialog = Gtk.FileDialog(title="Choose a game folder", modal=True)
        dialog.set_initial_folder(Gio.File.new_for_path(str(Path.home())))

        def picked(dlg, result):
            try:
                folder = dlg.select_folder_finish(result)
            except GLib.Error:
                return                              # cancelled or dismissed
            path = folder.get_path() if folder else None
            if path:
                # json.dumps yields a valid JS string literal, so no path - not
                # even one with quotes or backslashes - can break out of it.
                run_js("window.dlss5Picked && window.dlss5Picked(%s)" % json.dumps(path))
        dialog.select_folder(state["window"], None, picked)

    def pick_dll():
        # Your own nvngx_dlssnr.dll (the installer and the still view take it), or
        # a .zip: the amd engine's package, or a streamline.zip holding the DLL.
        dialog = Gtk.FileDialog(title="Choose your nvngx_dlssnr.dll or package", modal=True)
        wanted = Gtk.FileFilter(name="nvngx_dlssnr.dll or .zip")
        wanted.add_pattern("*.dll")
        wanted.add_pattern("*.zip")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(wanted)
        dialog.set_filters(filters)

        def picked(dlg, result):
            try:
                chosen = dlg.open_finish(result)
            except GLib.Error:
                return                              # cancelled or dismissed
            path = chosen.get_path() if chosen else None
            if path:
                run_js("window.dlss5PickedDll && window.dlss5PickedDll(%s)" % json.dumps(path))
        dialog.open(state["window"], None, picked)

    def on_message(_manager, value):
        try:
            msg = json.loads(value.to_json(0) or "null")
        except (TypeError, ValueError):
            return
        op = msg.get("op") if isinstance(msg, dict) else None
        if op == "browse":
            browse_folder()
        elif op == "pickdll":
            pick_dll()
        elif op == "ping":
            run_js("window.dlss5Pong && window.dlss5Pong()")

    def on_decide_policy(_view, decision, kind):
        # Only our own origin belongs in this view. Anything else - a link to
        # a mod page, target=_blank, a window.open() - goes to the real browser,
        # where the user has their tabs, passwords and an address bar.
        if kind not in (WebKit.PolicyDecisionType.NAVIGATION_ACTION,
                        WebKit.PolicyDecisionType.NEW_WINDOW_ACTION):
            return False
        uri = decision.get_navigation_action().get_request().get_uri() or ""
        parsed = urlparse(uri)
        ours = "%s://%s" % (parsed.scheme, parsed.netloc) == state["origin"]
        if kind == WebKit.PolicyDecisionType.NAVIGATION_ACTION and (
                ours or parsed.scheme in ("about", "data", "blob")):
            return False
        decision.ignore()
        if parsed.scheme in ("http", "https", "mailto"):
            open_external(uri)
        return True

    def on_create(_view, action):
        # window.open() asks for a new view through "create" rather than a
        # NEW_WINDOW_ACTION decision. Returning None refuses the window; the
        # URL goes to the browser like every other off-origin link.
        uri = action.get_request().get_uri() or ""
        if urlparse(uri).scheme in ("http", "https", "mailto"):
            open_external(uri)
        return None

    def on_web_process_terminated(view, reason):
        # A crashed renderer leaves a blank window; the server still has all
        # the state, so a reload brings the user straight back - unless it
        # keeps crashing, in which case reloading again would only loop.
        if state["done"]:
            return
        if self_test:
            finish(1, "SELFTEST FAIL: web view process ended (%s)" % reason.value_nick)
            return
        now = time.monotonic()
        state["crashes"] = [t for t in state["crashes"] if now - t < CRASH_WINDOW] + [now]
        if len(state["crashes"]) >= CRASH_LIMIT:
            print("dlss5-app: web view process ended (%s) %d times in %d s - not reloading "
                  "again; use Reload or open the page in your web browser"
                  % (reason.value_nick, len(state["crashes"]), CRASH_WINDOW), file=sys.stderr)
            state["title"].set_subtitle("The page keeps crashing - try Reload, "
                                        "or open it in your web browser")
            return
        print("dlss5-app: web view process ended (%s) - reloading" % reason.value_nick,
              file=sys.stderr)
        view.load_uri(state["url"])

    def reload_page():
        # A manual reload is the user's go-ahead to try again after the crash
        # limit stopped the automatic ones.
        state["crashes"] = []
        state["title"].set_subtitle("")
        state["view"].reload()

    # -- leaving while jobs run --

    def quit_when_idle():
        """Hide the window and exit once the last job ends. The server and the
        jobs' reader threads stay up meanwhile, so nothing is cut off."""
        if state["quit_when_idle"]:
            return
        state["quit_when_idle"] = True
        if state["window"] is not None:
            state["window"].set_visible(False)

        def poll():
            if not state["quit_when_idle"]:
                return False                        # re-activated in the meantime
            if running_jobs(gui) == 0:
                app.quit()
                return False
            return True
        GLib.timeout_add(500, poll)

    def ask_before_leaving(count):
        what = "A job is" if count == 1 else "%d jobs are" % count
        heading = "%s still running" % what
        body = ("Closing now would cut it off mid-way and could leave a game folder "
                "half-installed. You can keep the window open, or close it and let the "
                "app finish in the background, then exit on its own.")
        if hasattr(Adw, "AlertDialog"):             # libadwaita 1.5+
            dialog = Adw.AlertDialog(heading=heading, body=body)
            present = lambda: dialog.present(state["window"])
        else:                                       # libadwaita 1.2 - 1.4
            dialog = Adw.MessageDialog(transient_for=state["window"], heading=heading,
                                       body=body)
            present = dialog.present
        dialog.add_response("keep", "Keep Open")
        dialog.add_response("background", "Close When Finished")
        dialog.set_default_response("keep")
        dialog.set_close_response("keep")

        def responded(_dialog, response):
            if response == "background":
                quit_when_idle()
        dialog.connect("response", responded)
        present()

    def request_quit():
        """Every way out - window close, Ctrl+Q, app.quit over D-Bus - ends here."""
        count = running_jobs(gui)
        if count == 0 or state["done"]:
            app.quit()
        elif state["window"] is not None and state["window"].get_visible():
            ask_before_leaving(count)
        else:
            quit_when_idle()

    def on_close_request(_window):
        request_quit()
        return True                                 # request_quit decides

    def on_signal(signum):
        # SIGINT (Ctrl-C in the launching terminal) and SIGTERM (logout, kill)
        # come through the main loop instead of as a KeyboardInterrupt, so the
        # server is stopped by on_shutdown like any other exit. With jobs still
        # running the first signal waits for them; a second one means "now".
        count = running_jobs(gui)
        if count == 0 or state["signalled"]:
            if count:
                print("dlss5-app: quitting with %d job(s) still running" % count,
                      file=sys.stderr, flush=True)
            app.quit()
        else:
            state["signalled"] = True
            print("dlss5-app: %d job(s) still running - exiting when they finish "
                  "(signal again to quit now)" % count, file=sys.stderr, flush=True)
            quit_when_idle()
        return True

    def add_action(target, name, callback):
        action = Gio.SimpleAction.new(name, None)
        action.connect("activate", lambda *_: callback())
        target.add_action(action)

    # -- self-test --

    def finish(rc, message):
        if state["done"]:
            return
        state["done"] = True
        state["rc"] = rc
        print(message, flush=True)
        app.quit()

    def poll_ready():
        if state["done"]:
            return False

        def got(view, result):
            try:
                info = json.loads(view.evaluate_javascript_finish(result).to_string())
            except (GLib.Error, TypeError, ValueError) as exc:
                finish(1, "SELFTEST FAIL: page script did not answer (%s)" % exc)
                return
            if info["errors"]:
                finish(1, "SELFTEST FAIL: page raised %s" % info["errors"])
            elif not info["loading"] and info["chips"] and info["bridge"]:
                finish(0, "SELFTEST OK games=%d preflight_chips=%d browse_button=%s "
                          "bridge=%s url=%s"
                       % (info["games"], info["chips"], info["browse"], info["bridge"],
                          state["url"].split("?")[0]))
            else:
                state["last"] = info
                GLib.timeout_add(250, poll_ready)

        state["view"].evaluate_javascript(READY_JS, -1, None, None, None, got)
        return False

    def on_load_changed(_view, event):
        if self_test and event == WebKit.LoadEvent.FINISHED:
            GLib.timeout_add(250, poll_ready)

    def on_load_failed(_view, _event, uri, error):
        if self_test:
            finish(1, "SELFTEST FAIL: could not load %s (%s)" % (uri, error.message))
        return False

    def on_timeout():
        finish(1, "SELFTEST FAIL: page not ready after %d s (last state: %s)"
               % (SELFTEST_TIMEOUT, state.get("last")))
        return False

    # -- application lifecycle --

    def on_startup(_app):
        # startup only runs in the primary instance, so a second launch never
        # binds a second server: it just asks this one to activate.
        state["server"], state["url"] = start_server(gui, port)
        parsed = urlparse(state["url"])
        state["origin"] = "%s://%s" % (parsed.scheme, parsed.netloc)
        print("dlss5-app  ->  %s" % state["url"], flush=True)

        add_action(app, "quit", request_quit)
        app.set_accels_for_action("app.quit", ["<Control>q"])
        app.set_accels_for_action("win.reload", ["<Control>r", "F5"])
        app.set_accels_for_action("win.fullscreen", ["F11"])
        Gtk.Window.set_default_icon_name(APP_ID)

    def build_window():
        manager = WebKit.UserContentManager()
        manager.connect("script-message-received::dlss5", on_message)
        manager.register_script_message_handler("dlss5", None)
        if self_test:
            manager.add_script(WebKit.UserScript.new(
                ERROR_TRAP_JS, WebKit.UserContentInjectedFrames.TOP_FRAME,
                WebKit.UserScriptInjectionTime.START, None, None))

        # Ephemeral: the token changes every run, so a disk cache could only
        # ever hold pages nobody can load again, and the page keeps no state
        # of its own in the browser.
        view = WebKit.WebView(user_content_manager=manager,
                              network_session=WebKit.NetworkSession.new_ephemeral())
        view.set_vexpand(True)
        view.set_hexpand(True)
        view.connect("decide-policy", on_decide_policy)
        view.connect("create", on_create)
        view.connect("web-process-terminated", on_web_process_terminated)
        view.connect("load-changed", on_load_changed)
        view.connect("load-failed", on_load_failed)
        state["view"] = view

        window = Adw.ApplicationWindow(application=app, title=APP_NAME,
                                       default_width=1500, default_height=950)
        state["window"] = window

        header = Adw.HeaderBar()
        state["title"] = Adw.WindowTitle(title=APP_NAME, subtitle="")
        header.set_title_widget(state["title"])
        reload_btn = Gtk.Button(icon_name="view-refresh-symbolic",
                                tooltip_text="Reload (Ctrl+R)", action_name="win.reload")
        header.pack_start(reload_btn)
        browser_btn = Gtk.Button(icon_name="web-browser-symbolic",
                                 tooltip_text="Open in your web browser")
        browser_btn.connect("clicked", lambda *_: open_external(state["url"]))
        header.pack_end(browser_btn)

        if hasattr(Adw, "ToolbarView"):            # libadwaita 1.4+
            body = Adw.ToolbarView()
            body.add_top_bar(header)
            body.set_content(view)
        else:
            body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            body.append(header)
            body.append(view)
        window.set_content(body)

        add_action(window, "reload", reload_page)
        add_action(window, "fullscreen", lambda: window.unfullscreen()
                   if window.is_fullscreen() else window.fullscreen())
        # Closing the window is quitting - the server only exists for it - but
        # not while a job is running; request_quit asks first.
        window.connect("close-request", on_close_request)

        view.load_uri(state["url"])
        return window

    def on_activate(_app):
        # Launching the app again while it waits for jobs to finish brings the
        # window back and cancels the pending exit.
        state["quit_when_idle"] = False
        state["signalled"] = False
        if state["window"] is None:
            build_window()
            if self_test:
                GLib.timeout_add_seconds(SELFTEST_TIMEOUT, on_timeout)
        state["window"].present()

    def on_shutdown(_app):
        state["done"] = True
        stop_server(state["server"])

    app.connect("startup", on_startup)
    app.connect("activate", on_activate)
    app.connect("shutdown", on_shutdown)

    # GLib 2.80 moved the Unix signal source to GLibUnix and deprecated the
    # old name; older hosts only have the old one.
    try:
        import gi
        gi.require_version("GLibUnix", "2.0")
        from gi.repository import GLibUnix
        signal_add = GLibUnix.signal_add
    except (ImportError, ValueError, AttributeError):
        signal_add = GLib.unix_signal_add
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal_add(GLib.PRIORITY_DEFAULT, signum, on_signal, signum)

    # Register up front to learn whether another copy already owns the app id.
    # If one does, run() below only asks it to raise its window: this process
    # starts no server, so say so instead of exiting silently.
    try:
        app.register(None)
    except GLib.Error as exc:
        print("dlss5-app: cannot register with the session bus (%s)" % exc.message,
              file=sys.stderr, flush=True)
    if app.get_is_registered() and app.get_is_remote():
        print("dlss5-app: already running - raised the existing window", flush=True)
        if port:
            print("dlss5-app: --port %d ignored; the running copy keeps its own server "
                  "(use --browser for a separate one)" % port, file=sys.stderr, flush=True)

    # Our flags are parsed above; GApplication only gets the program name so
    # it does not reject --port and friends as unknown options.
    try:
        rc = app.run([sys.argv[0]])
    except KeyboardInterrupt:
        # Backstop: a SIGINT that lands before the main loop's handler is live.
        print("\nstopped")
        rc = 0
    return state["rc"] if self_test else rc


def main():
    # Inside the AppImage the CLI has no command of its own; say how to reach it.
    epilog = None
    if os.environ.get("APPIMAGE"):
        epilog = ("This AppImage also carries the command-line tool: run it as "
                  "'%s cli --help', or symlink it as dlss5-linux "
                  "(ln -s %s ~/.local/bin/dlss5-linux)."
                  % (os.environ["APPIMAGE"], os.environ["APPIMAGE"]))
    parser = argparse.ArgumentParser(description="%s - desktop app for dlss5-linux.sh" % APP_NAME,
                                     epilog=epilog)
    parser.add_argument("--browser", action="store_true",
                        help="skip the native window and open the system browser")
    parser.add_argument("--port", type=int, default=0,
                        help="port on 127.0.0.1 (default 0: any free one)")
    parser.add_argument("--self-test", action="store_true",
                        help="load the page headlessly, check it works, exit 0/1")
    args = parser.parse_args()

    gui = load_gui()

    if args.browser:
        if args.self_test:
            sys.exit("--self-test needs the native window; drop --browser")
        return run_browser(gui, args.port)

    mods, reason = load_gi()
    if mods is None:
        if args.self_test:
            print("SELFTEST FAIL: %s" % reason, flush=True)
            return 1
        print("dlss5-app: %s - using the browser instead" % reason, file=sys.stderr, flush=True)
        return run_browser(gui, args.port)

    if args.self_test:
        # Backstop for a main loop that never gets to run its own timeout -
        # a wedged web process, a display that accepts but never maps.
        def hard_stop():
            print("SELFTEST FAIL: hard timeout after %d s" % (SELFTEST_TIMEOUT + 5), flush=True)
            os._exit(1)
        timer = threading.Timer(SELFTEST_TIMEOUT + 5, hard_stop)
        timer.daemon = True
        timer.start()

    return run_native(gui, mods, args.port, args.self_test)


if __name__ == "__main__":
    sys.exit(main())
