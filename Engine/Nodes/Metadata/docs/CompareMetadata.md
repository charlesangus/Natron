# CompareMetadata

Tolerance: none recorded.

- `CompareMetadata`: lists, in its read-only Differences box, the keys on which
  the metadata of A and B disagree at the current frame, one line per key in
  ascending key order: `A only: <key>=<value>`, `B only: <key>=<value>` or
  `differs: <key>: A=<value> B=<value>`. Keys both hold with the same value are
  not listed. Values are compared and shown as text, vectors comma-separated.
  With B disconnected every key of A reads as A only. The image and metadata of
  A pass through unchanged. Port of the OpenFX example
  `org.openfx.examples.metadataCompare`, whose Source and Mask inputs are A and B
  here.
