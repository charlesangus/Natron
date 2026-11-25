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
(`colorspace` and the rest below) in `OcioSpaceKnobs.*`, which takes a
script-name prefix so the Write can share it.

- **Formats.** `OiioReadSupport::readableExtensions()` is OIIO's
  `extension_list` attribute minus the formats named in `kExcludedOiioFormats`
  (`raw`, `null`, `term`, `ffmpeg`), lower case and sorted. There is no
  hand-written list, so a build with more OIIO plug-ins reads more. A path with
  any other extension gets an "Unsupported format" error from
  `NativeRead::rejectUnsupportedFormat()`. A camera raw extension
  (`RawExtensions::kExtensions`, `RawExtensions.h`) is claimed before that
  list: it is left out of `readableExtensions()` even when a kept OIIO format
  lists it, and the file is decoded by rawspeed, never by OIIO.
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
- **Knobs.** Scripts, tests and the host hooks (DopeSheet included) look knobs
  up by name: `filename`, `proxy`,
  `proxyThreshold`, `originalProxyScale`, `customProxyScale`,
  `originalFrameRange`, `firstFrame`, `lastFrame`, `beforeFirstFrame`,
  `afterLastFrame`, `onMissingFrame`, `frameOffsetMode` (`startAt` or
  `offset`), `startAt`, `frameOffset`, `timeDomainUserEdited`, `colorspace`,
  the `ocioContext` group of `contextKey1`..`contextKey4` and
  `contextValue1`..`contextValue4`, and the hidden `ocioConfigFile`,
  `ocioWorkingSpace` and `availableViews`. `reload` (`kNatronReadParamReload`)
  is a Reload button on the `filename` row, and reloads `proxy` as well. It is
  the panel's only reload: `KnobFile` explains why a file knob with a Reload
  button shows no reload icon.
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
  (`KnobFile::reloadOtherNodesReadingTheseFiles()`). The reload icon of a file
  knob on another node (`KnobFile::reloadFile()`) does the same.
- **Colour.** Pixels are converted to the project's working space through the
  project's OCIO config (`IoColorSpace::toWorkingProcessor()`). The default
  input space (`IoColorSpace::defaultInputSpace()`) is, in order: the config's
  file rules (`fileRuleColorSpace()`; a match on the default rule alone does not
  count), the file's `oiio:ColorSpace` tag if the config has a colourspace, role
  or alias of that name (`embeddedColorSpace()`), then the project's file
  default for the pixel depth (`categoryOf()`: float, 16-bit or 8-bit). The
  `colorspace` menu's first entry, `projectDefault`, labelled with the space it
  currently stands for ("Project default (sRGB - Texture)"), follows that chain live: it re-resolves when the file, the config or the project's
  file defaults change (`Project::fileColorSpacesChanged`), and the resolved
  space is part of the node's hash. Any other entry overrides it. The
  non-empty `contextKey`/`contextValue` pairs are set as OCIO context variables when the
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
  frame to a file, black or an error from `beforeFirstFrame`/`afterLastFrame` (hold, loop, bounce,
  black, error) and `onMissingFrame` (previous, next, nearest, error, black;
  searches up to `kMissingFrameSearchRange` frames each way). `frameOffsetMode` picks
  `startAt` or `frameOffset` for the shift.
- **Proxies.** The `proxy` file is read instead of `filename` when the render
  scale is at or below `proxyThreshold`; it has its own frame listing.
- **Metadata.** `deriveOutputMetadata()` publishes
  `natron/pixelaspect` and, for a frame that loads a file, the file path, source
  frame, mtime and size plus the file's own attributes
  (`OiioReadSupport::attributeMetadata()`, so a frame rate in the header appears under the
  format's prefix like any other attribute), for downstream native nodes and, via
  `OfxMetadataBridge`, OFX nodes. A black or error frame carries only the pixel aspect. A Read has no frame rate: it
  leaves the output frame rate at the host default and publishes no `natron/framerate`.
- **Camera raw.** A raw file is decoded by rawspeed through
  `RawDecodeSupport.*` and output as `eDataKindRaw`: the undemosaiced sensor
  data, one channel for a colour filter array mosaic, or the file's 3 or 1
  channels for linear data such as a linear DNG, in the file's units (no black
  subtraction, scaling or colour). The region of definition is the active area
  rawspeed crops to, in Natron's coordinates (y up), and the format is the
  whole sensor. `deriveOutputMetadata()` adds the `raw/` keys
  (`RawMetadata.h`) with `raw/stage` = `raw`. rawspeed only decodes whole
  files, so the first look at a file decodes it; the decoded image is kept in a
  cache bounded to a quarter of the project's RAM cache, shared by every Read,
  and the header outlives it. The colourspace knobs are hidden and no
  conversion is made. The proxy file is disabled, because downscaling a mosaic
  mixes the colours of its photosites. A camera the linked rawspeed rejects or
  does not support gives a persistent error naming the make, model and
  rawspeed version. Frame sequences and the time domain work as for images.
