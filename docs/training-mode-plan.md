# Training mode — design plan

First game mode for ZEQ2-Lite: a Dragon Ball training arc driven by a fact/rule
database. Written to be iterated on — every decision below records *why*, so
changing it is a knowing choice rather than a guess.

Every source fact in this document was measured from the tree, not estimated.

**Anchor on symbol names; treat line numbers as hints.** An earlier revision of
this document drifted by roughly seven lines against a later checkout, uniformly
— ordinary decay, but a document that promises re-checkability should not depend
on it. Cite the function, macro or enum member, with the line as a convenience.

## Scope

A player flies to a master (Roshi, King Kai), receives an objective, performs a
measurable feat, and is rewarded with progress toward tier unlocks. Solo
playable. No AI opponents, no new netcode, no external services.

Explicitly **not** in scope for v1: PvE mobs, item drops, global accounts,
melee-targetable objects, a graph editor.

## Why a training arc

The mode was chosen by elimination against what the tree can actually support.

- **No bot AI on this branch.** The Q3 bot code was stripped from this fork, and
  `GT_SINGLE_PLAYER` (`bg_public.h:116`) has nothing to fill a server with. This
  is a *merge dependency*, not a permanent constraint — see "Relationship to
  combat-and-ai" below.
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
| gravity | `ps->gravity[2]` (set in `PM_*` movement, `bg_pmove.c` ~1200) |
| beam struggle | `ps->timers[tmStruggleEnergy]` |
| aura | `ps->eFlags & EF_AURA` |
| flight | `ps->bitFlags & (usingJump\|usingSoar)` |
| break limit | fraction pool in `PM_CheckLimitBreak`, `bg_pmove.c` ~842 |

Two existing systems make this more attractive than it looks:

- **`g_radar.c` is a ki-sense system**, not a minimap — it already broadcasts
  every living player's position, `plCurrent`, `plMax`, charge state
  (`RADAR_WARN`) and aura/boost (`RADAR_BURST`).
- **Beam struggles are fully implemented** (`Think_NormalMissileStruggle`,
  `tmStruggleEnergy`, push-struggles at `g_usermissile.c:1620`) and are
  currently the most DB-authentic mechanic in the game.

Training also gives the tier/`breakLimit` system somewhere to lead. It was implemented and
almost invisible — one static icon for what is really a progress bar — until
`hud-stat-gauges` landed on `master` and gave it a gauge row and a top-up flash
(`HUD_ROW_BL_Y`, `breakLimitReadyTime`, `BREAKLIMIT_FLASH_TIME`).

## Architecture

Three data structures and a closed set of verbs. Modelled on Valve's rule
database (Ruskin, GDC 2012) with gameplay tags borrowed from Unreal's GAS.

### Facts

A fixed `int` array per client. Most entries are direct reads from playerState;
accumulators (`airborneTime`, `auraTime`) are advanced in the same pass.

**Refresh and evaluation run from `G_RunFrame`/`ClientEndFrame`, never from
`ClientThink`.** `ClientThink` is called once per usercmd, not once per server
frame — `sv_fps` is 20 while `cl_maxpackets` defaults to 30 and is archived and
client-settable. Evaluating there would let a client raise the server's
per-client rule cost by raising its own packet rate.

**Storage, against `ClientSpawn`.** `g_client.c` preserves only `client->pers`,
`client->sess` and `ps.persistant[]` across a spawn and clears the rest. So:

| Data | Lives in | Because |
|---|---|---|
| `airborneTime`, `auraTime` | wiped part of `gclient_t` | resetting on death is correct |
| **tag set** | **`client->pers`** | `trained.roshi.flight` must survive dying |

Tags in the wrong half is a bug that only appears when someone dies mid-arc —
the worst possible discovery schedule.

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

**Ties are a load-time error, not a runtime coin-flip.** If two rules can match
the same state with equal specificity, the loader rejects the file and names both
rules. First-in-file-wins would be a silent no-op by another route, and the whole
language design exists to convert quiet failures into loud ones.

### Tags

A bitfield of labels (`trained.roshi.flight`, `seen.firstAscension`). Rules carry
require/forbid sets. This supplies quest chains, prerequisites and one-shot
events with no extra machinery — a rule that grants a tag it also forbids is
self-terminating.

