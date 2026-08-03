# Training mode — design plan

First game mode for ZEQ2-Lite: a Dragon Ball training arc driven by a fact/rule
database. Written to be iterated on — every decision below records *why*, so
changing it is a knowing choice rather than a guess.

Every source fact in this document was measured from the tree, not estimated.
File:line references are given so they can be re-checked when the code moves.

## Scope

A player flies to a master (Roshi, King Kai), receives an objective, performs a
measurable feat, and is rewarded with progress toward tier unlocks. Solo
playable. No AI opponents, no new netcode, no external services.

Explicitly **not** in scope for v1: PvE mobs, item drops, global accounts,
melee-targetable objects, a graph editor.

## Why a training arc

The mode was chosen by elimination against what the tree can actually support.

- **No bot AI.** `Game/Game/ai_*.c` does not exist — the Q3 bot code was
  stripped from this fork. `GT_SINGLE_PLAYER` is in the gametype enum
  (`bg_public.h:116`) with nothing to fill a server with. Any design needing
  enemies is a bot port first.
- **No item system.** There is no `g_items.c` and no `bg_itemlist`; `ET_ITEM`
  is a vestigial enum value. World pickups and drops do not exist as a concept.
- **Melee cannot target non-players.** Melee locks onto
  `playerState *lockedPlayer` (`q_shared.h:1203`) — a raw playerState pointer.
  Objects have no playerState, so destructible targets would be ki-damage-only
  without a refactor.
- **Ki damage is already generic.** `G_UserWeaponDamage`
  (`g_usermissile.c:456`) guards on `target->takedamage`, not on
  `target->client`, and wraps its whole body in `if(tgClient)`. A non-client
  entity flows through and does nothing — so PvE damage is one `else` branch
  away, when we want it.

Training objectives need none of that, because every objective is a predicate
over the player's own state. That state is unusually rich here and already
tracked:

| Signal | Source |
|---|---|
| power level, fatigue, health | `ps->powerLevel[plCurrent/plFatigue/plHealth]` |
| tier | `ps->powerLevel[plTierCurrent/plTierTotal]` |
| gravity | `ps->gravity[2]` (`bg_pmove.c:1193`) |
| beam struggle | `ps->timers[tmStruggleEnergy]` |
| aura | `ps->eFlags & EF_AURA` |
| flight | `ps->bitFlags & (usingJump\|usingSoar)` |
| break limit | fraction pool, `bg_pmove.c:835` |

Two existing systems make this more attractive than it looks:

- **`g_radar.c` is a ki-sense system**, not a minimap — it already broadcasts
  every living player's position, `plCurrent`, `plMax`, charge state
  (`RADAR_WARN`) and aura/boost (`RADAR_BURST`).
- **Beam struggles are fully implemented** (`Think_NormalMissileStruggle`,
  `tmStruggleEnergy`, push-struggles at `g_usermissile.c:1620`) and are
  currently the most DB-authentic mechanic in the game.

Training also makes the tier/`breakLimit` system legible. It is implemented and
almost invisible — `cg_draw.c:700` draws one static icon for a mechanic that is
really a progress bar.

## Architecture

Three data structures and a closed set of verbs. Modelled on Valve's rule
database (Ruskin, GDC 2012) with gameplay tags borrowed from Unreal's GAS.

### Facts

A fixed `int` array per client, refreshed once per frame from playerState. Most
entries are direct reads; accumulators (`airborneTime`, `auraTime`) live in the
client struct and are advanced in the same pass.

```c
typedef enum {
	fPowerCurrent, fPowerPercent, fFatigue, fHealth,
	fTierCurrent, fTierTotal, fGravity, fStruggleEnergy,
	fPowerRaiseTime, fAirborneTime, fAuraTime, fMasterNear,
	fFactCount
} factKey_t;
```

### Rules

Every criterion is a **range**, which removes the need for an operator enum
entirely — `atLeast 45s` is `[45000, INT_MAX]`, `is roshi` is `[1,1]`.

