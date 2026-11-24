# M562 - USDEditPrim Relationship Knobs

USDEditPrim sets a schema's attributes, but a schema's relationships have no knob: `SchemaKnobMapper::describeSchema` (`Engine/Nodes/Scene/SchemaKnobMapper.cpp` ~line 310) puts every property that is not an attribute into `SchemaDescription::unsupported`, and `USDEditPrim::rebuildSchemaKnobs` shows that list in the read-only "Unsupported Attributes" label (`Engine/Nodes/Scene/USDEditPrim.cpp`). So a light cannot be linked to the geometry it lights, a collection cannot be given its members, and `proxyPrim` or `material:binding` cannot be set from the node. This milestone gives each relationship of the chosen schema a knob whose value is a prim pattern (`PrimSelector`, the same syntax and picker as Prim Pattern) and an authoring mode that matches Solaris Edit Properties: Set, Append, Prepend, Remove, Block (`.local/docs/houdini/nodes/lop/editproperties.md`). The mode and value are recorded only once the user touched them, through `AuthoredKnobSet` as attributes are.

In USD 24+ UsdLux light linking is not a pair of relationships on the light: `lightLink` and `shadowLink` are `UsdCollectionAPI` instances (`collection:lightLink:includes`, `collection:lightLink:excludes`, `collection:lightLink:includeRoot`, and the `shadowLink` equivalents). USDEditPrim already applies a multiple-apply schema by Instance Name and sets its attributes (`includeRoot`, `expansionRule`), so with relationship knobs `CollectionAPI` with Instance Name `lightLink` (or `shadowLink`) sets both link collections with no UsdLux-specific code. The same covers `light:filters` (LightAPI) and `proxyPrim` (Imageable).

## Scope

