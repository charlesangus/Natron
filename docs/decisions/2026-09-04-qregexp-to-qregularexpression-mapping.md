# Qt 6 / QRegExp → QRegularExpression migration (retrospective)

2026-09-04. This project has completed its Qt 6 migration and removed `QRegExp`
entirely. This decision record documents the mapping rule that guided that
migration, for future reference — no QRegExp usage remains in the codebase.

## Migration status

No `QRegExp` usage remains in `Engine` or `Gui`. All call sites now use
`QRegularExpression` exclusively, verified as of this date.

## Surviving QRegularExpression sites

The following files use `QRegularExpression` or
`QRegularExpression::wildcardToRegularExpression()`:

- `Engine/FileSystemModel.cpp`
- `Gui/NodeCreationDialog.cpp`
- `Gui/PreferencesPanel.cpp`
- `Gui/NodeGraph45.cpp`
- `Gui/RenderStatsDialog.cpp`

## Mapping rule (historical)

This rule is no longer needed for porting — all old call sites have been
migrated — but is preserved here to document the semantics of the migration for
future maintainers.

**Matching mode conversion:** `QRegExp` had two matching modes: `indexIn()`
found a pattern *anywhere* in the string, while `exactMatch()` required the
*whole* string to match. `QRegularExpression::match()` is find-anywhere and has
**no** `exactMatch` equivalent. Therefore:

- Old `indexIn()` sites → `QRegularExpression::match()` directly
- Old `exactMatch()` sites → anchor the pattern using `\A…\z` or
  `QRegularExpression::anchoredPattern()`

**Wildcard conversion:** `QRegExp::WildcardUnix` maps to
`QRegularExpression::wildcardToRegularExpression(pattern)`. This conversion
required careful attention because case sensitivity and anchoring behavior
differ from the old wildcard mode and must be evaluated per call site.
