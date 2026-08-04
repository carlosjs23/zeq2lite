#!/usr/bin/env python3
"""MCP stdio server that drives a running ZEQ2-Lite engine.

The engine speaks a small, permanent line-oriented JSON protocol on a
loopback socket (Engine/server/sv_debugsocket.c, gated on net_debugPort).
Everything version-shaped - the MCP handshake, the tool schemas, the content
block encoding - lives here, so a spec revision is a change to one Python
file and never a change to C.

Python and no SDK on purpose: this machine has neither @modelcontextprotocol/sdk
nor the `mcp` package installed, every other script in Tools/dev is
stdlib-only Python by policy, MCP stdio is plain JSON-RPC 2.0 over two pipes,
and the TGA->PNG conversion the screenshot tool needs already exists next
door in Tools/dev/tga2png.py. Adding a node_modules tree to a game repo to
avoid ~120 lines of framing would be the worse trade.

Run it by hand for a smoke test:

    python3 Tools/mcp/zeq2_mcp.py --selftest
"""

import base64
import errno
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "dev"))
import tga2png  # noqa: E402  - Tools/dev, stdlib only

SERVER_NAME = "zeq2"
SERVER_VERSION = "1.0.0"
PROTOCOL_VERSION = "2024-11-05"

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", ".."))


# ---------------------------------------------------------------- paths

def env_paths():
    arch = os.environ.get("ZEQ2_ARCH") or os.uname().machine
    if arch == "arm64":
        # The build tree is named for `uname -p`, which is "arm" on this mac
        # even though the engine's own ARCH_STRING is arm64. See CLAUDE.md.
        arch = "arm"
    root = os.environ.get("ZEQ2_ROOT", ROOT)
    plat = os.uname().sysname.lower()
    build = os.environ.get("ZEQ2_BUILD",
                           os.path.join(root, "Build", "Release-%s-%s" % (plat, arch)))
    game = os.environ.get("ZEQ2_GAME", "ZEQ2")
    return {
        "root": root,
        "arch": arch,
        "build": build,
        "game": game,
        "gamedir": os.path.join(build, game),
        "bin": os.path.join(build, "ZEQ2.%s" % arch),
        "config": os.path.join(build, game, "zeq2config.cfg"),
        "gameslog": os.path.join(build, game, "games.log"),
        "shots": os.path.join(build, game, "screenshots"),
    }


def temp_dir():
    """Where Sys_TempPath writes zeq2lite.pid.

    On macOS that is FSFindFolder(kTemporaryFolderType), i.e.
    $TMPDIR/TemporaryItems and not $TMPDIR - removing the obvious path fixes
    nothing. Derive it rather than hardcoding /var/folders/...
    """
    if os.uname().sysname == "Darwin":
        try:
            out = subprocess.run(["getconf", "DARWIN_USER_TEMP_DIR"],
                                 capture_output=True, text=True, timeout=5)
            base = out.stdout.strip() or os.environ.get("TMPDIR", "/tmp")
        except Exception:
            base = os.environ.get("TMPDIR", "/tmp")
        return os.path.join(base.rstrip("/"), "TemporaryItems")
    return os.environ.get("TMPDIR", "/tmp").rstrip("/")


def clear_stale_pid():
    """A killed run leaves a pid file and the next launch opens a modal.

    Com_Init writes it and only Sys_Exit removes it, so a run that was killed
    at a deadline leaves a pid that is not running. The next launch decides
    the last session crashed and opens the "Abnormal Exit" NSAlert, which
    nobody is here to answer: the launch hangs forever. A pid that IS running
    is left alone - a concurrent session writes the same file.
    """
    cleared = []
    d = temp_dir()
    for name in ("zeq2lite.pid", "zeq2lite_server.pid"):
        path = os.path.join(d, name)
        try:
            with open(path) as fh:
                pid = int(fh.read().strip())
        except Exception:
            continue
        try:
            os.kill(pid, 0)
            continue          # still alive, not ours to remove
        except OSError as exc:
            if exc.errno == errno.EPERM:
                continue
        try:
            os.remove(path)
            cleared.append("%s (pid %d)" % (path, pid))
        except OSError:
            pass
    return cleared


# ------------------------------------------------------- the debug socket

class DebugSocketError(RuntimeError):
    pass


