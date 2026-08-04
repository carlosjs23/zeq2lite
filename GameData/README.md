# Patched mod files

GLSL programs (`glsl/`), shader scripts (`scripts/`), tier config (`players/`)
and skill definitions (`skills/`, `players/*/default.phys`) that this source
tree has modified, mirrored at their mod-relative paths.

`Tools/dev/zeq2build.sh` copies them into `Build/<config>/ZEQ2/` on every build,
including `--stage-only`.

Skill data is loose in the mod directory - no `.phys` file lives inside
`zeq2_base.pk3` - so a file staged from here replaces the shipped one outright
rather than shadowing a pk3 entry.

These files are CRLF. Edit them in binary mode, or the whole file reads as
changed against the stock assets.

Reinstalling the mod directory overwrites them; run the build again to restore.

Edit the copy here, not the one under `Build/`. The next build overwrites the
installed copy.

Not tracked: `zeq2config.cfg`, `default.cfg`, `*.backup-*`.
