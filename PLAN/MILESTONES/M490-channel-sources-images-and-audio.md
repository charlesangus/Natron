# M490 - Channel Sources From Images And Audio

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Two more channel sources:
- **Image Sample:** samples an image input per frame into channels: a pixel, or a region's mean, min, max or luminance, per layer channel. Its cook renders the input over the network range through the normal render path, cancellably, with progress.
- **Audio File:** reads WAV and FLAC into channels at the audio rate, with Resample to the frame rate. The curve editor draws the audio waveform under its channels, and playback scrubbing stays silent.

Blocked on: M486 - Channel Network Core.

Acceptance sketch:
- Image Sample of a flickering plate drives a Grade to cancel the flicker.
- An Audio File channel resampled to 24 fps matches a reference envelope.
- Cancelling a long Image Sample cook leaves the previous cache in place.

## Scope

**Edge cases:**
- An Image Sample whose input depends on a knob the same network drives: a cycle, refused.

**Assumptions:**
- Unconfirmed: audio decode uses the bundled FFmpeg (M75 - Native Read's), not a new library.
