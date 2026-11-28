# M585 - USD Asset Gallery

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A dockable browser of USD assets with thumbnails, tags and stars, from which an asset is dropped into the graph as a USDReference. Design reference: Solaris Asset Gallery (`.local/docs/houdini/ref/panes/assetgallery.md`).

Member of F43 - USD Asset Pipeline. It lists the component folders M586 - USD Component Builder publishes, alongside plain USD files.

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead); then M586 - USD Component Builder.

Acceptance sketch:
- Pointing the gallery at a directory lists its USD assets with thumbnails.
- Dragging one into the node graph creates a USDReference to it.
