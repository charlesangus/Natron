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

#ifndef PARALLELRENDERARGS_H
#define PARALLELRENDERARGS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/NodeMetadata.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

//This controls how many frames a plug-in can pre-fetch (per view and per input)
//This is to avoid cases where the user would for example use the FrameBlend node with a huge amount of frames so that they
//do not all stick altogether in memory
#define NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING 4

NATRON_NAMESPACE_ENTER

typedef std::map<EffectInstancePtr, RectD> RoIMap; // RoIs are in canonical coordinates
typedef std::map<ViewIdx, std::vector<RangeD> > FrameRangesMap;
typedef std::map<int, FrameRangesMap> FramesNeededMap;
typedef std::shared_ptr<FramesNeededMap> FramesNeededMapPtr;

struct InputMatrix
{
    EffectInstancePtr newInputEffect;
    Transform::Matrix3x3Ptr cat;
    int newInputNbToFetchFrom;
};

typedef std::map<int, InputMatrix> InputMatrixMap;
typedef std::shared_ptr<InputMatrixMap> InputMatrixMapPtr;


class NodeFrameRequest;

/**
 * @brief A node and the key of the context it renders under: the hash of the context restricted to
 * what the node's output can read (Node::getUpstreamContextReadSet()). Key 0, the empty context, is
 * the node's one entry when nothing upstream of it reads a context variable. Converts from a bare
 * node with key 0, for callers without a context.
 **/
struct NodeCtxKey {
    NodePtr node;
    U64 ctxKey = 0;

    NodeCtxKey() = default;

    NodeCtxKey(const NodePtr& node_,
               U64 ctxKey_ = 0)
        : node(node_)
        , ctxKey(ctxKey_)
    {
    }

    bool operator<(const NodeCtxKey& other) const
    {
        if (node != other.node) {
            return node < other.node;
        }

        return ctxKey < other.ctxKey;
    }

    bool operator==(const NodeCtxKey& other) const
    {
        return (node == other.node) && (ctxKey == other.ctxKey);
    }
};

/**
 * @brief The frame args of each (node, nonzero context key) a request pass reached, each a copy of
 * the node's own frame args carrying that context, its request and its hash.
 **/
typedef std::map<NodeCtxKey, ParallelRenderArgsPtr> ContextFrameArgsMap;
typedef std::shared_ptr<ContextFrameArgsMap> ContextFrameArgsMapPtr;

/**
 * @brief The metadata answers one frame args entry keeps for the times and views its frame reads
 * the node at, keyed by (time, view).
 *
 * Frame args are copied to make an entry under another context, whose hash and answers differ, so
 * a copy starts empty and an assignment empties the target.
 **/
class RetainedFrameMetadata {
public:
    RetainedFrameMetadata() = default;

    RetainedFrameMetadata(const RetainedFrameMetadata&)
    {
    }

    RetainedFrameMetadata& operator=(const RetainedFrameMetadata& other)
    {
        if (this != &other) {
            std::lock_guard<std::mutex> l(mutex);
            answers.clear();
        }

        return *this;
    }

    std::mutex mutex;
    std::map<std::pair<double, int>, NodeMetadataConstPtr> answers;
};

/**
 * @brief Thread-local arguments given to render a frame by the tree.
 * This is different than the RenderArgs because it is not local to a
 * renderRoI call but to the rendering of a whole frame.
 **/
class ParallelRenderArgs
{
public:
    // Developer note: the fields were reordered to optimize packing.
    // see http://www.catb.org/esr/structure-packing/

    ///The initial time requested to render.
    ///This may be different than the time held in RenderArgs
    ///which are local to a renderRoI call whilst this is local
    ///to a frame being rendered by the tree.
    double time;

    ///To check the current time on the timeline
    const TimeLine* timeline;

    ///The hash of the node at the time we started rendering
    U64 nodeHash;

    /// The node's metadata at time and view, derived under nodeHash when the frame was set up and
    /// read by the render instead of the live answer (see EffectInstance::getRenderMetadata()).
    /// Null when the frame was set up without walking upstream.
    NodeMetadataConstPtr metadata;

    /// The context the node renders under, restricted to what its output can read; null for the
    /// empty context. contextKey is its hash, already folded into nodeHash.
    RenderContextPtr context;
    U64 contextKey = 0;

