# NativeWrite

Tolerance: none recorded.

`NativeWrite` (`NativeWrite.h`) mirrors the Read. It is the only plug-in
registered under `PLUGINID_NATRON_WRITE` (`Write`), at one
version, `PLUGIN_MAJOR_NATRON_WRITE`. There is no Write container and no OFX
writer behind it: `WriteNode` is gone, and `OfxHost::loadOFXPlugins()` skips any
OFX plug-in whose contexts include `kOfxImageEffectContextWriter`. Encoding
lives in `OiioWriteSupport.*`; the output colourspace knobs reuse
`OcioSpaceKnobs.*`.

- **Formats.** `OiioWriteSupport::writableExtensions()` is the extensions of
  OIIO's runtime `output_format_list` (every format `ImageOutput::create` can
  make, if OIIO lacks that attribute) minus `kExcludedOiioFormats`, lower case
  and sorted. There is no hand-written list and no video. A path with any other
  extension gets an "Unsupported format" error from
  `NativeWrite::rejectUnsupportedFormat()`.
- **Entry points.** The file dialogs and `KnobGuiFile` take their filters from
  `AppManager::getSupportedWriterFileFormats()`, and
  `AppManager::getWriterPluginIDForFileType()` maps an extension to
  `PLUGINID_NATRON_WRITE` (or empty). Everything that creates a Write (Render
  Selected, `app.createWriter`, the command-line render of an output file)
  names `PLUGINID_NATRON_WRITE` directly. Don't hard-code an extension set elsewhere.
- **Knobs.** The script names are WriteOIIO's, because tests, `tools/bench` and
  host code look them up by name: `filename`, `overwrite`, `formatType`,
  `clipToRoD`, `frameRange`, `firstFrame`, `lastFrame`, `frameIncr`,
  `bitDepth`, `compression`, `quality`, `dwaCompressionLevel`,
  `zipCompressionLevel`, `tileSize`, `partSplitting`, `viewsSelector`,
  `ocioOutputSpace`, `ocioOutputSpaceIndex`, `ocioOutputSpaceSet`, the `Context`
  group of `key1`..`key4` and `value1`..`value4`, the hidden `ocioConfigFile`
  and `ocioWorkingSpace`, `startRender`, `writeMetadata`, `readBack`,
  and `reload` (a Reload button on the `readBack` row). The compression, bit-depth and per-format knobs follow the
  file name through `OiioWriteSupport::paramVisibility()`.
- **Encoder.** `OiioWriteSupport::writeFile()` pulls rows from a `RowSource` one
  band (about `kDefaultBandBytes`) at a time and is the only pixel memory held:
  there is no full-frame copy, and sizes, strides and offsets are 64-bit, so the
  frame size is bounded by the file format. The file is written under a
  temporary name beside the target and renamed onto it on success; on failure
  or cancellation the temporary is removed and the old file is untouched.
- **Colour.** Rows go from the working space to the output space through the
  project's OCIO config, in the band callback. The default output space is
  from the config's file rules, then the project's file default for the pixel
  depth. The file is tagged with `oiio:ColorSpace`, set to the space's Color
  Interop ID when the config has one, else to the space name
  (`IoColorSpace::fileTag()`).
- **Metadata.** With `writeMetadata` on, the upstream metadata is written by
  `OiioWriteSupport::applyMetadata()`, the inverse of
  `OiioReadSupport::attributeMetadata()`. The routing rules are in the
  Metadata section above.
- **When the file is written.** Only a frame of the Write's own render to
  disk encodes: the panel's Render button, Render Selected, `app.render` and
  the `-w` command-line render all run through `startWritersRendering()` and
  the Write's `RenderEngine`, whose frames are sequential and rooted at the
  Write. `EffectInstance::isRenderingOwnSequence()` reads exactly that from
  the frame's `ParallelRenderArgs`. A Viewer, a preview or any node downstream
  that pulls the Write roots its frame elsewhere, so the Write returns its
  output without touching the disk. The image cache lookup skips a writer only
  on its own render to disk.
- **Output.** The output, on a pull and on a render to disk alike, is what the
  file of the frame holds, so the cache holds one image either way. One
  planner, `planFile()` in `NativeWrite.cpp`, decides the file's planes and
  channels from the channel set and the format's capabilities; the encoder
  writes its plan, and the output follows it in three places: the produced
  planes (`getComponentsNeededAndProduced()`), the colour layout
  (`getPreferredMetadataAt()` makes an RGBA input RGB when the file gets no
  alpha), and the pixels the render copies. So a partial R/G/B selection shows
  the whole RGB triple the file gets, a layer the file gets only some channels
  of is declared as a layer of those channels alone under its own name (what
  the native Read makes of the file; the render fetches the input's whole layer
  and copies the written channels), a layer or alpha the format cannot hold
  is absent, and no other layer passes through
  (`producesMetadataLayerImplicitly()` is false and
  `isPassThroughForNonRenderedLayers()` blocks the rest). Without a writable
  format, only the channel set restricts the output. A colour row selecting
  alpha alone keeps an RGBA plane whose RGB is zero.
- **Read back.** `readBack` is offered only while the file name is an EXR (the
  one format whose header can hold the gate below). A frame written to an EXR
  carries the header attribute `natron:outputHash`, the Write's output content
  hash (`getOutputContentHash()`: the node hash's content without `readBack`,
  taken when the frame's render action starts) as 16 hex digits, on every part
  of the file. With `readBack` on, a frame shows its file only when
  `readBackFrame()` finds the file, reads its header and finds the stored hash
  equal to the current output content hash; any other frame (no file, no
  attribute, another hash) passes the input through with no error. The output
  content hash covers the Write's own knobs, the upstream graph and the project
  render settings, so an edit anywhere upstream turns the file away and setting
  the edit back (or undoing it) brings the file back. Leaving `readBack` out of
  it lets a file written with read back off match once it is on. The node hash
  does include `readBack`, because the decoded file and the input differ in
  pixels and metadata and identical nodes share cache entries; turning it on
  or off therefore switches cache entries rather than purging them. The Render
  button is disabled and Reload enabled while it is on. A file shown is decoded by
  `OiioReadSupport::decode()` inside the Write (`renderReadBack()`), read in the
  output space it was written in, and the produced planes, format and RoD come
  from its header (`readBackLayers()`), so the channel set and a disconnected
  input no longer decide what downstream sees. The input is optional while
  `readBack` is on (`isInputOptional()`), and `outputFollowsLayerKnob()` is false
  for a frame shown from its file. A finished sequence calls `purgeCaches()`
  whether `readBack` is on or not, since what was cached while it was on no
  longer matches the files.
- **Reload.** `readBackFrame()` looks at each file once, keeping its header
  and stored hash, and does not stat it again while frames render. The Reload
  button on the `readBack` row is enabled only while `readBack` is on. It runs
  `purgeCaches()`, as does a finished sequence:
  the kept files, the cached headers of the files of the frame range, the
  output metadata, the action results and the cached images of this node and
  of every node downstream are dropped, so a file changed on disk is read
  again.
