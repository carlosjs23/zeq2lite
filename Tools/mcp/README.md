# Tools/mcp — drive the running game through typed tools

Two pieces, split by what changes:

| where | what | why there |
| --- | --- | --- |
| `Engine/server/sv_debugsocket.c` | a line-oriented JSON socket on loopback | small and permanent; C99 with no JSON library |
| `Tools/mcp/zeq2_mcp.py` | an MCP stdio server that translates onto it | all the protocol churn lands here |

The engine deliberately does **not** speak MCP. MCP has a handshake and a
dated spec that is revised periodically; implementing and maintaining that in
C would mean touching the engine every time the spec moves. The engine speaks
one JSON object per line, with a version number, and never changes.

## Enabling it

The socket is off unless `net_debugPort` is set:

```bash
Build/Release-darwin-arm/ZEQ2.arm +set fs_game ZEQ2 +set net_debugPort 27960 +devmap desert
```

`net_debugPort` is **latched** (it cannot be turned on from the console
mid-session), not archived (a development run cannot leave it enabled in the
player's saved config), and the listener binds `127.0.0.1` and hangs up on
any peer that is not loopback. It is a development facility. Anything that
can open a TCP connection to it gets the console, the cvar table and the
server's view of the world.

`zeq2_launch` passes the `+set` for you, so nothing has to be enabled by hand
when driving through MCP.

## The socket protocol

Request and reply are one JSON object per line. Fields are flat - nested
objects and arrays in a *request* are rejected rather than half-parsed.

```
-> {"v":1,"id":"4","op":"eval","cmd":"masterlist"}
<- {"v":1,"id":"4","ok":true,"op":"eval","time":18022,"cmd":"masterlist","output":"masters on desert:\n   1 rhogan: -40810 4681 1486 radius 768, 32 away\n   2 oberak: -29790 -28265 6062 radius 768, 34381 away\n","buffered":false,"truncated":false}
```

`id` is echoed back as a string. A failure is `{"v":1,"id":"4","ok":false,"error":"..."}`.

| op | fields | returns |
| --- | --- | --- |
| `ping` | — | `pong` |
| `version` | — | `protocol`, `netProtocol`, `engine`, `fs_game` |
| `state` | — | `server`, `client`, `player` (see below) |
| `eval` | `cmd`, `buffer` | `output` — what the player would see |
| `entities` | `max` | `entities[]`, `count` |
| `cvar` | `mode` (`get`/`set`), `name`, `value` | `name`, `value`, `integer` |

`state.player` carries origin, angles, velocity, groundEntityNum, weapon and
weaponstate, power level, health, fatigue, tier, the selectable skill list,
and `training` = `{objective, progress, master}`. It also carries
`powerLevelRaw`, `statsRaw` and `persistantRaw`: the named fields are
hand-copied mirrors of the game module's enums (the engine cannot include
`bg_public.h`), so the raw arrays are there as the fallback when they drift.
`tests/lint/check_debugsocket_enum_mirror.py` is the gate that stops them
drifting — it exists because `PERS_TRAINING_OBJECTIVE` was once copied as 9
instead of 6 and the snapshot reported a plausible, wrong number.

`eval` captures `Com_Printf` through `Com_BeginRedirect`, the same seam rcon
uses, so `ruledump`, `masterlist` and `arenalist` come back as strings a
script can read. Commands that cannot run inside a frame — `map`,
`map_restart`, `quit` — need `"buffer":true`, and those return no output.

## The MCP tools

| tool | arguments |
| --- | --- |
| `zeq2_launch` | `map`, `gametype`, `training`, `cheats`, `dedicated`, `port`, `extra[]`, `timeout` |
| `zeq2_console` | `cmd`, `buffer` |
| `zeq2_state` | — |
| `zeq2_entities` | `max` |
| `zeq2_cvar` | `name`, `mode`, `value` |
| `zeq2_input` | `spec`, `ms` |
| `zeq2_key` | `mode` (`text`/`char`/`key`), `text`, `code`, `keynum`, `down` |
| `zeq2_screenshot` | `timeout` |
| `zeq2_logs` | `filter` (regex), `since`, `limit` |
| `zeq2_shutdown` | — |

`zeq2_screenshot` returns an MCP **image** content block, so an agent sees
the frame instead of a path to it. The engine's own `screenshot` reads the
backbuffer from inside the render command queue, which needs none of the
macOS Screen Recording permission `screencapture` wants.

`zeq2_launch` encodes the tribal knowledge that a hand-written launch keeps
getting wrong: it clears a stale `zeq2lite.pid` (under
`FSFindFolder(kTemporaryFolderType)/…/TemporaryItems`, not `$TMPDIR`), backs
up `zeq2config.cfg` and restores it at shutdown, states `g_log games.log`
because the shipped `default.cfg` blanks it, loads with `devmap` so
cheat-gated commands are answered, and waits for the map to become playable
rather than for the process to exist.

Three things it does that are worth knowing:

- **The engine's stdin is detached.** The tty console reads fd 0 whether or
  not it is a terminal, so an inherited stdin means the engine swallows this
  server's JSON-RPC requests off the pipe — and the whole session dies with
  no error anywhere.
- **`ZEQ2_NO_SIGHANDLER=1` is set.** `Sys_SigHandler` otherwise catches the
  fault signals and a crash surfaces only as `=== fatal signal, exiting ===`
  with no address and no stack. The termination signals stay handled, so
  shutdown is still orderly.
- **A dead engine is an error, never a hang.** Every tool checks the process
  first and fails with the log tail attached. `zeq2_logs` says so explicitly:
  a log that ends mid-line means the handler caught the fault.

## Language choice

Python 3, standard library only, no MCP SDK.

Neither `@modelcontextprotocol/sdk` nor the `mcp` package is installed on
this machine, every other script in `Tools/dev` is stdlib-only Python by
policy, MCP stdio is plain JSON-RPC 2.0 over two pipes (the framing here is
about 60 lines), and the TGA→PNG conversion the screenshot tool needs
already exists next door in `Tools/dev/tga2png.py`. Adding a `node_modules`
tree to a game repository to avoid that framing would be the worse trade.

## Registration

`.mcp.json` in the repository root registers the server project-wide:

```json
{ "mcpServers": { "zeq2": { "command": "python3", "args": ["Tools/mcp/zeq2_mcp.py"] } } }
```

**It only affects sessions started after it exists.** A session already
running when the file was written does not see the server; start a new one.
Claude Code asks for approval the first time a project-scoped server is
seen — answer yes, or run `claude mcp list` to check what is registered.
Nothing else in this repository used MCP before, so this file is new and
there is no existing convention it has to match.

Without a client, drive it from a script over stdin/stdout — which is what
the acceptance test does.

## The acceptance test

```bash
python3 Tools/mcp/proof_flight_lesson.py
```

Spawns the MCP server as a subprocess, speaks JSON-RPC to it, and prints
every request and response verbatim: launch desert with training on,
teleport to Rhogan, complete the 45-second flight lesson with held input,
screenshot, assert `trained.rhogan.flight` and `budokai.entry` were granted
and the objective reached 100, shut down cleanly. Exit status is the verdict.

Two things about driving that lesson are not obvious and the script
documents them in place: `up` alone holds x and y exactly while `jump` held
in the air soars along the view yaw, and Rhogan's radius is a 768-unit
sphere — so the run lifts off with `jump,up`, switches to `up`, and snaps
the altitude back with `setviewpos` about once a second. And the training
profile persists while every rule forbids the tag it grants, so the script
moves the profiles aside for the duration and puts them back.
