# M106 - Live Session Presence And Undo

Finishes the user's v1 scope for live sessions on top of M105 - Live Session Core: **presence**
(who is connected, and each user's selection and open panels in their colour), **per-user undo**
(Ctrl+Z undoes only your own edits), and **follow a user's viewer**. Depends on M105 - Live
Session Core.

## Design (applies to every task)

- **Presence messages** are separate from operations and are never recorded or saved.
  `presence{userId, selection: [nodeId], openPanels: [nodeId], viewer?: {...}}` is throttled to
  10 Hz and relayed by the host. Each user has a stable colour assigned at welcome.
- **Per-user undo.** Each peer's local undo stack holds only its own commands, because remote
  operations are never pushed to it (M105 - Live Session Core). Undoing a local command changes
  the graph, and the recorder sends the change as new operations, so other users see the undo
  as an ordinary edit. When a remote operation deletes a node that local commands refer to,
  those commands are made obsolete (`QUndoCommand::setObsolete`) so undo skips them. A local
  undo of a knob edit restores the user's own old value even if someone else changed the knob
  since. That is accepted under last-writer-wins, and the status bar notes it.
- **Follow viewer:** a follower mirrors the leader's viewer frame, active viewer input node,
  zoom and pan, and channels. Following stops on any local viewer interaction. The leader's
  state is sent in presence.

## Phase 106.1: Presence

- [ ] M106.P1.T1 — Presence messages and relay
  - files: `Serialization/Session/SessionProtocol.h/.cpp`, `Engine/LiveSession/SessionHost.cpp`, `Engine/LiveSession/SessionClient.cpp`, `Tests/Serialization/SessionProtocol_Test.cpp`
  - approach: add the `presence` message, the 10 Hz client throttle, host relay, and a per-user presence table on each client that emits a Qt signal when it changes. A user's entry is cleared on disconnect.
  - verify: compiled at P1.T4. Protocol round-trip; in a loopback test, client A's selection reaches client B within 200 ms and is cleared when A disconnects.
  - size: M

- [ ] M106.P1.T2 — Remote selection and open panels in the node graph
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp`, `Gui/DockablePanel.cpp`
  - approach: a node selected by a remote user gets a coloured outline in that user's colour, plus a small name tag; several users stack their outlines. A node whose panel a remote user has open shows a coloured dot in its settings-panel header. Local selection and open panels feed the presence sender.
  - verify: compiled at P1.T4. The P3.T3 GUI test checks the outline and dot on B after A selects a node and opens its panel.
  - size: M

- [ ] M106.P1.T3 — Connected-users panel
  - files: new `Gui/LiveSessionUsersPanel.h`, new `Gui/LiveSessionUsersPanel.cpp`, `Gui/CMakeLists.txt`, the Gui pane registration
  - approach: a dock listing users with their colour swatch and name, a "you" marker, the host marker, and a Follow toggle used by P3. Double-clicking a user frames their selection.
  - verify: compiled at P1.T4, and exercised by the P3.T3 GUI test.
  - size: M

- [ ] M106.P1.T4 — Phase 106.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 106.2: Per-user undo

- [ ] M106.P2.T1 — Make local undo safe under remote edits
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/KnobUndoCommand.h`, `Gui/KnobUndoCommand.cpp`, `Engine/LiveSession/SessionClient.cpp`
  - approach: commands record the node IDs they touch. When a remote `DeleteNode` arrives, every local command touching that ID is marked obsolete, and the stack drops it. Commands keep their nodes by ID, not by `NodeGuiPtr`, wherever the stale pointer would otherwise crash. Remote applies never call `pushUndoCommand`, which is asserted in debug builds. An undo whose target changed remotely restores the local old value and posts the status-bar note.
  - verify: compiled at P2.T2, and exercised by P2.T2.
  - size: L

- [ ] M106.P2.T2 — Per-user undo GUI test
  - files: new `Tests/gui/m99_live_undo.py`
  - approach: two processes, A and B:
    - A edits knob k1 and B edits k2; A undoes, and only k1 reverts on both sides;
    - A creates a node and B moves it; A undoes the create, and the node is gone on both sides;
    - B deletes a node A edited; A undoes, and the obsolete command is skipped without a crash;
    - redo is checked for each case.
  - verify: passes under the GUI harness. The build is required.
  - size: M

## Phase 106.3: Follow a user's viewer

- [ ] M106.P3.T1 — Viewer state in presence
  - files: `Serialization/Session/SessionProtocol.h/.cpp`, `Gui/ViewerTab.cpp`, `Gui/ViewerGL.cpp`
  - approach: viewer state is the current frame, the active viewer node ID with its input index, zoom and pan, and the displayed channels. It is captured on change and sent in presence, throttled. A follower applies it without recording it and without moving its own timeline cursor when it is not following.
  - verify: compiled at P3.T3, and exercised by P3.T3.
  - size: M

- [ ] M106.P3.T2 — Follow toggle and auto-stop
  - files: `Gui/LiveSessionUsersPanel.cpp`, `Gui/ViewerTab.cpp`
  - approach: Follow on a user mirrors their viewer state, and only one leader can be followed at a time. Local scrubbing, zoom, pan or a viewer-input change stops following, with a status-bar note. The leader's name is shown in the viewer corner while following.
  - verify: compiled at P3.T3, and exercised by P3.T3.
  - size: M

- [ ] M106.P3.T3 — Presence and follow GUI test, full ctest and smoke
  - files: new `Tests/gui/m99_presence_follow.py`
  - approach: two processes. A selects a node and opens its panel, and B sees the outline and dot. B follows A, A scrubs to frame 20 and zooms, and B matches. B scrubs, and following stops. Then build and run `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: M

## Phase 106.4: Close-out

- [ ] M106.P4.T1 — AppImage UAT
  - files: `build/appimages/M106-<sha>.AppImage`, `build/appimages/M106-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist covers two or three users on one comp: presence colours, the users panel, per-user undo across the cases in P2.T2, and following a viewer during playback.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** `m99_live_undo.py` and `m99_presence_follow.py` pass, full ctest, smoke
and CI are green, and the user signs off the UAT.

## Decisions

- 2026-10-11 — **User decision:** v1 includes presence, per-user undo, and follow-a-user's-viewer.
- 2026-10-11 — Per-user undo replays the user's own inverse as new operations (last-writer-wins). Commands invalidated by remote deletes are dropped, not transformed.
