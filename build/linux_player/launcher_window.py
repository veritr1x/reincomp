"""Native GTK window for the packaged Bunker page. The parent owns the game and server."""
import json
import os
from pathlib import Path
import sys
import threading

# This helper runs with system Python -I to use the desktop's GI bindings.
TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from web_launch import BASE_ASSET_PORT, has_resources, saved_asset_port, save_asset_port

try:
    import gi
    gi.require_version("Gtk", "3.0")
    gi.require_version("WebKit2", "4.1")
    gi.require_version("JavaScriptCore", "4.1")
    from gi.repository import GLib, Gtk, JavaScriptCore, WebKit2
    if not hasattr(WebKit2.UserContentManager, "register_script_message_handler_with_reply"):
        raise ImportError("WebKitGTK 2.40 or newer is required")
except (ImportError, ValueError):
    if __name__ != "__main__":
        raise
    print("Bunker window unavailable. Install Python GObject, GTK 3 and WebKitGTK 4.1, or use --no-launcher.",
          file=sys.stderr)
    sys.exit(78)


class Launcher:
    def __init__(self, kind, path):
        self.kind = kind
        self.path = Path(path) if path else None
        self.state = Path.cwd() / ".reincomp"
        self.port_file = self.state / "server-ports.json"
        self.busy = False
        self.message = ""
        self.progress = None
        self.exit_code = 0
        self.window = Gtk.Window(title="Bunker")
        self.window.set_default_size(920, 520)
        self.window.connect("destroy", lambda *_: Gtk.main_quit())
        manager = WebKit2.UserContentManager()
        manager.connect("script-message-with-reply-received::bunker", self.request)
        manager.register_script_message_handler_with_reply("bunker", None)
        self.view = WebKit2.WebView(user_content_manager=manager, is_ephemeral=True)
        self.view.connect("decide-policy", self.navigation)
        self.view.connect("web-process-terminated", self.web_process_ended)
        self.window.add(self.view)
        self.view.load_html((TOOLS / "launcher.html").read_text(encoding="utf-8"), None)

    def navigation(self, view, decision, kind):
        if kind == WebKit2.PolicyDecisionType.NEW_WINDOW_ACTION or (
                kind == WebKit2.PolicyDecisionType.NAVIGATION_ACTION and
                decision.get_navigation_action().get_request().get_uri() != "about:blank"):
            decision.ignore()
            return True
        return False

    def web_process_ended(self, view, reason):
        print(f"Bunker's WebKit process stopped ({reason.value_nick}).", file=sys.stderr)
        self.exit_code = 1
        self.window.destroy()

    def ready(self):
        if not self.path:
            return False
        if self.kind == "dump":
            return self.path.is_file() and self.path.suffix.lower() in (".zip", ".7z")
        return self.kind in ("local", "assets") and has_resources(self.path)

    def choose(self, folder):
        action = Gtk.FileChooserAction.SELECT_FOLDER if folder else Gtk.FileChooserAction.OPEN
        title = "Choose the imported resource folder" if folder else "Choose the Android resource archive"
        dialog = Gtk.FileChooserNative.new(title, self.window, action, "Choose", "Cancel")
        if not folder:
            archives = Gtk.FileFilter()
            archives.set_name("Android resource archives (.zip, .7z)")
            for pattern in ("*.[zZ][iI][pP]", "*.7[zZ]"):
                archives.add_pattern(pattern)
            dialog.add_filter(archives)
        try:
            if dialog.run() != Gtk.ResponseType.ACCEPT:
                return
            path = Path(dialog.get_filename())
            if folder and not has_resources(path):
                raise ValueError("Choose the imported folder containing assets/revisions.")
            if not folder and (not path.is_file() or path.suffix.lower() not in (".7z", ".zip")):
                raise ValueError("Choose a .7z or .zip resource archive.")
            self.kind, self.path = ("assets" if folder else "dump"), path
            self.message = ""
        finally:
            dialog.destroy()

    def dispatch(self, action, value):
        if action == "state":
            save = self.state / "save/game.db"
            return dict(ready=self.ready(), busy=self.busy, archive=self.kind == "dump", source=str(self.path or ""),
                        message=self.message, progress=self.progress, saveBytes=save.stat().st_size if save.is_file() else 0,
                        override=os.environ.get("REINCOMP_BUNKER_PORT_OFFSET"))
        if self.busy:
            raise ValueError("The game is starting.")
        if action == "preferences":
            if value is not None:
                return {"assetPort": save_asset_port(self.port_file, value)}
            try:
                port = saved_asset_port(self.port_file)
            except ValueError:
                port = BASE_ASSET_PORT  # Allow an invalid settings file to be repaired in the dialog.
            return {"assetPort": port}
        if action in ("in_place", "archive"):
            self.choose(action == "in_place")
        elif action == "deploy" and self.ready():
            self.busy = True
            self.message = "Importing the resource archive…" if self.kind == "dump" else "Preparing game files…"
            print(json.dumps(dict(kind=self.kind, path=str(self.path))), flush=True)
        else:
            raise ValueError("Choose a valid resource archive or imported folder first.")

    def request(self, manager, message, reply):
        try:
            request = json.loads(message.to_string())
            result = self.dispatch(request["action"], request.get("value"))
            reply.return_value(JavaScriptCore.Value.new_string(message.get_context(), json.dumps(result)))
        except (OSError, ValueError, KeyError, TypeError) as error:
            reply.return_error_message(str(error))
        return True

    def parent_event(self, event):
        self.progress = event.get("progress")
        if "error" in event:
            self.busy = False
            self.message = event["error"]
            self.window.present()
        elif event.get("stage") == "playing":
            self.window.hide()
            return False
        elif event.get("stage") == "preparing":
            if self.kind == "dump":
                self.kind, self.path = "local", self.state / "resources"
            self.message = "Preparing game files…"
        elif event.get("stage") == "starting":
            self.message = "Starting Lunar Tear…"
        self.view.evaluate_javascript("update()", -1, None, None, None, None)
        return False

    def listen(self):
        for line in sys.stdin:
            GLib.idle_add(self.parent_event, json.loads(line))
        GLib.idle_add(self.window.destroy)

    def run(self):
        threading.Thread(target=self.listen, daemon=True).start()
        self.window.show_all()
        Gtk.main()
        return self.exit_code


if __name__ == "__main__":
    if not Gtk.init_check()[0]:
        sys.exit(78)
    sys.exit(Launcher(*sys.argv[1:]).run())