**A bitfield is flat; dots are not hierarchy for free.** `trained.roshi.flight`
is just a name containing dots. For `requires trained.roshi.*` to work, bits must
be **allocated by prefix at declaration time in `tags.def`**, so a prefix becomes
a contiguous mask. That is a constraint to design in now, not to retrofit — the
same argument the plan makes for world facts.

**State the cap.** `MAX_TAG_BITS` bounds total declared tags, and since
undeclared tags are a load error, the cap is a hard content limit. It must fail
with "tag budget exhausted (N of MAX_TAG_BITS used)", never with something
confusing. Prefix-grouped allocation also means a *prefix* can exhaust its group
while bits remain elsewhere; that error needs its own wording.

### World facts and world tags

Facts are per-client, but every mode beyond solo training needs state that is
not attached to a player — round state, event timers, team scores, a shared
clock.

```c
int      g_worldFacts[ fWorldFactCount ];   // round state, event timer, scores
tagSet_t g_worldTags;                       // event.meteor.active, round.inProgress
```

Rules therefore match against `(clientFacts, worldFacts, clientTags, worldTags)`.

**This goes in Phase 1 even though the training arc does not use it.** It is a
handful of lines now and a painful retrofit later, because every rule, every
test vector and every piece of authored content would have to be rewritten once
content exists.

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
6. **Inline tests, executed by Criterion.** The matcher is a pure function of
   `(facts, tags) → rule`, so content carries its own vectors and is verifiable
   without launching the game:
   ```
   test "fatigued out gets the failure line, not the pass" {
       given   masterNear    roshi
       given   airborneTime  46s
       given   fatigue       0
       given   tags          trained.roshi.greeting
       expect  roshi_flight_fail
   }
   ```

**These run as a Criterion suite, not only as a console command.** `tests/` already
builds game-module sources against stubs — `SUITES` includes `tiers` and
`usermissile`, with `SRC_tiers := Game/Game/g_tiers.c …` and a comment calling
`g_tiers.c` "the best-isolated unit in the game module." A `g_rules` suite is a
three-line addition to that Makefile and then runs under `make test` and
`Tools/dev/zeq2linux.sh test` with ASan and UBSan armed. A `ruletest` console
command is a convenience for authors; it runs in nobody's CI and must not be the
only gate.

Not JSON: no comments, no parser in this tree, and `COM_Parse` already
tokenizes this shape. What makes a format model-friendly is the validator and
the error messages, not the punctuation.

Parser follows the house pattern of `parseTier` (`g_tiers.c`) — `COM_Parse` loop,
`if(!token[0]){break;}` guards, defaults-then-override layering as at
`loadTierConfigs` building `players/%s/tier%i/tier.cfg`.

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

**Which protocol does this target?** `master` is on 71 and `combat-and-ai` is on
72. This work builds on `master`, so **Phase 2 ships on 71** — not because
compatibility is required, but because nothing here needs a wire change. 72
arrives with the `combat-and-ai` merge and costs this plan nothing either way.

Free `persistant[]` slots are chosen because they are the cheapest correct
answer, not because the wire format forces it. A dedicated `playerState_t` field
is available whenever it is genuinely the better fit — the project has no
obligation to stay wire-compatible.

**CLAUDE.md is stale on this point.** It states "The protocol is stock 71, so
this tree stays wire-compatible… keep [72] off this branch." That reflected an
earlier decision; wire compatibility is no longer a goal. CLAUDE.md should be
corrected separately.

Three rules:

- **Append, never renumber.** `PERS_SCORE` carries
  `// !!! MUST NOT CHANGE, SERVER AND GAME BOTH REFERENCE !!!`.
- **Quantize.** Progress is a percent `0..100`, not elapsed milliseconds.
  Milliseconds change every frame; percent changes ~100 times per lesson and the
  bar interpolates locally.
- **`persistant[]` is 16-bit on the wire.** `msg.c` writes with `MSG_WriteShort`
  and reads with `MSG_ReadShort`, so values are capped at ±32767 whatever the
  `int` in the struct says. Objective ID, progress percent and master ID all fit
  easily — but a power level or a timestamp put here later truncates *silently*.
  (The change mask is genuinely 32 bits: `MSG_WriteBits(msg, persistantbits,
  MAX_PERSISTANT)`.)

Precedent, and a caution: `g_rankings.c` already references `PERS_MATCH_RATING`
and `PERS_MATCH_TIME`, neither of which is in the enum. The file is not in the
Makefile, so it is dead — but reviving it would want two slots back.

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
   `vmMain` in `cg_main.c`.

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

