# M225 - Clean Shutdown And Memory Release

High priority (user, 2026-10-26). Closing Natron doesn't always end it: processes stay running after the window closes, and even after the process is gone its RAM may not be released.

Investigate before fixing, with evidence on main:
- What survives a close: the main process (threads not joined, a modal or event loop still running, a render or cache-writer thread blocking exit), child processes (separate-process renders, NatronRenderer workers, Python helpers, crash reporter), and their parent/child relationships.
- What holds memory after exit: shared memory (`/dev/shm`, memfd, SysV segments), mmapped cache files (disk/viewer cache), tmpfs files, GPU allocations held by a lingering process, zombies, and children reparented to init.
- Paths: File > Quit, the window close button, closing the last project window, SIGTERM/SIGINT, closing with renders running, closing with a separate-process render or farm render running, closing with a modal dialog open, and AppImage launches (the mount helper process).

Acceptance sketch:
- After every close path, no Natron process (main, children, AppImage mount helper) remains within a bounded time (a few seconds; renders are cancelled or, where the user chose to keep a background render, that is explicit and visible).
- No shared-memory segments, cache mmaps or temp files owned by the closed instance remain, apart from the persistent caches that are meant to stay on disk.
- An automated test launches Natron under Xvfb, exercises each close path (with renders running where relevant), and asserts no leftover processes or shared-memory segments.
