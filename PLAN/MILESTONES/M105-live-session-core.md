# M105 - Live Session Core

Live multi-user editing: one Natron session **hosts** a project, and others **join** and see
every graph, knob and position edit live, like a shared Google Doc. Each peer renders locally
with its own cache. Only the script is shared. Depends on M102 - Graph Edit Operations
(recording, encoding and applying edits). Presence, per-user undo and viewer following are
M106 - Live Session Presence And Undo.

Scouting facts (2026-10-11): `Qt::Network` is already linked into Engine, Gui and Renderer
(`CMakeLists.txt:69-71`, `QT_COMPONENTS`). Qt WebSockets is not linked, and nothing uses
`QTcpSocket` yet. The existing IPC is `ProcessHandler` (`QLocalServer`, `Engine/ProcessHandler.h:83-253`).

## Design (applies to every task)

- **Host-authoritative (user decision).** The host's GUI process runs a `QTcpServer`. Frames
  are a u32 big-endian length followed by one YAML document. Message types:
  - `hello{protocol, name, natron version}` and `welcome{userId, colour, seq, snapshot}`;
  - `ops{clientSeq, ops}` and `broadcast{seq, originUser, clientSeq, ops}`;
  - `reject{clientSeq, reason}` and `resyncNode{nodeId, NodeSerialization|deleted}`;
  - `checksum{seq, hash}`, `saveRequest` and `bye`.

  No new dependency. A protocol-version mismatch refuses the join with a clear message.
- **Join:** the host sends its full in-memory bundle (`.ntpz` bytes, including `gui.yaml`) and
  its current seq. The client loads it as an unsaved, session-bound project, titled
  `Live: <host project> @ <host>`.
- **Edits.** Recorded local operations go to the host, tagged with a client seq. The host
  assigns a global seq, applies the operations if they came from a client, and broadcasts them
  to everyone, the originator included as its ack. The client applies its own edits
  optimistically. When a broadcast arrives from another user and touches a key (node+knob,
  node+input, node existence, node UI) that the client still has pending, the client skips it:
  the host will apply the client's operation afterwards, so last-writer-wins converges. When an
  operation's target no longer exists on the host, the host sends `reject` plus `resyncNode` to
  the originator.
- **Divergence check:** every 5 s the host broadcasts the hash of the canonical `project.yaml`
  at a seq. A client compares at the same seq and requests a full resync on a mismatch, logging it.
- **Saving.** Only the host writes the file, and autosave runs only on the host. Ctrl+S on a
  client sends `saveRequest`. Save As on a client writes a private copy and leaves the session.
- **Disconnect.** A client that loses the host keeps an unsaved local copy, is told, and can
  save it. When the host closes the session, every client gets `bye`.
- Remote operations are applied under `Suppress` and are **never pushed onto local undo
  stacks**.
- **No password in v1 (user decision).** The host dialog picks a bind address and port and
  warns that anyone who can reach the port can join.

## Phase 105.1: Protocol, host and client (headless)