```c
typedef struct { int key; int min; int max; } criterion_t;

typedef struct {
	char        name[32];
	int         numCriteria;
	criterion_t criteria[MAX_RULE_CRITERIA];
	tagSet_t    requireTags;
	tagSet_t    forbidTags;
	int         numActions;
	action_t    actions[MAX_RULE_ACTIONS];
} rule_t;
```

Matching returns the rule with the **most criteria** that fully matches. That
single rule gives free fallbacks: author the specific and generic cases side by
side and the specific one wins whenever it applies.

### Tags

A bitfield of hierarchical labels (`trained.roshi.flight`, `seen.firstAscension`).
Rules carry require/forbid sets. This supplies quest chains, prerequisites and
one-shot events with no extra machinery — a rule that grants a tag it also
forbids is self-terminating.

### Actions

Deliberately closed. The moment `runScript` appears, we have built a language
with no debugger (Bilas, GDC 2002).

```
grant <tag>          remove <tag>
say "<text>"         objective "<text>" track <fact> goal <value>
setGravity <value>   unlock tier <n>
```

The payoff is that this is not a quest system — it is one system that also
drives announcer lines, unlock gating and one-shot events, because all of them
are "when the facts look like this, do that."

## Script language

Optimised for LLM and newcomer authorship. The failure mode being designed
against is the **silent no-op**: a typo'd fact or tag makes a rule never match,
with no error, and nobody notices for a month.

```
rule roshi_flight_pass {
    when  masterNear    is       roshi
    when  airborneTime  atLeast  45s
    when  fatigue       above    0

    requires  trained.roshi.greeting
    forbids   trained.roshi.flight

    grant   trained.roshi.flight
    say     "Not bad. You have the endurance of a delivery boy."
    unlock  tier 2
}
```

Design rules:

1. **Named operators** — `is`, `atLeast`, `atMost`, `above`, `below`,
   `between X and Y`. All compile to the same two ints.
2. **Unit suffixes** — `45s`, `2500pl`, `10g`. Bare numbers are unreadable and
   unverifiable; wrong units become a parse error.
3. **Named enum values** — `masterNear is roshi`, never `masterNear 1 1`.
4. **Declared vocabulary.** `facts.def` is generated from the C enum by
   `ruledump facts`; `tags.def` lists every legal tag. Anything undeclared is a
   **load error**, which converts the worst failure mode into the loudest one.
5. **Errors written for a machine reader**, with Levenshtein suggestions:
   ```
   rules/roshi.rules:14: unknown fact 'airbornTime' - did you mean 'airborneTime'?
   rules/roshi.rules:19: tag 'trained.roshi.fligth' not declared in tags.def
   ```
6. **Inline tests.** The matcher is a pure function of `(facts, tags) → rule`,
   so content carries its own vectors and is verifiable without launching the
   game:
   ```
   test "fatigued out gets the failure line, not the pass" {
       given   masterNear    roshi
       given   airborneTime  46s
       given   fatigue       0
       given   tags          trained.roshi.greeting
       expect  roshi_flight_fail
   }
   ```

Not JSON: no comments, no parser in this tree, and `COM_Parse` already
tokenizes this shape. What makes a format model-friendly is the validator and
the error messages, not the punctuation.

Parser follows the house pattern at `g_tiers.c:253` — `COM_Parse` loop,
`if(!token[0]){break;}` guards, defaults-then-override layering as at
`g_tiers.c:239`.

## Transport

Three options exist and only two are worth using.

| | Protocol impact | Idle cost | Predicted | Text |
|---|---|---|---|---|
| New `playerState_t` field | **bumps 71 → 72** | ~free | yes | no |
| Free `persistant[]` slot | **none** | free | yes | no |
| Server command | none | free | no | yes |

`MAX_PERSISTANT` is **32** (`q_shared.h:1145`) and only six indices are used —
`PERS_SCORE`, `PERS_TEAM`, `PERS_SPAWN_COUNT`, `PERS_PLAYEREVENTS`,
`PERS_ATTACKER`, `PERS_KILLED`. **26 slots are free.**

