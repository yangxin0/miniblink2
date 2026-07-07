# Glyph — notes for Claude

## Build parallelism

Never let a build take the whole machine: cap ninja at **60% of the CPU
cores**. On this 12-core machine that means `MB_JOBS=7` for
`scripts/build-lib.sh` (the script honors it) and `-j 7` when invoking
ninja directly. This applies to every compile, including quick incremental
ones — an unthrottled Chromium build makes the machine unusable.

## Commit messages

Write commits the way a human maintainer would: a tight summary line, plus a
short body explaining the *why* when it helps. Do **not** add any
AI-attribution trailer — no `Co-Authored-By: Claude …` line and no
"Generated with Claude Code" footer. Keep messages free of assistant/tool
markers.
