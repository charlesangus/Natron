# Graph merge: node identity and merge scope

This report covers the identity and provenance spike for template merges. A supervisor maintains a
template, and artists import it into shots and change it there. Later, the supervisor's new version
of the template is merged into each shot. Two questions decide whether that merge does the right
thing:

1. **Matching:** which node of the shot, and which node of the new template, stands for each node of
   the template the artist imported (the base)?
2. **Scope:** which shot nodes may the merge touch? The template's own nodes are in scope. An artist
   node is part of the template's chain or sits outside it; the scope rule decides which.

The corpus is `Tests/fixtures/merge-scenarios` (15 scenarios, 16 merges, at the time of the spike).
The chosen matcher is `Serialization/Merge/NodeMatcher.{h,cpp}`.
`Tests/Serialization/NodeMatcher_Test.cpp` builds the corpus graphs in C++, asserts that the matcher
and the scope rule give every expected result, and prints the score table.

## Provenance used by the spike

Node IDs are random, and they are kept only when a project is loaded. Import, paste, duplicate and
group from selection all mint fresh IDs. A shot node therefore cannot be matched to its template
node by ID. The spike gives each imported node an **origin**: the import's key, and the node's ID
in the template. The template's nodes keep their IDs between versions, because the supervisor
edits the file that was loaded.

The spike keeps origins in a sidecar, `mine.provenance.yaml`, instead of a node field. Only an
import with a template entry names a key. Nodes pasted from another shot keep that shot's origins,
but this shot has no entry for them.

## Strategies

- **IdOnly.** A shot node claims the base node its origin names. Without an origin, a node claims
  the base node with its own ID. A unique claim matches; a contested one matches nothing.
- **IdPlusAdoption.** IdOnly, plus two structural steps:
  - A contested base node goes to the claimant whose neighbours agree with it best, when one does.
  - Adoption: an unmatched base node adopts an unmatched node that has no origin, when that node
    has the same plug-in, the same parent, and the same matched neighbours on the same input labels,
    upstream and downstream. Each must be the other's only candidate. Adoption repeats until nothing
    more is adopted.
- **NameBased.** Matches nodes by path, ignoring IDs and origins.

## Scores

Each cell is correct/wrong/missing over the base nodes: 7 per merge, 8 for the roto scenario.
*Correct* means the strategy picked the expected counterpart, or none when none is expected.

> These numbers come from the spike's simulation, which ran the prototype matchers on hand-built
> graphs with the corpus's names, wiring, plug-ins and origins. `NodeMatcher_Test` now prints the
> table for the chosen matcher and two simpler strategies over the grown corpus.

### Base against mine (the artist's shot)

| Scenario | IdOnly | IdPlusAdoption | NameBased |
|---|---|---|---|
| rename | 7/0/0 | 7/0/0 | 6/0/1 |
| delete-recreate-grade | 6/0/1 | **7/0/0** | 6/0/1 |
| delete-recreate-other-type | 7/0/0 | 7/0/0 | 7/0/0 |
| duplicate-template-node | 6/0/1 | 7/0/0 | 7/0/0 |
| supervisor-replaces-node | 7/0/0 | 7/0/0 | 7/0/0 |
| both-insert-input0 | 7/0/0 | 7/0/0 | 7/0/0 |
| both-insert-side-input | 7/0/0 | 7/0/0 | 7/0/0 |
| imported-twice [grade_tpl] | 7/0/0 | 7/0/0 | 7/0/0 |
| imported-twice [grade_tpl_2] | 7/0/0 | 7/0/0 | **0/7/0** |
| pasted-from-other-shot | 7/0/0 | 7/0/0 | 6/1/0 |
| wrap-into-group | 7/0/0 | 7/0/0 | 7/0/0 |
| reconnect | 7/0/0 | 7/0/0 | 7/0/0 |
| keyframes-both-sides | 7/0/0 | 7/0/0 | 7/0/0 |
| roto-both-sides | 8/0/0 | 8/0/0 | 8/0/0 |
| artist-adds-around-template | 7/0/0 | 7/0/0 | 7/0/0 |
| delete-recreate-as-two | 7/0/0 | 7/0/0 | 7/0/0 |
| **total (113)** | 111/0/2 | **113/0/0** | 103/8/2 |

### Base against theirs (the supervisor's new template)

| Scenario | IdOnly | IdPlusAdoption | NameBased |
|---|---|---|---|
| supervisor-replaces-node | 6/0/1 | **7/0/0** | 6/0/1 |
| wrap-into-group | 5/0/2 | 5/0/2 | 5/0/2 |
| every other merge | all correct | all correct | all correct |
| **total (113)** | 110/0/3 | **111/0/2** | 110/0/3 |

## Failure cases

- **IdOnly**
  - *delete-recreate-grade:* Grade2 has no origin, so Grade1 counts as deleted in the shot. The
    supervisor's gamma becomes a delete-modify conflict on a node the artist still has, in effect.
  - *duplicate-template-node:* Blur1 and Blur1_copy both carry Blur1's origin. The claim is
    contested, so Blur1 matches nothing, and the merge sees the template's Blur1 as deleted.
  - *supervisor-replaces-node:* Blur2 is a new ID, so the artist's size edit on Blur1 meets a
    deletion instead of a per-dimension conflict.
  - *wrap-into-group:* see below.