class DebugSocket:
    """One JSON object per line in, one per line out."""

    def __init__(self, port, host="127.0.0.1", timeout=20.0):
        self.port = port
        self.host = host
        self.timeout = timeout
        self.sock = None
        self.buf = b""
        self.seq = 0

    def connect(self, deadline):
        last = None
        while time.time() < deadline:
            try:
                s = socket.create_connection((self.host, self.port), 1.0)
                s.settimeout(self.timeout)
                self.sock = s
                return
            except OSError as exc:
                last = exc
                time.sleep(0.2)
        raise DebugSocketError("could not reach the engine debug socket on %s:%d (%s)"
                               % (self.host, self.port, last))

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def call(self, op, **kwargs):
        if not self.sock:
            raise DebugSocketError("not connected to the engine")
        self.seq += 1
        req = {"v": 1, "id": str(self.seq), "op": op}
        req.update(kwargs)
        line = json.dumps(req, separators=(",", ":")) + "\n"
        try:
            self.sock.sendall(line.encode("utf-8"))
        except OSError as exc:
            raise DebugSocketError("engine went away while sending %r: %s" % (op, exc))
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise DebugSocketError("engine did not answer %r within %.0fs"
                                       % (op, self.timeout))
            except OSError as exc:
                raise DebugSocketError("engine went away while reading %r: %s" % (op, exc))
            if not chunk:
                raise DebugSocketError("engine closed the debug socket during %r" % op)
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        try:
            reply = json.loads(line.decode("utf-8", "replace"))
        except ValueError as exc:
            raise DebugSocketError("engine sent a line that is not JSON: %s" % exc)
        if not reply.get("ok"):
            raise DebugSocketError("engine refused %r: %s" % (op, reply.get("error")))
        return reply


# ------------------------------------------------------------ the engine

