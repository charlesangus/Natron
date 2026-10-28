# ContributeMetadata

Tolerance: none recorded.

- `ContributeMetadata`: publishes six demonstration keys, one per metadata type,
  under `fr.natron.ContributeMetadata.`: `note` (string, the Note knob),
  `revision` (int 1), `quality` (double 0.75), `tags` (string
  `reviewed, approved`), `renderRegion` (int vector 0, 0, 1280, 720) and
  `weights` (double vector 1, 0.5, 0.25), plus `ofx/framerate` = 30. Mode keeps
  all of Source's metadata, all of it but the Drop Key key (a key Source does
  not hold is a no-op), or none of it; the node's own keys always win. The image
  passes through unchanged.
- Port of the OpenFX example `org.openfx.examples.metadataContribute`, with two
  forced deviations: the keys use the node's own ID as prefix instead of
  `org.openfx.examples.metadataContribute.`, and `tags`, a two-element string
  vector in the example, is one comma-joined string, because `ImageMetadata`
  has no string vector type.