- [x] M105.P1.T1 — Session protocol messages and framing
  - files: new `Serialization/Session/SessionProtocol.h`, new `Serialization/Session/SessionProtocol.cpp`, `Serialization/CMakeLists.txt`, new `Tests/Serialization/SessionProtocol_Test.cpp`
  - approach: message structs and their YAML encoding (operations through M102's `GraphOpYaml`). The framing codec is incremental: it takes bytes and yields complete messages, and rejects frames over a 512 MiB cap.
  - verify: compiled at P1.T5. Every message type round-trips; split and coalesced byte streams decode identically; an oversize or garbage frame gives an error and no crash.
  - size: M

- [x] M105.P1.T2 — Session host
  - files: new `Engine/LiveSession/SessionHost.h`, new `Engine/LiveSession/SessionHost.cpp`, `Engine/CMakeLists.txt`
  - approach: a `QTcpServer` that handles the join handshake (snapshot from the live project through the bundle writer, in memory), sequencing, apply-then-broadcast (applying through `GraphOpApply` under `Suppress`), rejects with `resyncNode`, checksum broadcast, and disconnect cleanup. It also subscribes to the host's own recorder, so the host user's edits are sequenced the same way.
  - verify: compiled at P1.T5, and exercised by P1.T4.
  - size: L

- [x] M105.P1.T3 — Session client
  - files: new `Engine/LiveSession/SessionClient.h`, new `Engine/LiveSession/SessionClient.cpp`
  - approach: connect and hello; load the welcome snapshot; send local recorded operations with a client seq while keeping a pending-key set; apply broadcasts using the skip rule in **Design**; clear pending entries on ack; handle reject and `resyncNode`; compare checksums and request a full resync on a mismatch.
  - verify: compiled at P1.T5, and exercised by P1.T4.
  - size: L

- [x] M105.P1.T4 — Headless convergence test
  - files: new `Tests/LiveSessionConvergence_Test.cpp`
  - approach: on loopback, start one host and three clients, each with its own `AppInstance` in the test process. If one process cannot hold several apps, run the clients as `NatronRenderer` subprocesses driven by a test script instead. Each peer makes 200 random edits concurrently from a fixed seed: knob sets, creates, deletes, rewires, renames, and same-knob races between peers. Then quiesce and require every peer's canonical `project.yaml` to be byte-identical to the host's. A dedicated case covers delete-versus-edit, where the reject and resync converge.
  - verify: compiled at P1.T5, where it passes 20 seeds in a row.
  - size: L

- [x] M105.P1.T5 — Phase 105.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: M

## Phase 105.2: App integration

- [x] M105.P2.T1 — Session-bound project lifecycle
  - files: `Engine/Project.h`, `Engine/Project.cpp`, `Engine/AppInstance.h`, `Engine/AppInstance.cpp`
  - approach: load a project from in-memory bundle bytes. Add a session-bound flag that changes the title and makes save route to `saveRequest` on clients and skip autosave. On disconnect, leave a plain unsaved project with the message from **Design**. Save As on a client leaves the session.
  - verify: compiled at P2.T5. gtests: loading from bytes equals loading from a file; a client save sends `saveRequest` and the host writes the file; after a disconnect, the client project is unsaved and editable.
  - size: M

- [x] M105.P2.T2 — Host and Join dialogs and the status indicator
  - files: new `Gui/LiveSessionDialogs.h`, new `Gui/LiveSessionDialogs.cpp`, `Gui/Gui20.cpp` (File menu: Host Live Session…, Join Live Session…, Leave Session), `Gui/CMakeLists.txt`
  - approach: the host dialog takes a bind address, port and display name, and shows the security warning. The join dialog takes host, port and display name, with recent hosts remembered in settings. A status-bar indicator shows the session state and the number of peers connected.
  - verify: compiled at P2.T5, and exercised by P2.T3.
  - size: M

- [x] M105.P2.T3 — Two-process GUI test of interactive edits
  - files: new `Tests/gui/m98_live_session.py`, `Tests/gui/run-gui-test.sh` (if it needs a two-process mode)
  - approach: start a host and a client Natron on loopback. On the client, drag a knob slider, set a keyframe, move nodes, create and connect a node, draw a roto stroke, and add a user knob. On the host, rename and delete a node. After each step, both sides' canonical `project.yaml` must match. Any GUI edit path that misses the recorder is a bug to fix in M102's hooks, filed as a new task in this phase.
  - verify: passes under the GUI harness. The build is required.
  - size: M

- [x] M105.P2.T4 — Phase 105.2 build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: M

## Phase 105.3: Close-out

- [ ] M105.P3.T1 — AppImage UAT across two machines
  - files: `build/appimages/M105-<sha>.AppImage`, `build/appimages/M105-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist:
    - host a bench comp on one machine and join from a second (two processes on one host if no second machine);
    - both edit the same comp at once: knobs, nodes, roto, rewiring;
    - the same knob dragged on both sides converges;
    - each side renders on its own;
    - a client save writes the host's file;
    - pulling the network cable on the client leaves a savable local copy.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** `LiveSessionConvergence_Test` passes 20 seeds; `m98_live_session.py`,
full ctest, smoke and CI are green; the user signs off the UAT.

## Decisions

- 2026-10-11 — **User decision:** host-authoritative with last-writer-wins, peers connected, no offline editing (CRDTs and node locks rejected).
- 2026-10-11 — **User decision:** no session password in v1; the host dialog warns instead.
- 2026-10-11 — Transport: plain TCP with length-prefixed YAML frames over the already-linked `Qt::Network`. This avoids adding Qt WebSockets, and reuses M102's operation encoding.
- 2026-10-11 — Only the host writes the shared file; clients' Save sends a request to the host.
- 2026-11-20 — Phase 105.1 T1–T4 in 3249df10c. Changes from the plan:
  - `Project::loadProjectFromBundleBytes` is in already, ahead of P2.T1.
  - SessionShared.{h,cpp} holds the code both sides use.
  - New `resyncRequest` and `bye{reason}` messages; `resyncNode` carries recreate ops.
  - No gui.yaml exists in bundles.
  - The current frame is left out of the checksum.
  - The host renames clashing names.
  - The client's copy loads unsaved.

  Risk: several apps in one test process; the fallback is NatronRenderer subprocesses. Build running.
- 2026-11-20 — Build fix de4f9b8d5 (INSTANTIATE_TEST_CASE_P). main is merged into F5 (fast-forward) and into the lane (7703067ce).
- 2026-11-20 — The convergence and delete-vs-edit tests segfaulted. In background mode every AppInstance's Python id is `app`, so the clients' nodes collided with the host's (`app.Blur1 already exists`) and were left unnamed. **PM call:** clients run as child Tests processes driven over stdin (the `LiveSessionPeerProcess` suite), rather than changing background-mode app naming in the engine. NatronRenderer subprocesses aren't possible yet, because SessionClient isn't exposed to Python. A real use-after-free in `SessionHost::stop()` is also fixed: a flush emitted `disconnected` and erased the peer being iterated. The seed suite's timeout is 900 s. Gate running.
- 2026-11-21 — The convergence test exposed three real divergences: (1) replayed creations renamed the legitimate holder of a name via `freeName`; (2) a reset-to-default restored values but not the dimension fold state; (3) `enableMask_<input>` changes were shared, although each copy can derive them. Fixes are committed. The canonical YAML was fine. The `mismatches == 0` assertion stays, because checksums run only after quiesce. Still open: hand-set enableMask racing a remote input change, and create-and-connect in one batch; neither is exercised by the test.
- 2026-11-21 — Second divergence round (seeds 4, 5, 8, 17, 18):
  1. A resynced node kept a mask switch the host later cleared.
  2. A connection from a node the peer was deleting was skipped instead of applied as a disconnection.
  3. A host connection that failed (a cycle with the peer's own unconfirmed edges) was dropped instead of waiting.

  Fixes: disconnect-on-deleting-source; waiting failed host edits, retried after confirmations and resyncs; resyncs keep unconfirmed edits; GraphOpApply always refreshes the mask switch. Re-gating. Still open: a hand-set mask switch racing a remote input change, and a resync re-deriving the switch.
- 2026-11-21 — Phase 105.1 checkpoint green on 8a61def28 (gate m105-p1e, both schedulers). Phase 105.2 dispatched as one unit.
- 2026-11-21 — Phase 105.2 implemented; P2.T4 is the gate, now running.
  - **PM calls:** a client's Save asks the host to save; Save As or Save and Increment Version leave the session; joining with work in the current graph opens a new window.
  - **Still open:** the two Phase-1 mask-switch races; the window title keeps the host's file name after a disconnect (fix before UAT); a client peer count is left to M106.
- 2026-11-21 — Open defects fixed in 4ee9bdee8 (milestone/m105-live-session-core): after any host edit a client re-asserts mask switches from its own pending edits (both race directions), resyncs end with explicit switch values, and a detached copy is titled Untitled.ntpz; tests in LiveSessionConvergence_Test (LiveSessionMaskSwitchTest) and LiveSessionProject_Test. **Behaviour change beyond live sessions:** the project loader now keeps a saved mask switch turned off when it reconnects a mask input (it used to reopen switched on); PM accepted it as the correct save/load round trip, flagged to the user. P2.T4 runs remotely (`tools/ci/remote/build.sh debug --tests`) while the local container is busy.
- 2026-11-21 — P2.T4 green remotely (run 44954667714, debug, 899/899 on both schedulers plus smoke) on 1b1d7cf90, which also fixes two LiveSessionProjectTest failures present since the tests were added: a project loaded from bytes kept an empty [Project] folder, and a host never marked client edits unsaved (now Project::markUnsavedChanges from SessionHost::applyClientOps). P3.T1 (two-machine AppImage UAT) goes to F5's Feature UAT. Next: PR into feature/f5-live-sessions-v1 and its review.
- 2026-11-23 — Merged into F5 as PR #333 (a32f91511); review fixes 56c832c94, 855e7a5dd. P3.T1 (two-machine AppImage UAT) moves to F5 - Live Sessions V1 UAT, since a Feature member gets no UAT of its own. The branch is kept for M106, which is stacked on it.
