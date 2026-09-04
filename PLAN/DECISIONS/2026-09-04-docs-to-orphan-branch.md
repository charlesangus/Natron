# Move Documentation/ to an orphan docs branch

2026-09-04. The inherited `Documentation/` tree — ~70 pages of upstream 2.4-era
Sphinx guide, `.readthedocs.yaml`, and `README-Natron-documentation.md` — moves
to an orphan `docs` branch, out of the main development tree. Fixing and
updating the content is backlogged and paused; the branch preserves the material
for future work without it cluttering the working tree or misleading
contributors into thinking it is current.

This resolves the open question "Does this fork publish user documentation at
all?" by deferring the publish decision while removing the stale content from
the code branches. M14 is rescoped accordingly: move the tree to the orphan
branch rather than delete it or fix it in place.
