# M182 - Loop Feedback

Feedback mode for M180 - Loop Block Core (user, 2026-10-24): iteration k's LoopBegin output is iteration k−1's body result, and iteration 0 takes LoopBegin's external input. Iterated erode, smear, trails and reaction-diffusion-style effects follow. Graph cycles are refused at connection time (`Engine/NodeInputs.cpp:611-627`), so feedback is not a real edge: LoopBegin, under iteration k's context, requests the body's last node (LoopEnd's input) under iteration k−1's context through M178 - Render Context Core's cross-context request. With distinct context keys the unrolled graph is acyclic.

## Scope

**In:**
- `mode = Feedback` on LoopEnd; LoopBegin's back-reference under feedback.
- Feedback together with frame iteration (each iteration also offsets time, for trails).
- RoI propagation through the chain: iteration k's RoI on the body grows by each step's expansion.

**Out:**
- Feedback across frames (frame N reading frame N−1's result): M190 - Cross-Frame Feedback.

**Edge cases:**
- RoI growth: a body that expands RoI by r per iteration requests (n·r)-expanded regions at iteration 0; the request pass computes this exactly, iteration by iteration.
- Only the last iteration's output leaves the loop; intermediate iterations are not cached in the image cache (they would never be reused), but they are kept in the FrameStore until their single consumer runs.
- A body that reads no loop variable still renders once per iteration in feedback (its input differs), so its context key includes `loop.iteration` implicitly: LoopBegin reports `loop.iteration` as an extra read under feedback.
- Single pass k under feedback shows iteration k's result, which still requires iterations 0..k.

## Phase 182.1: Feedback

- [ ] M182.P1.T1 — Implement LoopBegin's feedback back-reference
  - files: `Engine/Nodes/Loop/LoopBegin.cpp`, `Engine/Nodes/Loop/LoopEnd.cpp`
  - approach: Under feedback, LoopBegin at iteration k > 0 requests LoopEnd's input node under the k−1 context (M178.P2.T1's target-context request) instead of its own input, and reports `loop.iteration` through `getExtraContextReads`. LoopEnd requests only the last iteration. RoIs propagate through the chain in the request pass.
  - verify: at the phase build, `ctest -R NativeLoopFeedback` (T2) passes.
  - size: L

- [ ] M182.P1.T2 — Test feedback
  - files: `Tests/Native/NativeLoopFeedback_Test.cpp`
  - approach: Erode 1 px in a feedback loop of 5 equals Erode 5 px (square kernel). A Transform translate 1 px × 10 iterations equals translate 10 px (impulse filter). RoI growth requested at iteration 0 matches n·r. Single pass k=2 equals 3 iterations. Both schedulers bit-identical.
  - verify: at the phase build, `ctest -R NativeLoopFeedback` passes.
  - size: M

- [ ] M182.P1.T3 — Document feedback
  - files: `Engine/Nodes/Loop/docs/LoopEnd.md`
  - approach: Semantics, RoI growth, memory, and combining with frame iteration.
  - verify: reviewed at the gate.
  - size: S

## Decisions

- 2026-10-27: the conform-warning failure was the test reusing a QString (Node::getPersistentMessage appends); the warning itself was correct. Diagnostics stripped; release gate green (run 40625018090, b38a0a48b).
- 2026-10-27 — Written; known limits to settle at review: legacy fallback to a deep recursive render when no stored image; concurrent legacy tiles may each run the chain; LoopBegin copies rather than forwards the previous image; groups in a feedback body partly covered by dirtying; body knobs reading body metadata use stored hashes. Cross-frame feedback stays out (M190).
- 2026-10-27 — Feedback design (consultant-reviewed, PM): (1) under feedback LoopBegin's hash folds a masked Merkle digest of the body (computeContentHash factored so the input-hash term is a lookup; LoopBegin a fixed tag; body nodes hashed recursively, iteratively walked) plus LoopEnd's iteration knobs; outside inputs join LoopBegin's HashInputs; nested feedback loops recompute under the union of masks. Dirtying via a virtual hook in Node::getHashDependents (body nodes list their LoopBegin), not the metadata-reader registration. LoopBegin's metadata stays its external input's (tail images are conformed, with a warning on mismatch); only the RoD follows the tail. (2) At k>0 LoopBegin is non-identity and requests the tail at k−1 via getContextNodeRequests; forwards the stored image when formats match. Legacy scheduler: LoopEnd drives k=0..n−1 at the request-pass RoI with a render-scoped store handing iteration k−1 to LoopBegin(k) (no ad-hoc legacy image caching). (3) A virtual hook in Node::getContextReadSuccessors adds the tail as LoopBegin's read successor so the body and LoopBegin share one read-set (keeps the unrolled graph acyclic).
- 2026-10-27 — PM: Houdini's knob model — LoopEnd `mode` = {Accumulate, Feedback}; Single Pass becomes a separate toggle (keeps `singlePassIteration`), reworking M180's knob (no backward compat). Feedback's LoopBegin hashing/caching design under consultant review.
- 2026-10-25 — **Cross-frame feedback is its own milestone (user):** M190 - Cross-Frame Feedback, blocked on this one.

**Verification gate:** the full build and ctest pass, including `NativeLoopFeedback`; `tools/ci/remote/build.sh debug --tests` passes; under Xvfb an iterated-erode and a smear-trail project are rendered and screenshots shared with the user before sign-off.
