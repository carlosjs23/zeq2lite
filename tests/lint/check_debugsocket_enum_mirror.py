#!/usr/bin/env python3
"""Gate: the debug socket's DBG_* constants must match the game's enums.

Engine/server/sv_debugsocket.c names a handful of playerState array indices -
which slot holds fatigue, which holds the training objective - so that
`state` can report them by name. The engine cannot include
Game/Game/bg_public.h (it is a module header), so those indices are
hand-copied constants, and a hand-copied index is wrong silently: the
snapshot keeps parsing and just reports the wrong number.

That already happened once. PERS_TRAINING_OBJECTIVE was copied as 9 because
the enum has a long comment above it; the socket reported a training
objective of 0 while ruledump reported 3, and the only reason it was caught
is that the two were compared by hand.
"""

import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOCKET = os.path.join(ROOT, "Engine", "server", "sv_debugsocket.c")
BG = os.path.join(ROOT, "Game", "Game", "bg_public.h")

# DBG_ name in the socket -> enumerator name in bg_public.h
MIRRORS = {
    "DBG_PL_CURRENT": "plCurrent",
    "DBG_PL_FATIGUE": "plFatigue",
    "DBG_PL_HEALTH": "plHealth",
    "DBG_PL_MAXIMUM": "plMaximum",
    "DBG_PL_TIER_CURRENT": "plTierCurrent",
    "DBG_PL_TIER_TOTAL": "plTierTotal",
    "DBG_ST_SKILLS": "stSkills",
    "DBG_PERS_TRAINING_OBJECTIVE": "PERS_TRAINING_OBJECTIVE",
    "DBG_PERS_TRAINING_PROGRESS": "PERS_TRAINING_PROGRESS",
    "DBG_PERS_TRAINING_MASTER": "PERS_TRAINING_MASTER",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def enum_values(header):
    """Every enumerator in every plain (unassigned) enum, with its ordinal."""
    text = strip_comments(header)
    values = {}
    for body in re.findall(r"typedef\s+enum\s*\{(.*?)\}", text, flags=re.S):
        if "=" in body:
            continue                      # an enum with explicit values; skip
        index = 0
        for token in body.split(","):
            name = token.strip()
            if not re.fullmatch(r"[A-Za-z_]\w*", name):
                continue
            values.setdefault(name, index)
            index += 1
    return values


def main():
    with open(SOCKET) as fh:
        socket_src = fh.read()
    with open(BG) as fh:
        values = enum_values(fh.read())

    failures = []
    for macro, enumerator in sorted(MIRRORS.items()):
        m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % re.escape(macro),
                      socket_src, flags=re.M)
        if not m:
            failures.append("%s is not defined in sv_debugsocket.c" % macro)
            continue
        if enumerator not in values:
            failures.append("%s is not an enumerator in bg_public.h" % enumerator)
            continue
        got, want = int(m.group(1)), values[enumerator]
        if got != want:
            failures.append("%s is %d but %s is %d" % (macro, got, enumerator, want))

    label = "debug socket enum mirrors match bg_public.h"
    if failures:
        print("FAIL  %s" % label)
        for f in failures:
            print("      %s" % f)
        return 1
    print("PASS  %s" % label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
