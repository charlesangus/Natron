# Niik-l defocus: port the lens tracing, measure before touching the engine

Decided 2026-11-01 (user, /cat-plan). Niik-l's fork added LensDefocus, DeepDefocus and a scatter engine (DefocusCore), with real-lens ray tracing (LensTrace). The user judged their node "a bit rough around the edges" and does not want it ported as is: "The most interesting part is the lens tracing work."

The work is shaped as follows:
- LensTrace is ported into our own BokehKernel, ZDefocus and Defocus: M292 - Traced Lens Bokeh.
- A reference harness scores our ZDefocus against a test-only port of Niik-l's engine: M291 - Defocus Reference Harness. The harness also tests whether P/N passes help; the user suspects they rarely do.
- ZDefocus gains debug outputs: M293 - ZDefocus Debug Outputs.
- The engine revisit is planned from the harness results and stays fully GPU-resident: M294 - ZDefocus Engine Revisit.
- A separate DeepDefocus follows the revisit: M295 - Native DeepDefocus.

The following are not wanted: no LensDefocus node, no tilt or World Point focus, no camera focus link, no extra depth conventions, no CleanBg.
