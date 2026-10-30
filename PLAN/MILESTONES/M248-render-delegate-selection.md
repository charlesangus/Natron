# M248 - Render Delegate Selection

Let USDRender and the 3D viewer render through any Hydra render delegate installed beside
USD, not only Storm. This milestone builds the plumbing — discovery, a per-delegate engine,
a Renderer knob on USDRender and a renderer dropdown in the viewer — and proves it without
a third-party renderer: Cycles and MoonRay arrive in M251 - Cycles Rendering and M252 -
MoonRay Rendering. Storm stays the default everywhere. Builds on M20 - 3D Node Vocabulary's
long-lived engine and its dedicated Hydra render thread.

## Scope

**In:**
- A delegate registry over `HdRendererPluginRegistry`: id, display name, whether it loads.
- `USDRender` gets a `Renderer` choice knob (Storm default). The 3D viewer gets its own renderer dropdown, also defaulting to Storm (user, 2026-10-27).
- One long-lived engine per (consumer, delegate); switching the delegate rebuilds only that consumer's engine.
- Delegates that need no GL context (CPU renderers) render without entering the GL render thread's context; AOV read-back works for CPU-owned render buffers as well as Storm's GL textures.
- A saved delegate that is not installed keeps its value, shows a persistent node error naming it, and never silently falls back to Storm.

**Out:**
- Cycles and MoonRay builds — M249 - Bundle Cycles Delegate and M250 - Bundle MoonRay Delegate.
- Rendering through them, AOVs beyond rgba/depth.Z/primId — M251 - Cycles Rendering and M252 - MoonRay Rendering.
- Render settings knobs — M253 - Delegate Render Settings.
- Progressive refinement — M254 - Progressive Viewer Render.
- Deep output — M257 - Cycles Deep Output and M258 - MoonRay Deep Output.

**Edge cases:**
- Delegate plug-in found but its library fails to load: listed as unavailable with the loader's error; selecting it is a node error.
- Switching delegate mid-render: the render is cancelled, the old engine is released, the new one is created on next render.
- Headless (NatronRenderer, no display): CPU delegates work; Storm keeps its EGL surfaceless path (M20 - 3D Node Vocabulary).
- Free GPU Memory drops only Storm engines; CPU-delegate engines are released by a separate purge of their scene memory.

**Assumptions:**
- `UsdImagingGLEngine::SetRendererPlugin` switches delegates on an existing engine or a new one without a GL context for non-Storm plug-ins. Verify against the pinned USD headers and `usdview` before relying on it.
- Unconfirmed: the viewer composites a CPU-delegate frame by uploading its colour buffer to a texture, as M254 - Progressive Viewer Render will need.

## Phase 248.1: Discovery and engines

- [ ] M248.P1.T1 — Delegate registry
  - files: `Engine/Nodes/Scene/RenderDelegates.h`, `Engine/Nodes/Scene/RenderDelegates.cpp`, CMake source list, `Tests/Scene/RenderDelegates_Test.cpp`
  - approach: wrap `HdRendererPluginRegistry::GetRendererPluginDescs` / `UsdImagingGLEngine::GetRendererPlugins` behind a small Natron type (id token, display name, loadable flag, load error). Storm is always present and first. Read the pinned headers and usdview's renderer menu for the exact calls and mark anything recalled from memory.
  - verify: unit test lists Storm; a stub plugInfo.json on `PXR_PLUGINPATH_NAME` declaring a renderer whose library is missing is listed as unavailable with an error string.
  - size: M

- [ ] M248.P1.T2 — Engine per delegate in the Hydra render path
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.h`, `Engine/Nodes/Scene/HydraHandoff.cpp`
  - approach: key the engine entry by delegate id. Storm keeps the GL render thread, Hgi and the AOV-texture hand-off. A non-Storm engine is created with `SetRendererPlugin`, takes no GL context, and its AOVs go through the existing `MappedAov` read-back. Per M20 - 3D Node Vocabulary's long-lived-scene decision the engine is not rebuilt per edit — only on delegate change.
  - verify: existing Storm tests pass unchanged; a test selects the stub delegate from M248.P1.T1 and gets a node error, not a crash, with the Storm engine count unchanged.
  - size: L

## Phase 248.2: Knobs

- [ ] M248.P2.T1 — USDRender Renderer knob
  - files: `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/RenderDelegates.cpp`, `Tests/Scene/USDRender_Test.cpp`
  - approach: a choice knob named `renderer`, label `Renderer`, populated from the registry at node creation and on refresh. Storm is the default. An unavailable saved id is kept as a hidden entry so project save/load round-trips it, and shown as a persistent node error. Hash the id into the node's render hash.
  - verify: test sets the knob to the stub delegate, saves and reloads the project, and gets the same id plus the error; changing the knob changes the node hash.
  - size: M

- [ ] M248.P2.T2 — Viewer renderer dropdown
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneViewport.h`, `Tests/SceneViewport_Test.cpp`
  - approach: a dropdown on the 3D viewer, defaulting to Storm and independent of any USDRender node (user, 2026-10-27). Changing it rebuilds only the viewer's engine. The choice is stored with the viewer's other per-viewer state.
  - verify: GUI test under the strict runner switches Storm → stub delegate → Storm without a crash, the viewer shows the error for the stub, and the Storm engine count returns to one.
  - size: M

**Verification gate:** CI green; `USDRender` and the viewer list Storm plus any discovered delegate and render unchanged on Storm; a missing or unloadable delegate is a named error that round-trips through save/load; Storm reference images match as before; a Xvfb screenshot of the dropdown is shared with the user.

## Decisions

- 2026-10-28: implemented (build pending). Non-Storm delegates run on their own non-GL render thread, yielding to scene writes between passes; abort checked on the caller's thread. The registry loads every delegate once on first listing so a missing library is reported up front. An unavailable delegate keeps the node's existing engine. A saved renderer id no plug-in provides stays as a visible "(not installed)" entry. The viewer always passes the Storm id explicitly (otherwise USD's priority can pick Embree); its renderer choice is a Renderer submenu in the viewer's context menu, not saved with the project (per-viewer state isn't saved today). Clear All Caches doesn't drop viewer engines. Open to user: submenu vs a saved toolbar combo.
- 2026-10-27 — **Viewer and USDRender choose separately (user):** the viewer keeps Storm as its fast interactive view while USDRender produces the final frame in Cycles or MoonRay.
- 2026-10-30 — **UAT passed (user).** Render delegate selection is accepted with the current viewer Renderer submenu.