class Engine:
    """Process lifecycle plus the tribal knowledge a launch needs."""

    def __init__(self):
        self.proc = None
        self.sock = None
        self.paths = env_paths()
        self.port = None
        self.log = None
        self.config_backup = None
        self.launch_notes = []

    # -- config ---------------------------------------------------------

    def backup_config(self):
        cfg = self.paths["config"]
        if os.path.exists(cfg):
            fd, tmp = tempfile.mkstemp(prefix="zeq2config-", suffix=".bak")
            os.close(fd)
            shutil.copyfile(cfg, tmp)
            self.config_backup = tmp

    def restore_config(self):
        if self.config_backup:
            try:
                shutil.copyfile(self.config_backup, self.paths["config"])
            finally:
                try:
                    os.remove(self.config_backup)
                except OSError:
                    pass
                self.config_backup = None

    # -- lifecycle ------------------------------------------------------

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def log_tail(self, lines=40):
        if not self.log or not os.path.exists(self.log):
            return ""
        with open(self.log, "r", errors="replace") as fh:
            return "".join(fh.readlines()[-lines:])

    def require(self):
        """Raise with the log tail if the engine is not answering."""
        if not self.alive():
            code = self.proc.returncode if self.proc else None
            raise DebugSocketError(
                "the engine is not running (%s).\n"
                "Recent log:\n%s" % (describe_exit(code), self.log_tail()))
        if not self.sock or not self.sock.sock:
            raise DebugSocketError("no debug socket; call zeq2_launch first")

    def launch(self, map_name="desert", gametype=None, training=False,
               cheats=True, dedicated=False, port=27960, extra=None,
               timeout=90.0, fullscreen=0, mode=3, hunkmegs=256):
        if self.alive():
            raise DebugSocketError("an engine is already running; call zeq2_shutdown first")

        p = self.paths
        if not os.path.isfile(p["bin"]):
            raise DebugSocketError("engine binary not found at %s - build it with "
                                   "Tools/dev/zeq2build.sh" % p["bin"])
        self.launch_notes = []
        cleared = clear_stale_pid()
        if cleared:
            self.launch_notes.append("cleared stale pid file(s): " + ", ".join(cleared))

        self.backup_config()
        self.port = int(port)
        self.log = os.path.join(p["build"], "zeq2_mcp.log")

        # `map` clears sv_cheats, `devmap` sets it. Anything cheat-gated -
        # testinput, testkey, setviewpos, dummy, ai - needs devmap.
        loader = "devmap" if cheats else "map"
        args = [p["bin"],
                "+set", "fs_game", p["game"],
                "+set", "r_fullscreen", str(fullscreen),
                "+set", "r_mode", str(mode),
                "+set", "com_hunkMegs", str(hunkmegs),
                # The shipped default.cfg sets g_log to "" and the cvar is
                # archived, so a run that does not state it logs or does not
                # log depending on which run came before it.
                "+set", "g_log", "games.log",
                "+set", "net_debugPort", str(self.port)]
        if training:
            args += ["+set", "g_training", "1"]
        if gametype is not None:
            args += ["+set", "g_gametype", str(int(gametype))]
        if dedicated:
            args += ["+set", "dedicated", "1"]
        if extra:
            args += list(extra)
        args += ["+%s" % loader, map_name]

        env = dict(os.environ)
        # Sys_SigHandler swallows fault signals and a crash surfaces only as
        # "=== fatal signal, exiting ===" with no address. Leave the fault
        # signals to the OS so a crash under the adapter is diagnosable; the
        # termination signals stay handled, so shutdown is still orderly.
        env.setdefault("ZEQ2_NO_SIGHANDLER", "1")

        logfh = open(self.log, "w")
        # stdin MUST be detached. The engine's tty console reads fd 0 whether
        # or not it is a terminal, so an inherited stdin means the engine
        # swallows this server's JSON-RPC requests off the pipe and the whole
        # session dies with no error anywhere.
        self.proc = subprocess.Popen(args, cwd=p["build"], stdin=subprocess.DEVNULL,
                                     stdout=logfh, stderr=subprocess.STDOUT, env=env,
                                     start_new_session=True)
        logfh.close()

        deadline = time.time() + timeout
        self.sock = DebugSocket(self.port)
        try:
            self.sock.connect(deadline)
        except DebugSocketError:
            self.kill()
            raise DebugSocketError(
                "the engine never opened its debug socket on port %d.\n"
                "Recent log:\n%s" % (self.port, self.log_tail()))

        state = self.wait_for_map(map_name, deadline)
        return {
            "pid": self.proc.pid,
            "port": self.port,
            "log": self.log,
            "map": state["server"].get("map", ""),
            "gametype": state["server"].get("gametype"),
            "training": state["server"].get("training"),
            "cheats": state["server"].get("cheats"),
            "argv": args,
            "notes": self.launch_notes,
        }

    def wait_for_map(self, map_name, deadline):
        """A connected socket only means Com_Init finished."""
        last = None
        while time.time() < deadline:
            if not self.alive():
                raise DebugSocketError(
                    "the engine exited during map load (%s).\nRecent log:\n%s"
                    % (describe_exit(self.proc.returncode), self.log_tail()))
            last = self.sock.call("state")
            srv = last.get("server", {})
            if srv.get("running") and srv.get("state") == 2 and \
                    last.get("player", {}).get("valid"):
                return last
            time.sleep(0.25)
        raise DebugSocketError(
            "map %r never reached a playable state.\nLast snapshot: %s\nRecent log:\n%s"
            % (map_name, json.dumps(last), self.log_tail()))

    def kill(self):
        if self.sock:
            self.sock.close()
            self.sock = None
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=10)
            except Exception:
                try:
                    os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
                except Exception:
                    pass
        code = self.proc.returncode if self.proc else None
        self.proc = None
        self.restore_config()
        clear_stale_pid()
        return code

    def shutdown(self):
        code = None
        if self.alive():
            try:
                # `quit` reaches Com_Quit_f, which shuts the subsystems down
                # in order and removes the pid file. Only fall back to a
                # signal if that does not take.
                self.sock.call("eval", cmd="quit", buffer=True)
            except DebugSocketError:
                pass
            try:
                code = self.proc.wait(timeout=15)
            except Exception:
                code = None
        return self.kill() if code is None else self._finish(code)

    def _finish(self, code):
        if self.sock:
            self.sock.close()
            self.sock = None
        self.proc = None
        self.restore_config()
        clear_stale_pid()
        return code


def describe_exit(code):
    if code is None:
        return "still running"
    if code == 0:
        return "exit 0 (clean)"
    if code < 0:
        return "killed by signal %d" % -code
    if code == 134:
        return "exit 134 (SIGABRT - likely __stack_chk_fail / abort)"
    if code == 139:
        return "exit 139 (SIGSEGV)"
    if code > 128:
        return "exit %d (killed by signal %d)" % (code, code - 128)
    return "exit %d" % code


ENGINE = Engine()


# -------------------------------------------------------------- the tools

def tool_launch(args):
    info = ENGINE.launch(
        map_name=args.get("map", "desert"),
        gametype=args.get("gametype"),
        training=bool(args.get("training", False)),
        cheats=bool(args.get("cheats", True)),
        dedicated=bool(args.get("dedicated", False)),
        port=args.get("port", 27960),
        extra=args.get("extra"),
        timeout=float(args.get("timeout", 90)),
    )
    return text_result(json.dumps(info, indent=2))