**Phase 3 has a local trust problem, and it is the one that actually ships.**
`cl_guid` is `CVAR_USERINFO | CVAR_ROM`. ROM stops the *stock* client changing
it, but userinfo is client-supplied, so a modified client sends whatever guid it
likes. Nothing in `Game/` or `Engine/server/` reads it today — grep is empty —
so there is no existing scheme to inherit.

Keying save files on guid therefore means **any player can load any other
player's progress by editing one userinfo string.** Two honest options:

- **Trust-on-first-use.** Key on guid, accept that it is forgeable, and say so.
  Fine for a co-op community where progress is personal and nothing competitive
  rides on it. This is the v1 default.
- **Server-side name plus password.** A real login the server verifies. Needed the
  moment progress gates anything a player would want to steal — which, given the
  progression design, it does.

Pick before Phase 3 ships, not after someone notices.

Global accounts are a separate, later problem, and the blocker there is not the
database: any server that can report progress can lie about it, so global
progression means deciding who is allowed to host. That is community governance,
not engineering.

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

## Relationship to combat-and-ai

`origin/combat-and-ai` is where AI and training dummies are being built, and it
changes several assumptions here. Measured against `master`: **+3622 lines**,
including `g_ai.c` (1330 lines), `g_dummy.c` (398), `bg_pmove.c` (+667),
`g_combat.c` (+81). It is already on **`PROTOCOL_VERSION 72`**.

What that means for this plan:

- **Solo melee training is free there.** `G_SpawnDummy( owner, model, distance,
  fights )` takes a client slot via `G_DummySlot()`, so a dummy has a
  playerState and `lockedPlayer` works on it unchanged. The 105-reference
  refactor is not needed — that was only ever required for melee against
  *objects*. `Cmd_Dummy_f`, `Cmd_AI_f` and `Cmd_DummyClear_f` already exist.
- **PvE modes become reachable**, which lifts the ceiling described below.
- **Protocol 72 is already taken there.** Since this fork has no obligation to
  stay wire-compatible, training work should target 72 rather than carefully
  preserving 71.
- **`hud-stat-gauges` is in both** — it has landed on `master` as well, so the
  gauge vocabulary is available here without waiting for a merge.

**Sequencing: this work is built on `master`.** Phase 1 touches none of the same
files as `combat-and-ai`, so the two proceed in parallel and meet at a merge.
Phase 1b (melee legibility) is the exception — it edits `bg_pmove.c`, which that
branch has changed by 667 lines, so it should be rebased onto or re-applied
against `combat-and-ai` rather than merged blind.

## Melee legibility

The melee system is far deeper than it plays. There are **18 states**
(`stMeleeInactive` … `stMeleeChargingStun`, `bg_public.h:454–471`) driven by
aggress/degress, charge timing, breakers, block, evade, boost and directional
knockback, resolving as a genuine rock-paper-scissors:

| Attacker | Defender | Result |
|---|---|---|
| Charge breaker | Speed melee | **Backfires** — 950ms freeze plus fatigue |
| Breaker | Breaker | Clash, both frozen 500ms |
| Breaker | anything not evade | Hit, defender frozen, their charge cancelled |
| Attack | Block | Damage ×0.3 |
| Attack | Evade | No damage; attacker pays 0.8× fatigue, defender 0.4× |
| Attack | enemy in knockback | Auto power melee — juggle continuation |

None of this is visible. Five `PM_AddEvent` calls in `PM_Melee` are commented
out, and they are exactly the payoff moments — the ones that would teach a
player the system exists:

```c
//PM_AddEvent(EV_MELEE_BREAKER_BACKFIRE);   // bg_pmove.c ~2500, ~2528
//PM_AddEvent(EV_MELEE_BREAKER_CLASH);      // ~2506
//PM_AddEvent(EV_MELEE_CLASH);              // ~2534, ~2566
```

**These are not five lines to uncomment.** The three events they name do not
exist in `entity_event_t` — it has only `EV_MELEE_SPEED`, `MISS`, `KNOCKBACK`,
`BREAKER`, `STUN`, `CHECK`, `KNOCKOUT`, and `cg_event.c` handles five of those.
Uncommenting the calls does not compile.

Actual work:

1. Three new `entity_event_t` values. **Append** — these go on the wire as
   `es.event`. Capacity is fine: the field is 8 bits and ~91 of 256 values are
   used.