**In:**
- `SchemaKnobMapper` describes a schema's relationships: a new `RelationshipKnob` list in `SchemaDescription` (name, documentation, whether the schema's instance name is part of the name), replacing the relationship half of `unsupported`.
- USDEditPrim builds, for each relationship of the selected schema, an `AuthoredKnobSet`-style pair: a prim-pattern string knob (a `PrimSelector`, script name `<relationship name with colons as underscores>`, label the readable name) and a mode choice (`Set`, `Append`, `Prepend`, `Remove`, `Block`; default `Set`). The "Unsupported Attributes" label keeps listing what still has no knob (array and matrix attributes) and drops relationships.
- Authoring at render: the pattern resolves against the input scene to prim paths; `Set` authors an explicit target list, `Append` and `Prepend` add the paths to the back or the front of the list-op, `Remove` authors them as deleted, `Block` blocks the relationship. Authored on every prim the node edits or defines, with the multiple-apply instance name in the relationship name (`collection:<instance>:includes`).
- CollectionAPI includes/excludes settable end to end, and UsdLux `lightLink`/`shadowLink` through CollectionAPI instances, proved by test (including the composed result: a light's `UsdCollectionAPI` membership query reports the linked prims).
- Node docs: a `USDEditPrim.md` page in `Engine/Nodes/Scene/docs/` (none exists on main), covering relationships and the light-linking recipe. Tooltips say what each mode does and that a relationship's value is a prim pattern.
- ctest coverage and one Xvfb GUI script.

**Out:**
- Relationships of an arbitrary name on USDSetAttribute (the raw single-attribute node): no milestone; its Value knob has no relationship type.
- Attribute connections (`inputs:` sources, shader wiring): no milestone; materials have their own nodes.
- A dedicated light-linking node or viewer UI for links: no milestone.
- Relationship target remapping across references (USD does this on composition): nothing to do here.
- Animating relationships: USD relationships have no time samples.
- Disconnecting or blocking connectable attributes (Solaris "Disconnect Input"): no milestone.
- Metadata edits (kind, active, instanceable...): M564 - USDConfigurePrim Node.

**Edge cases:**
- A blank pattern with `Set` and the toggle on authors an explicit empty target list ("includes nothing"); a blank pattern with `Append`, `Prepend` or `Remove` authors nothing. A knob whose toggle is off authors nothing.
- A pattern that matches no prim: `Set` authors the empty list and the node warns "<pattern> matched no prims" (via `SceneNodeAuthoring::describeNoMatch`); the others author nothing.
- A plain absolute path (a union of plain paths) naming a prim that does not exist in the input scene: the target is authored anyway and the node warns "<path> does not exist in the input scene" (USD allows dangling targets). Wildcard patterns that match nothing are not authored; they follow the no-match rule above.
- A pattern matching prims in an unloaded payload: not searched, and never loaded for the edit (the viewer never loads payloads without user action); `describeNoMatch` reports the unsearched payloads. A plain path below an unloaded payload follows `matchPattern`'s existing rule.
- Switching the Schema or Instance Name rebuilds the knobs; relationship knobs of the old schema are dropped with their recorded values, as attribute knobs are today. A saved project restores the saved schema's knobs before values load (`onKnobsAboutToBeLoaded`).
- Targets are authored as absolute paths in the node's own layer; a target that is the edited prim itself is allowed (USD permits it).
- Remove of a target that is not in the composed list is a no-op in USD; no warning.
- Define mode: the relationship is authored on the defined prim, and targets resolve against the input scene only (the prim being defined does not exist upstream, so a pattern cannot select it to target itself).
- A schema with no relationships shows no relationship knobs and no empty section.

**Assumptions:**
- `UsdRelationship::SetTargets`, `AddTarget(path, UsdListPositionFrontOfPrependList / BackOfAppendList)`, `RemoveTarget` and `BlockTargets` express the five modes in the node's own edit layer. Task T3 proves each on an in-memory stage.
- `UsdCollectionAPI`'s `includes`/`excludes` relationships appear in the prim definition's `GetPropertyNames()` for the instance-qualified names only after `ApplyAPI` with an instance name, so the knob names are built from the Instance Name as attribute knobs already are.

## Phase 562.1: Describe relationships

- [ ] M562.P1.T1 — SchemaKnobMapper lists relationships
  - files: `Engine/Nodes/Scene/SchemaKnobMapper.{h,cpp}`, `Tests/Scene/SchemaKnobMapper_Test.cpp` (new, or beside the existing mapper tests), `Tests/CMakeLists.txt`
  - approach: add `struct RelationshipKnob { std::string name; std::string documentation; }` and `std::vector<RelationshipKnob> relationships` to `SchemaDescription`. In `describeSchema` a property whose `UsdPrimDefinition::Property` is a relationship (`property.IsRelationship()` or the absence of an attribute view, checked against `SdfSpecTypeRelationship`) goes to `relationships` instead of `unsupported`. Add `getRelationshipName(const RelationshipKnob&, instanceName)` and `getRelationshipScriptName` mirroring the attribute helpers (colons become underscores, instance name substituted for multiple-apply schemas), and reuse `getKnobLabel`.
  - verify: gtest on `describeSchema`: `CollectionAPI` lists `includes` and `excludes` under `relationships` and no longer in `unsupported`; `Imageable` lists `proxyPrim`; `SphereLight` or `LightAPI` lists `light:filters`; a schema with no relationship has an empty list; `getRelationshipName` gives `collection:lightLink:includes` for instance `lightLink`.
  - size: M

## Phase 562.2: Knobs and authoring

- [ ] M562.P2.T1 — Relationship knobs in USDEditPrim
  - files: `Engine/Nodes/Scene/USDEditPrim.{h,cpp}`, `Engine/Nodes/Scene/USDEditPrim.reg.inc` (only if the knob names need registering), `Tests/Scene/EditPrim3D_Test.cpp`
  - approach: in `rebuildSchemaKnobs` create, per `SchemaDescription::relationships` entry, a `PrimSelector` (held in a `std::vector<RelationshipKnobs>` in `Implementation`, each with its mode `KnobChoice` and an authored companion bool set up with `AuthoredKnobSet::add` or an equivalent pair, deleted with the other schema knobs on rebuild and restored by `onKnobsAboutToBeLoaded`). Labels are the readable relationship name; the mode choice options and tooltips explain Set, Append, Prepend, Remove, Block; the pattern's tooltip says it selects the prims the relationship points at. `unsupported` label text drops relationships. Every relationship knob uses the Instance Name for multiple-apply schemas and rebuilds when it changes (as the attribute knobs do).
  - verify: gtest builds the node with CollectionAPI and asserts the `includes` and `excludes` knobs exist with the expected script names and default mode `Set`; switching the schema removes them; the Unsupported label no longer contains `includes`.
  - size: M

- [ ] M562.P2.T2 — Author relationship targets at render
  - files: `Engine/Nodes/Scene/USDEditPrim.cpp` (`renderScene`, `authorPrim`), `Tests/Scene/EditPrim3D_Test.cpp`
  - approach: in `renderScene` collect the relationships whose toggle is on. For each, resolve its pattern with `SceneNodeAuthoring::matchPattern` (own `PrimPatternCache` per knob, the input scene, `args.context`) to a sorted path list; fold the resolved paths and modes into the layer's facts digest so a changed stage membership re-authors. In `authorPrim`, after the API schema is applied and attributes are authored, author each relationship by mode: `Set` -> `SetTargets(paths)`; `Append` -> `AddTarget(p, UsdListPositionBackOfAppendList)` per path; `Prepend` -> `AddTarget(p, UsdListPositionFrontOfPrependList)`; `Remove` -> `RemoveTarget(p)` per path; `Block` -> `BlockTargets()`. Create the relationship with `prim.CreateRelationship(name, false)`. Warn through `describeNoMatch` when a `Set` wildcard pattern matches nothing (it still authors the empty list). For a pattern that is a union of plain absolute paths (`PrimSelector::getReach` `literalOnly`), author every literal path whether or not its prim exists, and warn naming the missing ones.
  - verify: gtest on an in-memory stage with `/World/lightA`, `/World/geoA`, `/World/geoB`: `Set` authors `[/World/geoA]`; `Append` onto an upstream list `[geoA]` of `geoB` composes `[geoA, geoB]`; `Prepend` composes `[geoB, geoA]`; `Remove` of `geoA` composes `[geoB]`; `Block` composes `[]` with `HasAuthoredTargets`; blank-pattern `Set` authors an explicit empty list; a plain path `/World/notYet` to a missing prim is authored as a target and the node warns naming it; a toggle-off knob authors nothing.
  - size: L

- [ ] M562.P2.T3 — CollectionAPI and light linking
  - files: `Tests/Scene/EditPrim3D_Test.cpp`
  - approach: tests only, on the node as built in T1-T2. Apply CollectionAPI with Instance Name `lightLink` to a `SphereLight`, set Include Root off, `includes` = the geometry pattern, then query `UsdCollectionAPI::GetCollection(prim, "lightLink").ComputeMembershipQuery()` on the output stage; repeat for `shadowLink` with `excludes`. Cover Edit mode over several matched lights and Define mode. No code beyond what T1-T2 wrote unless a test finds a gap, which is fixed in the owning file.
  - verify: the new tests pass: membership is exactly the linked prims for `includes`, and excluded prims are out for `excludes`.
  - size: M

- [ ] M562.P2.T4 — Params-to-knob audit
  - files: `Tests/Scene/EditPrim3D_Test.cpp`
  - approach: a parameterized test (`INSTANTIATE_TEST_CASE_P`) over every schema `SchemaKnobMapper::listSchemas()` returns: for each, `describeSchema` and the built node must have one knob per attribute that maps and one per relationship, and every name in `unsupported` must be an array/matrix/other unmappable attribute, never a relationship.
  - verify: the audit passes for every listed schema; the failure message names the schema and property if any relationship has no knob.
  - size: M

## Phase 562.3: Docs and GUI check

- [ ] M562.P3.T1 — Node doc page
  - files: `Engine/Nodes/Scene/docs/USDEditPrim.md`
  - approach: write the page in the style of `USDWrite.md` and the other Scene docs: Mode, Prim Pattern, Schema, Instance Name, the attribute knobs and their enable toggles, the relationship knobs with the five modes, a worked light-linking example (CollectionAPI `lightLink` on a light, Include Root off, Includes = the geometry), and what still shows under Unsupported Attributes. No mention of other applications.
  - verify: a reviewer reads the page against the node's tooltips; the mode names and script names match the knobs.
  - size: S

- [ ] M562.P3.T2 — Xvfb GUI script
  - files: `Tests/gui/usd_editprim_relationships.py`
  - approach: under Xvfb at 1920x1080 build SphereLight + Cube -> USDEditPrim from Python, choose CollectionAPI with Instance Name `lightLink`, set the `includes` pattern knob and mode through the Python API, assert the knobs exist after a project save and reload, the Unsupported label no longer lists them, and the viewer-side composed query reports the link. Grab a screenshot of the knob page.
  - verify: the script passes in the remote `--gui` gate; the screenshot goes to the user for sign-off (one file, not a contact sheet).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SchemaKnobMapper_Test` and `EditPrim3D_Test` (including the relationship, light-linking and audit tests) pass; `Tests/gui/usd_editprim_relationships.py` passes under Xvfb at 1920x1080; the user OKs the screenshot of the relationship knobs.

## Decisions

- 2026-11-24 — **Relationships are set through USDEditPrim (user):** USDEditPrim is the schema-driven node and gains relationship knobs whose value is a prim pattern (same syntax and picker as Prim Pattern); CollectionAPI includes/excludes and UsdLux lightLink/shadowLink must be settable through it. Part of the settled "Shape A plus Configure Layer": SetAttribute stays the raw single-attribute node, ConfigurePrim takes metadata only.
- 2026-11-24 — Authoring modes follow Solaris Edit Properties' relationship handling: Set, Append, Prepend, Remove, plus Block, with the per-field enable toggle standing in for "Do Nothing" (PM draft; matches the "match Solaris when in doubt" default).
- 2026-11-24 — lightLink and shadowLink are reached as `CollectionAPI` instances, because UsdLux defines them that way; no UsdLux-specific code (PM draft).
- 2026-11-24 — **Missing plain-path targets are authored with a warning (user):** a plain absolute path to a prim absent from the input scene is authored as a relationship target and the node warns; wildcard patterns that match nothing author nothing.