    /// The frame args of the nodes rendering under a nonzero context key this frame, set when the
    /// request pass reached any, so that an input pulled under a context finds its own.
    std::weak_ptr<const ContextFrameArgsMap> contextArgs;

    /// The node's answers under nodeHash at the other times and views the frame read it at, kept
    /// from the first read so every later one in the frame agrees (see
    /// EffectInstance::getRenderMetadata()). Shared by the threads rendering the frame.
    RetainedFrameMetadata retainedMetadata;

    ///If set, contains data for all frame/view pair that are going to be computed
    ///for this frame/view pair with the overall RoI to avoid rendering several times with this node.
    NodeFrameRequestPtr request;

    ///The initial view requested to render.
    ///This may be different than the view held in RenderArgs
    ///which are local to a renderRoI call whilst this is local
    ///to a frame being rendered by the tree.
    ViewIdx view;

    ///A number identifying the current frame render to determine if we can really abort for abortable renders
    AbortableRenderInfoWPtr abortInfo;

    ///A pointer to the node that requested the current render.
    NodePtr treeRoot;

    ///In the tree originating from the root treeRoot, how many times this node is visited ? this is used to determine
    ///if we should cache the output and whether we should do GPU rendering or not
    int visitsCount;

    ///List of the nodes in the rotopaint tree
    NodesList rotoPaintNodes;

    ///Various stats local to the render of a frame
    RenderStatsPtr stats;

    ///The OpenGL context to use for the render of this frame
    OSGLContextWPtr openGLContext;

    ///The texture index of the viewer being rendered, only useful for abortable renders
    int textureIndex;

    ///Current thread safety: it might change in the case of the rotopaint: while drawing, the safety is instance safe,
    ///whereas afterwards we revert back to the plug-in thread safety
    RenderSafetyEnum currentThreadSafety;

    ///Current OpenGL support: it might change during instanceChanged action
    PluginOpenGLRenderSupport currentOpenglSupport;

    /// is this a render due to user interaction ? Generally this is true when rendering because
    /// of a user parameter tweek or timeline seek, or more generally by calling RenderEngine::renderCurrentFrame
    bool isRenderResponseToUserInteraction : 1;

    /// Is this render sequential ? True for Viewer playback or a sequential writer such as WriteFFMPEG
    bool isSequentialRender : 1;

    ///Was the render started in the instanceChangedAction (knobChanged)
    bool isAnalysis : 1;

    /// Installed only for the node's own instance-changed action: no render is running, so code
    /// that reads a render's state (its context, its time) reads as it would outside one.
    bool isInstanceChangedAction : 1;

    ///If true, the attached paint stroke is being drawn currently
    bool isDuringPaintStrokeCreation : 1;

    ///When true, this is a hint for plug-ins that the render will be used for draft such as previewing while scrubbing the timeline
    bool draftMode : 1;

    ///The support for tiles is local to a render and may change depending on GPU usage or other parameters
    bool tilesSupported : 1;

    /// Whether this node or anything upstream of it is frame varying or animated; only meaningful if frameVaryingComputed
    bool isFrameVaryingOrAnimated : 1;
    bool frameVaryingComputed : 1;

    ParallelRenderArgs();

    bool isCurrentFrameRenderNotAbortable() const;
};

struct FrameViewPair
{
    double time;
    ViewIdx view;
};

struct FrameViewRequestGlobalData
{
    ///The transforms associated to each input branch, set on first request
    InputMatrixMapPtr transforms;
    std::shared_ptr<std::map<int, EffectInstancePtr> > reroutesMap;

    ///The required frame/views in input, set on first request
    FramesNeededMap frameViewsNeeded;

    ///Set when the first request is made, set on first request
    RectD rod;
    bool isProjectFormat;

    //Identity data, set on first request
    bool isIdentity;
    int identityInputNb;
    ViewIdx identityView;
    double inputIdentityTime;

    /// For an entry under a nonzero context key, the node's metadata at this frame/view under that
    /// context, read with the entry's hash; null otherwise, or when the node moved meanwhile.
    NodeMetadataConstPtr metadata;
};

struct FrameViewRequestFinalData
{
    RectD finalRoi;
};

struct FrameViewPerRequestData
{
    RoIMap inputsRoi;
};