- **IdPlusAdoption**
  - *wrap-into-group* is its only failure, and every strategy shares it. Group from selection
    copies the nodes into the group with fresh IDs, and the new nodes have a different parent and
    new neighbours (the group's Input and Output nodes). Nothing ties Look.Blur1 to Blur1. The fix
    is in provenance, not in matching: make group from selection and ungroup *move* nodes and keep
    their IDs, and match IDs anywhere in the tree, not only within one parent. Name-based matching
    would also need to look through groups.
  - Breaking a contested claim by neighbour agreement depends on the copy being wired elsewhere.
    A duplicate placed beside the original, with the same input, and the original then deleted,
    would leave the claim contested; the base node stays unmatched, which is the safe failure.
- **NameBased**
  - *rename:* a renamed node is a deleted node.
  - *imported-twice:* the second import's nodes are renamed on import, so the second merge pairs
    every base node with the *first* import's node. All seven are wrong: the merge silently edits
    the wrong copy. This is the worst failure in the table, because nothing flags it.
  - *pasted-from-other-shot:* the template's Grade1 matches the shot's own Grade1, a node the
    template never had. The pasted Grade, renamed Grade2, gets nothing.
  - It is right on duplicate and delete-recreate-other-type only because names happen to be kept.

## Should a deleted-and-recreated node count as the same node?

The user's example is *delete-recreate-grade*: the artist deletes Grade1 and makes a fresh Grade in
the same place. Two readings are possible:

- **The same node.** The artist was rebuilding it. The supervisor's change to it should land on the
  new node. This is IdPlusAdoption's answer: the corpus expects it, and only adoption gets it.
- **A different node.** The artist meant to drop Grade1 and add unrelated work that happens to be a
  Grade in the same spot. The supervisor's change should become a delete-modify conflict.

The graph cannot tell these apart. Adoption picks the first reading, and here is when that goes
wrong:

- **Values the artist never set.** The new node starts from defaults. Adoption makes it stand for
  Grade1, so the base value of every knob the template had set now differs from mine. The three-way
  merge reads that as the artist resetting those knobs, and the artist's defaults win. The supervisor
  only sees it where the supervisor also changed the knob, as a conflict. This is the real cost of
  adoption: a silent reset of template settings is attributed to the artist, though it is also
  what the artist's graph shows.
- **A deliberate replacement.** If the artist meant "not the template's Grade", the supervisor's
  later edits to Grade1 keep landing on the artist's node. They should have been dropped or
  flagged.
- **The supervisor side.** Adoption runs on theirs too (*supervisor-replaces-node*). A supervisor who
  replaces a node to reset it gets their new values, but merged against the artist's edits as if
  the node had only been changed.

Adoption is limited to keep these rare: the same plug-in, the same parent, every matched neighbour
agreeing on both sides, at least one wired edge, a unique candidate on each side, and never a node
that already has an origin. *delete-recreate-other-type* and *delete-recreate-as-two* both stay
delete-modify conflicts, as the corpus expects.

**Recommendation:** count the recreated node as the same node, and mark it. The merge records each
adopted pair (`NodeCorrespondence::adopted`), and the resolution UI can show "matched by position"
for it, so the artist can split it into a delete-modify conflict.

## Scope rules

Each rule picks the shot nodes the merge of one import may touch. The children of an in-scope group
are in scope.

- **OriginOnly:** the nodes whose origin names the import.
- **Enclosed:** those, plus nodes on a path from one of them to another.
- **MatchedEnclosed:** the nodes IdPlusAdoption matched to a base node, plus nodes on a path from
  one of them to another.
- **Connected:** the origin nodes plus everything wired to them, directly or not.

Results over the 16 merges (simulated, as above):

| Rule | Exact | Failures |
|---|---|---|
| OriginOnly | 9/16 | misses every artist node inside the template: Grade2, ArtistCC, ArtistBlur, MatteGrade, ArtistGrade, GradeA and GradeB; includes Blur1_copy |
| Enclosed | 15/16 | includes Blur1_copy, which carries an origin but is no longer the template's node |
| **MatchedEnclosed** | **16/16** | none |
| Connected | 0/16 | pulls in Plate, ShotOut and every other shot node wired to the template |

MatchedEnclosed scores best because the duplicate's origin no longer decides scope: only the node
that won the match does. Artist nodes after the template (ShotGrade) or on an unconnected side
branch (GarbageMatte, which feeds the template but is fed by nothing in it) stay out of scope, as
the corpus expects.

## Recommendation

1. **Matching: IdPlusAdoption.** It is the only strategy with no wrong matches and no misses
   outside the group case. It also gets the user's delete-and-recreate example right. The
   production version needs an index of unmatched nodes by plug-in and parent; the prototype's
   adoption pass is quadratic.
2. **Scope: MatchedEnclosed.**
3. **Provenance amendments for P2:**
   - Group from selection and ungroup move nodes and keep their IDs. Matching looks up IDs across
     the whole tree. This fixes *wrap-into-group* for every strategy.
   - On paste or duplicate, drop a node's origin when the destination already holds a node with
     the same origin (same key and node). The duplicate then becomes artist work, which removes
     the tie-break from *duplicate-template-node*. A paste from another shot keeps its origins,
     so the pasted copy stays linked.
   - Each import gets its own key, as in *imported-twice*. A key per template path would merge both
     copies as one.
