# Publish the whole decision backlog, not just M2's

`publish_decisions: docs/decisions/` was set when the plan moved to the orphan
`plan` branch during M2, but M2 shipped before anything was published — and the
milestones before it ran under the in-repo layout, where publication does not
apply because the plan was already tracked in the repo. The migration therefore
left the project with *no* decision trail visible from the code at all.

So the first publication copies every project-wide decision from M0 onward, not
only the ones M2 touched, and it rides M10's branch rather than a PR of its own:
M2's gate has already passed, and a doc-only PR would cost a full container CI
run to land text. Later milestones publish their own decisions at their own
gate, as PLAN-FORMAT.md §3a describes.

The published copies are a mirror; the plan-branch files stay canonical and
`INDEX.md` keeps pointing at them. A short `docs/decisions/README.md` says so,
so nobody edits the copy expecting it to take effect.
