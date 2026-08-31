# 2026-08-31 — Keep the plan in-repo until M2 lands, then migrate to `.plan/`

The canonical layout keeps the plan on an orphan `plan` branch checked out at
`.plan/`, so planning churn stays out of every code branch and PR diff. This
project still uses the in-repo layout: `PLAN.md` and `PLAN/` are tracked on the
default branch, so plan commits ride the milestone branches alongside code.

**Decision: do not migrate now; migrate once M2 has merged.** M2's branch was
just rebased and already interleaves 54 plan and code commits. Moving the plan
mid-milestone would mean reconciling plan history twice — once for the rebase
that just happened, once for the migration — for no benefit that cannot wait.
The in-repo layout is fully supported, not a defect to be urgently corrected.

**Follow-up action, to run immediately after the migration:**
`FREETYPE_HARFBUZZ_FINDINGS.md` sits untracked at the repo root. It is the
investigation write-up behind `M2.P1.T2i` (the `NatronRenderer` FreeType/
HarfBuzz link failure) and M2's milestone file references it by name. It is
deliberately **not** committed to the code repo — a one-off forensic document
is not a source artifact. Once the plan lives at `.plan/`, fold its substance
into M2's milestone file at its new location, as part of the `M2.P1.T2i`
story, and drop the standalone file. Until then it stays untracked in the
working tree; note that `git clean -xdf` at the repo root would delete it.