def tool_console(args):
    ENGINE.require()
    cmd = args.get("cmd", "")
    if not cmd:
        raise DebugSocketError("zeq2_console needs a cmd")
    buffered = bool(args.get("buffer", False))
    # Commands the client forwards to the server - masterlist, where - are
    # answered a frame or more later, so a capture that closes immediately
    # returns an empty string that reads like "no output". Six frames is
    # under a tenth of a second and covers the round trip.
    frames = 0 if buffered else int(args.get("frames", 6))
    reply = ENGINE.sock.call("eval", cmd=cmd, buffer=buffered, frames=frames)
    return text_result(json.dumps({
        "cmd": reply.get("cmd", cmd),
        "buffered": reply.get("buffered", False),
        "truncated": reply.get("truncated", False),
        "output": reply.get("output", ""),
    }, indent=2))


def tool_state(args):
    ENGINE.require()
    reply = ENGINE.sock.call("state")
    reply.pop("v", None)
    reply.pop("id", None)
    return text_result(json.dumps(reply, indent=2))


def tool_entities(args):
    ENGINE.require()
    reply = ENGINE.sock.call("entities", max=int(args.get("max", 128)))
    return text_result(json.dumps({"count": reply.get("count"),
                                   "entities": reply.get("entities", [])}, indent=2))


def tool_cvar(args):
    ENGINE.require()
    name = args.get("name", "")
    if not name:
        raise DebugSocketError("zeq2_cvar needs a name")
    kw = {"mode": args.get("mode", "get"), "name": name}
    if kw["mode"] == "set":
        kw["value"] = str(args.get("value", ""))
    reply = ENGINE.sock.call("cvar", **kw)
    return text_result(json.dumps({"name": reply.get("name"),
                                   "value": reply.get("value"),
                                   "integer": reply.get("integer")}, indent=2))


def tool_input(args):
    """testinput: the usercmd path, so pmove sees a real held button."""
    ENGINE.require()
    spec = args.get("spec", "")
    ms = int(args.get("ms", 1000))
    if spec == "clear":
        cmd = "testinput clear"
    else:
        if not spec:
            raise DebugSocketError("zeq2_input needs a spec, or 'clear'")
        # The separator is a comma, not a plus: Com_ParseCommandLine breaks
        # the command line on every '+'.
        cmd = "testinput %s %d" % (spec.replace("+", ","), ms)
    reply = ENGINE.sock.call("eval", cmd=cmd)
    return text_result(json.dumps({"cmd": cmd, "output": reply.get("output", "")},
                                  indent=2))


def tool_key(args):
    """testkey: the key catcher, which is where menus and the console read."""
    ENGINE.require()
    mode = args.get("mode", "text")
    if mode == "text":
        cmd = "testkey text %s" % args.get("text", "")
    elif mode == "char":
        cmd = "testkey char %d" % int(args.get("code", 0))
    elif mode == "key":
        cmd = "testkey key %d %d" % (int(args.get("keynum", 0)),
                                     1 if args.get("down", True) else 0)
    else:
        raise DebugSocketError("zeq2_key mode must be text, char or key")
    reply = ENGINE.sock.call("eval", cmd=cmd)
    return text_result(json.dumps({"cmd": cmd, "output": reply.get("output", "")},
                                  indent=2))


def frame_stats(width, height, rows):
    """Distinct colours and greyscale share - the mechanical way to tell a
    drawn world (~40k colours, <10% grey) from a flat frame (~2.5k, >90%).

    Not tga2png.stats(): that one prints, and anything on stdout here
    corrupts the JSON-RPC stream.
    """
    seen = {}
    grey = 0
    for row in rows:
        for x in range(width):
            px = row[x * 3:x * 3 + 3]
            seen[px] = 1
            if px[0] == px[1] == px[2]:
                grey += 1
    return len(seen), 100.0 * grey / float(width * height)


