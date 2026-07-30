# Patched mod files

GLSL programs (`glsl/`), shader scripts (`scripts/`) and tier config
(`players/`) that this source tree has modified, mirrored at their mod-relative
paths.

`Tools/dev/zeq2build.sh` copies them into `Build/<config>/ZEQ2/` on every build,
including `--stage-only`.

Reinstalling the mod directory overwrites them; run the build again to restore.

Edit the copy here, not the one under `Build/`. The next build overwrites the
installed copy.

Not tracked: `zeq2config.cfg`, `default.cfg`, `*.backup-*`.
