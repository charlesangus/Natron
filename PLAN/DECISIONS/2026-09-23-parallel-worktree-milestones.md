# Independent milestones may run in parallel worktrees

2026-09-23, user direction. M28 (page-cache reading) and M30 (automated beta releases) run alongside M34 in git worktrees at `build/wt/m28` and `build/wt/m30`. They live under `build/` because the natron-dev container mounts only the main checkout, and `build/` is git-ignored. They share the container's ccache.

This relaxes "at most one `doing`" for milestones that share no code with the one in flight. Each branches off `main` and opens its PR against `main`; the stacked-PR rule applies only to the M34 chain. Builds still serialize: one build in the container at a time, from whichever tree. A worktree build uses its own `build/wt/<m>/build/<type>` directory.

Chosen over running M29 (repo recreation would disrupt the open stacked PRs) and M27/M25/M22 (full rebuilds or engine overlap with M34).