def tool_screenshot(args):
    """The engine's own `screenshot`, returned as an MCP image block.

    macOS screencapture needs Screen Recording permission a headless shell
    does not have; the engine reads its own backbuffer from inside the render
    command queue and needs nothing.
    """
    ENGINE.require()
    shots = ENGINE.paths["shots"]
    before = set(os.listdir(shots)) if os.path.isdir(shots) else set()
    ENGINE.sock.call("eval", cmd="screenshot")

    deadline = time.time() + float(args.get("timeout", 15))
    path = None
    while time.time() < deadline:
        if os.path.isdir(shots):
            new = [f for f in os.listdir(shots)
                   if f.endswith(".tga") and f not in before]
            if new:
                cand = os.path.join(shots, sorted(new)[-1])
                # Wait for the write to settle before reading the header.
                size = -1
                while size != os.path.getsize(cand):
                    size = os.path.getsize(cand)
                    time.sleep(0.05)
                path = cand
                break
        time.sleep(0.1)
    if not path:
        raise DebugSocketError("no screenshot appeared in %s.\nRecent log:\n%s"
                               % (shots, ENGINE.log_tail(15)))

    width, height, rows = tga2png.read_tga(path)
    png = os.path.splitext(path)[0] + ".png"
    tga2png.write_png(png, width, height, rows)
    with open(png, "rb") as fh:
        data = fh.read()
    colours, grey = frame_stats(width, height, rows)
    return {
        "content": [
            {"type": "text",
             "text": json.dumps({"path": png, "width": width, "height": height,
                                 "distinctColours": colours,
                                 "greyscalePercent": round(grey, 1)}, indent=2)},
            {"type": "image", "mimeType": "image/png",
             "data": base64.b64encode(data).decode("ascii")},
        ]
    }


def tool_logs(args):
    """The engine log and games.log, correlated and filtered."""
    filt = args.get("filter")
    since = float(args.get("since", 0))
    limit = int(args.get("limit", 120))
    rx = re.compile(filt) if filt else None

    out = []
    for label, path in (("engine", ENGINE.log),
                        ("games", ENGINE.paths["gameslog"])):
        if not path or not os.path.exists(path):
            continue
        if since and os.path.getmtime(path) < since:
            continue
        with open(path, "r", errors="replace") as fh:
            lines = fh.read().splitlines()
        if rx:
            lines = [l for l in lines if rx.search(l)]
        for line in lines[-limit:]:
            out.append("[%s] %s" % (label, line))

    status = describe_exit(ENGINE.proc.returncode if ENGINE.proc else None)
    header = "engine: %s" % status
    if ENGINE.proc and ENGINE.proc.poll() is not None:
        # A log that just stops is the signature of a caught fatal signal.
        header += ("\nNOTE: the engine is gone. A log that ends mid-line means "
                   "Sys_SigHandler caught the fault; relaunch with "
                   "ZEQ2_NO_SIGHANDLER=1 for a real crash report.")
    return text_result(header + "\n" + "\n".join(out))


def tool_shutdown(args):
    code = ENGINE.shutdown()
    return text_result(json.dumps({"exit": code, "status": describe_exit(code),
                                   "configRestored": True}, indent=2))


