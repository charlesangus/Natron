# Drop the render-path NaN check app-wide

The user decided on 2026-10-14 that Natron no longer scans rendered images for NaNs or replaces them. Users neither expect nor need the app to do this.

`Image::checkForNaNsAndFix` / `checkForNaNsNoLock`, their call in `EffectInstance::renderHandler`, and the NaN-handling preference all go. This removes a single-threaded full-frame pass that ran under the output image's write lock: 35% of busy samples in the Grade profile in M90 - Render Path Serial Overhead's P1.T1. NaNs now flow through like any other value. Any node that needs NaN-safe maths handles it in its own kernel.
