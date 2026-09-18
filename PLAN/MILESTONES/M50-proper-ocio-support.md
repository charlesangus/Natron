# Milestone 50: Proper OCIO support as a project property

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Make OCIO configuration a first-class property of the project, and have the
Viewer use OCIO for display transforms, rather than today's ad hoc
handling.

Blocked on: not yet prioritized — captured as backlog (user request
2026-09-18); needs a scoping pass on current OCIO usage (M3 already made
ACES 2.0 Studio the default OCIO config) before elaboration.

Acceptance sketch:
- The project has an OCIO config property, persisted with the project.
- The Viewer's display transform is driven by that OCIO config.
