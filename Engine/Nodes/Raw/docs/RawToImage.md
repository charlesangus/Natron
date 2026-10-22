# RawToImage

Tolerance: none recorded.

The explicit converter from raw data to an image, with no processing. It is the way to use raw
values in an image graph: an image input never takes raw directly, and only the Viewer shows raw
without it.

- Raw in, image out, a Custom op with no knobs beyond the framework's. Any `raw/stage` is
  accepted.
- The conversion is `RawDisplay` (`RawDisplay.h`), the one the Viewer shows raw input through:
  - a colour filter array mosaic becomes a grey image, red, green and blue all the photosite's
    value;
  - linear raw keeps its channels (three as red, green and blue, one as grey);
  - alpha is 1.
- No demosaic, scaling or colour: the values stay in the raw data's units, so an unprepared
  mosaic is in sensor counts. Use RawPrepare, RawDemosaic and RawInputColor to develop it.
- The region of definition is the source's. Renders at full resolution only, since a mosaic
  resampled to a coarser level mixes the colours of its photosites.
- The metadata loses `raw/cfa_pattern`, `raw/cfa_origin` and `raw/stage`, which describe a mosaic
  and its place in the develop chain. The levels, white balance and camera matrix stay, since they
  still describe the values.

Tests: `Tests/Native/NativeRawToImage_Test.cpp`, on a mosaic and a linear fixture.
