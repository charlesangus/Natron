# Live sessions and graph merge

User decisions, 2026-10-11, for M102 - Graph Edit Operations, M103 - Graph Diff And Merge, M104 - Merge Resolution UI, M105 - Live Session Core and M106 - Live Session Presence And Undo (filed as M95–M99 and renumbered on 2026-10-12, because those IDs were already taken):

  - **Live sessions are host-authoritative.** The host orders every edit and rebroadcasts it. On the same knob, the last writer wins. Peers stay connected (LAN/VPN) and there is no offline editing. CRDTs and node locking were rejected.
  - **Live-session v1 scope:** graph, knob and position sync, plus presence (each user's selection and open panels, in their colour), per-user undo, and follow-a-user's-viewer. **No session password in v1**, so the host dialog warns that anyone who can reach the port can join.
  - **Each peer renders locally with its own cache.** Only the script is shared, and media paths are assumed to resolve on every machine (shared storage).
  - **Merge is available in the app and on the CLI.** The CLI (`natron-merge`) auto-applies clean changes, records conflicts in the file, and doubles as a git merge driver and textconv. The in-app merge view highlights conflicted nodes and resolves per knob.
  - **A template is "just a bunch of nodes in a script" that lives in a file on disk.** Merge therefore works on the set of nodes that came from template T, not on a whole project or a Group.
  - **Node identity:** persist a node ID, since live sessions need one, but treat it as one matching signal among several. The user's example: an artist deletes a Grade and makes a new one in the same place, which "in a sense" is the same node. The merge's matching policy is decided by a scored spike, M103.P1, and **the user makes the call**.
  - **Node-specific merge resolution:** nodes and knob kinds can carry their own merge handlers, for example per-keyframe curve merge, per-item roto merge and per-track tracker merge. Prior art: the user's `charlesangus/nuke_diff`, whose lessons are recorded in M103's notes.