struct FrameViewRequest
{
    /**
     * @brief One renderRoI call this frame/view makes on an upstream frame/view before rendering. An identity
     * frame/view has a single edge to the frame/view it forwards its render to, with inputNb the identity input
     * (-2 when identity of itself at another time).
     **/
    struct TaskEdge {
        NodeWPtr node;
        double time;
        ViewIdx view;
        unsigned int mipmapLevel;

        /// inputNb of an edge to a node requested under a named context rather than read as an input.
        static const int kContextRequestInputNb = -3;
        int inputNb;

        /// The dependency's own context key, which need not be its consumer's.
        U64 contextKey = 0;
    };

    ///All different requests led by different branches in the tree
    //std::list<std::pair<RectD, FrameViewPerRequestData> > requests;

    ///Final datas that are computed once the whole tree has been cycled through
    FrameViewRequestFinalData finalData;

    ///Global datas for this frame/view set upon first request
    FrameViewRequestGlobalData globalData;

    /// Frames past NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING are left to getImage during the render and have no edge here
    std::vector<TaskEdge> dependencies;

    /// Number of distinct (consumer frame/view, input) edges pointing at this frame/view
    int consumers = 0;

    /// Union of the layers requested from this frame/view by its consumers, including getImage pulls past the pre-fetch cap
    std::list<ImageLayerDesc> componentsRequested;

    /// Every dependency reachable on the first walk of this frame/view has a lower number
    int dfsPostOrder = -1;

    /// Set while the request pass walks what this frame/view depends on: reached again meanwhile,
    /// it depends on itself.
    bool walking = false;
};

struct FrameView_compare_less
{
    bool operator() (const FrameViewPair & lhs,
                     const FrameViewPair & rhs) const
    {
        if (lhs.time < rhs.time) {
            return true;
        } else if (lhs.time > rhs.time) {
            return false;
        } else {
            if (lhs.view < rhs.view) {
                return true;
            } else if (lhs.view > rhs.view) {
                return false;
            } else {
                return false;
            }
        }
    }
};

typedef std::map<FrameViewPair, FrameViewRequest, FrameView_compare_less> NodeFrameViewRequestData;

class NodeFrameRequest
{
public:
    NodeFrameViewRequestData frames;

    ///Set on first request
    U64 nodeHash;
    RenderScale mappedScale;

    /// The context the node renders under for this entry, restricted to what its output can read
    /// (null for the empty context), and its hash, folded into nodeHash.
    RenderContextPtr context;
    U64 contextKey = 0;

    /// Under a nonzero context key, the extra action-cache state the node keeps for this entry.
    std::shared_ptr<void> actionsCacheReservation;

    bool getFrameViewCanonicalRoI(double time, ViewIdx view, RectD* roi) const;

    const FrameViewRequest* getFrameViewRequest(double time, ViewIdx view) const;

    FrameViewRequest* findFrameViewRequest(double time, ViewIdx view);
};

/**
 * @brief The request pass's entries, one per node and context key. A bare node looks up key 0.
 **/
class FrameRequestMap
    : public std::map<NodeCtxKey, NodeFrameRequestPtr> {
public:
    FrameViewRequest* findFrameViewRequest(const NodeCtxKey& node, double time, ViewIdx view);

    const FrameViewRequest* findFrameViewRequest(const NodeCtxKey& node, double time, ViewIdx view) const;

    /**
     * @brief Creates the frame args of each entry with a nonzero context key that has none yet, from
     * the node's own args as baseArgsOf returns them, and points every args baseArgsOf returns for an
     * entry at them. Does nothing when no entry has a nonzero key. Not MT-safe: call it while setting
     * the frame up.
     **/
    void installContextArgs(const std::function<ParallelRenderArgsPtr(const NodePtr&)>& baseArgsOf) const;

    /**
     * @brief The frame args the request pass planned for node under contextKey, or null.
     **/
    ParallelRenderArgsPtr getContextArgs(const NodeCtxKey& key) const;

    int nextDfsPostOrder = 0;

    /// Null until the pass reaches a node under a nonzero context key.
    ContextFrameArgsMapPtr contextArgs;

    /// The nodes that pulled an input under contexts of their own (see
    /// EffectInstance::getInputContexts()), in the order the pass reached them, each with whether
    /// it was reached under the empty context, which makes it an outermost one.
    std::vector<std::pair<NodePtr, bool>> contextFanOuts;

    /// By node, the action-cache states reserved for its entries under nonzero context keys. Each
    /// entry holds its reservation until it is destroyed with the frame.
    std::map<const Node*, int> reservedActionsCacheStates;
};

