# M290 - Native Cryptomatte Keyer

A native `Cryptomatte` node that extracts mattes from Cryptomatte layers in any image —
Natron Utility's output from M285 - Utility Renderer AOVs, or EXRs from other renderers — by
picking in the viewer or by name from the manifest, following the Cryptomatte specification
(Psyop, v1.2).

## Scope

**In:**
- Inputs: one Image with Cryptomatte layers; optional mask input.
- Knobs: `layer` ("Layer", choice of the Cryptomatte layer sets found in metadata), `matteList` ("Matte List", names/wildcards, editable text), `pick` ("Pick") and `remove` ("Remove") picker colour knobs using the standard colour-knob eyedropper (memory: pickers use the regular colour picker; the tooltip says the ID under the cursor is what matters), `matteOnly`, `singleSelection`, `outputChannel` ("Output To", default alpha), `previewMode` (keyable colour preview of all IDs).
- Manifest from metadata (embedded JSON or sidecar `manif_file` path), name ↔ hash lookup, wildcard matching.
- Matte = sum of ranked coverages whose ID is in the list; preview shows the standard ID colours.
- GPU and CPU paths per the native-node framework (Point weight).

**Out:**
- Producing Cryptomatte layers — M285 - Utility Renderer AOVs.
- Deep Cryptomatte — none.
- Nuke-gizmo Python compatibility (`cryptomatte_utilities`) — none.

**Edge cases:**
- No Cryptomatte metadata on the input: node error "no Cryptomatte layers found".
- A picked pixel with an ID missing from the manifest: added by hash (`<0x…>` form) and still keyed.
- Manifest absent but layers present: picking works by hash; name matching is unavailable and says so.
- Several Cryptomatte sets in one image: the Layer knob lists all; switching keeps the list text.

**Assumptions:**
- Natron image metadata from Read/EXR carries the `cryptomatte/<key>/…` keys through to node evaluation, as M285 assumes for writing.

## Phase 290.1: Keyer

- [ ] M290.P1.T1 — Cryptomatte metadata and manifest parsing
  - files: `Engine/Cryptomatte.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Cryptomatte_Test.cpp`
  - approach: parse the spec's metadata keys (name, hash, conversion, manifest / manif_file) into layer sets; MurmurHash3 name → float ID matching the spec and the fork's `primPathHash`; wildcard name matching.
  - verify: gtest on the spec's sample metadata — names map to the published hash values; a sidecar manifest loads; wildcard `*/cube*` matches the expected names.
  - size: M

- [ ] M290.P1.T2 — Cryptomatte node and matte extraction
  - files: `Engine/Nodes/Keyer/Cryptomatte.{h,cpp,reg.inc}`, `Engine/AppManager.cpp`, `Engine/Nodes/Keyer/docs/Cryptomatte.md`, `Tests/Native/NativeCryptomatte_Test.cpp`
  - approach: native node per the native-node framework; knobs as listed in Scope with labels matching script names; render sums the ranked coverages for listed IDs (CPU and Slang GPU kernel), preview mode colours IDs; picker knobs read the ID under the cursor from the rank-0 channel and add or remove its name.
  - verify: gtest — a two-object fixture keyed by name gives each object's coverage (edges partial); pick and remove edit the list; an input without metadata gives the node error.
  - size: L

**Verification gate:** full build and ctest green; a GUI script picks an object in the viewer on a Natron Utility render under Xvfb and the screenshot is shared with the user.