2. Three `cg_event.c` cases.
3. Sounds and effects, whose **assets are not in this repository** — they live in
   the ~324MB data set. This is the part most likely to be underestimated.

Still commented on `combat-and-ai` too, so this is not duplicate work either way.

On top of that, the HUD shows none of the 18 states, and the charge caps —
550ms for power, 1000ms for stun — have no meter.

**This is the same defect as `breakLimit`: implemented, invisible.** Two systems
now with that shape, which is worth naming as a pattern — this codebase's
problem is not missing depth, it is *unteachable* depth. That also answers the
retention question: mastery is the retention engine for a fighting game, and
ours is already built and hidden.

The pass:

1. Add the three events, wire them through `cg_event.c`, author the assets.
2. Melee state and charge readout, borrowing the gauge vocabulary already on
   `master` (`HUD_BAR_WIDTH`, `HUD_GAUGE_INSET`, the row heights).
3. Only then judge whether melee needs redesigning. It may already be the system
   we wanted.

**Do not put the charge meter in the status panel**, for two reasons.

The mechanical one: it is a fixed 288x86 plate with hardcoded rows
(`HUD_ROW_PL_Y`, `HUD_ROW_HP_Y`, `HUD_ROW_ST_Y`, `HUD_ROW_BL_Y`) whose sizes are
coupled to art generated by `Tools/dev/make_hud_gauge.py` — the header itself
warns to "change them together or the frames land off their windows", so a fifth
row means regenerating the plate.

The design one matters more. That panel is a status readout in the corner, but
melee charge and breaker windows are transient, sub-second, and happen where the
player is actually looking — at the locked target. Corner placement would make
the feedback technically present and practically still unreadable, which is the
exact failure this pass exists to fix. Same reasoning applies to the training
objective tracker.

This is worth doing whether or not the training arc ships, and it is a
prerequisite for writing melee lessons — a drill can only teach a mechanic the
player can perceive.

## Progression and PvP balance

Both techniques and transformations are gated behind training. A player who
joins today is at a disadvantage, and that is accepted. What follows is about
keeping that disadvantage survivable.

**What a new player has on day one:**

- The **entire melee system** — all 19 states, charge timing, breakers, counters,
  block, evade. No unlocks required, and it is the deepest system in the game.
- A basic ki blast
- Flight, boost, zanzoken
- Tier 1

**What they do not have:** named techniques and transformations.

**Tiers follow the reward-shape decision.** Training unlocks the *ability* to
ascend; `breakLimit` still earns the ascension in every fight. Goku learned Super
Saiyan once and then had to power up in every battle afterwards.

### Gate length is the real variable

Whether to gate is settled. How long the gate lasts is what decides whether new
players stay, and it matters here more than in most games because **there is no
matchmaking** — no accounts, one small population, so newcomers and veterans
share a server with no way to sort them.

Tiers are raw stat multipliers, not sidegrades — `goku/tier2/tier.cfg` carries
`percentMeleeAttack 1.4`, `percentEnergyAttackDamage 1.75`, `speed 1.16` plus
better defense, compounding through tier 5. An untransformed player against a
transformed one is in a different weight class, not merely behind.

So:

- Unlocking the first transformation in **20 hours** means weeks of being farmed.
  With this population, those players do not come back.
- Unlocking it in **30 minutes** makes it a tutorial. Nobody minds being weaker
  on their first evening.

**Content rule: the first transformation and two or three techniques must be
reachable inside the first session — under an hour.** That is the "I can
compete" threshold. Everything after it can be as slow as we like; higher tiers
and exotic techniques are the long tail and slow is correct there.

The failure to guard against is not the disadvantage on day one. It is the
disadvantage still being there in week three.

### Unlock breadth, not magnitude

A technique must be a *different* tool, not a strictly better one. Kamehameha
should be slow, chargeable, beam-struggle capable and punishable on whiff — not
"ki blast with triple damage." If every unlock is simply stronger, option
progression collapses back into power progression and the gate length stops
mattering because the gap never closes.

### Implementation

No unlock infrastructure exists to inherit. `currentSkill[MAX_WEAPONS]` looks
like a candidate but holds the *active* weapon's runtime state
(`WPSTAT_CHANGED`, `WPSTAT_CHRGREADY`, `WPSTAT_BITFLAGS`), not ownership.

So a technique unlock is a tag — `grant technique.kamehameha` — and weapon
availability tests for it. Same machinery as everything else, no new system.

### Risks