/**
 * @brief The count of contexts a node may render under in one frame before a warning names the
 * nodes fanning them out, and the most action-cache states a node keeps for them.
 **/
const int kContextCountWarningThreshold = 4096;

/**
 * @brief Makes the effect's input pulls on this thread, for its lifetime, request the inputs under
 * context instead of the context the effect renders under. An effect that fans an input out into
 * several contexts (see EffectInstance::getInputContexts()) pulls each one with getImage() under
 * one of these. Scopes nest; only pulls made by consumer on the constructing thread see it.
 **/
class InputContextScope {
public:
    InputContextScope(const EffectInstance* consumer,
                      const RenderContextPtr& context);
    ~InputContextScope();

    InputContextScope(const InputContextScope&) = delete;
    InputContextScope& operator=(const InputContextScope&) = delete;

    /**
     * @brief Whether a scope for consumer is open on this thread, and if so its context.
     **/
    static bool current(const EffectInstance* consumer,
                        RenderContextPtr* context);
};

/**
 * @brief The context the actions run on this thread answer for, for the scope's lifetime: a node
 * asked for its region of definition, identity, components, metadata or render hash while one is
 * open answers under it restricted to what the node's output reads, whatever frame args the node
 * has installed. Opened by ContextFrameArgsScope and InputContextArgs, so the inputs an effect
 * queries while it renders or is planned under a context answer under that context too, and by
 * the metadata derivation. A null context stands for the empty one. Scopes nest, innermost wins.
 **/
class ActionContextScope {
public:
    ActionContextScope() = default;
    ActionContextScope(bool active,
                       const RenderContextPtr& context);
    ~ActionContextScope();

    ActionContextScope(const ActionContextScope&) = delete;
    ActionContextScope& operator=(const ActionContextScope&) = delete;

    /**
     * @brief Opens the scope, once.
     **/
    void open(const RenderContextPtr& context);

    /**
     * @brief Whether a scope is open on this thread, and if so its context.
     **/
    static bool current(RenderContextPtr* context);

private:
    bool _open = false;
};

/**
 * @brief Pushes args on effect's frame args for its lifetime when args is not null, so the effect
 * renders under args' context, and opens an ActionContextScope for that context; pops them on
 * destruction.
 **/
class ContextFrameArgsScope {
public:
    ContextFrameArgsScope() = default;
    ContextFrameArgsScope(const EffectInstancePtr& effect,
                          const ParallelRenderArgsPtr& args);
    ~ContextFrameArgsScope();

    ContextFrameArgsScope(const ContextFrameArgsScope&) = delete;
    ContextFrameArgsScope& operator=(const ContextFrameArgsScope&) = delete;

    void install(const EffectInstancePtr& effect,
                 const ParallelRenderArgsPtr& args);

private:
    EffectInstancePtr _effect;
    ActionContextScope _actions;
};

/**
 * @brief The context key input renders under when consumer pulls it, and the frame args for it:
 * the context consumer pulls under restricted to what the input's output can read. That context is
 * the one of an open InputContextScope; else, given inputNb, the single context
 * consumer->getInputContexts() names for that input at time/view; else the one consumer renders
 * under. An input getInputContexts() names several contexts for is pulled under the one consumer
 * renders under unless a scope is open. Key 0 and null args when the context is empty, the input's
 * own args then applying unchanged. install() pushes the args on the input for the object's
 * lifetime and opens an ActionContextScope for the input's context (the empty one under key 0).
 **/
class InputContextArgs {
public:
    InputContextArgs(const EffectInstance* consumer,
                     const EffectInstancePtr& input,
                     int inputNb = -1,
                     double time = 0.,
                     ViewIdx view = ViewIdx(0));

    U64 contextKey() const
    {
        return _contextKey;
    }

    const ParallelRenderArgsPtr& args() const
    {
        return _args;
    }

