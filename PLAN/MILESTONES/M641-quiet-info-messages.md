# M641 - Quiet Info Messages

`USDToGeo.cpp` (~:426) posts its info through an info-type persistent message, which is never stored on the node. On a render thread it reaches only the error log; on the main thread it opens a modal dialog (`Engine/Node.cpp:7218-7220`) that can block a re-render. M418 - VDB Conversion Ops fixed the same problem in VDBFromPolygons and VDBFromParticles by adding the node's quiet info message (`setQuietInfoMessage`, `Engine/Node.cpp` ~:7247). That API exists only on F22 - Native VDB's branch, so this milestone waits for F22 to land. Then move USDToGeo onto it, and audit the other nodes that post info-type persistent messages; `Engine/Nodes/Scene/USDRender.cpp:1438` posts one on every render.

## Scope

**In:**
- Every node posting an informational (not error or warning) persistent message, moved to the quiet info message.

**Out:**
- The general non-modal notification system (M590 - Non-Modal Notifications).
- Warning and error messages.

**Edge cases:**
- **A node that really needs the user's attention:** stays a warning, not quiet info; the audit says which.

## Phase 641.1: Move to quiet info

- [ ] M641.P1.T1 — List every node that posts an info-type persistent message
  - files: read-only: `grep` for `setPersistentMessage` with the info message type across `Engine/`, `Gui/`; list to `build/m641/info-audit.md`
  - approach: per call site, note the node, the message, whether it runs on a render thread, and whether quiet info fits or it should be a warning.
  - verify: the list covers every call site with file:line and a verdict
  - size: S
- [ ] M641.P1.T2 — Move USDToGeo, USDRender and every other listed node to the quiet info message
  - files: `Engine/Nodes/Geo/USDToGeo.cpp` (~:426), `Engine/Nodes/Scene/USDRender.cpp` (~:1438), and each call site in T1's list; the nodes' tests
  - approach: replace the info-type persistent message with `setQuietInfoMessage` (and its clear) per T1's verdicts. Add a test that USDToGeo's info lands on the node without a dialog, mirroring M418's VDBFromPolygons test.
  - verify: the new test passes; the nodes' existing tests pass; under Xvfb, cooking USDToGeo on the main thread opens no dialog
  - size: M

**Verification gate:** full build and ctest green; no informational dialog appears during a GUI check that cooks USDToGeo and USDRender.
