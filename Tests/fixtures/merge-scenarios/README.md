# Merge scenarios

Three-way merge cases for a template that a supervisor maintains and artists import into shots.
`Tests/Serialization/MergeScenarios.h` builds each scenario's three snapshots in C++:

- base: the template as the artist imported it.
- theirs: the supervisor's new version of the template. Its nodes keep the template's IDs.
- mine: the artist's shot. It holds the imported template nodes, which have fresh IDs and an origin
  naming the import's key and the template node's ID, plus the shot's own nodes and the artist's
  edits.

## The template

`TplIn` (Dot) feeds `Grade1`, then `Blur1`, then `Merge1` input B. `Constant1` feeds `Merge1` input A.
`Merge1` feeds `ColorCorrect1`, then `TplOut` (Dot). In the roto scenario, a Roto node `Matte`
with shapes `Garbage` and `Core` feeds `Merge1` input Mask. In the shot, `Plate` (CheckerBoard)
feeds `TplIn`, and `TplOut` feeds `ShotOut` (Dot).

## Scenarios

| Scenario | Artist (mine) | Supervisor (theirs) |
|---|---|---|
| rename | renames Grade1 to KeyGrade, raises its multiply | raises Blur1's size |
| delete-recreate-grade | replaces Grade1 with Grade2, same plugin and wiring | sets Grade1's gamma |
| delete-recreate-other-type | replaces Grade1 with a ColorCorrect, ArtistCC | changes Grade1's multiply |
| duplicate-template-node | duplicates Blur1 as Blur1_copy, after the template | changes Blur1's size |
| supervisor-replaces-node | changes Blur1's size | replaces Blur1 with Blur2 |
| both-insert-input0 | inserts ArtistBlur on Grade1 -> Blur1 | inserts TplTransform there |
| both-insert-side-input | inserts MatteGrade on Constant1 -> Merge1.A | inserts MatteBlur there |
| imported-twice | imports the template again (Grade2, Blur2, ...), changes Grade2 | sets Grade1's gamma |
| pasted-from-other-shot | pastes the template beside its own Grade1, without a template entry | changes Grade1's multiply |
| wrap-into-group | changes Blur1's size | wraps Blur1 and Merge1 into a group, Look |
| reconnect | wires ArtistMatte into Merge1.A | wires Constant2 there |
| keyframes-both-sides | moves the multiply key at 10, adds one at 20 | moves the keys at 1 and 10, adds one at 30 |
| roto-both-sides | moves a Garbage point, adds shape ArtistFix | moves another Garbage point, lowers Core's opacity, adds Edge |
| artist-adds-around-template | adds ArtistGrade inside, ShotGrade after, GarbageMatte on the mask | changes ColorCorrect1's saturation |
| delete-recreate-as-two | replaces Grade1 with GradeA -> GradeB | sets Grade1's gamma |
| delete-recreate-adjacent-pair | replaces Grade1 and Blur1 with Grade2 -> Blur2 | sets Grade1's gamma and Blur1's size |
| cut-paste-template-node | cuts Grade1 and pastes it back unconnected, TplIn -> Blur1 | sets Grade1's gamma |
| duplicate-then-delete-original | duplicates Blur1, deletes it, wires the copy in its place | changes Blur1's size |
| artist-groups-template-nodes | wraps Blur1 and Merge1 into a group, ArtistLook | changes Blur1's size |
| both-sides-reparent | wraps Blur1 into ArtistLook | wraps Blur1 into a different group, Look |
| rename-plus-expression | renames Grade1, which Blur1's size expression names, to KeyGrade | adds a saturation expression on ColorCorrect1 naming Grade1 |

## Expected results

`expected/<scenario>.yaml` names nodes by path (script names joined by `.`):

```yaml
description: ...
merges:
  - template: grade_tpl       # the import being merged
    base_mine: {Grade1: KeyGrade, ...}     # every base node; ~ for no counterpart
    base_theirs: {Grade1: Grade1, ...}
    scope: [KeyGrade, ...]    # the shot nodes this merge may touch
    conflicts:
      - {node: Grade1, knob: multiply, kind: value}
```

Conflict kinds: `value`, `curve-key`, `input`, `structural-insert`, `delete-modify`, `reparent`,
`adopted`, `roto-shape`. Each conflict names the base node, plus `knob`, `dim`, `time`, `input` or
`item` where they apply. An `adopted` conflict lists, as `defaulted` (space-separated), the knobs
the adopted node leaves at their default while the base node sets them.

## Checking

`Tests/Serialization/NodeMatcher_Test.cpp` asserts that the matcher and the merge scope give every
scenario's `base_mine`, `base_theirs` and `scope`, and prints how simpler strategies score.
`Tests/Serialization/StructuralMerge_Test.cpp` merges every scenario and compares the conflicts it
lists, every field included.
