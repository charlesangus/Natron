# M629 - Stable USDRead Placeholder Digest

Since F4 - Heavy Scene Viewer, a USDRead's file digest is a random per-process value until its first scene walk (`Engine/Nodes/Scene/SceneFileSource.cpp:72-81` `makeUnwalkedDigest`, deliberate per the comment there; also :246, :341). `USDRead::appendNodeContentHash` puts that digest in the node hash. A cache keyed on a downstream hash taken before the input scene is evaluated stores under a key that is never looked up again. Texture-paint tiles did this, and F11 - Texture Painting fixed its own case by evaluating the input first. Make the pre-walk digest stable, and audit the other persistent caches keyed on scene-node hashes.

## Scope

**In:**
- A stable, content-derived pre-walk digest, and the audit of persistent caches keyed on scene hashes.

**Out:**
- F11's texture-paint tile fix, which is already done on its branch.

**Edge cases:**
- **File changes on disk between sessions:** the stable digest must still change, so derive it from what is cheap to read before a walk (resolved path, size, mtime of the root layer), never from the path alone.
- **Unsaved or anonymous layers:** keep a per-process value. These never reach a persistent cache anyway.
- **Walk result differs from placeholder:** the digest still switches to the walked value after the walk. The hash changing once is expected; what is fixed is that the pre-walk key is the same across sessions.

**Assumptions:**
- The random value was chosen to avoid a stale hit before the walk. A path+size+mtime digest gives the same protection.

## Phase 629.1: Stable digest

- [ ] M629.P1.T1 — Derive the pre-walk digest from the root layer's resolved path, size and mtime
  - files: `Engine/Nodes/Scene/SceneFileSource.{h,cpp}` (`makeUnwalkedDigest` :72-81 and its callers :246, :341); `Tests/Scene/` SceneFileSource or USDRead hash test
  - approach: replace the random value with a hash of (resolved root layer path, file size, mtime ns), plus a fixed tag so it can't collide with a walked digest. Keep the per-process random fallback only when the file can't be stat'ed (anonymous layer). Update the comment at :72-75 to say why the digest is stable.
  - verify: a test creates two SceneFileSource instances on the same file (simulating two sessions) and gets the same pre-walk digest; touching the file changes it; `ctest -R "SceneFileSource|USDRead"` green
  - size: M

## Phase 629.2: Cache audit

- [ ] M629.P2.T1 — Audit persistent caches keyed on scene-node hashes
  - files: read-only: disk cache tier (`Engine/Cache*`), baked textures, geometry file cache (`Engine/Geo/GeoFileFormat*`, GeoFileCache node), texture-paint tiles; report to `build/m629/cache-audit.md`
  - approach: list every cache that persists across sessions or outlives a single evaluation and is keyed on a hash that includes a USDRead. For each, note whether the key could be taken before the input scene's first walk, and whether T1's change makes it stable.
  - verify: the report names each cache with file:line and a verdict; the PM checks any "still unstable" verdict and files a task for it
  - size: M

**Verification gate:** full build and ctest green; the audit report is attached to the PR body, with any remaining unstable cache either fixed here or filed.
