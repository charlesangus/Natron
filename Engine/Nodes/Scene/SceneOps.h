/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef Engine_Nodes_Scene_SceneOps_h
#define Engine_Nodes_Scene_SceneOps_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a scene node's authoring function is handed: the fresh layer to author its edits
 * into, the payload it reads from (its input's), and the project time settings animated knobs
 * sample over. Knobs that are not animated author defaults; animated ones author time samples at
 * every frame of [firstFrame, lastFrame]. The layer is sealed against edits once authoring
 * returns.
 **/
struct SceneLayerEdit {
    PXR_NS::SdfLayerRefPtr layer;
    ScenePayload upstream;
    double firstFrame = 0.;
    double lastFrame = 0.;
    double framesPerSecond = 24.;
    // The project's scene units, which the layer authors as its stage metadata.
    SceneMetadata::UpAxisEnum upAxis = SceneMetadata::eUpAxisY;
    double metersPerUnit = 1.;
    // The project's shared materials root prim name, empty when off.
    std::string materialsScope;
    // Links out of their root that layer composes from the layers it sublayers, which the scan of
    // layer itself does not reach: an author sublayering a file records the file's here.
    SceneLinkIndex* sublayerLinks = nullptr;
    // The live textures the author authors (SceneNodeAuthoring::recordLiveTexture), kept with the
    // layer. Null where a layer cannot record any.
    LiveTextureSourceList* liveTextures = nullptr;
    // Set by an author whose layer leaves values for the stage that renders it to resolve
    // (SceneProjectionCameras). Null where a layer cannot.
    bool* deferredProjections = nullptr;
    // What layer composes from outside the stack, such as the stack layer of another input it
    // references (createStackLayer()): the author sets it, and the layer's entry holds it.
    std::shared_ptr<const SceneLayerDependencies>* dependencies = nullptr;
};

/**
 * @brief What SceneOps::reparentRoots grafts, and where. Every root prim of every input is
 * re-parented under destinationPath, inputs weakest first: where two inputs graft a root under the
 * same name, they compose with the later input stronger, as in USDMerge, unless autoRename gives
 * the later one the first free "_<n>" suffix.
 **/
struct SceneReparentArgs {
    std::vector<ScenePayload> inputs;
    // An absolute prim path, such as /Transform1 or /set/props/Transform1.
    std::string destinationPath;
    // The schema type names the destination and its missing ancestors are defined with (Xform,
    // Scope). Empty defines them typeless.
    std::string destinationType;
    std::string ancestorType;
    bool autoRename = false;
    // A root prim name, such as the project's materials scope, that every input defining it keeps
    // at its own path, /<name>, instead of grafting it: never renamed, and the relationships and
    // connections into it re-pointed there. Inputs sharing it compose, the later one stronger.
    // Empty grafts every root.
    std::string passThroughRoot;
    // Optional: authors the destination's own opinions, such as its transform, into a layer of its
    // own, memoized on overridesKey. It may only author "over" specs, so an edit of it changes
    // properties without resyncing the grafted subtree. Handed no upstream: the key cannot see it.
    SceneLayerMemo::AuthorFunc authorOverrides;
    SceneLayerMemo::Key overridesKey;
    // Names the authored layers, for debugging.
    std::string layerTag;
};

/**
 * @brief Where one input root prim landed. The seam for code that follows a graft, such as
 * re-targeting relationships that point from one root into another, which a reference alone
 * cannot carry.
 **/
struct SceneRootGraft {
    std::size_t inputIndex = 0;
    std::string sourcePath;
    std::string graftedPath;
    // Whether the root is the args' passThroughRoot, left at its own path.
    bool passedThrough = false;
    // Whether, passed through, it holds a UsdGeomGprim, which a materials scope should not.
    bool holdsGprim = false;
};

/**
 * @brief A relationship target or attribute connection a graft could not carry: it lies under no
 * root prim its input grafts, so no grafted path stands for it. The grafted property is authored
 * without it.
 **/
struct SceneUnmappedLink {
    std::size_t inputIndex = 0;
    // The property as grafted.
    std::string propertyPath;
    // The target as the input names it.
    std::string targetPath;
};

/**
 * @brief What SceneOps::authorMetricsCorrection converts a source scene from and to, and which of
 * the two aspects it converts.
 **/
struct SceneMetricsCorrection {
    SceneMetadata::UpAxisEnum fromUpAxis = SceneMetadata::eUpAxisY;
    double fromMetersPerUnit = 0.01;
    SceneMetadata::UpAxisEnum toUpAxis = SceneMetadata::eUpAxisY;
    double toMetersPerUnit = 1.;
    bool correctUpAxis = false;
    bool correctUnits = false;
    // Maps the source stage's times to the target layer's, for the time samples it rescales.
    PXR_NS::SdfLayerOffset sourceToLayer;
};

class SceneStage;

/**
 * @brief The thin interface scene nodes use to author layers, stack them into payloads and
 * compose them. It is deliberately not an abstraction over USD's semantics: nodes author with
 * the Sdf and Usd APIs directly.
 **/
namespace SceneOps {
/**
 * @brief Whether OpenUSD found the plugInfo.json files of the libraries Natron links, which USD
 * looks for in usd/ beside libusd_plug.so. Without them USD's first lookup of a type they declare
 * (the usda file format, behind every anonymous layer) is a fatal error that aborts the process,
 * so every scene entry point checks this before calling into USD. Returns false with
 * *errorMessage set when any is missing. Checked once per process.
 **/
bool checkUsdRuntime(std::string* errorMessage);

/**
 * @brief An empty anonymous layer for one node's edits. The tag only shows up in the layer's
 * identifier, for debugging.
 **/
PXR_NS::SdfLayerRefPtr createNodeLayer(const std::string& tag);

/**
 * @brief The payload made of base's layers with layer on top as the strongest. layerHash must
 * identify layer's content: equal hashes over equal bases are taken to mean equal stacks.
 * The metadata a layer authors on its pseudo-root overrides base's. The entry's link index is
 * findLayerLinks() of layer.
 **/
ScenePayload appendLayer(const ScenePayload& base,
                         const PXR_NS::SdfLayerRefPtr& layer,
                         U64 layerHash);

/**
 * @brief The properties layer authors whose relationship targets or attribute connections lie
 * outside the root prim the property sits under: an Sdf-level scan of layer's specs alone, not of
 * what it sublayers or references.
 **/
SceneLinkIndex findLayerLinks(const PXR_NS::SdfLayerRefPtr& layer);

/**
 * @brief findLayerLinks() over layer's layer stack: layer and every layer it sublayers,
 * recursively, each once.
 **/
SceneLinkIndex findLayerStackLinks(const PXR_NS::SdfLayerRefPtr& layer);

/**
 * @brief The union of the link indexes of payload's layers.
 **/
SceneLinkIndex getLinks(const ScenePayload& payload);

/**
 * @brief appendLayer() for an entry taken from another payload, keeping what it depends on alive
 * in the stack it returns too. Code that restacks layers goes through here, so a graft layer
 * never loses the wrapper it references.
 **/
ScenePayload appendEntry(const ScenePayload& base,
                         const SceneLayerEntry& entry);

/**
 * @brief Grafts every input's root prims under args.destinationPath, the layers authored and
 * memoized in memo. Sets *output to a payload of its own, not stacked on any input: the structure
 * layer defining the destination and its ancestors, the overrides layer when args has an author
 * for it, then one graft layer per input with root prims, in input order. A graft layer defines
 * <destination>/<name> for each root of its input, each internally referencing that root in an
 * anonymous wrapper layer that sublayers the input's stack and carries its metadata. The payload
 * holds the wrappers and input stacks itself, so it composes for as long as it is held, past the
 * memo's next authoring and past the memo itself.
 *
 * Root prims are the defined, non-abstract children of the input's composed pseudo-root, read
 * through SceneQueryPool. An input with none, an empty one included, contributes no layer. Graft
 * layers carry their input's timeCodesPerSecond, upAxis and metersPerUnit, so its time samples
 * keep their timing and the result's metadata follows the strongest input, as a merge's does; a
 * default prim is not carried, as the input's roots are no longer roots.
 *
 * A reference to a root drops the relationship targets and attribute connections that lie
 * outside it. For each property of a grafted root that the input's link index lists and whose
 * composed targets leave its root, the graft layer authors an explicit target list at the
 * property's grafted path, each target moved to where its root was grafted from the same input.
 * Targets under no root of that input are left out and listed in unmappedLinks, when given.
 *
 * A root named args.passThroughRoot is defined at its own path instead, referencing the same
 * root of the wrapper, and maps to itself, so links into it from grafted roots are re-pointed
 * there; it may be the destination itself.
 *
 * When grafts is given, it receives where each input root landed. Returns false with
 * *errorMessage set, keeping memo's layers, when the destination is not an absolute prim path, an
 * input root other than a passed-through one is the destination itself, or authoring fails.
 **/
bool reparentRoots(const SceneReparentArgs& args,
                   SceneGraftMemo* memo,
                   ScenePayload* output,
                   std::vector<SceneRootGraft>* grafts,
                   std::vector<SceneUnmappedLink>* unmappedLinks,
                   std::string* errorMessage);

/**
 * @brief An anonymous, read-only layer sublayering input's stack, strongest first, and carrying
 * the stage metadata its layers author: a layer of one node's stack can reference or load as a
 * payload a prim of another input's composed scene through it, every layer of that input
 * included. Whatever holds a layer referencing it must hold it and input too
 * (SceneLayerEdit::dependencies), as nothing else keeps an anonymous layer alive. Null when input
 * is empty or the layer cannot be created.
 **/
PXR_NS::SdfLayerRefPtr createStackLayer(const ScenePayload& input,
                                        const std::string& tag);

/**
 * @brief The payload's layers, weakest first.
 **/
std::vector<SceneLayerEntry> getLayers(const ScenePayload& payload);

/**
 * @brief Every live texture the payload's layers record, those of the stacks they compose from
 * (a grafted material's) included, each layer's once.
 **/
LiveTextureSourceList getLiveTextures(const ScenePayload& payload);

/**
 * @brief True when a layer of the payload, or of the stacks they compose from, leaves projection
 * cameras for the stage that composes it to resolve (SceneProjectionCameras::resolve).
 **/
bool hasDeferredProjections(const ScenePayload& payload);

/**
 * @brief The stage-level metadata layer authors on its pseudo-root, over weaker for whatever it
 * does not author.
 **/
SceneMetadata readLayerMetadata(const PXR_NS::SdfLayerRefPtr& layer,
                                const SceneMetadata& weaker);

/**
 * @brief The identifiers of payload's layers in the order a root layer sublayers them, strongest
 * at index 0: the stack's order reversed.
 **/
std::vector<std::string> getSubLayerPaths(const ScenePayload& payload);

/**
 * @brief Writes metadata onto root's pseudo-root, as the stage-level metadata of the stage root
 * is the root layer of. When applied is given, only the fields that differ from it are written,
 * so a stage on root hears of nothing that did not change. An empty defaultPrim clears it.
 **/
void writeRootMetadata(const PXR_NS::SdfLayerRefPtr& root,
                       const SceneMetadata& metadata,
                       const SceneMetadata* applied);

/**
 * @brief Opens a stage on root with no session layer, its payloads loaded as load says. Every stage
 * Natron composes is opened here, so getStageOpenCount() counts them all.
 **/
PXR_NS::UsdStageRefPtr openStage(const PXR_NS::SdfLayerRefPtr& root,
                                 PXR_NS::UsdStage::InitialLoadSet load = PXR_NS::UsdStage::LoadAll);

/**
 * @brief How many stages openStage() has opened in this process.
 **/
U64 getStageOpenCount();

/**
 * @brief Composes payload into a new stage: an anonymous root layer carrying the payload's
 * metadata and sublayering its layers, strongest first. Each call opens a new stage, so it is
 * for one-off uses (tests, writing a file); consumers that follow a changing stack keep a
 * SceneStage, and nodes reading their upstream borrow one from SceneQueryPool.
 **/
PXR_NS::UsdStageRefPtr compose(const ScenePayload& payload);

/**
 * @brief The payload asset paths layer authors anywhere in its prims and variants, as authored.
 **/
std::set<std::string> getAuthoredPayloadAssets(const PXR_NS::SdfLayerRefPtr& layer);

/**
 * @brief The file layers layer composes: layer itself when it is backed by a file, then every
 * layer its sublayers and references name, recursively, each once. Asset paths resolve relative
 * to the layer naming them. Files that cannot be opened are skipped, without posting errors to
 * the caller's TfErrorMark, and their anchored asset paths are appended to *missingPaths when it
 * is given, each once. Opens the layers not yet open, and the returned references keep them open.
 * Payload arcs are never opened, so a stage's unloaded payloads stay unread: their anchored asset
 * paths are appended to *payloadPaths when it is given, each once, and what they bring in is not
 * followed.
 **/
std::vector<PXR_NS::SdfLayerRefPtr> getFileLayerClosure(const PXR_NS::SdfLayerRefPtr& layer,
                                                        std::vector<std::string>* missingPaths = nullptr,
                                                        std::vector<std::string>* payloadPaths = nullptr);

/**
 * @brief The file layers already open that layer brings in through sublayers, references and
 * payloads, layer's own first, recursively. Opens nothing, so a Reload reaches the payloads a
 * stage has loaded and leaves the others closed.
 **/
std::vector<PXR_NS::SdfLayerRefPtr> getOpenFileLayers(const PXR_NS::SdfLayerRefPtr& layer);

/**
 * @brief Reloads the file layers among layers in place (SdfLayer::ReloadLayers: only those whose
 * file changed since it was read), so every stage composing them, live ones included, sees the
 * files' current content. Anonymous layers are skipped: reloading one clears it. Runs inside
 * SceneWriteBarrier::runExclusive, so it fails when the calling thread holds a ReadScope.
 * Returns false with *errorMessage set when the barrier refuses the write or a layer fails to
 * reload.
 **/
bool reloadFileLayers(const std::vector<PXR_NS::SdfLayerRefPtr>& layers,
                      std::string* errorMessage);

/**
 * @brief How many times reloadFileLayers() has reloaded layers in this process. A reload can
 * change what any open file layer sublayers, references or loads as a payload, so a
 * getFileLayerClosure() result taken at one count may be stale at a later one.
 **/
std::uint64_t getFileLayerReloadCount();

/**
 * @brief The timecode of payload's composed stage at frame of a project running at
 * framesPerSecond: the same instant in seconds, frame x the stage's time codes per second /
 * framesPerSecond. Identical to frame when the rates match.
 **/
double frameToTimeCode(const ScenePayload& payload,
                       double frame,
                       double framesPerSecond);

/**
 * @brief Parses text as the USD path expression a prim selector holds (SdfPathExpression): paths
 * whose names may hold glob wildcards (/World/*, /World/B?x), // for any depth (/World//*,
 * //*Light*), predicates in braces ({isa:Mesh}), and the set operators: whitespace or + for union,
 * & for intersection, - for difference, ~ for complement, with parentheses. Blank text is the empty
 * expression, which selects nothing. Returns false with *errorMessage naming the failure when
 * text does not parse, holds a relative path, or references a collection (%name), which a node's
 * selector has nothing to resolve against.
 **/
bool parsePrimExpression(const std::string& text,
                         PXR_NS::SdfPathExpression* expression,
                         std::string* errorMessage);

/**
 * @brief The prims of stage that expression (parsePrimExpression()) selects, in depth-first stage
 * order. Only active, loaded, defined, non-abstract prims are visited, so a prim under an inactive one
 * never matches. Returns false with *errorMessage set, and *paths empty, when expression does not
 * parse; blank text selects nothing.
 **/
bool selectPrims(const PXR_NS::UsdStageRefPtr& stage,
                 const std::string& expression,
                 std::vector<PXR_NS::SdfPath>* paths,
                 std::string* errorMessage);

/**
 * @brief selectPrims() that polls cancelled as it walks and gives up as soon as it turns true,
 * returning false with *paths and *errorMessage empty.
 **/
bool selectPrims(const PXR_NS::UsdStageRefPtr& stage,
                 const std::string& expression,
                 std::vector<PXR_NS::SdfPath>* paths,
                 std::string* errorMessage,
                 const std::function<bool()>& cancelled);

/**
 * @brief Whether matchDrawRules() needs a stage for rules: false when every rule names plain paths
 * only and walks nothing below them, so loads cannot change what they match.
 **/
bool drawRulesNeedStage(const SceneDrawRules& rules);

/**
 * @brief The graph policy rules ask for on stage, in their order (sceneAddDrawRuleMatch()). A
 * pattern matches the active, defined, non-abstract prims stage has composed, an unloaded payload's
 * own prim included; plain paths are taken without a stage, whether they exist or not. A rule
 * that matches nothing, or whose pattern does not parse, adds nothing and is no error. stage may
 * be null. Polls cancelled as it walks; returns false with *policy empty once it turns true.
 **/
bool matchDrawRules(const PXR_NS::UsdStageRefPtr& stage,
                    const SceneDrawRules& rules,
                    SceneDrawStates::PolicyStates* policy,
                    const std::function<bool()>& cancelled);

// One prim a draw rule's pattern matched, and the rows the rule sets at and below it, in order.
struct SceneDrawRuleMatch {
    std::string path;
    std::vector<std::pair<std::string, SceneDrawStateEnum>> rows;
};

// For each rule, in the rules' order, its matches in path order.
typedef std::vector<std::vector<SceneDrawRuleMatch>> SceneDrawRuleMatches;

/**
 * @brief matchDrawRules() stopping short of the policy: what each rule matched, which
 * rematchDrawRulesUnder() can bring up to date after loads, and getDrawRulePolicy() turns into the
 * policy. Returns false with *matches empty once cancelled turns true.
 **/
bool matchDrawRules(const PXR_NS::UsdStageRefPtr& stage,
                    const SceneDrawRules& rules,
                    SceneDrawRuleMatches* matches,
                    const std::function<bool()>& cancelled);

/**
 * @brief Brings *matches, made by matchDrawRules() for rules on stage, up to date with the prims
 * now at and below roots, which loads or unloads changed: only those subtrees are walked, and a
 * match outside them keeps its rows unless its walk reaches into one. Returns false, leaving
 * *matches unusable, once cancelled turns true.
 **/
bool rematchDrawRulesUnder(const PXR_NS::UsdStageRefPtr& stage,
                           const SceneDrawRules& rules,
                           const std::vector<std::string>& roots,
                           SceneDrawRuleMatches* matches,
                           const std::function<bool()>& cancelled);

// The graph policy matches make for rules, as matchDrawRules() returns it.
void getDrawRulePolicy(const SceneDrawRules& rules,
                       const SceneDrawRuleMatches& matches,
                       SceneDrawStates::PolicyStates* policy);

// The stage load rules loadSet stands for: every payload for a null one.
PXR_NS::UsdStageLoadRules getLoadRules(const SceneLoadSet* loadSet);

// rules as a load set, without the rules that change nothing.
SceneLoadSetPtr makeLoadSet(const PXR_NS::UsdStageLoadRules& rules);

/**
 * @brief prefixes (PrimSelector::Reach::prefixes), each replaced by the outermost prim above it
 * on stage that carries a payload: loading or unloading that payload adds or removes what lies
 * at the prefix, whatever else is loaded. Paths already present are not repeated.
 **/
std::vector<std::string> getLoadReach(const PXR_NS::UsdStageRefPtr& stage,
                                      const std::vector<std::string>& prefixes);

/**
 * @brief Whether what a pattern reaching below prefixes selects can differ between stages loading
 * the payloads of from and of to, null standing for every payload. prefixes come from
 * getLoadReach() on a stage composed with from, so that a payload above a prefix counts.
 **/
bool loadChangeReaches(const SceneLoadSet* from,
                       const SceneLoadSet* to,
                       const std::vector<std::string>& prefixes);

/**
 * @brief Makes the payloads stage includes those of loadSet (every one for a null set) with
 * LoadAndUnload(), which recomposes only the prims whose inclusion changes. Rules on paths the
 * stage cannot load yet, with no prim at or above them but the root, are left out. Returns
 * whether anything was loaded or unloaded.
 **/
bool applyLoadSet(const PXR_NS::UsdStageRefPtr& stage,
                  const SceneLoadSet* loadSet);

/**
 * @brief How many payloads stage leaves unloaded at or below the prims of paths, counting a
 * nested payload only once the one above it is loaded. A path with no prim counts the unloaded
 * payload above it, if there is one.
 **/
std::size_t countUnloadedPayloads(const PXR_NS::UsdStageRefPtr& stage,
                                  const std::vector<std::string>& paths);

/**
 * @brief A prim selection evaluated on a private stage that loads every payload, so it reaches
 * what an evaluator has left unloaded, for previews only: what it finds never feeds an
 * evaluation, and no other stage is touched. The search runs on a thread of its own and stops
 * as soon as it is cancelled, or stopWhen() turns true, even in the middle of a payload file.
 **/
class UnloadedSearch {
public:
    enum StatusEnum {
        eStatusRunning,
        eStatusDone,
        eStatusCancelled,
        eStatusFailed
    };

    // Searches scene for the prims expression selects.
    static std::shared_ptr<UnloadedSearch> start(const ScenePayload& scene,
                                                 const std::string& expression,
                                                 const std::function<bool()>& stopWhen = std::function<bool()>());

    // Searches what effect's input inputNb evaluates to, as a render sees it.
    static std::shared_ptr<UnloadedSearch> start(const EffectInstancePtr& effect,
                                                 int inputNb,
                                                 const std::string& expression,
                                                 const std::function<bool()>& stopWhen = std::function<bool()>());

    // Cancels the search and waits for its thread.
    ~UnloadedSearch();

    void cancel();

    StatusEnum getStatus() const;

    // Waits for the search to end, at most timeoutMs when it is not negative. Returns whether it has.
    bool wait(double timeoutMs = -1.) const;

    // The selected prims, in depth-first stage order, once the status is eStatusDone.
    std::vector<std::string> getMatches() const;

    // Why the search failed, for eStatusFailed.
    std::string getError() const;

    struct State;

private:
    UnloadedSearch();

    std::shared_ptr<State> _state;
    std::thread _thread;
};

/**
 * @brief The rotation about X, in degrees, that turns correction's from up axis into its to up
 * axis: +90 for Y to Z, -90 for Z to Y, 0 when the axes match or correctUpAxis is off.
 **/
double getMetricsCorrectionAngle(const SceneMetricsCorrection& correction);

/**
 * @brief The uniform scale fromMetersPerUnit / toMetersPerUnit, or 1 when the two are equal
 * within UsdGeomLinearUnitsAre()'s tolerance or correctUnits is off.
 **/
double getMetricsCorrectionScale(const SceneMetricsCorrection& correction);

/**
 * @brief Authors into layer the "over"s that convert source, a composed stage that layer is
 * stronger than, from correction's metrics to its target ones, without reparenting anything.
 *
 * The topmost xformable prims under source's defined, non-abstract roots (the root itself, or the
 * first xformables under a Scope or a typeless prim), and every xformable below them whose op
 * stack resets, each get xformOp:rotateX:upAxisCorrection and/or xformOp:scale:unitsCorrection
 * prepended to their composed xformOpOrder, after a leading !resetXformStack!. A name the prim
 * already has takes the first free "_<n>" suffix. Under a scale, every camera's lengths the
 * transform does not reach (clippingRange, focusDistance, the focal length and apertures, the
 * clipping planes' distances) are rescaled, its default and every time sample. Authors nothing
 * when neither aspect converts anything. Returns the number of prims given corrective ops.
 **/
std::size_t authorMetricsCorrection(const PXR_NS::SdfLayerRefPtr& layer,
                                    const PXR_NS::UsdStageRefPtr& source,
                                    const SceneMetricsCorrection& correction);

/**
 * @brief Records the node that authored the layers of payload, its top layer alone when topOnly,
 * for authoringNodeOf(). Holds neither the layers nor the node; a layer re-recorded keeps its
 * latest owner. MT-safe.
 **/
void setLayerOwner(const ScenePayload& payload,
                   const NodePtr& owner,
                   bool topOnly);

// The node recorded as layer's author, or null.
NodePtr getLayerOwner(const PXR_NS::SdfLayerHandle& layer);

/**
 * @brief The prims owner's own layers in payload hold specs for, sorted and without repeats: a
 * property's spec counts for its prim. Reads the layers, not any stage, so the cost follows what
 * the node authored. Empty when no layer of payload is recorded as owner's.
 **/
std::vector<std::string> getAuthoredPrimPaths(const ScenePayload& payload,
                                              const NodePtr& owner);

/**
 * @brief The layers of payload recorded as owner's, in payload order. Empty when none is.
 **/
std::vector<PXR_NS::SdfLayerRefPtr> getOwnedLayers(const ScenePayload& payload,
                                                   const NodePtr& owner);

/**
 * @brief layers as USDA, one after the other, each under a comment naming its identifier. Reads
 * the layers only, whatever their size.
 **/
std::string exportLayersText(const std::vector<PXR_NS::SdfLayerRefPtr>& layers);

/**
 * @brief A new in-memory stage holding prim's composed definition at its path: specifier, type,
 * and the metadata that is not a composition arc. Its properties are added by
 * flattenPropertyInto(), so a caller can release the stage lock between them. Null when prim is
 * invalid. Caller holds the stage lock.
 **/
PXR_NS::UsdStageRefPtr beginPrimDefinition(const PXR_NS::UsdPrim& prim);

/**
 * @brief Copies property's composed opinion onto the prim of definition at the same path.
 * Caller holds the lock of property's stage.
 **/
void flattenPropertyInto(const PXR_NS::UsdStageRefPtr& definition,
                         const PXR_NS::UsdProperty& property);

// definition as USDA. Needs no stage lock.
std::string exportDefinitionText(const PXR_NS::UsdStageRefPtr& definition);

/**
 * @brief The node whose layer holds the opinion attribute resolves to at time: the strongest spec
 * with a default value, time samples or a spline. Null when no recorded layer holds it, a file
 * read in by a node or a schema fallback; *layerIdentifier is then that spec's layer, or empty
 * for a fallback.
 **/
NodePtr authoringNodeOf(const PXR_NS::UsdAttribute& attribute,
                        PXR_NS::UsdTimeCode time,
                        std::string* layerIdentifier);

/**
 * @brief One arc of a prim's composition, strongest first, as the inspector shows it.
 **/
struct ScenePrimArc {
    enum Type {
        eRoot,
        eInherit,
        eVariant,
        eRelocate,
        eReference,
        ePayload,
        eSpecialize
    };

    Type type = eRoot;
    int depth = 0;
    bool ancestral = false;
    bool implicit = false;
    // False for an authored payload that is not loaded, which has no target in the prim index.
    bool loaded = true;
    // Variant arcs carry the selection, as in /Asset{shadingVariant=red}.
    std::string targetPath;
    std::string targetLayer;
    std::string introducingLayer;
    // Null for a file or an anonymous layer no node recorded.
    NodePtr targetOwner;
    NodePtr introducingOwner;
};

struct ScenePrimComposition {
    std::vector<ScenePrimArc> arcs;
    // Set for a prototype or an instance proxy, which have no prim index of their own to walk.
    std::string prototypePath;
    bool isPrototype = false;
    bool isInstanceProxy = false;
};

/**
 * @brief The arcs composing prim: its prim index in strength order, then the payloads authored on
 * it that are not loaded. Constructs a UsdPrimCompositionQuery, which recomputes the arcs every
 * time, so call it for the selected prim only. Caller holds the stage lock.
 **/
ScenePrimComposition getPrimComposition(const PXR_NS::UsdPrim& prim);

/**
 * @brief One spec contributing an opinion to a prim or to one of its properties.
 **/
struct SceneOpinion {
    // The node that authored the spec's layer, or null for a file or an unrecorded layer.
    NodePtr owner;
    std::string layerIdentifier;
    // "def Xform" for a prim spec, empty for a property spec.
    std::string primDescription;
    // Null for a prim spec. Lets readOpinionValue() read the value under a later lock.
    PXR_NS::SdfPropertySpecHandle propertySpec;
    // Set by readOpinionValue(): the default value of an attribute spec, or the target paths of a
    // relationship spec; empty when the spec holds neither.
    PXR_NS::VtValue value;
    std::size_t numTimeSamples = 0;
    bool hasSpline = false;
    double layerOffset = 0.;
    double layerScale = 1.;
};

/**
 * @brief The specs composing prim, strongest first, each with the node that authored its layer.
 * Caller holds the stage lock.
 **/
std::vector<SceneOpinion> getPrimOpinions(const PXR_NS::UsdPrim& prim);

/**
 * @brief The specs composing the property of prim named name, strongest first, as of time, without
 * their values, which can hold millions of elements: see readOpinionValue(). Empty when the prim
 * has no such property. Caller holds the stage lock.
 **/
std::vector<SceneOpinion> getPropertyOpinions(const PXR_NS::UsdPrim& prim,
                                              const PXR_NS::TfToken& name,
                                              PXR_NS::UsdTimeCode time);

/**
 * @brief Fills opinion's value from its property spec, doing nothing for a prim spec or a spec gone
 * since it was listed. One spec per call, so that a caller can release the stage lock between
 * them. Caller holds the stage lock.
 **/
void readOpinionValue(SceneOpinion* opinion);

/**
 * @brief value as text, at most maxLength bytes of it, stopping there rather than formatting the
 * rest and cutting it, so that the cost follows maxLength whatever the value's size. Sets *cut if
 * the text stops short of the value's end. Also stops once *cancelled is set. A VtValue shares its
 * data by reference counts, so this needs no stage lock.
 **/
std::string valueText(const PXR_NS::VtValue& value,
                      std::size_t maxLength = std::numeric_limits<std::size_t>::max(),
                      const std::atomic<bool>* cancelled = nullptr,
                      bool* cut = nullptr);

// The file of a layer identifier, without its arguments, or empty for an anonymous layer.
std::string layerFileOf(const std::string& layerIdentifier);

struct SceneAuthor {
    // Null for a file or an unrecorded layer.
    NodePtr node;
    std::string layerIdentifier;
    bool schemaFallback = false;
};

enum SceneValueSource {
    eSceneValueSourceNone,
    eSceneValueSourceFallback,
    eSceneValueSourceDefault,
    eSceneValueSourceTimeSamples,
    eSceneValueSourceValueClips,
    eSceneValueSourceSpline
};

struct ScenePrimAttribute {
    std::string typeName;
    PXR_NS::VtValue value;
    SceneValueSource source = eSceneValueSourceNone;
    SceneAuthor author;
};

struct ScenePrimRelationship {
    std::vector<std::string> targets;
    SceneAuthor author;
};

struct ScenePrimMetadata {
    struct Field {
        std::string key;
        PXR_NS::VtValue value;
        SceneAuthor author;
    };

    std::vector<Field> fields;
    // The prototype of an instance or of an instance proxy's prim.
    std::string prototypePath;
    bool isPrototype = false;
};

/*
 * The reads below address a prim by path, so a caller needs no pxr call to make them. Each is one
 * short read for one row: an attribute can hold millions of points and the main thread may be
 * waiting for the lock, so a caller takes the lock once per row. Caller holds the stage lock. A
 * path naming no prim, or a name it does not have, reads as empty or false.
 */
std::vector<std::string> getAttributeNames(const SceneStage& stage,
                                           const std::string& primPath);

bool readAttribute(const SceneStage& stage,
                   const std::string& primPath,
                   const std::string& name,
                   double frame,
                   double framesPerSecond,
                   ScenePrimAttribute* attribute);

std::vector<std::string> getRelationshipNames(const SceneStage& stage,
                                              const std::string& primPath);

bool readRelationship(const SceneStage& stage,
                      const std::string& primPath,
                      const std::string& name,
                      ScenePrimRelationship* relationship);

bool readMetadata(const SceneStage& stage,
                  const std::string& primPath,
                  ScenePrimMetadata* metadata);

bool getPrimComposition(const SceneStage& stage,
                        const std::string& primPath,
                        ScenePrimComposition* composition);

bool getPrimOpinions(const SceneStage& stage,
                     const std::string& primPath,
                     std::vector<SceneOpinion>* opinions);

std::vector<SceneOpinion> getPropertyOpinions(const SceneStage& stage,
                                              const std::string& primPath,
                                              const std::string& name,
                                              double frame,
                                              double framesPerSecond);

// beginPrimDefinition(), with the names of the properties to flatten into it. Null if no such prim.
PXR_NS::UsdStageRefPtr beginPrimDefinition(const SceneStage& stage,
                                           const std::string& primPath,
                                           std::vector<std::string>* propertyNames);

void flattenPropertyInto(const PXR_NS::UsdStageRefPtr& definition,
                         const SceneStage& stage,
                         const std::string& primPath,
                         const std::string& name);

// One address per layer, to tell whether two reads got the same layers.
std::vector<const void*> getLayerIdentities(const std::vector<PXR_NS::SdfLayerRefPtr>& layers);
} // namespace SceneOps

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneOps_h
