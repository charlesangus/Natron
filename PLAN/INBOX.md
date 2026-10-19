## Pending

### 2026-10-16T12:30:00Z — change-request
- refs: none
- New milestone requested (user): **Native Metadata Utilities** (E3, after M77 - Native Metadata Nodes). Native versions of the OFX example metadata nodes that M77 left without a native equivalent — `org.openfx.examples.metadataCompare`, `metadataContribute` and `metadataTimeCode` — then retire the OFX ones from the bundle and the asset fetch. Also replace the three OFX example plug-ins the test suite uses as metadata-bridge fixtures (Contribute, Print, TimeCode in `build/assets/TestPlugins/`) with an in-repo test-only OFX plug-in, so the bridge keeps coverage once they're gone. Deps: M77.