TOOLS = [
    {
        "name": "zeq2_launch",
        "description": ("Start the ZEQ2-Lite engine on a map with the debug socket open. "
                        "Clears a stale pid file, backs up zeq2config.cfg, waits for the "
                        "map to become playable, and returns a handle."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "map": {"type": "string", "default": "desert"},
                "gametype": {"type": "integer", "description": "g_gametype; 1 is the Budokai tournament"},
                "training": {"type": "boolean", "default": False, "description": "g_training 1"},
                "cheats": {"type": "boolean", "default": True, "description": "load with devmap so cheat-gated commands work"},
                "dedicated": {"type": "boolean", "default": False},
                "port": {"type": "integer", "default": 27960, "description": "net_debugPort"},
                "extra": {"type": "array", "items": {"type": "string"}, "description": "extra engine argv"},
                "timeout": {"type": "number", "default": 90},
            },
        },
        "handler": tool_launch,
    },
    {
        "name": "zeq2_console",
        "description": ("Run a console command and return what the player would see. "
                        "Set buffer=true for commands that cannot run inside a frame "
                        "(map, map_restart, quit); those return no output."),
        "inputSchema": {
            "type": "object",
            "properties": {"cmd": {"type": "string"},
                           "buffer": {"type": "boolean", "default": False},
                           "frames": {"type": "integer", "default": 6,
                                      "description": "frames to keep capturing; "
                                                     "commands the client forwards "
                                                     "to the server answer late"}},
            "required": ["cmd"],
        },
        "handler": tool_console,
    },
    {
        "name": "zeq2_state",
        "description": ("Structured snapshot: server/map/gametype/training, the local "
                        "player's origin, angles, velocity, health, power level, "
                        "fatigue, tier, weapon and selectable skills, and the active "
                        "training objective, progress and master."),
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_state,
    },
    {
        "name": "zeq2_entities",
        "description": "Compact list of live entities: number, type, origin, model, name.",
        "inputSchema": {"type": "object",
                        "properties": {"max": {"type": "integer", "default": 128}}},
        "handler": tool_entities,
    },
    {
        "name": "zeq2_cvar",
        "description": "Get or set a cvar.",
        "inputSchema": {
            "type": "object",
            "properties": {"name": {"type": "string"},
                           "mode": {"type": "string", "enum": ["get", "set"], "default": "get"},
                           "value": {"type": "string"}},
            "required": ["name"],
        },
        "handler": tool_cvar,
    },
    {
        "name": "zeq2_input",
        "description": ("Hold usercmd buttons for a duration (testinput). Names: jump "
                        "attack altattack lock block boost powerup up down forward back "
                        "left right, comma separated. `jump` alone does not lift you - "
                        "sustained flight is `jump,up`. Pass spec='clear' to stop."),
        "inputSchema": {
            "type": "object",
            "properties": {"spec": {"type": "string"},
                           "ms": {"type": "integer", "default": 1000}},
            "required": ["spec"],
        },
        "handler": tool_input,
    },
    {
        "name": "zeq2_key",
        "description": ("Inject a key event into the key catcher (testkey), which is "
                        "where the console, chat line and menus read input."),
        "inputSchema": {
            "type": "object",
            "properties": {"mode": {"type": "string", "enum": ["text", "char", "key"],
                                    "default": "text"},
                           "text": {"type": "string"},
                           "code": {"type": "integer"},
                           "keynum": {"type": "integer"},
                           "down": {"type": "boolean", "default": True}},
        },
        "handler": tool_key,
    },
    {
        "name": "zeq2_screenshot",
        "description": "Capture a frame from the running engine and return it as an image.",
        "inputSchema": {"type": "object",
                        "properties": {"timeout": {"type": "number", "default": 15}}},
        "handler": tool_screenshot,
    },
    {
        "name": "zeq2_logs",
        "description": "The engine log and games.log, correlated, with a regex filter.",
        "inputSchema": {
            "type": "object",
            "properties": {"filter": {"type": "string", "description": "python regex"},
                           "since": {"type": "number", "description": "unix mtime floor"},
                           "limit": {"type": "integer", "default": 120}},
        },
        "handler": tool_logs,
    },
    {
        "name": "zeq2_shutdown",
        "description": "Quit the engine cleanly and restore zeq2config.cfg.",
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_shutdown,
    },
]

TOOLS_BY_NAME = {t["name"]: t for t in TOOLS}


def text_result(text):
    return {"content": [{"type": "text", "text": text}]}


# ----------------------------------------------------------- JSON-RPC 2.0

def handle(msg):
    method = msg.get("method")
    params = msg.get("params") or {}

    if method == "initialize":
        return {"protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION}}
    if method == "tools/list":
        return {"tools": [{k: v for k, v in t.items() if k != "handler"} for t in TOOLS]}
    if method == "tools/call":
        name = params.get("name")
        tool = TOOLS_BY_NAME.get(name)
        if not tool:
            raise LookupError("unknown tool %r" % name)
        try:
            return tool["handler"](params.get("arguments") or {})
        except DebugSocketError as exc:
            # A dead or wedged engine must read as a failed tool call with the
            # log tail attached, never as a hang.
            return {"isError": True,
                    "content": [{"type": "text", "text": str(exc)}]}
    if method in ("notifications/initialized", "ping"):
        return {}
    raise LookupError("unknown method %r" % method)


def serve():
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if "id" not in msg:
            continue                      # a notification; nothing to answer
        try:
            reply = {"jsonrpc": "2.0", "id": msg["id"], "result": handle(msg)}
        except LookupError as exc:
            reply = {"jsonrpc": "2.0", "id": msg["id"],
                     "error": {"code": -32601, "message": str(exc)}}
        except Exception as exc:          # noqa: BLE001 - never take the server down
            reply = {"jsonrpc": "2.0", "id": msg["id"],
                     "error": {"code": -32603, "message": "%s: %s"
                               % (type(exc).__name__, exc)}}
        out.write(json.dumps(reply) + "\n")
        out.flush()


def selftest():
    print(json.dumps(handle({"method": "initialize"}), indent=2))
    print(json.dumps([t["name"] for t in TOOLS], indent=2))
    print("paths:", json.dumps(env_paths(), indent=2))


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
    else:
        try:
            serve()
        finally:
            if ENGINE.alive():
                ENGINE.kill()
            ENGINE.restore_config()