**The benefit is subtle.** A wider roster and faster ascension are real
advantages, but a player who cannot perceive them will not feel rewarded for
training. This is the legibility problem for the third time, and it is why the
melee pass comes first.

**Open, and only playtesting settles it:** whether a small *permanent* edge —
tier 2 unlocked outright, tiers 3+ earned in-match — feels rewarding without
being oppressive. The rule engine makes this a config change, so both are cheap
to try.

## Generalizing to other modes

The engine is worth building because the training arc is not the only thing it
runs. ZEQ2 is already an open world — large maps, free flight, other players —
with no content placed in it. The rule database is a system for placing content
in that world, and masters are simply the first markers.

With no new C, once the engine and world facts exist:

- **Saga / story missions.** A mission is a tag-gated group of rules;
  completing it grants the tag that opens the next. Structurally identical to a
  training lesson.
- **Time trials and checkpoint races.** Checkpoints are trigger volumes exactly
  like masters; elapsed time is already a fact.
- **Dynamic world events.** A rule fires on world facts and a timer, the
  announcer calls a location, players converge.
- **Ki-sense hide and seek.** `g_radar.c` already broadcasts position, power
  level and charge state; rules define the win conditions.
- **Budokai rules.** Ring-out is a trigger volume, round state is world facts,
  the bracket is `GT_TOURNAMENT`, which is already wired.

Where it stops, and what each would cost:

- **New verbs need C** — spawning entities, carrying an object, awarding team
  score. The action vocabulary is closed on purpose, so each is a small,
  deliberate addition.
- **Dragon Ball hunt needs a carry mechanic.** The rules half is free; there is
  no item system and `PW_` was fully repurposed, so pickups do not exist.
- **PvE needs the `else` branch** in `G_UserWeaponDamage`, plus the
  `lockedPlayer` refactor to punch rather than only blast.

**Ceiling:** on this branch alone the world stays empty of *characters* —
objectives, events, races and missions are reachable, but wandering enemies and
scripted actors are not. A master who stands still and talks is believable; a
Frieza soldier who stands still is not.

That ceiling lifts on merge with `combat-and-ai`, and everything here composes
with it: an AI enemy is just another fact source, and a dummy is a client slot
that rules can already reason about.

## Prior art: Fortnite Creative

Worth studying because it is the same architecture at enormous scale, and its
mistakes are already public.

