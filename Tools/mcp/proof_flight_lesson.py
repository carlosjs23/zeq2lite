#!/usr/bin/env python3
"""End-to-end acceptance run, driven THROUGH the MCP server.

Nothing here touches the engine or the debug socket directly: it spawns
Tools/mcp/zeq2_mcp.py as an MCP stdio server and speaks JSON-RPC 2.0 to it
over two pipes, exactly as a client would. Every request and response is
printed verbatim, so the transcript is the evidence.

The loop: launch desert with training on -> teleport to Rhogan -> complete
the flight lesson with held input -> screenshot -> assert the objective
completed and trained.rhogan.flight was granted -> shut down cleanly.

Two things about driving this lesson that are not obvious:

- `up` alone holds x and y exactly and climbs about 455 units a second,
  while `jump` held in the air soars along the view yaw. Rhogan's radius is
  a 768-unit sphere, so the run lifts off with `jump,up`, switches to `up`,
  and snaps the altitude back with setviewpos about once a second. The
  player never lands, so airborneTime keeps accumulating.
- the training profile is per player and persists, and every rule here
  forbids the tag it grants - so a second run would prove nothing. The
  profiles are moved aside for the duration and put back afterwards.

    python3 Tools/mcp/proof_flight_lesson.py
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "zeq2_mcp.py")
sys.path.insert(0, HERE)
import zeq2_mcp  # noqa: E402  - for the install paths only

RHOGAN = "-40810 4681 1560 90"
HOVER_SECONDS = 55


class Client:
    def __init__(self):
        self.proc = subprocess.Popen([sys.executable, "-u", SERVER],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        self.id = 0

    def call(self, method, params=None, quiet=False):
        self.id += 1
        req = {"jsonrpc": "2.0", "id": self.id, "method": method}
        if params is not None:
            req["params"] = params
        print("\n--> " + json.dumps(req))
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise SystemExit("the MCP server closed stdout")
        reply = json.loads(line)
        if quiet:
            shown = dict(reply)
            shown["result"] = "<%d bytes elided>" % len(json.dumps(reply.get("result")))
            print("<-- " + json.dumps(shown))
        else:
            print("<-- " + json.dumps(reply))
        return reply

    def tool(self, name, args=None, quiet=False):
        return self.call("tools/call", {"name": name, "arguments": args or {}}, quiet)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=15)
        except Exception:
            self.proc.kill()


def payload(reply):
    text = reply["result"]["content"][0]["text"]
    try:
        return json.loads(text)
    except ValueError:
        return text


def ruledump_field(dump, prefix):
    for line in dump.splitlines():
        if line.startswith(prefix):
            return line
    return ""


def main():
    profiles = os.path.join(zeq2_mcp.env_paths()["gamedir"], "training")
    stash = None
    if os.path.isdir(profiles):
        stash = tempfile.mkdtemp(prefix="zeq2-training-")
        for name in os.listdir(profiles):
            shutil.move(os.path.join(profiles, name), os.path.join(stash, name))
        print("# moved %d training profile(s) aside to %s" %
              (len(os.listdir(stash)), stash))

    c = Client()
    failures = []
    try:
        c.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                              "clientInfo": {"name": "proof", "version": "1"}})
        c.call("tools/list", quiet=True)

        # 1. launch. devmap, so the cheat-gated commands below are answered.
        c.tool("zeq2_launch", {"map": "desert", "training": True, "cheats": True,
                               "port": 27960})

        # 2. teleport to Rhogan. Cmd_SetViewpos_f clears the velocity and the
        #    knockback hold, so a scripted run lands on what it asked for.
        c.tool("zeq2_console", {"cmd": "setviewpos " + RHOGAN})
        time.sleep(2)
        c.tool("zeq2_console", {"cmd": "masterlist"})
        before = payload(c.tool("zeq2_state", quiet=True))
        print("    on arrival: objective=%s progress=%s master=%s" %
              (before["player"]["training"]["objective"],
               before["player"]["training"]["progress"],
               before["player"]["training"]["master"]))

        # 3. the lesson: 45 seconds airborne inside Rhogan's radius. Held
        #    through the usercmd, so pmove sees a real press.
        c.tool("zeq2_input", {"spec": "jump,up", "ms": 1200})
        time.sleep(1.5)
        c.tool("zeq2_input", {"spec": "up", "ms": (HOVER_SECONDS + 5) * 1000})
        start = time.time()
        tick = 0
        while time.time() - start < HOVER_SECONDS:
            time.sleep(1.0)
            c.tool("zeq2_console", {"cmd": "setviewpos " + RHOGAN}, quiet=True)
            tick += 1
            if tick % 10 == 0:
                st = payload(c.tool("zeq2_state", quiet=True))["player"]["training"]
                print("    %2ds  objective=%s progress=%s master=%s" %
                      (round(time.time() - start), st["objective"],
                       st["progress"], st["master"]))
                if st["progress"] >= 100:
                    break
        time.sleep(2)
        c.tool("zeq2_input", {"spec": "clear"})

        # 4. a frame, returned as an MCP image content block.
        shot = payload(c.tool("zeq2_screenshot", quiet=True))
        print("    screenshot: " + json.dumps(shot))

        # 5. the assertions.
        state = payload(c.tool("zeq2_state"))
        dump = payload(c.tool("zeq2_console", {"cmd": "ruledump"}))["output"]
        print("\n--- ruledump, client block ---")
        for prefix in ("client ", "  progress:", "  pers:", "  facts:", "  tags:"):
            print(ruledump_field(dump, prefix))

        tags = ruledump_field(dump, "  tags:")
        if "trained.rhogan.flight" not in tags:
            failures.append("tag trained.rhogan.flight was not granted")
        if "budokai.entry" not in tags:
            failures.append("tag budokai.entry was not granted")
        if state["player"]["training"]["progress"] < 100:
            failures.append("objective progress is %s, not 100"
                            % state["player"]["training"]["progress"])

        logs = payload(c.tool("zeq2_logs",
                              {"filter": "objective-complete|rhogan.flight|tier",
                               "limit": 12}))
        print("\n--- correlated logs ---\n" + logs)
        if "objective-complete" not in logs:
            failures.append("games.log records no objective-complete")

        # 6. clean stop, config restored.
        result = payload(c.tool("zeq2_shutdown"))
        if result.get("exit") != 0:
            failures.append("engine did not exit cleanly: %s" % result.get("status"))
    finally:
        c.close()
        if stash:
            for name in os.listdir(profiles):
                os.remove(os.path.join(profiles, name))
            for name in os.listdir(stash):
                shutil.move(os.path.join(stash, name), os.path.join(profiles, name))
            os.rmdir(stash)
            print("# restored the training profiles")

    print("\n=== %s ===" % ("PASS" if not failures else "FAIL"))
    for f in failures:
        print("  " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
