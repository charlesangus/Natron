# Debug float traps are a test-input contract

2026-10-27 — user, during M240 - Float Trap And Denormal Audit.

Debug builds keep trapping float overflow, invalid operations and divide-by-zero on render threads. Node code is not made robust against non-finite or extreme *pixel* data in debug builds, and no pixel loop runs with traps held. Debug builds only process inputs we control, so tests and fixtures keep pixel data finite and in range. A debug render that traps on a test means the test fed it bad data, unless the trap is in our own maths on sane data.

Knob and parameter setup still narrows and divides without trapping: `knobValueAsFloat`, guarded reciprocals, and bounded sizes. That is cheap hygiene, done once per render, with no trap holds. Release builds and GPU kernels are unaffected: there, non-finite values flow through as IEEE results.

Why: holding traps in pixel loops hides real bugs. Making every loop safe for arbitrary inf/NaN input either costs a per-pixel check or changes results, so debug, release and GPU would disagree. Neither is worth it for builds that never see production images.
