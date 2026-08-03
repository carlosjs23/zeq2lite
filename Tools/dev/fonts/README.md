# Bundled display faces

`make_training_font.py` bakes the training UI's glyph atlases from a TrueType
face at build time. It used to land on the macOS system fonts — Impact and
Avenir Next Condensed — which are proprietary, so every build printed a licence
caveat saying the atlases could not ship. These two faces are checked in so a
clean checkout bakes shippable atlases with nothing installed.

Both are SIL Open Font License 1.1, which permits redistribution (including
bundled with software) provided the licence travels with the font. That is what
the `*-OFL.txt` files beside them are for — do not remove them.

| file | role | upstream |
| --- | --- | --- |
| `Anton-Regular.ttf` | display — headers, state words, objectives | [google/fonts `ofl/anton`](https://github.com/google/fonts/tree/main/ofl/anton) |
| `BarlowSemiCondensed-Medium.ttf` | body — sentences, labels, tags | [google/fonts `ofl/barlowsemicondensed`](https://github.com/google/fonts/tree/main/ofl/barlowsemicondensed) |

Anton is the closest free face to the Impact look the approved deck was drawn
against; Barlow Semi Condensed keeps the body copy narrow enough that a full
objective line still fits the tracker at 640x480.

The generator searches this directory ahead of `~/Library/Fonts` and the system
fonts, so dropping a face into `~/Library/Fonts` no longer silently changes the
build. To try a different face, pass it explicitly:

    make_training_font.py <out> --display /path/to/Oswald-Bold.ttf

Note the atlases themselves are build products under `Build/` — only these
sources are tracked.
