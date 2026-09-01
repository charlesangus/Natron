# The format gate checks changed lines, not whole files

M10's `format` job runs `clang-format --dry-run --Werror` over every file a
change touches, picked from the merge-base diff. That was chosen so the gate
wouldn't be an unbounded wall of red over a codebase that predates its
`.clang-format` by a decade — but the unit is the *file*, not the line, so
touching one function in a legacy file still fails on the other thousand.

M3 is the first PR to actually exercise it: the gate landed in M10 (#7), after
the last C++ PR (M2, #6), and `checks.yml` only triggers `format` on push/PR to
`main`. Measured at HEAD, `Engine/Settings.cpp` alone produces **~1610**
violations; the ACES default change adds ~20, all in the same house style
(`foo( bar )`), which the WebKit-based config systematically rejects.

**The gate moves to changed lines** (`git clang-format --diff` against the
merge base). New and edited lines are held to the style; the untouched body of a
legacy file is not. The alternatives were all worse: a wholesale reformat buries
the actual change in ~1600 lines of unrelated diff and rewrites the file's
blame; tuning `.clang-format` until it reproduces a decades-old hand style is an
open-ended chase that may still leave files red; and an exemption list goes
stale and leaves touched legacy code unchecked forever.

This preserves M10's stated intent — hold a change to the style, not the
codebase's history — at a finer grain than M10 implemented it.
