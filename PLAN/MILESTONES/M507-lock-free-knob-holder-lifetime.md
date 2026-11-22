# M507 - Lock-Free Knob Holder Lifetime

#292 (b4dcb78db) added `EffectInstance::lockHolder` (`Engine/EffectInstance.cpp:255`), which takes the global `liveEffectsMutex`, and `KnobHelperPrivate::lockHolder` (`Engine/Knob.cpp:753`) for cold paths. Per-value hot paths still read the raw atomic holder: `shouldUseGuiCurve` (Knob.cpp:761, used by every curve choice, e.g. :1251, :7233), the metadata walks (`addMetadataDependency` :4299, `appendHashedMetadataSources` :4439, `expressionReadClosesMetadataCycle` :4471, `getMetadataDependencies` :4519), and the `KnobImpl.h` getters (:1206, :1422, ...). That is safe for a render's own effect. It is not safe for another node's knob read through an expression while that node is being deleted. Add a lifetime reference on the knob that can be taken without a lock, and move the hot paths onto it.

## Scope

**In:**
- A lock-free holder lifetime reference on `KnobHelper`; hot-path migration; a deterministic lifetime test.

**Out:**
- Deleted-knob listener bookkeeping (M505 - Deleted Knob Listener Dependencies).

**Edge cases:**
- Knobs created in an effect constructor, before a `shared_ptr` exists: the reference is set at attach/initialise time, and until then it falls back to the raw holder, which the effect owns.
- Holders that are not effects (Project, TrackMarker): they live as long as the app, so the raw pointer stays valid.

**Assumptions:**
- Falling back to the raw holder during effect construction is safe; P1.T1 confirms it.

## Phase 507.1: Design and audit

- [ ] M507.P1.T1 — Choose the reference and list every hot-path raw holder read
  - files: read-only: `Engine/Knob.cpp`, `Engine/KnobImpl.h`, `Engine/KnobTypes.cpp`, `Engine/EffectInstance.cpp` (:244–270), `Engine/Node.cpp`
  - approach: consultant compares (a) a `std::weak_ptr<EffectInstance>` on `KnobHelperPrivate`, set at attach and cleared by `detachKnobs`, against (b) an atomic shared-state token. Judge the cost of `lock()` per value read and the races with detach. Classify each `getHolder()`/`holder.load` site as hot or cold, and cross-node or own-effect.
  - verify: a Decisions entry with the chosen mechanism and the site list
  - size: L

## Phase 507.2: Implement

- [ ] M507.P2.T1 — Add the lifetime reference and make `lockHolder` use it
  - files: `Engine/Knob.cpp`, `Engine/Knob.h`, `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`
  - approach: per P1.T1; `KnobHelperPrivate::lockHolder` stops taking `liveEffectsMutex`.
  - verify: `ctest -R ProjectResetListeners` passes at the phase build
  - size: L
- [ ] M507.P2.T2 — Move the hot paths P1.T1 listed onto the reference
  - files: per the P1.T1 list (`Engine/Knob.cpp`, `Engine/KnobImpl.h`)
  - approach: each cross-node site holds the reference for the duration of its read; a failed lock behaves like no holder.
  - verify: existing ctest passes; `node_profile.sh` per-value timings show no regression versus main, same host and sitting
  - size: M
- [ ] M507.P2.T3 — Lifetime test: a reference keeps a deleted node's effect alive, then lets it go
  - files: `Tests/ProjectResetListeners_Test.cpp`
  - approach: take the reference from driver Grade's knob, delete the driver node, and check (with a `weak_ptr` to the effect) that the effect is alive while the reference is held and gone after release. A new lock then returns null, and the reader's expression falls back. Old gtest rules (no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`).
  - verify: passes; with the reference not held, the effect is gone right after deletion
  - size: M

**Verification gate:** `ctest -R ProjectResetListeners` 20/20 under `--repeat-test`; GUI bench rule (`Engine/Knob*`) recorded in Decisions; full debug PR CI green.