Using one is not a protocol change. `msg.c:1374` sends the array as a changed
mask plus only the changed entries, and `msg.c:1437` skips the whole array
section with one bit when nothing changed. The `netField_t` table — which is
what protocol 71 actually is — is untouched.

**Decision:** three `PERS_` slots for active objective ID, progress percent and
master ID; server commands for toasts, completion events and journal sync.

Two rules:

- **Append, never renumber.** `PERS_SCORE` carries
  `// !!! MUST NOT CHANGE, SERVER AND GAME BOTH REFERENCE !!!`.
- **Quantize.** Progress is a percent `0..100`, not elapsed milliseconds.
  Milliseconds change every frame; percent changes ~100 times per lesson and the
  bar interpolates locally.

`MAX_RELIABLE_COMMANDS` is 64 (`qcommon.h:141`) and overflow disconnects the
client, so server commands are for events only — never per-frame.

## UI

**The UI here is not data-driven.** `MISSIONPACK` appears zero times in the
Makefile, so the Team Arena menu system — `CG_LoadHud_f`, `CG_LoadMenus`,
feeders, ownerdraws — is compiled out and `Game/UI/menudef/*.txt` are dead
files. `ui_shared.c` is 22 lines. The shipping UI is classic Q3
(`ui_qmenu.c`, 1731 lines), and the HUD is hand-written C in `cg_draw.c`.

Data path, one hop:

```
g_rules.c → trap_SendServerCommand / persistant[] → cg_servercmds.c → cg_draw.c
```

Four surfaces, all in cgame:

1. **Objective tracker** — small, persistent, corner.
2. **Progress gauge** — the `breakLimit` fraction pool, finally visible.
3. **Toast / master prompt** — from the rule's `say` action.
4. **Journal** — full-screen. Lives in cgame, *not* the UI module: the UI module
   is a separate VM that receives no server commands, and with per-server
   progression the data is on the server anyway. cgame can take input —
   `CG_KEY_EVENT`, `CG_MOUSE_EVENT`, `CG_EVENT_HANDLING` are dispatched at
   `cg_main.c:57–63`.

**Trap:** all 2D is authored in a 640x480 virtual space with two mappings. The
HUD is aspect-correct; a new element must use the same mapping as its
neighbours or it drifts as the display aspect changes. Stretch is only for
full-screen backdrops.

The action vocabulary *is* the UI contract — `objective ... track <fact> goal
<value>` places a tracked bar, `say` produces a toast. Content authors control
the screen without touching C and cannot invent widgets. UI work is bounded and
done once.

## Persistence

**Per-server, flat files, v1.** Written by the game module with `G_FS_WRITE`
(`g_public.h:135`).

Per-server is also what the engine already assumes: `cl_guid` is an MD5 of a
file on the player's own disk (`cl_main.c:1330`) — a machine identifier, not
authentication — and `cl_guidServerUniq` defaults to `1` (`cl_main.c:3429`),
deliberately hashing a *different* GUID per server.

Global accounts are deferred, and the blocker is not the database. Any server
that can report progress can lie about it, so global progression means deciding
who is allowed to host. That is community governance, not engineering.

**Emit structured events from day one** via `G_LogPrintf` → `games.log`
(`g_main.c:1024`). Keeping the seam at "the mod emits events, something else
stores them" means a later backend — Postgres, SpacetimeDB, nothing — is a
sidecar change, not a rewrite.

For reference if that day comes: the game module has **no network syscalls**
(44 `G_*` entries in `g_public.h`, no sockets). The engine has curl
(`USE_CURL=1`, `Engine/client/cl_curl.c`) but it is client-only — `Q3DOBJ`
contains zero curl references — and it is a pk3 downloader, not an HTTP client.
Reaching a database from the mod means a server-side curl build plus an async
syscall pumped from `SV_Frame`; a synchronous call would stall the frame loop.

## Memory

`POOLSIZE` is `256 * 1024` (`g_mem.c:31`) and `G_Alloc` is a bump allocator
with no `G_Free`. This is not a protocol constraint — it is a `#define` in our
own source, inherited from 1999.

