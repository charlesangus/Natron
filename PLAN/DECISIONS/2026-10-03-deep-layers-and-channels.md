# Deep images get layers and channels like flat images

2026-10-03, M60 (user answers Q1–Q3, 2026-10-02).

- **Grouping.** `DeepLayers::groupDeepChannels` groups a deep image's channel names into `ImageLayerDesc` layers. Colour comes first, as one storage layer chosen by the R/G/B/A bits present: always Alpha or RGBA, never RGB, because every deep stream has A. `Z`/`ZBack` are never layers. Dotted names group by prefix (`diffuse.R/G/B` → `diffuse`). Within a layer, channels are ordered canonically (R, G, B, A, then alphabetically), because a deep image's channel map is sorted. `channelName`/`findChannel` map a layer channel back to the deep name: bare R/G/B/A for colour, `layer.c` otherwise.
- **Reporting.** `EffectInstance::getDeepLayers(time, view)` reports a deep stream's layers per frame. Its default is the pass-through input's layers; DeepRead reads its file's header. A deep branch of `getComponentsNeededAndProduced_public` splits pass-through from produced layers by exact layout, then applies M37's `filterPassThroughLayers`. Deep layers therefore list as colour views and AOVs in every layer menu, and they register in the project like flat layers.
- **Nodes.**
  - DeepFromImage converts the layers its channel set selects (default All), takes depth from a chosen Z channel, and always writes A. An alpha-less source gives A = 1.
  - DeepToImage flattens the layers its channel set selects (default All) in one pass and passes nothing through.
  - DeepRecolor recolours the channels its set selects (default `rgb`).
  - DeepExpression gets one layer picker plus four expression fields relabelled with that layer's channels, and fixed Z/ZBack fields (Q2).
  - DeepMerge reports the union of its inputs' layers, or A's alone in holdout. DeepCrop and DeepReformat pass their input's layers through.
- **DeepRemoveLayers / DeepAddLayers (Q1).** These are separate native nodes that mirror M37's semantics on deep data. They use button-less channel sets, and DeepAddLayers zero-fills only what the stream lacks.
- **Structural alpha (Q3).** Deep alpha can never be removed. Removing `rgba` or `rgb` drops R/G/B only, and keep mode always keeps A. DeepRead refuses files with no A, and DeepFromImage always writes A. The deep render pipeline fails any render that produces samples without A.
- **Viewer.** The viewer flattens the selected deep layer, with the alpha layer as coverage. Each layer's flatten is cached separately under the node's hash, matched on exact layout and mip level, so a hash change purges all of them.
- **Clean break.** DeepExpression's knobs are renamed with no legacy shims.
