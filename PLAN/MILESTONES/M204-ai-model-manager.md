# M204 - AI Model Manager

Models reach users on demand. Natron carries a pinned manifest of darktable-ai models: `Engine/Ai/data/ai_models.json`, taken from darktable's `data/ai_models.json` and the `.dtmodel` release assets of github.com/darktable-org/darktable-ai. A Preferences "AI Models" page downloads models into the user data dir and deletes them. Every download is checked against the manifest's sha256 and unpacked from its `.dtmodel` zip (`config.json` plus `.onnx`). Before a download starts, the page shows the model's weight licence and training-data licence. darktable-ai's licence rule applies: weights must carry a GPL-3-compatible licence. AI nodes pick a model with a choice knob filtered by task. A node whose model is missing shows a persistent error and a "Download model" button. Headless farms use `NatronRenderer --download-model <id>`.

Deps: M203 - ONNX Inference Backend. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- The manifest schema, the pinned manifest, and a `tools/ai/update-manifest.py` that regenerates it from a darktable-ai release tag.
- `ModelRegistry`, which lists models by task and resolves an id to its installed path or to "missing".
- A downloader built on `Engine/FileDownloader`, with progress, cancel, resume-by-restart, sha256 verification, and atomic install (temp dir then rename).
- `.dtmodel` unpacking and `config.json` parsing: inputs, outputs, tile size, multiple-of, colour space, scale, and optimisation level.
- The Preferences "AI Models" page.
- A `ModelKnob` helper for nodes: a choice knob filtered by task, a "Download model" button, and a persistent error when the model is missing.
- The `--download-model` and `--list-models` CLI flags.

**Out:**
- Extra model repositories and the update-check preference from darktable master: none.
- User-supplied arbitrary models outside the manifest: none. A manifest entry with a `file://` URL serves tests only.
- Any node: M205–M208.

**Edge cases:**
- **Offline:** downloads fail with the network error, and installed models keep working.
- **sha256 mismatch:** the temp dir is deleted, the error names the model, and nothing is installed.
- **A model deleted while a node uses it:** the session cache entry is dropped, and the next render shows the missing-model error.
- **A project naming a model id that the manifest no longer lists:** the knob keeps the id, and the node shows "unknown model `<id>`".
- **Two downloads of the same model:** the second joins the first.

**Assumptions:**
- `Serialization/Bundle/ZipStore.h`'s `ZipReader` can read deflate members. If not, P1.T2 inflates with zlib, which is already linked (root `CMakeLists.txt:71`).
- darktable-ai's release asset URLs are stable per tag.

## Phase 204.1: Registry and downloads

- [ ] M204.P1.T1 — Manifest schema, pinned manifest and registry
  - files: new `Engine/Ai/data/ai_models.json`, new `Engine/Ai/ModelRegistry.h`, `Engine/Ai/ModelRegistry.cpp`, new `tools/ai/update-manifest.py`, `Engine/CMakeLists.txt`
  - approach:
    - Each manifest entry holds: `id`, `task` (`mask-object`, `denoise`, `denoise-raw-bayer`, `denoise-linear`, `upscale-2x`, `upscale-4x`), `name`, `url`, `sha256`, `size`, `weightsLicence`, `trainingDataLicence`, `osaid` (darktable-ai's classification), `cardUrl` and `default`.
    - The script reads a darktable-ai release's assets and model cards at a tag and writes the file. Pin the tag in the file header.
    - The models list is SegNext ViT-B SAx2 HQ, SAM 2.1 tiny/small/base-plus, NIND UNet, NAFNet-SIDD, RealPLKSR 2x/4x, BSRGAN 2x/4x, and RawNIND UtNet2 Bayer and linear. The script refuses any entry whose `weightsLicence` isn't on the GPL-3-compatible allow-list.
    - The manifest is compiled in as a Qt resource.
    - The registry resolves installs under `StandardPaths eStandardLocationData`/`models/<id>/`.
  - verify:
    - gtest `Tests/Ai/ModelRegistry_Test.cpp`: the manifest parses, `byTask` returns the expected ids, an installed fixture model resolves, and a missing one reports missing.
    - The script run at the pinned tag reproduces the committed file.
  - size: M

- [ ] M204.P1.T2 — Downloader and installer
  - files: new `Engine/Ai/ModelDownloader.h`, `Engine/Ai/ModelDownloader.cpp`, new `Engine/Ai/ModelPackage.h`, `Engine/Ai/ModelPackage.cpp`
  - approach:
    - Wrap `FileDownloader` (`Engine/FileDownloader.h:62`) with progress and cancel signals.
    - Stream to `models/.tmp-<id>/`, check the sha256, then unpack: try `ZipReader` and fall back to zlib inflate.
    - Parse `config.json` into a `ModelConfig` struct (input and output names, channels, colour space `srgb`/`linear-rec709`/`linear-rec2020`/`camera`, tile and `multipleOf`, scale, opt level).
    - `rename` into place. Downloads of the same id are deduplicated.
  - verify: a test with a `file://` manifest entry pointing at a fixture `.dtmodel` built from `Tests/fixtures/ai/identity.onnx` installs it and parses its config. A corrupted copy fails the sha256 check and leaves no directory behind.
  - size: L

## Phase 204.2: UI and node integration

- [ ] M204.P2.T1 — Preferences "AI Models" page
  - files: `Gui/PreferencesPanel.cpp`, new `Gui/AiModelsPage.h`, `Gui/AiModelsPage.cpp`, `Gui/CMakeLists.txt`
  - approach:
    - A table with columns name, task, size, weights licence, training-data licence (with an OSAID badge), and status (Installed, Missing, or a percentage).
    - Download and Delete buttons, and a "Default for task" radio per task.
    - The first download of a model shows a licence confirm with the card link. SAM 2.1's confirm calls out its research-only training data.
    - It sits next to M203's "AI" page.
  - verify: an Xvfb GUI script opens the page, installs the `file://` fixture model, and sees the status flip to Installed. A screenshot is shared.
  - size: L

- [ ] M204.P2.T2 — `ModelKnob` helper for nodes
  - files: new `Engine/Ai/ModelKnob.h`, `Engine/Ai/ModelKnob.cpp`
  - approach:
    - `ModelKnob::create(effect, task)` adds a `model` choice (installed and missing ids, showing names, defaulting to the task default) and a `downloadModel` button.
    - `resolve()` returns a `ModelConfig` plus its path, or sets the persistent message (`setPersistentMessage`) for missing, unknown or AI-disabled.
    - The button starts a download and refreshes the node when it finishes.
  - verify: gtest on a test-only native node: a missing model sets the persistent error, and installing it clears the error on the next render.
  - size: M

- [ ] M204.P2.T3 — CLI flags
  - files: `Engine/CLArgs.cpp`, `Engine/CLArgs.h`, `Engine/AppManager.cpp`
  - approach: `--list-models` prints id, task, installed status and licence. `--download-model <id>` downloads it synchronously and exits non-zero on failure. Both work in NatronRenderer without a project.
  - verify: a test or script runs both flags against the `file://` fixture manifest via a `NATRON_AI_MANIFEST` test override.
  - size: M

**Verification gate:**
- The full build and ctest pass.
- Under Xvfb, the AI Models page downloads a real darktable-ai model, SegNext, over the network, or the fixture where the sandbox blocks the network, with a screenshot shared before sign-off.
- `NatronRenderer --download-model` works.
- An AppImage built with `package.sh` is handed over for UAT.
