# Niik-l's DefocusCore, as a test candidate

`DefocusCore.h`, `DefocusCore.cpp`, `DefocusGpu.h` and `LensTrace.h` are copied unchanged from
Niik-l's Natron fork (<https://github.com/Niik-l/Natron>, branch `RB-2.6`, commit
`1449b4eecda2b7ee2a651c97256dab156c8a820f`, directory `Engine/Dev/PostFX/`). They keep their
licence header (GPL-2.0-or-later, the Natron developers).

Only the headers of DefocusGpu and LensTrace come along: `DefocusCore.cpp` includes them for
their types. `DefocusStubs.cpp` gives the two functions and the one member it calls from their
sources a body that links but is never used, since the harness never turns on the GPU scatter or
a traced lens.

The defocus reference harness drives the engine through `NiiklCandidate.h`. It is test code and
does not ship.