- **Layered documents.** `Engine/Nodes/IO/LayeredDocs/` registers four
  in-tree OIIO `ImageInput`s, ahead of `readableExtensions()`, so they win over
  any OIIO plug-in on the same extension: OpenRaster (`ora`), Photoshop (`psd`,
  `psb`, `pdd`; a PDD is read as a PSD), Krita (`kra`) and GIMP (`xcf`). Each
  reader builds a `LayeredDocument` (canvas, composite, layers) that
  `LayeredImageInput` exposes as subimages: subimage 0 is the composite and
  subimage N the Nth layer, bottom to top. The full window of every subimage
  is the canvas. RGB and Grayscale documents only, at each format's stored
  depth: PSD 8, 16 and 32 bit; Krita 8 and 16 bit integer, half and float;
  OpenRaster PNG 8 and 16 bit; XCF 8, 16 and 32 bit integer, half and float. A
  Grayscale document fills R, G and B as any greyscale file does.
  - **Layer model.** The composite is the colour plane. The stored composite is
    used when the file has one (PSD merged image, Krita `mergedimage.png`,
    OpenRaster `mergedimage.png`). When it is missing or empty (a PSD saved
    without "maximize compatibility", an OpenRaster file without a merged
    image) and always for XCF, which stores none, the composite is flattened
    from the layers: visible layers, in order, with Normal "over" at the layer
    and group opacity, whatever the blend mode (other blend modes are not
    re-composited; they are listed in the metadata). Every layer that has
    pixels becomes a Natron layer, so a layer's pixels appear in the layer
    menu next to the colour plane. A layer is a picture in the composite's
    encoding, so its colour is converted from the `colorspace` input space to
    the working space like the composite's, alpha untouched; each layer
    subimage carries the `natron:colourLayer` attribute that tells
    `OiioReadSupport::fileLayers()` so (`FileLayer::colour`). The layers of
    other formats, such as EXR depth or motion, are data and are read as
    stored. A layer mask (PSD user mask, XCF layer
    mask, Krita transparency mask) is folded into the layer's alpha. A layer
    outside the canvas is cropped to it. A group is not a layer of its own: its
    path becomes part of each member's name.
  - **Layer names.** The name is the group path and the layer name joined by
    `_`, with every character outside `A-Z a-z 0-9 _` (including spaces,
    dots and non-ASCII) replaced by `_`, leading and trailing `_` trimmed
    (`layer` if nothing is left). A name that is already taken, or that
    `LayerRegistry` or `OiioReadSupport` reserve (`rgba`, `rgb`, `alpha`,
    `depth` and the like, compared without case), gets `_2`, `_3` and so on.
    This name is the `<id>` in the metadata keys.
  - **Metadata.** Every key is under `layered/` and is published through
    `OiioReadSupport::attributeMetadata()` like any file attribute:
    `layered/layers` (the layer ids, bottom to top), `layered/skipped` (see
    below), `layered/composite` (`stored` or `flattened`), and per layer
    `layered/<id>/name` (the original name), `layered/<id>/blendMode` (the
    format's own mode name: a PSD four-character key such as `norm`, an
    OpenRaster `svg:` or `krita:` composite-op, a Krita `compositeop`, an XCF
    mode name; `normal` or `svg:src-over` when the file gives none),
    `layered/<id>/opacity` (0 to 1), `layered/<id>/visible` (0 or 1),
    `layered/<id>/clipping` (0 or 1: PSD clipping, Krita inherit-alpha) and
    `layered/<id>/group` (the enclosing group names joined by `/`, empty at
    the top level).
  - **Skipped, and listed.** Layers that are not raster pixels are not read as
    layers. Each is an entry of `layered/skipped`, `<group path>/<name> (<why>)`:
    an empty layer, a layer outside the canvas, PSD text, adjustment, fill and
    vector layers, Krita filter, fill, clone, vector, file and reference
    layers and its filter, transform, selection and colorize masks, a hidden
    transparency mask or one on a group or without pixels, an animated Krita
    layer, an OpenRaster element that is not a layer or stack, an XCF floating
    selection, a disabled layer mask or a layer mask on a group. An empty
    layer or group divider keeps its place in the group structure.
  - **Refused.** The file fails to open, with an error naming the cause, for
    any colour mode other than RGB and Grayscale (CMYK, Lab, indexed, duotone,
    multichannel; in XCF a layer in Indexed mode too), an unsupported depth, a
    PSD or PSB canvas side over 300 000 px, a truncated or malformed file, a
    Krita tile format other than version 2 or tiles not compressed with LZF, an
    XCF written by CinePaint, or by a development GIMP at more than 8 bits
    (byte order unknown), or an unknown tile compression. A document with an
    empty canvas is refused too.
  - **Decode per window.** Headers parse without reading pixels, so a PSB of up
    to 300 000 px opens at once. A region of a layer or of the composite
    decodes only the rows or tiles it covers (PSD and PSB through the RLE row
    offsets or raw row strides, Krita and XCF per tile); a ZIP-compressed PSD
    channel decodes whole, once, and is cached. An OpenRaster PNG cannot be
    windowed and decodes whole, once per plane.
- **Not read.** Writing layered documents; re-compositing blend modes. OIIO's
  `raw` plug-in is excluded: camera raw files go to rawspeed.