    void install();

private:
    EffectInstancePtr _input;
    ParallelRenderArgsPtr _args;
    U64 _contextKey = 0;
    ContextFrameArgsScope _scope;
    ActionContextScope _emptyActions;
    bool _installed = false;
};

/**
 * @brief context restricted to what node's output can read, written to restricted when given
 * (null when empty), and its hash: the node's context key under context. Returns 0 without
 * computing node's read-set when context is null or empty.
 **/
U64 restrictContextToNode(const RenderContextPtr& context,
                          const NodePtr& node,
                          RenderContextPtr* restricted);

/**
 * @brief Per-render context is captured up front into thread-local storage
 * rather than read from live node state while rendering, so any new render
 * entry point must install this setter or renders will see inconsistent
 * state.
 **/
class ParallelRenderArgsSetter
{
    std::shared_ptr<std::map<NodePtr, ParallelRenderArgsPtr> > argsMap;
    NodesList nodes;

protected:

    OSGLContextWPtr _openGLContext;

public:
    /**
     * @brief Set the TLS for rendering a frame on the tree upstream of treeRoot (including it) and all nodes that
     * can be reached through expressions.
     * We do this because TLS is needed to know the correct frame, view at which the frame is evaluated (i.e rendered)
     * even in nodes that do not belong in the tree. The reason why is because the nodes in the tree may have parameters
     * relying on other nodes that do not belong in the tree through expressions.
     * When setUpstreamArgs is false only treeRoot gets TLS: callers that do not render use this to avoid walking the
     * whole graph, and EffectInstance::getImage installs args on an input lazily if it ends up being pulled.
     **/
    ParallelRenderArgsSetter(double time,
                             ViewIdx view,
                             bool isRenderUserInteraction,
                             bool isSequential,
                             const AbortableRenderInfoPtr& abortInfo,
                             const NodePtr& treeRoot,
                             int textureIndex,
                             const TimeLine* timeline,
                             const NodePtr& activeRotoPaintNode,
                             bool isAnalysis,
                             bool draftMode,
                             const RenderStatsPtr& stats,
                             bool setUpstreamArgs = true);

    ParallelRenderArgsSetter(const std::shared_ptr<std::map<NodePtr, ParallelRenderArgsPtr> >& args);

    typedef std::vector<std::pair<NodePtr, ParallelRenderArgsPtr>> ArgsInstallSequence;

    /**
     * @brief Creates the frame args the first constructor installs, without installing them. out maps each node to
     * the args getParallelRenderArgsTLS() returns once they are installed. installSequence, when given, receives
     * every installation in order, including a node installed twice, and collectedNodes the nodes whose args the
     * destructor invalidates.
     **/
    static void buildArgsMap(double time,
                             ViewIdx view,
                             bool isRenderUserInteraction,
                             bool isSequential,
                             const AbortableRenderInfoPtr& abortInfo,
                             const NodePtr& treeRoot,
                             int textureIndex,
                             const TimeLine* timeline,
                             const NodePtr& activeRotoPaintNode,
                             bool isAnalysis,
                             bool draftMode,
                             const RenderStatsPtr& stats,
                             bool setUpstreamArgs,
                             const OSGLContextPtr& glContext,
                             std::map<NodePtr, ParallelRenderArgsPtr>* out,
                             ArgsInstallSequence* installSequence = 0,
                             NodesList* collectedNodes = 0);

    void updateNodesRequest(const FrameRequestMap& request);

    /**
     * @brief Called by buildArgsMap() on the setting-up thread after each node's metadata is
     * snapshotted, so a test can edit the graph in the middle of a snapshot.
     **/
    typedef std::function<void(const NodePtr& node)> MetadataSnapshotHook;
    static void setMetadataSnapshotHookForTests(const MetadataSnapshotHook& hook);

    /**
     * @brief The frame args the first constructor installed on this thread, each node mapped to the args
     * getParallelRenderArgsTLS() returns for it. Empty when built from an existing args map.
     **/
    const std::map<NodePtr, ParallelRenderArgsPtr>& getInstalledArgs() const
    {
        return _installedArgs;
    }

    virtual ~ParallelRenderArgsSetter();

private:
    std::map<NodePtr, ParallelRenderArgsPtr> _installedArgs;
};

NATRON_NAMESPACE_EXIT

#endif // PARALLELRENDERARGS_H