**A closed device vocabulary beat a scripting language for years.**
[Fortnite Creative](https://dev.epicgames.com/documentation/en-us/fortnite/using-trigger-devices-in-fortnite-creative)
shipped 100+ device types — spawners, triggers (timer, conditional, proximity),
mutators — with no code at all, and non-programmers built millions of islands
with it. [Verse](https://dev.epicgames.com/documentation/fortnite/verse-language-get-started-in-unreal-editor-for-fortnite)
arrived years later, and took Epic a dedicated language team to produce. That
is exactly the ordering chosen here: closed actions first, a language only if
and when the vocabulary demonstrably runs out.

**Do not build a numeric global bus.** Creative originally wired devices
together with Channels numbered 1–9999 — a trigger on channel 5 fires an item
granter on channel 5. It was replaced in v25.00 with direct function/event
binding, because a numeric bus does not scale: nothing tells you what channel 47
is for. Our named, declared tags are already the better design. Do not regress
to integer IDs for convenience.

**Mutators are a category we are missing.** Devices that modify attributes —
damage, health, movement — across a region or a match. As an action verb
(`mutate gravity 10g`, `mutate meleeDamage 0.5`) this is how mode variants get
authored without C. Cheap to add, and it generalizes the `setGravity` action we
already planned.

**Modes are data on one engine.** Battle Royale, Zero Build, Creative, Festival
and Racing run off shared systems rather than separate codebases. Same bet as
the section above.

**Bots fill lobbies.** Fortnite backfills matches with AI so a lobby is never
empty. That is the same problem this project has, solved the same way — more
evidence that bot AI is the highest-leverage work available.

**Counter-lesson: do not copy the battle pass.** Time-limited progression that
expires works for a company shipping seasons forever. For a hobbyist community
it mostly converts lapsed players into permanently-gone ones. Progression here
should never expire.

## Phases

Each phase is independently playable or verifiable.

**Phase 1 — rules engine, no UI.** `g_rules.c`: parser, matcher, tag system,
world facts and world tags, `facts.def`/`tags.def` generation, a `g_rules`
Criterion suite, and `ruledump`/`ruletest` console commands. `POOLSIZE` bumped.

**Phase 1 content must not use `masterNear`** — master triggers arrive in Phase
2, so a rule keyed on them cannot be exercised here. Build Phase 1 vectors from
facts that exist at this point: `airborneTime`, `auraTime`, `fatigue`,
`tierCurrent`, `gravity`. The Roshi and King Kai examples elsewhere in this
document are Phase 2 content.

**Phase 1b — melee legibility.** Restore the five commented feedback events, add
melee state and charge meter to the HUD. Independent of the rules engine, valuable
on its own, and a prerequisite for melee lessons. Best done on or after
`combat-and-ai`, which carries the gauge work.

**Phase 2 — the loop on screen.** Three `PERS_` slots, server command handling
in `cg_servercmds.c`, objective tracker and progress gauge in `cg_draw.c`,
master triggers placed from a per-map file — with an in-game `masterplace`
command following the JUHOX lens-flare editor pattern (`cgs.editMode`,
`CG_SaveLensFlareEntities_f` in `cg_consolecmds.c`; `MAPLENSFLARES` is 1, so it is live code) rather than hand-typed
coordinates. Playable end to end.

**Phase 3 — persistence.** Per-player files, tag gating across sessions,
structured event lines to `games.log`.

**Phase 4 — journal screen.** Most code, least necessary to prove the loop.

**Phase 5 — Tenkaichi Budokai.** `GT_TOURNAMENT` is already wired (`g_client.c`,
`g_cmds.c`, `g_main.c`, `g_arenas.c`) — add ring-out as a trigger volume and a
spectator ring. Mostly assembly. The canonical payoff: train, then test it.

## Decisions made

1. **Master placement** — per-map config file, written by an in-game
   `masterplace` command following the existing lens-flare editor pattern.
   Maps are not in this repo, and re-BSP'ing to move a trigger is a bad
   authoring loop; hand-typing coordinates is barely better.
2. **Lesson granularity** — one goal per rule. This does not cap lesson
   complexity: a multi-step lesson is chained rules, where each step grants the
   tag the next one requires. That also keeps each step independently testable.
3. **Reward shape** — **lessons unlock the ability; `breakLimit` still earns the
   ascension in-match.** Training is what teaches you the transformation; you
   still have to power up in every fight afterward. Keeps both systems
   meaningful and matches the source material.
4. **Protocol** — no obligation to keep 71; break it when something needs it.
   `combat-and-ai` is already on 72, so that is the target.
5. **Melee training** — resolved by `g_dummy.c` on `combat-and-ai`. Dummies take
   client slots and therefore have playerStates, so `lockedPlayer` works on them
   and no refactor is required. Partner drills work today; solo drills work on
   merge.
6. **Progression gating** — techniques and transformations are both gated behind
   training, with the first transformation and a few techniques reachable inside
   the first session. See "Progression and PvP balance".

## Open decisions

6. **Rule bucketing** — the matcher scans every rule for every client each server
   frame. At the budget the memory section implies (~500 rules in ~108KB) and
   `sv_fps` 20, that is small. Bucket rules by first criterion when a profile
   says to, not on a predicted threshold.

   *(An earlier draft claimed "~2.6M comparisons/sec at 16 players". It stated no
   rule count, implied roughly 2,700 rules, and so contradicted both the memory
   sizing and the bucketing threshold by an order of magnitude. Removed — per the
   repo's commit convention, a number earns its place only when it justifies a
   constant.)*
7. **Do AI opponents become fact sources?** An AI enemy's state (its power level,
   whether it is charging) could be exposed as facts, which would let rules
   author PvE encounters without new C. Attractive, unscoped.

## Traps

Live constraints that have bitten this tree before.

- **Configs are CRLF.** Every shipped `.cfg` uses `\r\n`. Text-mode rewrites
  convert silently and then every line reads as changed. Edit in binary mode.
- **QVM has no libc.** `Game/` compiles to bytecode on Linux; only `bg_lib.c`
  and the syscalls in `g_syscalls.asm` exist. Code can build clean on arm64 and
  break in the container.
- **Booleans are the string `True`** — `parseTier` tests `strlen(token) == 4`.
- **Build via `Tools/dev/zeq2build.sh`, never bare `make`** — bare `make` writes
  `cgamearm.dylib` while the engine loads `cgamearm64.dylib`, so fixes appear to
  do nothing.
