# NativeRead

Tolerance: none recorded.

`Engine/Nodes/IO/` holds the native Read and Write. `NativeRead` (`NativeRead.h`) is the
only plug-in registered under `PLUGINID_NATRON_READ`, at one version,
`PLUGIN_MAJOR_NATRON_READ`. There is no Read container and no OFX reader behind
it: `ReadNode` is gone, and `OfxHost::loadOFXPlugins()` skips any OFX plug-in
whose contexts include `kOfxImageEffectContextReader`, so none is registered.
Decoding lives in
`OiioReadSupport.*` (header cache, layer grouping, metadata), time mapping in
`ReadTimeDomain.*` and colour in `IoColorSpace.*`, with the colourspace knobs
(`ocioInputSpace` and the rest below) in `OcioSpaceKnobs.*`, which takes a
script-name prefix so the Write can share it.

- **Formats.** `OiioReadSupport::readableExtensions()` is OIIO's
  `extension_list` attribute minus the formats named in `kExcludedOiioFormats`
  (`raw`, `null`, `term`, `ffmpeg`, `psd`), lower case and sorted. There is no
  hand-written list, so a build with more OIIO plug-ins reads more. A path with
  any other extension gets an "Unsupported format" error from
  `NativeRead::rejectUnsupportedFormat()`. A camera raw extension
  (`RawExtensions::kExtensions`, `RawExtensions.h`) is checked before that
  list and never reaches OIIO; until the Read decodes raw it fails with a
  "Camera raw decoding is not available" error.
- **Output kind.** The Read declares `eDataKindPolymorphic` and resolves it
  from the `filename` extension (`NativeRead::resolveOutputDataKind()`):
  `eDataKindRaw` for a camera raw extension, `eDataKindImage` otherwise,
  including an empty filename. Only the extension counts, so the file need not
  exist. A `filename` change that flips the kind invalidates the node's cached
  kind, which re-checks every edge reachable from it; edges are kept, and a
  consumer that cannot take the new kind is put in error.
- **Entry points.** The file dialogs, drag and drop, `KnobGuiFile`, the CLI,
  and `PrecompNode` all ask
  `AppManager::getReaderPluginIDForFileType()` (extension to
  `PLUGINID_NATRON_READ`, or empty) and
  `AppManager::getSupportedReaderFileFormats()` (the extension list). Don't
  hard-code an extension set or a reader id elsewhere.
- **Knobs.** The script names are `GenericReader`'s, so existing scripts and
  the host hooks that look knobs up by name keep working: `filename`, `proxy`,
  `proxyThreshold`, `originalProxyScale`, `customProxyScale`,
  `originalFrameRange`, `firstFrame`, `lastFrame`, `before`, `after`,
  `onMissingFrame`, `frameMode`, `startingTime`, `timeOffset`,
  `timeDomainUserEdited`, `ocioInputSpace`,
  `ocioInputSpaceIndex`, `ocioInputSpaceSet`, the `Context` group of
  `key1`..`key4` and `value1`..`value4`, and the hidden `ocioConfigFile`,
  `ocioWorkingSpace` and `availableViews`. `reload` (`kNatronReadParamReload`)
  is a Reload button on the `filename` row.
- **Reload.** The node hash folds in a stamp of the file or sequence
  (`FileStampRegistry`: path, modification time, size and inode of every
  file). A file is stamped once, the first time the node is hashed in a
  session, and nothing watches or stats it again until Reload. The Reload
  button runs `purgeCaches()`: the file and proxy stamps are taken again, the
  frame listings, the cached headers of the files and proxy files, the output
  metadata and the action results are dropped, and the frame range is read
  from disk again. A file that changed gets a new stamp, so the node hash moves
  and the images and action results cached under the old hash are retired.
  Until then a file changed on disk is not seen: the hash, the produced
  layers, the region of definition and the cached images stay those of the
  file read before. An image that is not in the cache is decoded from the file
  as it is on disk now, though, so a Read whose output is not cached can show
  new pixels under the old hash. Every other node whose file knob covers the
  same files is reloaded with it
  (`KnobFile::reloadOtherNodesReadingTheseFiles()`). The file knob's own reload
  (`KnobFile::reloadFile()`) does the same.
- **Colour.** Pixels are converted to the project's working space through the
  project's OCIO config (`IoColorSpace::toWorkingProcessor()`). The default
  input space (`IoColorSpace::defaultInputSpace()`) is, in order: the config's
  file rules (`fileRuleColorSpace()`; a match on the default rule alone does not
  count), the file's `oiio:ColorSpace` tag if the config has a colourspace, role
  or alias of that name (`embeddedColorSpace()`), then the project's file
  default for the pixel depth (`categoryOf()`: float, 16-bit or 8-bit). Setting
  the input space by hand (`ocioInputSpaceSet`) overrides the default. The
  non-empty `key`/`value` pairs are set as OCIO context variables when the
  processor is built, and are part of its cache key.
- **Layers and views.** Every layer of the file becomes a produced plane
  (`getComponentsNeededAndProduced()`, grouped by `OiioReadSupport::fileLayers`)
  and the Read is multi-planar. A greyscale file (a lone `Y` or `I` channel)
  fills R, G and B from it. A multi-view EXR, as parts with a `view` attribute
  or channels prefixed per its `multiView` list, fills `availableViews` so the
  project gets those views; a project view the file lacks reads the file's
  default view.
- **Time.** A filename pattern resolves to one file per output frame, listed
  once per pattern (`FrameListing`). `ReadTimeDomain::resolve()` maps an output
  frame to a file, black or an error from `before`/`after` (hold, loop, bounce,
  black, error) and `onMissingFrame` (previous, next, nearest, error, black;
  searches up to `kMissingFrameSearchRange` frames each way). `frameMode` picks
  `startingTime` or `timeOffset` for the shift.
- **Proxies.** The `proxy` file is read instead of `filename` when the render
  scale is at or below `proxyThreshold`; it has its own frame listing.
- **Metadata.** `deriveOutputMetadata()` publishes
  `ofx/pixelaspect` and, for a frame that loads a file, the file path, source
  frame, mtime and size plus the file's own attributes
  (`OiioReadSupport::attributeMetadata()`, so a frame rate in the header appears under the
  format's prefix like any other attribute), for downstream native nodes and, via
  `OfxMetadataBridge`, OFX nodes. A black or error frame carries only the pixel aspect. A Read has no frame rate: it
  leaves the output frame rate at the host default and publishes no `ofx/framerate`.
- **Not read.** RAW (`raw` is excluded; a later milestone adds it) and layered
  documents such as PSD (`psd` is excluded; deferred to "M79 - Layered Document
  Readers").
