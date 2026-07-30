#!/usr/bin/env python3
"""Gate: a function that copies a parameter into a file-scope buffer must not be
called with that same buffer as the argument.

    static char s_backgroundLoop[MAX_QPATH];

    void S_Base_StartBackgroundTrack( const char *intro, const char *loop ) {
        Q_strncpyz( s_backgroundLoop, loop, sizeof( s_backgroundLoop ) );   <-- copies param 2 into the buffer
    }
    ...
    S_Base_StartBackgroundTrack( s_backgroundLoop, s_backgroundLoop );      <-- offender

That is the music-loop trap. Q_strncpyz wraps strncpy, and strncpy with
overlapping source and destination is undefined behaviour. It survived for years
because an unfortified strncpy usually appears to work when the buffers are
identical; macOS builds it with _FORTIFY_SOURCE, so __strncpy_chk detects the
overlap and traps. The result was a hard crash every time a music track reached
its end and tried to restart itself.

Why this needs a lint rather than a unit test: the buffer is file-static, so no
test can construct the aliasing call from outside the translation unit. The
aliasing is a property of the *call site*, which is exactly what a source-level
scan can see and a linked unit test cannot.

The rule is deliberately narrow - the destination must be a file-scope buffer in
the same file as the function, and the source must be one of that function's own
parameters. That combination is always an aliasing self-copy when the caller
passes the buffer back in, and it produces no false positives on this tree.

usage:
    check_self_aliasing_copies.py [paths ...]     (default: Game Shared Engine)

exit 0 = clean, 1 = offenders found.
"""
import os
import re
import sys

DEFAULT_PATHS = ("Game", "Shared", "Engine")

COPY_CALL = re.compile(r'\b(?:Q_strncpyz|strncpy|strcpy)\s*\(')
# File-scope char buffer: `static char name[...]` / `char name[...]` at column 0.
FILE_BUF = re.compile(r'^(?:static\s+)?(?:const\s+)?char\s+(\w+)\s*\[', re.M)
# A function definition opening at column 0: `type name( args ) {`
FUNC_DEF = re.compile(r'^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{]*?)\)\s*\{', re.M)
IDENT = re.compile(r'^[A-Za-z_]\w*$')


def split_args(text, open_paren):
    """Split the argument list of a call whose '(' sits at text[open_paren].
    Returns (args, index_after_close) or (None, None) when unbalanced."""
    depth = 0
    args, cur = [], []
    i = open_paren
    while i < len(text):
        c = text[i]
        if c in '"\'':
            quote, cur_start = c, i
            i += 1
            while i < len(text) and text[i] != quote:
                i += 2 if text[i] == '\\' else 1
            cur.append(text[cur_start:i + 1])
            i += 1
            continue
        if c == '(':
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif c == ')':
            depth -= 1
            if depth == 0:
                args.append(''.join(cur).strip())
                return args, i + 1
        elif c == ',' and depth == 1:
            args.append(''.join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    return None, None


def param_names(param_text):
    """Parameter identifiers, in order. `void` and varargs yield None entries."""
    out = []
    for raw in param_text.split(','):
        raw = raw.strip()
        if not raw or raw == 'void' or raw == '...':
            out.append(None)
            continue
        m = re.search(r'(\w+)\s*(?:\[\s*\])?$', raw)
        out.append(m.group(1) if m else None)
    return out


def function_bodies(text):
    """Yield (name, params, body) for each function defined at column 0."""
    for m in FUNC_DEF.finditer(text):
        name, params = m.group(1), m.group(2)
        depth, i = 0, m.end() - 1
        start = i
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    yield name, param_names(params), text[start:i]
                    break
            i += 1


def find_sinks(text):
    """(function, param_index, buffer) for each `copy(fileScopeBuf, param, ...)`."""
    buffers = set(FILE_BUF.findall(text))
    if not buffers:
        return []
    sinks = []
    for name, params, body in function_bodies(text):
        for m in COPY_CALL.finditer(body):
            args, _ = split_args(body, m.end() - 1)
            if not args or len(args) < 2:
                continue
            dest, src = args[0].strip(), args[1].strip()
            if dest not in buffers or not IDENT.match(src):
                continue
            if src in params:
                sinks.append((name, params.index(src), dest))
    return sinks


def find_offenders(text, path, sinks):
    hits = []
    for func, idx, buf in sinks:
        for m in re.finditer(r'\b%s\s*\(' % re.escape(func), text):
            args, _ = split_args(text, m.end() - 1)
            if not args or len(args) <= idx:
                continue
            if args[idx].strip() == buf:
                line = text.count('\n', 0, m.start()) + 1
                hits.append((path, line, func, idx + 1, buf))
    return hits


def iter_sources(paths):
    for root in paths:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, _, names in os.walk(root):
            for n in sorted(names):
                if n.endswith('.c'):
                    yield os.path.join(dirpath, n)


def main(argv):
    paths = argv[1:] or [p for p in DEFAULT_PATHS if os.path.isdir(p)]
    offenders = []
    sources = list(iter_sources(paths))
    for path in sources:
        with open(path, 'r', errors='replace') as fh:
            text = fh.read()
        sinks = find_sinks(text)
        if not sinks:
            continue
        # The aliasing call is almost always in the same file as the buffer,
        # since a file-scope static is not nameable from outside it.
        offenders += find_offenders(text, path, sinks)

    if offenders:
        for path, line, func, pos, buf in offenders:
            print("%s:%d: %s() copies argument %d into %s, but is called with %s "
                  "in that position - strncpy onto itself is undefined"
                  % (path, line, func, pos, buf, buf))
        print("\nFAIL  %d self-aliasing copy call(s)" % len(offenders))
        return 1

    print("PASS  no function is called with the buffer it copies into")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
