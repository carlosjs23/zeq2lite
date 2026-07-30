#!/usr/bin/env python3
"""Gate: a Q_strncpyz bound must not be sizeof() a *different field of the same
object* than the destination.

    Q_strncpyz( buf.a, src, sizeof(buf.b) )      <-- offender when a != b

This is the exact shape of the cg_weapGfxBuffer.weaponName overflow: the bound
came from a sibling field 24 bytes larger than the destination, and because
Q_strncpyz uses strncpy (which pads to n bytes) the write ran off the end of the
object on every call, whatever the input length.

Restricting the rule to "same object, different field" is what makes this
precise. A broader "destination and sizeof name different things" rule is not
usable: legitimate code passes the size of a separate buffer or of a type, and a
name-based scan cannot compare actual sizes, so it produces mostly noise. Same
object plus different field, by contrast, is nearly always a copy-paste slip -
and it is a bug even when the two fields happen to be the same size today,
because it silently becomes an overflow the moment one of them is resized.

usage:
    check_strncpyz_field_sizes.py [paths ...]     (default: Game Shared Engine)

exit 0 = clean, 1 = offenders found.
"""
import os
import re
import sys

CALL = re.compile(r'\bQ_strncpyz\s*\(')
# buf.field / buf->field / buf[i].field, capturing object and final field.
MEMBER = re.compile(r'^([A-Za-z_]\w*(?:\s*\[[^\]]*\])?)\s*(?:\.|->)\s*([A-Za-z_]\w*)$')
SIZEOF = re.compile(r'^sizeof\s*\(\s*(.+?)\s*\)$', re.S)


def split_args(text, start):
    """Split the argument list of a call whose '(' is at text[start].
    Returns (args, end_index) or (None, None) if unbalanced."""
    depth = 0
    args = []
    cur = []
    i = start
    while i < len(text):
        c = text[i]
        if c in '"\'':
            quote = c
            cur.append(c)
            i += 1
            while i < len(text):
                cur.append(text[i])
                if text[i] == '\\':
                    i += 2
                    if i - 1 < len(text):
                        cur.append(text[i - 1])
                    continue
                if text[i] == quote:
                    break
                i += 1
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
                args.append(''.join(cur))
                return [a.strip() for a in args], i
        elif c == ',' and depth == 1:
            args.append(''.join(cur))
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    return None, None


def scan(path):
    try:
        with open(path, errors='ignore') as fh:
            src = fh.read()
    except OSError:
        return []
    out = []
    for m in CALL.finditer(src):
        args, end = split_args(src, m.end() - 1)
        if not args or len(args) != 3:
            continue
        dest, _src_arg, bound = args
        so = SIZEOF.match(bound)
        if not so:
            continue
        d = MEMBER.match(re.sub(r'\s+', '', dest))
        b = MEMBER.match(re.sub(r'\s+', '', so.group(1)))
        if not d or not b:
            continue
        d_obj, d_field = d.group(1), d.group(2)
        b_obj, b_field = b.group(1), b.group(2)
        # Compare object names ignoring any subscript, so buf[i] and buf match.
        if re.sub(r'\[.*\]$', '', d_obj) != re.sub(r'\[.*\]$', '', b_obj):
            continue
        if d_field == b_field:
            continue
        out.append((src[:m.start()].count('\n') + 1, d_obj, d_field, b_field))
    return out


def main(argv):
    roots = argv[1:] or ['Game', 'Shared', 'Engine']
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, _dirs, names in os.walk(root):
            for n in names:
                if n.endswith(('.c', '.h')):
                    files.append(os.path.join(dirpath, n))

    offenders = []
    for f in sorted(files):
        for line, obj, dfield, bfield in scan(f):
            offenders.append((f, line, obj, dfield, bfield))

    if not offenders:
        print("PASS  no Q_strncpyz bound takes sizeof() a sibling field")
        return 0

    print("FAIL  %d Q_strncpyz call(s) size the copy from the wrong field:\n"
          % len(offenders))
    for f, line, obj, dfield, bfield in offenders:
        print("  %s:%d" % (f, line))
        print("      Q_strncpyz( %s.%s, ..., sizeof(%s.%s) )" % (obj, dfield, obj, bfield))
        print("      bound should be sizeof(%s.%s)" % (obj, dfield))
    print("\nQ_strncpyz uses strncpy, which pads to n bytes, so an oversized")
    print("bound overflows the destination on every call regardless of input.")
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
