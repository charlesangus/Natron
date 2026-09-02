# Three render-failure signalling gaps, measured and deferred to M5

Found while implementing `M3.P1.T8`, verified by reading the whole caller set
rather than inferred. All three are pre-existing, none is caused by the ACES
work, and none is fixed in M3 — the user's instruction for T8 was explicit:
"We should not do anything other than make a noise with a bad param enter an
error state." They belong to **M5 (test & correctness baseline)**.

**1. A persistent error message does not stop a render.**
`Node::setPersistentMessage(eMessageTypeError, …)` is GUI state only. Nothing in
the render or scheduling path — `EffectInstance::renderRoI`, `renderRoIInternal`,
`OutputSchedulerThread`, `RenderEngine`, `ViewerInstance`,
`AppInstance::startWritersRendering` — ever consults `hasPersistentMessage()` to
decide whether to proceed, and there is no pre-render graph walk for error nodes
anywhere in the tree. The complete caller set is internal forwarding in
`Engine/Node.cpp`; a wrapper and an `assert` in `Engine/EffectInstance.cpp`
(~3101, ~3116); `Engine/OfxEffectInstance.cpp` (~556, ~2100, ~2111), which read it
only to decide *what text* to fabricate for an already-failed action;
`Engine/PrecompNode.cpp` (~728), forwarding to the parent for display; and
`Gui/NodeGui.cpp` / `Gui/ViewerGL.cpp`, pure overlay refresh. The deciding code is
`Engine/OfxEffectInstance.cpp:2085-2122`, where `eStatusFailed` vs `eStatusOK`
comes solely from the plug-in's returned `OfxStatus`. **Consequence:** an OFX
plug-in that sets an error message but returns `kOfxStatOK` renders successfully
and caches a possibly-wrong image, with only a cosmetic error in the UI.

**2. `setPersistentMessage` stores nothing in background mode.**
`Engine/Node.cpp:3839-3841` — when `appPTR->isBackground()` it prints
`Persistent message: …` to stdout and returns, so `hasPersistentMessage()` is
always false in `NatronRenderer` **and in the `Tests` binary**. That is why
`Tests/ProjectOCIO_Test.cpp` asserts on captured stdout rather than on node
state; it looks like a lazy assertion and isn't.

**3. A failed render still exits 0.**
`NatronRenderer -w Write1 1-1` on a project that fails to render prints
`Error while rendering: Rendering Failed`, writes no output file, and exits **0**.
This is the same class of defect as the smoke test's exit code
(`M3.P1.T13`), one layer down, and it means no batch caller can detect a
failed render from its status.

Together these mean a wrong render is currently indistinguishable from a right
one to anything automated. M3 closes the specific colour hole it opened; M5
should close the signalling.
