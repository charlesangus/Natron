# TimeCodeMetadata

Tolerance: none recorded.

- `TimeCodeMetadata`: sets `ofx/timecode` to a non-drop-frame `HH:MM:SS:FF`
  counted on from Start Timecode, one frame per timeline frame, and
  `ofx/framerate` to the rate it counted at. Start Timecode lands on frame 1,
  or on Start Frame when Use Start Frame is on; the offset of frame `t` is
  `floor(t - origin + 0.5)`. The count wraps into one day both ways: frames
  before the origin wrap back through 00:00:00:00, and a count past 23:59:59:FF
  wraps to 00:00:00:00. The frames field counts at Rate rounded to the nearest
  whole number (at least 1), or at the `ofx/framerate` of Source when Rate From
  Metadata is on and Source has one. A start timecode with fewer than four
  numbers reads as 00:00:00:00. Every other key of Source passes through, and
  so does the image.
- Port of the OpenFX example `org.openfx.examples.metadataTimeCode`, same
  parameters, defaults and arithmetic.
