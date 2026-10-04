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

#include <list>
#include <map>
#include <set>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
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

    ///If true, the attached paint stroke is being drawn currently
    bool isDuringPaintStrokeCreation : 1;

    ///When true, all NaNs will be converted to 1
    bool doNansHandling : 1;

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
        int inputNb;
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
                return true;
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

    bool getFrameViewCanonicalRoI(double time, ViewIdx view, RectD* roi) const;

    const FrameViewRequest* getFrameViewRequest(double time, ViewIdx view) const;

    FrameViewRequest* findFrameViewRequest(double time, ViewIdx view);
};

class FrameRequestMap
    : public std::map<NodePtr, NodeFrameRequestPtr> {
public:
    FrameViewRequest* findFrameViewRequest(const NodePtr& node, double time, ViewIdx view);

    const FrameViewRequest* findFrameViewRequest(const NodePtr& node, double time, ViewIdx view) const;

    int nextDfsPostOrder = 0;
};

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

    virtual ~ParallelRenderArgsSetter();
};

NATRON_NAMESPACE_EXIT

#endif // PARALLELRENDERARGS_H