**Decision: raise to 4MB.** Not because the rule database needs it (~108KB with
pooled action strings) but so the number stops being a topic.

Cost, for QVM builds only: the pool lives in `bss`, so the data segment grows
and is rounded up to the next power of two (`vm.c:445`) out of the hunk
(`DEF_COMHUNKMEGS` is 1024, `common.c:41`). Watch the rounding — 256KB → 4MB can
push a 2MB segment to 8MB. On arm64 no QVMs are built at all, so the pool is
just lazily-mapped BSS in a dylib.

Keep `G_Alloc` rather than `malloc`, which native modules could use: QVMs are
architecture-independent, so a server ships one `.qvm` that any client
downloads and runs. Native-only means a matching binary per architecture.

The allocator also fits the workload exactly — rule content is fully known at
load time, so `G_InitGame` counts, allocates once, parses, and never allocates
again. Nothing is ever freed because nothing needs to be.

## Phases

Each phase is independently playable or verifiable.

**Phase 1 — rules engine, no UI.** `g_rules.c`: parser, matcher, tag system,
`facts.def`/`tags.def` generation, `ruledump` and `ruletest` console commands.
Verified by inline tests and console output. `POOLSIZE` bumped.

**Phase 2 — the loop on screen.** Three `PERS_` slots, server command handling
in `cg_servercmds.c`, objective tracker and progress gauge in `cg_draw.c`,
master triggers placed from a per-map file. Playable end to end.

**Phase 3 — persistence.** Per-player files, tag gating across sessions,
structured event lines to `games.log`.

**Phase 4 — journal screen.** Most code, least necessary to prove the loop.

**Phase 5 — Tenkaichi Budokai.** `GT_TOURNAMENT` is already wired (`g_client.c`,
`g_cmds.c`, `g_main.c`, `g_arenas.c`) — add ring-out as a trigger volume and a
spectator ring. Mostly assembly. The canonical payoff: train, then test it.

## Open decisions

Things to iterate on. Current leaning noted, none of them settled.

1. **Master placement** — per-map config file read at `G_InitGame`, or entities
   in the `.bsp`? *Leaning: config file.* Maps are not in this repo and
   re-BSP'ing every map to move a trigger is a bad authoring loop.
2. **Lesson granularity** — one goal per rule with environment as a modifier, or
   multi-goal lessons? *Leaning: one goal.* Two goals forces nested blocks, and
   the format has none.
3. **Reward shape** — do lessons unlock tiers directly, or feed the existing
   `breakLimit` rate? *Undecided.* Direct unlocks are legible; feeding
   breakLimit reuses a mechanic that already exists.
4. **Protocol 71** — worth keeping? Nothing in this plan needs 72. But if the
   fork is self-contained, moving to 72 would allow quest state directly in
   `playerState_t` with free prediction. *Leaning: stay on 71* while it costs
   nothing.
5. **Melee training** — punching a target needs the `lockedPlayer` refactor.
   Deferred, but it is the most DB-authentic objective type we cannot yet
   express.
6. **Rule bucketing** — scanning all rules per client per frame is ~2.6M
   comparisons/sec at 16 players. Fine, but wasteful. Bucket by first criterion
   when rule count passes a few hundred, not before.

## Traps

Live constraints that have bitten this tree before.

- **Configs are CRLF.** Every shipped `.cfg` uses `\r\n`. Text-mode rewrites
  convert silently and then every line reads as changed. Edit in binary mode.
- **QVM has no libc.** `Game/` compiles to bytecode on Linux; only `bg_lib.c`
  and the syscalls in `g_syscalls.asm` exist. Code can build clean on arm64 and
  break in the container.
- **Booleans are the string `True`** — `g_tiers.c:619` tests `strlen(token) == 4`.
- **Build via `Tools/dev/zeq2build.sh`, never bare `make`** — bare `make` writes
  `cgamearm.dylib` while the engine loads `cgamearm64.dylib`, so fixes appear to
  do nothing.
