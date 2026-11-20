# Merge scenarios

Three-way merge cases for a template that a supervisor maintains and artists import into shots.
Each scenario folder holds three snapshots:

- `base.ntpz`: the template as the artist imported it.
- `theirs.ntpz`: the supervisor's new version of the template. Its nodes keep the template's IDs.
- `mine.ntpz`: the artist's shot. It holds the imported template nodes, which have fresh IDs, plus
  the shot's own nodes and the artist's edits.

`mine.provenance.yaml` records which template import each shot node came from:

```yaml
templates:
  - key: grade_tpl            # one entry per import
    path: base.ntpz
    hash: <sha256 of base.ntpz's project.yaml>
origins:
  "<node id in mine>": {template: grade_tpl, node: "<node id in base>"}
```

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

## Expected results

`expected/<scenario>.yaml` names nodes by path (script names joined by `.`), because the
generator mints new IDs on every run:

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

Conflict kinds: `value`, `curve-key`, `input`, `structural-insert`, `delete-modify`,
`roto-shape`. Each conflict names the base node, plus `knob`, `dim`, `time`, `input` or `item`
where they apply.

## Regenerating

From the repository root, inside the dev container:

```sh
NatronRenderer -b Tests/fixtures/merge-scenarios/build_scenarios.py
```

`MERGE_SCENARIOS_OUT` sets another output folder, and `MERGE_SCENARIOS_ONLY=rename,reconnect`
builds only those scenarios. The script checks that `mimetype` is each bundle's first member.

To score the matchers and scope rules against the corpus:

```sh
natron-score-matchers Tests/fixtures/merge-scenarios [--verbose]
```
