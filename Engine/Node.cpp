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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "NodePrivate.h"

#include <algorithm> // min, max
#include <atomic>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <set>
#include <sstream> // stringstream
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Global/Macros.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QReadWriteLock>
#include <QTextStream>
#include <QWaitCondition>

#include <SequenceParsing.h>
#include <ofxNatron.h>

#include "Global/FStreamsSupport.h"
#ifdef DEBUG
#include "Global/FloatingPointExceptions.h"
#endif

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Backdrop.h"
#include "Engine/ContentHasher.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/DiskCacheNode.h"
#include "Engine/Dot.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileSystemModel.h"
#include "Engine/Format.h"
#include "Engine/GenericSchedulerThreadWatcher.h"
#include "Engine/GroupInput.h"
#include "Engine/GroupOutput.h"
#include "Engine/Hash64.h"
#include "Engine/Image.h"
#include "Engine/ImageParams.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Log.h"
#include "Engine/Lut.h"
#include "Engine/MemoryInfo.h" // printAsRAM
#include "Engine/NodeGroup.h"
#include "Engine/NodeGuiI.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxHost.h"
#include "Engine/OneViewNode.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/Plugin.h"
#include "Engine/PrecompNode.h"
#include "Engine/Project.h"
#include "Engine/RenderContext.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/Settings.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/Timer.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/UndoCommand.h"
#include "Engine/Utils.h" // convertFromPlainText
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/NativeEffectBase.h"

#include "Serialization/NodeSerialization.h"

#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif

///The flickering of edges/nodes in the nodegraph will be refreshed
///at most every...
#define NATRON_RENDER_GRAPHS_HINTS_REFRESH_RATE_SECONDS 1

// The openfx-misc generator base's own components choice (ofxsGenerator.h); it reads this
// unconditionally, so a generator's layer select must keep it in sync rather than leave it
// as a second, redundant control on the panel.
#define kParamGeneratorOutputComponents "outputComponents"

NATRON_NAMESPACE_ENTER

using std::make_pair;
using std::cout; using std::endl;
using std::shared_ptr;


// protect local classes in anonymous namespace
NATRON_NAMESPACE_ANONYMOUS_ENTER



NATRON_NAMESPACE_ANONYMOUS_EXIT



class RefreshingInputData_RAII
{
    Node::Implementation *_imp;

public:

    RefreshingInputData_RAII(Node::Implementation* imp)
        : _imp(imp)
    {
        ++_imp->isRefreshingInputRelatedData;
    }

    ~RefreshingInputData_RAII()
    {
        --_imp->isRefreshingInputRelatedData;
    }
};


/**
 *@brief Actually converting to ARGB... but it is called BGRA by
   the texture format GL_UNSIGNED_INT_8_8_8_8_REV
 **/
static unsigned int toBGRA(unsigned char r, unsigned char g, unsigned char b, unsigned char a) WARN_UNUSED_RETURN;
unsigned int
toBGRA(unsigned char r,
       unsigned char g,
       unsigned char b,
       unsigned char a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

namespace {
// Set while this thread computes a hash. A hash read from inside it, such as an expression's
// random seed, gets the stored hash: recomputing there would nest node locks in no fixed order.
thread_local int hashComputeDepth = 0;

class HashComputeScope {
public:
    HashComputeScope() { ++hashComputeDepth; }

    ~HashComputeScope() { --hashComputeDepth; }
};

// Set while the main thread refreshes a node's input-related data. The refresh orders the nodes
// it reaches itself, inputs first, so a reader inside it takes the data as it stands instead of
// starting another refresh from the middle of this one.
int inputRelatedDataRefreshDepth = 0;

class InputRelatedDataRefreshScope {
public:
    InputRelatedDataRefreshScope() { ++inputRelatedDataRefreshDepth; }

    ~InputRelatedDataRefreshScope() { --inputRelatedDataRefreshDepth; }
};

thread_local int inputRelatedDataRefreshDeferralDepth = 0;

bool
isSkippedByHashInputs(const Node& node)
{
    const EffectInstancePtr effect = node.getEffectInstance();

    return node.isEffectGroup() || dynamic_cast<GroupInput*>(effect.get()) || dynamic_cast<GroupOutput*>(effect.get()) || dynamic_cast<PrecompNode*>(effect.get());
}
} // anonymous namespace

Node::Node(const AppInstancePtr& app,
           const NodeCollectionPtr& group,
           Plugin* plugin)
    : QObject()
    , _imp( new Implementation(this, app, group, plugin) )
{
    QObject::connect( this, SIGNAL(pluginMemoryUsageChanged(qint64)), appPTR, SLOT(onNodeMemoryRegistered(qint64)) );
    QObject::connect(this, SIGNAL(mustDequeueActions()), this, SLOT(dequeueActions()));
    QObject::connect(this, SIGNAL(refreshIdentityStateRequested()), this, SLOT(onRefreshIdentityStateRequestReceived()), Qt::QueuedConnection);

    if (plugin && plugin->getPluginID().startsWith(QLatin1String("com.FXHOME.HitFilm"))) {
        _imp->requiresGLFinishBeforeRender = true;
    }
}

bool
Node::isGLFinishRequiredBeforeRender() const
{
    return _imp->requiresGLFinishBeforeRender;
}

bool
Node::isPartOfProject() const
{
    return _imp->isPartOfProject;
}

void
Node::createRotoContextConditionnally()
{
    assert(!_imp->curveContext);
    assert(_imp->effect);
    const EffectInstance::CurveContextKindEnum kind = _imp->effect->getCurveContextKind();
    if (kind == EffectInstance::eCurveContextKindNone) {
        return;
    }
    _imp->effect->beginChanges();
    if (kind == EffectInstance::eCurveContextKindPaint) {
        _imp->rotoContext = RotoContext::create( shared_from_this() );
        _imp->curveContext = _imp->rotoContext;
    } else {
        _imp->curveContext = RotoCurveContext::create(shared_from_this());
    }
    _imp->effect->endChanges(true);
    _imp->curveContext->createBaseLayer();
}

void
Node::createTrackerContextConditionnally()
{
    assert(!_imp->trackContext);
    assert(_imp->effect);
    ///Initialize the tracker context if any
    if ( _imp->effect->isBuiltinTrackerNode() ) {
        _imp->trackContext = TrackerContext::create( shared_from_this() );
    }
}

const Plugin*
Node::getPlugin() const
{
    return _imp->plugin;
}

void
Node::switchInternalPlugin(Plugin* plugin)
{
    _imp->plugin = plugin;
}

void
Node::setPrecompNode(const PrecompNodePtr& precomp)
{
    //QMutexLocker k(&_imp->pluginsPropMutex);
    _imp->precomp = precomp;
}

PrecompNodePtr
Node::isPartOfPrecomp() const
{
    //QMutexLocker k(&_imp->pluginsPropMutex);
    return _imp->precomp.lock();
}

void
Node::setWhileCreatingPaintStroke(bool creating)
{
    QMutexLocker k(&_imp->lastStrokeMovementMutex);

    _imp->duringPaintStrokeCreation = creating;
}

bool
Node::isDuringPaintStrokeCreation() const
{
    QMutexLocker k(&_imp->lastStrokeMovementMutex);

    return _imp->duringPaintStrokeCreation;
}

void
Node::setRenderThreadSafety(RenderSafetyEnum safety)
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    _imp->currentThreadSafety = safety;
}

RenderSafetyEnum
Node::getCurrentRenderThreadSafety() const
{
    if ( !isMultiThreadingSupportEnabledForPlugin() ) {
        return eRenderSafetyInstanceSafe;
    }
    QMutexLocker k(&_imp->pluginsPropMutex);

    return _imp->currentThreadSafety;
}

void
Node::revertToPluginThreadSafety()
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    _imp->currentThreadSafety = _imp->pluginSafety;
}

void
Node::setCurrentOpenGLRenderSupport(PluginOpenGLRenderSupport support)
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    _imp->currentSupportOpenGLRender = support;
}

PluginOpenGLRenderSupport
Node::getCurrentOpenGLRenderSupport() const
{

    if (_imp->plugin) {
        PluginOpenGLRenderSupport pluginProp = _imp->plugin->getPluginOpenGLRenderSupport();
        if (pluginProp != ePluginOpenGLRenderSupportYes) {
            return pluginProp;
        }
    }

    if (!getApp()->getProject()->isGPURenderingEnabledInProject()) {
        return ePluginOpenGLRenderSupportNone;
    }

    // Ok still turned on, check the value of the opengl support knob in the Node page
    KnobChoicePtr openglSupportKnob = _imp->openglRenderingEnabledKnob.lock();
    if (openglSupportKnob) {
        int index = openglSupportKnob->getValue();
        if (index == 1) {
            return ePluginOpenGLRenderSupportNone;
        } else if (index == 2 && getApp()->isBackground()) {
            return ePluginOpenGLRenderSupportNone;
        }
    }

    // Descriptor returned that it supported OpenGL, let's see if it turned off/on in the instance the OpenGL rendering
    QMutexLocker k(&_imp->pluginsPropMutex);

    return _imp->currentSupportOpenGLRender;
}

void
Node::setCurrentSequentialRenderSupport(SequentialPreferenceEnum support)
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    _imp->currentSupportSequentialRender = support;
}

SequentialPreferenceEnum
Node::getCurrentSequentialRenderSupport() const
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    return _imp->currentSupportSequentialRender;
}

void
Node::setCurrentSupportTiles(bool support)
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    _imp->currentSupportTiles = support;
}

bool
Node::getCurrentSupportTiles() const
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    return _imp->currentSupportTiles;
}

void
Node::setCurrentCanTransform(bool support)
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    _imp->currentCanTransform = support;
}

bool
Node::getCurrentCanTransform() const
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    return _imp->currentCanTransform;
}

void
Node::refreshDynamicProperties()
{
    PluginOpenGLRenderSupport pluginGLSupport = ePluginOpenGLRenderSupportNone;
    if (_imp->plugin) {
        pluginGLSupport = _imp->plugin->getPluginOpenGLRenderSupport();
        if (_imp->plugin->isOpenGLEnabled() && pluginGLSupport == ePluginOpenGLRenderSupportYes) {
            // Ok the plug-in supports OpenGL, figure out now if can be turned on/off by the instance
            pluginGLSupport = _imp->effect->supportsOpenGLRender();
        }
    }


    setCurrentOpenGLRenderSupport(pluginGLSupport);
    bool tilesSupported = _imp->effect->supportsTiles();
    bool multiResSupported = _imp->effect->supportsMultiResolution();
    bool canTransform = _imp->effect->getCanTransform();
    RenderSafetyEnum safety = _imp->effect->renderThreadSafety();
    setRenderThreadSafety(safety);
    setCurrentSupportTiles(multiResSupported && tilesSupported);
    setCurrentSequentialRenderSupport( _imp->effect->getSequentialPreference() );
    setCurrentCanTransform(canTransform);
}

bool
Node::isRenderScaleSupportEnabledForPlugin() const
{
    return _imp->plugin ? _imp->plugin->isRenderScaleEnabled() : true;
}

bool
Node::isMultiThreadingSupportEnabledForPlugin() const
{
    return _imp->plugin ? _imp->plugin->isMultiThreadingEnabled() : true;
}

void
Node::prepareForNextPaintStrokeRender()
{
    {
        QMutexLocker k(&_imp->lastStrokeMovementMutex);
        _imp->strokeBitmapCleared = false;
    }
    _imp->effect->clearActionsCache();
}

void
Node::setLastPaintStrokeDataNoRotopaint()
{
    {
        QMutexLocker k(&_imp->lastStrokeMovementMutex);
        _imp->strokeBitmapCleared = false;
        _imp->duringPaintStrokeCreation = true;
    }
    _imp->effect->setDuringPaintStrokeCreationThreadLocal(true);
}

void
Node::invalidateLastPaintStrokeDataNoRotopaint()
{
    {
        QMutexLocker k(&_imp->lastStrokeMovementMutex);
        _imp->duringPaintStrokeCreation = false;
    }
}

RectD
Node::getPaintStrokeRoD_duringPainting() const
{
    return getApp()->getPaintStrokeWholeBbox();
}

void
Node::getPaintStrokeRoD(double time,
                        RectD* bbox) const
{
    bool duringPaintStroke = _imp->effect->isDuringPaintStrokeCreationThreadLocal();
    QMutexLocker k(&_imp->lastStrokeMovementMutex);

    if (duringPaintStroke) {
        *bbox = getPaintStrokeRoD_duringPainting();
    } else {
        RotoDrawableItemPtr stroke = _imp->paintStroke.lock();
        if (!stroke) {
            throw std::logic_error("");
        }
        *bbox = stroke->getBoundingBox(time);
    }
}

bool
Node::isLastPaintStrokeBitmapCleared() const
{
    QMutexLocker k(&_imp->lastStrokeMovementMutex);

    return _imp->strokeBitmapCleared;
}

void
Node::clearLastPaintStrokeRoD()
{
    QMutexLocker k(&_imp->lastStrokeMovementMutex);

    _imp->strokeBitmapCleared = true;
}

void
Node::getLastPaintStrokePoints(double time,
                               unsigned int mipmapLevel,
                               std::list<std::list<std::pair<Point, double> > >* strokes,
                               int* strokeIndex) const
{
    bool duringPaintStroke;
    {
        QMutexLocker k(&_imp->lastStrokeMovementMutex);
        duringPaintStroke = _imp->duringPaintStrokeCreation;
    }

    if (duringPaintStroke) {
        getApp()->getLastPaintStrokePoints(strokes, strokeIndex);
        //adapt to mipmaplevel if needed
        if (mipmapLevel == 0) {
            return;
        }
        int pot = 1 << mipmapLevel;
        for (std::list<std::list<std::pair<Point, double> > >::iterator it = strokes->begin(); it != strokes->end(); ++it) {
            for (std::list<std::pair<Point, double> >::iterator it2 = it->begin(); it2 != it->end(); ++it2) {
                std::pair<Point, double> &p = *it2;
                p.first.x /= pot;
                p.first.y /= pot;
            }
        }
    } else {
        RotoDrawableItemPtr item = _imp->paintStroke.lock();
        RotoStrokeItem* stroke = dynamic_cast<RotoStrokeItem*>( item.get() );
        assert(stroke);
        if (!stroke) {
            throw std::logic_error("");
        }
        stroke->evaluateStroke(mipmapLevel, time, strokes);
        *strokeIndex = 0;
    }
}

ImagePtr
Node::getOrRenderLastStrokeImage(unsigned int mipmapLevel,
                                 double par,
                                 const ImageLayerDesc& components,
                                 ImageBitDepthEnum depth) const
{
    QMutexLocker k(&_imp->lastStrokeMovementMutex);
    std::list<RectI> restToRender;
    RotoDrawableItemPtr item = _imp->paintStroke.lock();
    RotoStrokeItemPtr stroke = std::dynamic_pointer_cast<RotoStrokeItem>(item);

    assert(stroke);
    if (!stroke) {
        throw std::logic_error("");
    }

    // qDebug() << getScriptName_mt_safe().c_str() << "Rendering stroke: " << _imp->lastStrokeMovementBbox.x1 << _imp->lastStrokeMovementBbox.y1 << _imp->lastStrokeMovementBbox.x2 << _imp->lastStrokeMovementBbox.y2;

    RectD lastStrokeBbox;
    std::list<std::pair<Point, double> > lastStrokePoints;
    double distNextIn = 0.;
    ImagePtr strokeImage;
    getApp()->getRenderStrokeData(&lastStrokeBbox, &lastStrokePoints, &distNextIn, &strokeImage);
    double distToNextOut = stroke->renderSingleStroke(lastStrokeBbox, lastStrokePoints, mipmapLevel, par, components, depth, distNextIn, &strokeImage);

    getApp()->updateStrokeImage(strokeImage, distToNextOut, true);

    return strokeImage;
}

void
Node::refreshAcceptedBitDepths()
{
    assert( QThread::currentThread() == qApp->thread() );
    _imp->effect->addSupportedBitDepth(&_imp->supportedDepths);
}

bool
Node::isNodeCreated() const
{
    return _imp->nodeCreated;
}

bool
Node::setStreamWarningInternal(StreamWarningEnum warning,
                               const QString& message)
{
    assert( QThread::currentThread() == qApp->thread() );
    std::map<Node::StreamWarningEnum, QString>::iterator found = _imp->streamWarnings.find(warning);
    if ( found == _imp->streamWarnings.end() ) {
        _imp->streamWarnings.insert( std::make_pair(warning, message) );

        return true;
    } else {
        if (found->second != message) {
            found->second = message;

            return true;
        }
    }

    return false;
}

void
Node::setStreamWarning(StreamWarningEnum warning,
                       const QString& message)
{
    if ( setStreamWarningInternal(warning, message) ) {
        Q_EMIT streamWarningsChanged();
    }
}

void
Node::setStreamWarnings(const std::map<StreamWarningEnum, QString>& warnings)
{
    bool changed = false;

    for (std::map<StreamWarningEnum, QString>::const_iterator it = warnings.begin(); it != warnings.end(); ++it) {
        changed |= setStreamWarningInternal(it->first, it->second);
    }
    if (changed) {
        Q_EMIT streamWarningsChanged();
    }
}

void
Node::clearStreamWarning(StreamWarningEnum warning)
{
    assert( QThread::currentThread() == qApp->thread() );
    std::map<Node::StreamWarningEnum, QString>::iterator found = _imp->streamWarnings.find(warning);
    if ( ( found == _imp->streamWarnings.end() ) || found->second.isEmpty() ) {
        return;
    }
    found->second.clear();
    Q_EMIT streamWarningsChanged();
}

void
Node::getStreamWarnings(std::map<StreamWarningEnum, QString>* warnings) const
{
    assert( QThread::currentThread() == qApp->thread() );
    *warnings = _imp->streamWarnings;
}

void
Node::declareRotoPythonField()
{
    if (getScriptName_mt_safe().empty()) {
        return;
    }
    assert(_imp->curveContext);
    std::string appID = getApp()->getAppIDString();
    std::string nodeName = getFullyQualifiedName();
    std::string nodeFullName = appID + "." + nodeName;
    std::string err;
    std::string script = nodeFullName + ".roto = " + nodeFullName + ".getRotoContext()\n";
    if ( !appPTR->isBackground() ) {
        getApp()->printAutoDeclaredVariable(script);
    }
    bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0);
    assert(ok);
    if (!ok) {
        throw std::runtime_error("Node::declareRotoPythonField(): interpretPythonScript(" + script + ") failed!");
    }
    _imp->curveContext->declarePythonFields();
}

void
Node::declareTrackerPythonField()
{
    if (getScriptName_mt_safe().empty()) {
        return;
    }

    assert(_imp->trackContext);
    std::string appID = getApp()->getAppIDString();
    std::string nodeName = getFullyQualifiedName();
    std::string nodeFullName = appID + "." + nodeName;
    std::string err;
    std::string script = nodeFullName + ".tracker = " + nodeFullName + ".getTrackerContext()\n";
    if ( !appPTR->isBackground() ) {
        getApp()->printAutoDeclaredVariable(script);
    }
    bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0);
    assert(ok);
    if (!ok) {
        throw std::runtime_error("Node::declareTrackerPythonField(): interpretPythonScript(" + script + ") failed!");
    }
    _imp->trackContext->declarePythonFields();
}

NodeCollectionPtr
Node::getGroup() const
{
    return _imp->group.lock();
}

void
Node::Implementation::appendChild(const NodePtr& child)
{
    QMutexLocker k(&childrenMutex);

    for (NodesWList::iterator it = children.begin(); it != children.end(); ++it) {
        if (it->lock() == child) {
            return;
        }
    }
    children.push_back(child);
}

void
Node::fetchParentMultiInstancePointer()
{
    NodesList nodes = _imp->group.lock()->getNodes();
    NodePtr thisShared = shared_from_this();

    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ( (*it)->getScriptName() == _imp->multiInstanceParentName ) {
            ///no need to store the boost pointer because the main instance lives the same time
            ///as the child
            _imp->multiInstanceParent = *it;
            (*it)->_imp->appendChild(thisShared);
            QObject::connect( it->get(), SIGNAL(inputChanged(int)), this, SLOT(onParentMultiInstanceInputChanged(int)) );
            break;
        }
    }
}

NodePtr
Node::getParentMultiInstance() const
{
    return _imp->multiInstanceParent.lock();
}

bool
Node::isMultiInstance() const
{
    return _imp->isMultiInstance;
}

///Accessed by the serialization thread, but mt safe since never changed
std::string
Node::getParentMultiInstanceName() const
{
    return _imp->multiInstanceParentName;
}

void
Node::getChildrenMultiInstance(NodesList* children) const
{
    QMutexLocker k(&_imp->childrenMutex);

    for (NodesWList::const_iterator it = _imp->children.begin(); it != _imp->children.end(); ++it) {
        children->push_back( it->lock() );
    }
}

U64
Node::getHashValue() const
{
    return getContentHash();
}

U64
Node::getContentHash() const
{
    if (hashComputeDepth == 0) {
        flushHashDirtyMarksBeforeRead(this);
        if (isHashDirty()) {
            const_cast<Node*>(this)->refreshDirtyHashes();
        }
    }

    return getStoredContentHash();
}

U64
Node::getStoredContentHash() const
{
    QReadLocker l(&_imp->contentHashMutex);

    return _imp->contentHash;
}

bool
Node::getHashValueIfUpToDate(U64* hash) const
{
    if (hashComputeDepth == 0) {
        flushHashDirtyMarksBeforeRead(this);
    }
    if (isHashDirty()) {
        return false;
    }
    *hash = getStoredContentHash();

    return true;
}

std::string
Node::getCacheID() const
{
    QMutexLocker k(&_imp->nameMutex);

    return _imp->cacheID;
}

bool
Node::computeHashInternal(U64 projectDigest,
                          const HashInputs& inputs,
                          U64* newContentHash)
{
    *newContentHash = 0;
    if (!_imp->effect) {
        return false;
    }
    if (!_imp->inputsInitialized) {
        qDebug() << "Node::computeHash(): inputs not initialized";
    }

    // Computed before taking contentHashMutex: knob content hashing and input reads must not run
    // under this node's write lock.
    const U64 contentHash = computeContentHash(projectDigest, inputs);

    U64 oldContentHash;
    {
        QWriteLocker l(&_imp->contentHashMutex);

        oldContentHash = _imp->contentHash;
        _imp->contentHash = contentHash;
    }
    *newContentHash = contentHash;

    return oldContentHash != contentHash;
} // Node::computeHashInternal

U64
Node::getKnobsContentDigest(bool* hasExpression) const
{
    // Read before any knob: an edit made meanwhile invalidates after it lands, so it moves the
    // generation past the one stored here and the next call rebuilds.
    const U64 generation = _imp->effect->getKnobsContentGeneration();
    const KnobHolder* holder = _imp->effect.get();

    QMutexLocker l(&_imp->knobsDigestMutex);
    if (!_imp->knobsDigestValid || (_imp->knobsDigestGeneration != generation)) {
        std::vector<Implementation::KnobsDigestEntry>& entries = _imp->knobsDigestEntries;
        entries.clear();
        bool anyExpression = false;
        bool anyLive = false;
        // Hashes are recomputed on render threads too, while the main thread may add or remove knobs.
        const KnobsVec knobs = _imp->effect->getKnobs_mt_safe();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            const KnobIPtr& knob = *it;
            if (!knob || !knob->isRenderHashed()) {
                continue;
            }
            bool knobHasExpression = false;
            bool readsOutsideKnob = false;
            knob->getContentHashDependencies(&knobHasExpression, &readsOutsideKnob);
            anyExpression = anyExpression || knobHasExpression;
            Implementation::KnobsDigestEntry entry;
            entry.name = knob->getName();
            entry.hash = 0;
            // A knob owned by another holder invalidates that holder's generation, not this one,
            // so it is hashed live like a knob that reads another knob or a file.
            if (readsOutsideKnob || (knob->getHolder() != holder)) {
                entry.liveKnob = knob;
                anyLive = true;
            } else {
                entry.hash = knob->getContentHash();
            }
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Implementation::KnobsDigestEntry& a, const Implementation::KnobsDigestEntry& b) {
                      return a.name < b.name;
                  });
        _imp->knobsDigestHasExpression = anyExpression;
        _imp->knobsDigestHasLiveEntries = anyLive;
        _imp->knobsDigestGeneration = generation;
        _imp->knobsDigestValid = true;
        if (!anyLive) {
            ContentHasher h;
            h.appendU64(entries.size());
            for (std::size_t i = 0; i < entries.size(); ++i) {
                h.appendString(entries[i].name);
                h.appendU64(entries[i].hash);
            }
            _imp->knobsDigest = h.finalize();
        }
    }

    *hasExpression = _imp->knobsDigestHasExpression;
    if (!_imp->knobsDigestHasLiveEntries) {
        return _imp->knobsDigest;
    }

    const std::vector<Implementation::KnobsDigestEntry>& entries = _imp->knobsDigestEntries;
    ContentHasher h;
    h.appendU64(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        h.appendString(entries[i].name);
        const KnobIPtr liveKnob = entries[i].liveKnob.lock();
        h.appendU64(liveKnob ? liveKnob->getContentHash() : entries[i].hash);
    }

    return h.finalize();
} // Node::getKnobsContentDigest

U64
Node::computeContentHash(U64 projectDigest,
                         const HashInputs& inputs) const
{
    ContentHasher h;

    h.appendString(getPluginID());
    if (_imp->plugin) {
        h.appendI64(_imp->plugin->getMajorVersion());
        h.appendI64(_imp->plugin->getMinorVersion());
        h.appendU64(_imp->plugin->getBinaryContentStamp());
    } else {
        h.appendBool(false);
    }

    bool hasExpression = false;
    h.appendU64(getKnobsContentDigest(&hasExpression));
    _imp->effect->appendNodeContentHash(h);

    // Expressions resolve thisNode/thisGroup and other nodes by name, so the same expression
    // under another name can evaluate differently.
    h.appendBool(hasExpression);
    if (hasExpression) {
        h.appendString(getFullyQualifiedName());
    }

    // Content hashes are never 0, so 0 marks an unconnected viewer input unambiguously.
    if (inputs.isViewer) {
        for (std::size_t i = 0; i < inputs.bySlot.size(); ++i) {
            h.appendI64(inputs.bySlot[i].first);
            const NodePtr& input = inputs.bySlot[i].second;
            h.appendU64(input ? input->getStoredContentHash() : 0);
        }
    } else {
        for (std::size_t i = 0; i < inputs.bySlot.size(); ++i) {
            h.appendI64(inputs.bySlot[i].first);
            h.appendU64(inputs.bySlot[i].second->getStoredContentHash());
        }
        h.appendU64(inputs.bySlot.size());
    }

    // Roto geometry lives in the context and the items, not in knobs, so it is hashed here. The
    // tags keep absent and present contributions from colliding.
    if (_imp->curveContext) {
        h.appendString("rotoContext");
        h.appendU64(_imp->curveContext->getContentHash());
    }
    RotoDrawableItemPtr strokeItem = _imp->paintStroke.lock();
    if (strokeItem) {
        h.appendString("rotoItem");
        h.appendU64(strokeItem->getContentHash());
    }

    // isNodeDisabled() inherits the disabled state and the lifetime of the enclosing groups, and
    // identity results are cached under this hash, so editing either on a group must move the
    // hashes of the nodes inside it.
    U64 enclosingGroups = 0;
    NodeCollectionPtr collection = getGroup();
    while (NodeGroup* enclosingGroup = dynamic_cast<NodeGroup*>(collection.get())) {
        NodePtr groupNode = enclosingGroup->getNode();
        if (!groupNode) {
            break;
        }
        KnobBoolPtr groupDisabled = groupNode->getDisabledKnob();
        h.appendBool(bool(groupDisabled));
        if (groupDisabled) {
            groupDisabled->appendContentHash(h);
        }
        KnobBoolPtr groupLifetimeEnabled = groupNode->_imp->enableLifeTimeKnob.lock();
        h.appendBool(bool(groupLifetimeEnabled));
        if (groupLifetimeEnabled) {
            groupLifetimeEnabled->appendContentHash(h);
        }
        KnobIntPtr groupLifetime = groupNode->_imp->lifeTimeKnob.lock();
        h.appendBool(bool(groupLifetime));
        if (groupLifetime) {
            groupLifetime->appendContentHash(h);
        }
        ++enclosingGroups;
        collection = groupNode->getGroup();
    }
    h.appendU64(enclosingGroups);

    ProjectPtr project = getApp()->getProject();
    // The project digest leaves the frame range out, so only the nodes that can read it re-key
    // when it changes. An expression may read it through the project's parameters.
    const bool usesProjectFrameRange = hasExpression || _imp->effect->dependsOnProjectFrameRange();
    h.appendBool(usesProjectFrameRange);
    if (usesProjectFrameRange) {
        double first = 0., last = 0.;
        project->getFrameRange(&first, &last);
        h.appendDouble(first);
        h.appendDouble(last);
    }

    h.appendU64(projectDigest ? projectDigest : project->getContentDigest());

    return h.finalize();
} // Node::computeContentHash

bool
Node::dependsOnProjectFrameRange() const
{
    if (!_imp->effect) {
        return false;
    }
    if (_imp->effect->dependsOnProjectFrameRange()) {
        return true;
    }
    const KnobsVec knobs = _imp->effect->getKnobs_mt_safe();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if (!*it) {
            continue;
        }
        for (int d = 0; d < (*it)->getDimension(); ++d) {
            if (!(*it)->getExpression(d).empty()) {
                return true;
            }
        }
    }

    return false;
}

void
Node::getHashDependents(std::vector<NodePtr>* dependents) const
{
    if (!_imp->effect) {
        return;
    }
    const bool isRotoPaint = _imp->effect->isRotoPaintNode();
    // The appended range doubles as the queue of outputs still to look at; entries it drops are
    // compacted away behind the read position.
    std::size_t write = dependents->size();
    appendOutputsWithGroupRedirection(dependents);
    std::vector<const Node*> passedThrough;
    for (std::size_t read = write; read < dependents->size(); ++read) {
        NodePtr output = (*dependents)[read];
        assert(output);
        // The paint tree's own nodes are added from the context below.
        if (isRotoPaint) {
            RotoDrawableItemPtr attachedStroke = output->getAttachedRotoItem();
            if (attachedStroke && (attachedStroke->getContext()->getNode().get() == this)) {
                continue;
            }
        }
        // Inputs read through a group, its GroupInput and GroupOutput nodes and a precomp, so no
        // hash folds these in and they can stay dirty while the nodes below them are recomputed.
        // A dirty mark walk stops at a dirty node, so the nodes past them are listed here too.
        if (isSkippedByHashInputs(*output) && (std::find(passedThrough.begin(), passedThrough.end(), output.get()) == passedThrough.end())) {
            passedThrough.push_back(output.get());
            output->appendOutputsWithGroupRedirection(dependents);
        }
        (*dependents)[write++] = std::move(output);
    }
    dependents->resize(write);

    if (_imp->rotoContext) {
        NodesList paintTree;
        _imp->rotoContext->getRotoPaintTreeNodes(&paintTree);
        dependents->insert(dependents->end(), paintTree.begin(), paintTree.end());
    }

    std::list<std::pair<KnobIPtr, int>> readers;
    getMetadataReaders(&readers);
    for (std::list<std::pair<KnobIPtr, int>>::const_iterator it = readers.begin(); it != readers.end(); ++it) {
        EffectInstance* readerEffect = dynamic_cast<EffectInstance*>(it->first->getHolder());
        NodePtr readerNode = readerEffect ? readerEffect->getNode() : NodePtr();
        if (readerNode && (readerNode.get() != this)) {
            dependents->push_back(readerNode);
        }
    }
}

U32
Node::getInputSlotCount() const
{
    // Hashes are recomputed on render threads while the main thread may grow the inputs.
    QMutexLocker l(&_imp->inputsMutex);

    return (U32)_imp->inputs.size();
}

void
Node::getHashInputs(HashInputs* inputs) const
{
    inputs->isViewer = false;
    inputs->bySlot.clear();
    inputs->metadataSources.clear();
    if (!_imp->effect) {
        return;
    }
    ViewerInstance* isViewer = dynamic_cast<ViewerInstance*>(_imp->effect.get());
    if (isViewer) {
        inputs->isViewer = true;
        int activeInput[2];
        isViewer->getActiveInputs(activeInput[0], activeInput[1]);
        for (int i = 0; i < 2; ++i) {
            inputs->bySlot.push_back(std::make_pair(activeInput[i], getInput(activeInput[i])));
        }
        appendMetadataSources(&inputs->metadataSources, NULL);

        return;
    }
    RotoDrawableItemPtr attachedStroke = _imp->paintStroke.lock();
    NodePtr attachedStrokeContextNode;
    if (attachedStroke) {
        attachedStrokeContextNode = attachedStroke->getContext()->getNode();
    }
    // Unconnected inputs are left out rather than marked: nodes such as Merge grow and keep empty
    // input slots, which must not make an otherwise identical graph hash differently. The RotoPaint
    // node is connected to the internal nodes of its paint tree; hashing it would make every stroke
    // node change with it.
    for (U32 i = 0, n = getInputSlotCount(); i < n; ++i) {
        NodePtr input = getInput(i);
        if (!input || (attachedStroke && (input == attachedStrokeContextNode))) {
            continue;
        }
        inputs->bySlot.push_back(std::make_pair((int)i, std::move(input)));
    }
    appendMetadataSources(&inputs->metadataSources, NULL);
}

bool
Node::knobMetadataReadsFoldIntoHash(const KnobI& knob) const
{
    // A hash-exempt knob is refreshed through its metadata reader registration instead, and may
    // read a node downstream, which as a hash input would make the hash order cyclic.
    return knob.isRenderHashed() || dynamic_cast<const ViewerInstance*>(_imp->effect.get());
}

void
Node::appendMetadataSources(std::vector<NodePtr>* sources,
                            std::set<std::pair<const KnobI*, int>>* reached) const
{
    // A walk looking for a knob must pass through every expression, metadata reads or not.
    if (!_imp->effect || (!reached && !KnobHelper::anyExpressionReadsMetadata())) {
        return;
    }
    std::vector<NodePtr> read;
    // Read before the knobs, so that a change landing during the walk leaves the result stale.
    const U64 generation = KnobHelper::getHashedMetadataSourcesGeneration();
    bool cached = false;
    if (!reached) {
        QMutexLocker l(&_imp->hashedMetadataSourcesMutex);
        if (_imp->hashedMetadataSourcesGeneration == generation) {
            cached = true;
            for (std::size_t i = 0; i < _imp->hashedMetadataSources.size(); ++i) {
                NodePtr source = _imp->hashedMetadataSources[i].lock();
                if (source) {
                    read.push_back(std::move(source));
                }
            }
        }
    }
    if (!cached) {
        std::set<std::pair<const KnobI*, int>> visited;
        std::set<std::pair<const KnobI*, int>>* walked = reached ? reached : &visited;
        const KnobsVec knobs = _imp->effect->getKnobs_mt_safe();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            const KnobHelper* knob = dynamic_cast<const KnobHelper*>(it->get());
            if (!knob || !knobMetadataReadsFoldIntoHash(*knob)) {
                continue;
            }
            for (int i = 0; i < knob->getDimension(); ++i) {
                knob->appendHashedMetadataSources(i, &read, walked);
            }
        }
        // A walk sharing reached with other nodes skips the knobs they walked, so its result is
        // partial.
        if (!reached) {
            QMutexLocker l(&_imp->hashedMetadataSourcesMutex);
            _imp->hashedMetadataSourcesGeneration = generation;
            _imp->hashedMetadataSources.assign(read.begin(), read.end());
        }
    }
    for (std::vector<NodePtr>::iterator it = read.begin(); it != read.end(); ++it) {
        if ((it->get() != this) && (std::find(sources->begin(), sources->end(), *it) == sources->end())) {
            sources->push_back(std::move(*it));
        }
    }
}

bool
Node::hashDependsOn(const Node* node,
                    const KnobI* knob,
                    int dimension) const
{
    // A diamond-shaped graph has exponentially many paths but linearly many nodes.
    std::unordered_set<const Node*> visited;
    std::vector<const Node*> stack;
    std::set<std::pair<const KnobI*, int>> reachedKnobs;
    visited.insert(this);
    stack.push_back(this);
    while (!stack.empty()) {
        const Node* current = stack.back();
        stack.pop_back();
        std::vector<NodePtr> dependencies;
        {
            QMutexLocker l(&current->_imp->inputsMutex);
            for (std::size_t i = 0; i < current->_imp->inputs.size(); ++i) {
                NodePtr in = current->_imp->inputs[i].lock();
                if (in) {
                    dependencies.push_back(in);
                }
            }
        }
        current->appendMetadataSources(&dependencies, knob ? &reachedKnobs : NULL);
        if (knob && (reachedKnobs.find(std::make_pair(knob, dimension)) != reachedKnobs.end())) {
            return true;
        }
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            const Node* dependency = dependencies[i].get();
            if (dependency == node) {
                return true;
            }
            if (visited.insert(dependency).second) {
                stack.push_back(dependency);
            }
        }
    }

    return false;
}

namespace {
std::atomic<U64>&
hashEpoch()
{
    static std::atomic<U64> epoch(1);

    return epoch;
}

// Nodes marked since the last flush, whose dependents are still to be marked. A mark is queued
// rather than walked at once because knob setters mark while holding knob, curve or roto locks,
// under which the walk through outputs and paint trees must not run.
struct HashDirtyEntry {
    NodeWPtr node;
    const Node* address; //< compared, never dereferenced
    bool includeGroupContents; //< whether the nodes inside a group are marked too
};

struct HashDirtyQueue {
    std::mutex entriesMutex; //< a leaf: nothing is taken under it
    std::vector<HashDirtyEntry> entries;
    std::atomic<std::size_t> pending { 0 }; //< entries not yet fully walked
    std::mutex flushMutex; //< held through a flush, so that a reader waits for the marks it must see

    // Reused by every flush, under flushMutex, so that a walk allocates nothing per node.
    U64 pass = 0;
    std::vector<HashDirtyEntry> batch;
    std::vector<NodePtr> toExpand;
    std::vector<NodePtr> dependents;
};

// Deliberately leaked, like the audit: nodes can still be marked during static destruction.
HashDirtyQueue&
hashDirtyQueue()
{
    static HashDirtyQueue* queue = new HashDirtyQueue;

    return *queue;
}

thread_local bool flushingHashDirtyMarks = false;

// The hash mark trace tests install. Off, a mark only loads the flag.
std::atomic<bool> hashMarkTraceOn(false);

struct HashMarkTraceSlot {
    std::mutex mutex; //< held through a call, so that removing the trace waits for one in progress
    Node::HashMarkTrace trace;
};

// Deliberately leaked, like the queue.
HashMarkTraceSlot&
hashMarkTraceSlot()
{
    static HashMarkTraceSlot* slot = new HashMarkTraceSlot;

    return *slot;
}

void
traceHashMark(const Node* node,
              const char* reason,
              const KnobI* knob)
{
    if (!hashMarkTraceOn.load(std::memory_order_relaxed)) {
        return;
    }
    HashMarkTraceSlot& slot = hashMarkTraceSlot();
    std::lock_guard<std::mutex> l(slot.mutex);
    if (slot.trace) {
        slot.trace(node, reason, knob);
    }
}

class FlushingHashDirtyMarksFlag {
public:
    FlushingHashDirtyMarksFlag() { flushingHashDirtyMarks = true; }

    ~FlushingHashDirtyMarksFlag() { flushingHashDirtyMarks = false; }
};
} // anonymous namespace

// One project digest per project and hash epoch for a whole refresh: every node in it then hashes
// the same project state, and a file stamp the digest folds in is taken once. A refresh can cross
// into a precomp's project, hence one entry per project.
struct Node::HashWalk {
    struct Digest {
        ProjectPtr project;
        U64 epoch;
        U64 digest;
    };

    U64 projectDigest(const Node& node,
                      U64 epoch)
    {
        AppInstancePtr app = node.getApp();
        if (!app) {
            return 0;
        }
        ProjectPtr project = app->getProject();
        if (!project) {
            return 0;
        }
        for (std::size_t i = 0; i < digests.size(); ++i) {
            if (digests[i].project == project) {
                // A digest taken before the epoch moved may predate the change that moved it.
                if (digests[i].epoch != epoch) {
                    digests[i].epoch = epoch;
                    digests[i].digest = project->getContentDigest();
                }

                return digests[i].digest;
            }
        }
        Digest entry;
        entry.project = project;
        entry.epoch = epoch;
        entry.digest = project->getContentDigest();
        digests.push_back(entry);

        return entry.digest;
    }

    std::vector<Digest> digests;
};

void
Node::markHashDirty()
{
    markHashDirtyInternal(false, "markHashDirty", 0);
}

void
Node::markHashDirtyInternal(bool includeGroupContents,
                            const char* reason,
                            const KnobI* knob)
{
    // Raised before the generation moves: a walk that finds the node dirty must also see that its
    // mark is pending, or it would stop at the node and leave the dependents clean.
    const bool alreadyQueued = _imp->hashDirtyQueued.exchange(true);
    _imp->hashDirtyGeneration.fetch_add(1);
    invalidateRenderContextReads();
    traceHashMark(this, reason, knob);
    if (alreadyQueued && !includeGroupContents) {
        return;
    }
    NodePtr self = weak_from_this().lock();
    if (!self) {
        // Not owned yet, or being destroyed: in no graph, so there is nothing downstream to mark.
        _imp->hashDirtyQueued.store(false);

        return;
    }
    HashDirtyQueue& queue = hashDirtyQueue();
    std::lock_guard<std::mutex> l(queue.entriesMutex);
    HashDirtyEntry entry;
    entry.node = self;
    entry.address = this;
    entry.includeGroupContents = includeGroupContents;
    queue.entries.push_back(entry);
    ++queue.pending;
}

void
Node::invalidateAllHashes(const char* reason,
                          const KnobI* knob)
{
    hashEpoch().fetch_add(1);
    invalidateRenderContextReads();
    traceHashMark(0, reason, knob);
}

void
Node::setHashMarkTraceForTesting(const HashMarkTrace& trace)
{
    HashMarkTraceSlot& slot = hashMarkTraceSlot();
    std::lock_guard<std::mutex> l(slot.mutex);

    slot.trace = trace;
    hashMarkTraceOn.store(bool(trace));
}

void
Node::flushHashDirtyMarksBeforeRead(const Node* reader)
{
    HashDirtyQueue& queue = hashDirtyQueue();

    if ((queue.pending.load() == 0) || flushingHashDirtyMarks) {
        return;
    }
    {
        // A node's mark only reaches its dependents, and in an acyclic graph none of them is folded
        // into its own hash, so a read of the only node marked leaves the walk to the next read of
        // another one. This is what keeps an edit's own reads (the instance-changed action, the
        // metadata refresh) from walking everything downstream of it. Not while a batch is being
        // walked: its marks may be ones the reader must see.
        std::lock_guard<std::mutex> l(queue.entriesMutex);
        if (!queue.entries.empty() && (queue.pending.load() == queue.entries.size())) {
            bool onlyReader = true;
            for (std::size_t i = 0; i < queue.entries.size(); ++i) {
                if (queue.entries[i].address != reader) {
                    onlyReader = false;
                    break;
                }
            }
            if (onlyReader) {
                return;
            }
        }
    }
    flushHashDirtyMarks();
}

void
Node::flushHashDirtyMarks()
{
    HashDirtyQueue& queue = hashDirtyQueue();

    if ((queue.pending.load() == 0) || flushingHashDirtyMarks) {
        return;
    }
    std::lock_guard<std::mutex> flushLock(queue.flushMutex);
    FlushingHashDirtyMarksFlag flushing;
    std::vector<HashDirtyEntry>& batch = queue.batch;
    std::vector<NodePtr>& toExpand = queue.toExpand;
    std::vector<NodePtr>& dependents = queue.dependents;
    for (;;) {
        batch.clear();
        {
            std::lock_guard<std::mutex> l(queue.entriesMutex);
            batch.swap(queue.entries);
        }
        if (batch.empty()) {
            break;
        }
        // Per batch: a node marked again after a walk went past it must be walked again.
        const U64 pass = ++queue.pass;
        auto visit = [pass](const NodePtr& node) {
            if (node->_imp->hashFlushVisit == pass) {
                return false;
            }
            node->_imp->hashFlushVisit = pass;

            return true;
        };
        auto reach = [&visit, &toExpand](const NodePtr& node) {
            if (!node || !visit(node)) {
                return;
            }
            const U64 previous = node->_imp->hashDirtyGeneration.fetch_add(1);
            traceHashMark(node.get(), "reached by a mark upstream", 0);
            const bool wasDirty = (previous != node->_imp->hashCleanGeneration.load()) || (hashEpoch().load() != node->_imp->hashCleanEpoch.load());
            // Whatever folds in a dirty node is dirty too, except below a node whose own mark is
            // still pending, so the walk stops at dirty nodes that are not pending.
            if (!wasDirty || node->_imp->hashDirtyQueued.load()) {
                toExpand.push_back(node);
            }
        };
        for (std::size_t i = 0; i < batch.size(); ++i) {
            NodePtr root = batch[i].node.lock();
            if (!root) {
                continue;
            }
            root->_imp->hashDirtyQueued.store(false);
            // Walking from a root does not count as reaching it: a read of the only node marked
            // recomputes it with its mark still queued, so a root may be clean, and another root
            // of the batch it folds in must still mark it. A root reached already was walked from.
            if ((root->_imp->hashFlushVisit != pass) && (root->_imp->hashFlushRootVisit != pass)) {
                root->_imp->hashFlushRootVisit = pass;
                toExpand.push_back(root);
            }
            if (batch[i].includeGroupContents) {
                NodeGroup* isGroup = dynamic_cast<NodeGroup*>(root->_imp->effect.get());
                if (isGroup) {
                    NodesList inner;
                    isGroup->getNodes_recursive(inner, false);
                    for (NodesList::const_iterator it = inner.begin(); it != inner.end(); ++it) {
                        reach(*it);
                    }
                }
            }
            while (!toExpand.empty()) {
                NodePtr node = std::move(toExpand.back());
                toExpand.pop_back();
                dependents.clear();
                node->getHashDependents(&dependents);
                for (std::size_t d = 0; d < dependents.size(); ++d) {
                    reach(dependents[d]);
                }
            }
        }
        const std::size_t walked = batch.size();
        // Nodes are not kept alive past the flush by the reused containers.
        batch.clear();
        dependents.clear();
        queue.pending.fetch_sub(walked);
    }
} // Node::flushHashDirtyMarks

U64
Node::getHashMarkCount() const
{
    // A queued mark has not reached this node's generation yet.
    if (hashComputeDepth == 0) {
        flushHashDirtyMarksBeforeRead(this);
    }

    // Both only grow, so the sum stays put only while neither moves.
    return _imp->hashDirtyGeneration.load() + hashEpoch().load();
}

CleanHashSnapshot::CleanHashSnapshot(const NodePtr& node,
                                     U64 contextKey)
    : _node(node)
    , _contextKey(contextKey)
    , _marks(node ? node->getHashMarkCount() : 0)
{
}

bool
CleanHashSnapshot::allowsStoreUnder(U64 hash) const
{
    if (!_node) {
        return false;
    }
    // The hash check comes first: a recompute it relies on may have read a value whose write
    // has not marked yet, and that write began before the in-flight check below.
    U64 current = 0;
    if (!_node->getHashValueIfUpToDate(&current) || (mixRenderContextKey(current, _contextKey) != hash)) {
        return false;
    }
    if (!KnobContentWrite::noneInFlight()) {
        return false;
    }

    return _node->getHashMarkCount() == _marks;
}

bool
Node::isHashDirty() const
{
    return (_imp->hashDirtyGeneration.load() != _imp->hashCleanGeneration.load()) || (hashEpoch().load() != _imp->hashCleanEpoch.load());
}

bool
Node::recomputeHashIfDirty(bool requireCleanInputs,
                           HashWalk* walk)
{
    if (!_imp->effect) {
        return true;
    }
    bool changed = false;
    U64 contentHash = 0;
    {
        QMutexLocker k(&_imp->hashRecomputeMutex);
        // Both are read before anything the hashes cover, so an edit landing from here on leaves
        // the node dirty.
        const U64 generation = _imp->hashDirtyGeneration.load();
        const U64 epoch = hashEpoch().load();
        if ((generation == _imp->hashCleanGeneration.load()) && (epoch == _imp->hashCleanEpoch.load())) {
            return true;
        }
        // Gathered and checked after the generation is read: an input marked or reconnected after
        // this marks this node again, while one dirty now would be folded in with its stale hash.
        // The inputs checked are the ones hashed.
        HashInputs inputs;
        getHashInputs(&inputs);
        bool inputsClean = true;
        for (std::size_t i = 0; inputsClean && (i < inputs.bySlot.size()); ++i) {
            const NodePtr& input = inputs.bySlot[i].second;
            inputsClean = !input || !input->isHashDirty();
        }
        for (std::size_t i = 0; inputsClean && (i < inputs.metadataSources.size()); ++i) {
            inputsClean = !inputs.metadataSources[i]->isHashDirty();
        }
        if (!inputsClean && requireCleanInputs) {
            return false;
        }
        {
            HashComputeScope computing;
            changed = computeHashInternal(walk->projectDigest(*this, epoch), inputs, &contentHash);
        }
        // Folded over a dirty input, the hash may stand for a state the graph has left, so the
        // node stays dirty: a mark flush stops at a dirty node that is not queued, on the promise
        // that everything folding it in is dirty too, and a clean node here would keep that stale
        // hash, and its mark count, through the edits that flush skips.
        if (inputsClean) {
            _imp->hashCleanEpoch.store(epoch);
            _imp->hashCleanGeneration.store(generation);
        }
    }

    // Run outside the recompute lock, since they take the effect's and knobs' locks. The cache entries
    // made under the node's other keys are left to the caches' LRU: a content key names one state only, so they stay valid for a return to it.
    if (changed) {
        _imp->effect->onNodeHashChanged(contentHash);
    }

    return true;
} // Node::recomputeHashIfDirty

void
Node::refreshDirtyHashes()
{
    // Another attempt is only needed when a concurrent mark lands mid-refresh. The last one
    // accepts dirty inputs, so that a read under contention or in a cycle still gets a hash, but
    // leaves the nodes that folded one in dirty.
    const int attempts = 4;
    NodePtr self = weak_from_this().lock();

    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!isHashDirty()) {
            return;
        }
        const bool lastAttempt = (attempt + 1 == attempts);
        HashWalk walk;
        if (!self) {
            recomputeHashIfDirty(false, &walk);

            return;
        }

        // Depth-first post-order over the dirty inputs, so every node comes after the inputs it
        // folds in. Clean inputs are not entered: marking a node marks everything that folds it
        // in, so nothing above a clean node is dirty.
        std::vector<NodePtr> order;
        std::unordered_map<const Node*, char> state; // 1: entered, 2: ordered
        std::vector<std::pair<NodePtr, bool>> stack(1, std::make_pair(self, false));
        HashInputs inputs;
        while (!stack.empty()) {
            NodePtr node = stack.back().first;
            if (stack.back().second) {
                stack.pop_back();
                state[node.get()] = 2;
                order.push_back(node);
                continue;
            }
            char& nodeState = state[node.get()];
            if (nodeState != 0) {
                // Reached again through another path, or through a cycle.
                stack.pop_back();
                continue;
            }
            nodeState = 1;
            stack.back().second = true;
            node->getHashInputs(&inputs);
            for (std::size_t i = 0; i < inputs.bySlot.size(); ++i) {
                const NodePtr& input = inputs.bySlot[i].second;
                if (input && input->isHashDirty() && (state.find(input.get()) == state.end())) {
                    stack.push_back(std::make_pair(input, false));
                }
            }
            for (std::size_t i = 0; i < inputs.metadataSources.size(); ++i) {
                const NodePtr& source = inputs.metadataSources[i];
                if (source->isHashDirty() && (state.find(source.get()) == state.end())) {
                    stack.push_back(std::make_pair(source, false));
                }
            }
        }

        bool complete = true;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (!order[i]->recomputeHashIfDirty(!lastAttempt, &walk)) {
                complete = false;
                break;
            }
        }
        if (complete) {
            return;
        }
    }
} // Node::refreshDirtyHashes

bool
Node::refreshHashNow()
{
    const U64 before = getStoredContentHash();

    markHashDirty();

    return getHashValue() != before;
}

void
Node::onKnobContentChanged(const KnobI* knob)
{
    // isNodeDisabled() of the nodes inside a group reads the group's disabled and lifetime knobs.
    bool groupState = false;
    if (knob && dynamic_cast<NodeGroup*>(_imp->effect.get())) {
        groupState = (knob == _imp->disableNodeKnob.lock().get()) || (knob == _imp->lifeTimeKnob.lock().get()) || (knob == _imp->enableLifeTimeKnob.lock().get());
    }
    markHashDirtyInternal(groupState, "knob content changed", knob);
}

bool
Node::mayHaveKnobExpressions() const
{
    QMutexLocker l(&_imp->knobsDigestMutex);

    return !_imp->knobsDigestValid || _imp->knobsDigestHasExpression;
}

void
Node::addMetadataReader(const KnobIPtr& knob,
                        int dimension)
{
    if (!knob) {
        return;
    }
    QMutexLocker l(&_imp->metadataReadersMutex);
    std::list<std::pair<KnobIWPtr, int>>& readers = _imp->metadataReaders;
    for (std::list<std::pair<KnobIWPtr, int>>::iterator it = readers.begin(); it != readers.end();) {
        KnobIPtr reader = it->first.lock();
        if (!reader) {
            it = readers.erase(it);
            continue;
        }
        if ((reader == knob) && (it->second == dimension)) {
            return;
        }
        ++it;
    }
    readers.push_back(std::make_pair(KnobIWPtr(knob), dimension));
}

void
Node::removeMetadataReader(const KnobI* knob,
                           int dimension)
{
    QMutexLocker l(&_imp->metadataReadersMutex);
    std::list<std::pair<KnobIWPtr, int>>& readers = _imp->metadataReaders;

    for (std::list<std::pair<KnobIWPtr, int>>::iterator it = readers.begin(); it != readers.end();) {
        KnobIPtr reader = it->first.lock();
        if (!reader || ((reader.get() == knob) && (it->second == dimension))) {
            it = readers.erase(it);
        } else {
            ++it;
        }
    }
}

void
Node::getMetadataReaders(std::list<std::pair<KnobIPtr, int>>* readers) const
{
    QMutexLocker l(&_imp->metadataReadersMutex);

    for (std::list<std::pair<KnobIWPtr, int>>::const_iterator it = _imp->metadataReaders.begin(); it != _imp->metadataReaders.end(); ++it) {
        KnobIPtr reader = it->first.lock();
        if (reader) {
            readers->push_back(std::make_pair(reader, it->second));
        }
    }
}

void
Node::onMetadataMayHaveChanged()
{
    refreshMetadataReaders(false);
}

void
Node::onMetadataChangedUnderSameHash()
{
    // The nodes downstream derive their metadata from this one's under hashes that do not move
    // either, so their caches and the expressions reading them are as stale as this node's.
    std::unordered_set<const Node*> visited;
    std::vector<NodePtr> toVisit;
    visited.insert(this);
    appendOutputsWithGroupRedirection(&toVisit);
    while (!toVisit.empty()) {
        NodePtr output = std::move(toVisit.back());
        toVisit.pop_back();
        if (!output || !visited.insert(output.get()).second) {
            continue;
        }
        EffectInstancePtr effect = output->getEffectInstance();
        if (effect) {
            effect->dropCachedOutputMetadata();
            effect->dropCachedMetadata();
        }
        output->_imp->metadataRevision.fetch_add(1);
        output->refreshMetadataReaders(true);
        output->appendOutputsWithGroupRedirection(&toVisit);
    }
    if (_imp->effect) {
        _imp->effect->dropCachedMetadata();
    }
    _imp->metadataRevision.fetch_add(1);
    refreshMetadataReaders(true);
}

U64
Node::getMetadataRevision() const
{
    return _imp->metadataRevision.load();
}

void
Node::refreshMetadataReaders(bool sameHash)
{
    std::list<std::pair<KnobIPtr, int>> readers;
    getMetadataReaders(&readers);
    // This node's render-hashed knobs reading its metadata are not registered as readers: a hash
    // change already drops their results. A change under the same hash is not one.
    if (sameHash && _imp->effect && KnobHelper::anyExpressionReadsMetadata()) {
        const KnobsVec knobs = _imp->effect->getKnobs_mt_safe();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            if (!*it || !(*it)->isRenderHashed()) {
                continue;
            }
            for (int i = 0; i < (*it)->getDimension(); ++i) {
                std::list<NodePtr> sources;
                (*it)->getMetadataDependencies(i, &sources);
                if (std::find_if(sources.begin(), sources.end(), [this](const NodePtr& source) { return source.get() == this; }) != sources.end()) {
                    (*it)->clearExpressionsResults(i);
                    (*it)->invalidateContentHash();
                }
            }
        }
    }
    if (readers.empty()) {
        return;
    }
    // Done here rather than on the main thread: a render reading the expression before the queued
    // refresh runs must not be served the old result, nor reuse renders keyed on the old hash.
    for (std::list<std::pair<KnobIPtr, int>>::const_iterator it = readers.begin(); it != readers.end(); ++it) {
        it->first->clearExpressionsResults(it->second);
        // A knob's content hash leaves out its own node's hash, but not the metadata revision.
        if (sameHash || (it->first->getHolder() != _imp->effect.get())) {
            it->first->invalidateContentHash();
        }
    }

    // Evaluating a knob change runs actions and requests renders, which a hash recompute on a
    // render thread must not do.
    if (!qApp || _imp->metadataReadersRefreshRequested.exchange(true)) {
        return;
    }
    NodeWPtr weakSelf = weak_from_this();
    QMetaObject::invokeMethod(qApp, [weakSelf]() {
        NodePtr node = weakSelf.lock();
        if (!node) {
            return;
        }
        node->_imp->metadataReadersRefreshRequested.store(false);
        std::list<std::pair<KnobIPtr, int>> readers;
        node->getMetadataReaders(&readers);
        for (std::list<std::pair<KnobIPtr, int>>::const_iterator it = readers.begin(); it != readers.end(); ++it) {
            it->first->evaluateValueChange(it->second, it->first->getCurrentTime(), ViewSpec::all(), eValueChangedReasonSlaveRefresh);
        } }, Qt::QueuedConnection);
}

void
Node::removeAllImagesFromCache(bool blocking)
{
    AppInstancePtr app = getApp();

    if (!app) {
        return;
    }
    ProjectPtr proj = app->getProject();

    if ( proj->isProjectClosing() || proj->isLoadingProject() ) {
        return;
    }
    appPTR->removeAllCacheEntriesForHolder(this, blocking);
}

void
Node::computeHash()
{
    markHashDirty();
}

namespace {
// One node whose data-kind resolution is in progress on this thread. A resolution recurses into
// other nodes' getEffectiveOutputDataKind(), which can come back to a node already being resolved:
// a node's own policy may consult a neighbour, and that neighbour's resolution reaches back. The
// stack detects that without threading an extra parameter through every recursive call.
struct DataKindResolutionFrame {
    const Node* node;
    bool reachedThroughCycle;

    explicit DataKindResolutionFrame(const Node* node_)
        : node(node_)
        , reachedThroughCycle(false)
    {
    }
};
}

static thread_local std::vector<DataKindResolutionFrame> dataKindResolutionStack;

// Truncating a resolution loop yields an answer that depends on which node the query started from,
// so nothing currently being resolved may be memoized: caching one of those would freeze one entry
// point's answer into the graph and make every later read depend on the order nodes were queried.
static void
markDataKindResolutionCycle()
{
    for (std::vector<DataKindResolutionFrame>::iterator it = dataKindResolutionStack.begin(); it != dataKindResolutionStack.end(); ++it) {
        it->reachedThroughCycle = true;
    }
}

// A node owning its resolution policy decides for itself which of its inputs its output kind
// follows. Everything else -- every OFX plugin, every built-in pass-through -- passes through.
static bool
nodePropagatesDataKindThroughInput(const NodePtr& node,
                                   int inputNb)
{
    if (!node) {
        return false;
    }
    NativeEffectBase* isNative = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

    return !isNative || isNative->inputParticipatesInDataKindPropagation(inputNb);
}

// The conversions the engine is willing to insert without a node in the graph to show for it. A
// fixed table rather than a registry on purpose: adding a row widens what the type system permits
// everywhere at once, which is a design decision taken here, not a capability a node -- or a
// plugin -- can grant itself by answering a virtual. Raw has no row: demosaicing and camera colour
// are user-visible nodes with their own parameters, never a silent conversion.
static bool
isRegisteredAdapter(DataKindEnum from,
                    DataKindEnum to)
{
    return (from == eDataKindDeep) && (to == eDataKindImage);
}

// Raw and scene reach an image input only at the Viewer, which feeds nothing downstream: it shows
// raw values as they are for display (RawDisplay) and hands a scene to its 3D view instead of
// rendering it as an image. They are kept out of the table above so that no other image input,
// whatever its virtual answers, can take raw without a RawToImage, or a scene at all.
static bool
isViewerOnlyAdapter(const EffectInstancePtr& effect,
                    DataKindEnum from,
                    DataKindEnum to)
{
    return ((from == eDataKindRaw) || (from == eDataKindScene)) && (to == eDataKindImage) && dynamic_cast<const ViewerInstance*>(effect.get());
}

static bool
inputAcceptsViaAdapter(const EffectInstancePtr& effect,
                       int inputNb,
                       DataKindEnum from,
                       DataKindEnum required)
{
    return (isRegisteredAdapter(from, required) || isViewerOnlyAdapter(effect, from, required)) && effect->inputAcceptsDataKindViaAdapter(inputNb, from);
}

// The node whose consumers receive what node delivers. A GroupOutput has none inside its group:
// what reaches it leaves through the group node, so the group node's consumers are the ones.
static NodePtr
dataKindDeliveringNode(const Node* node)
{
    if (dynamic_cast<GroupOutput*>(node->getEffectInstance().get())) {
        NodeGroup* group = dynamic_cast<NodeGroup*>(node->getGroup().get());
        NodePtr groupNode = group ? group->getNode() : NodePtr();
        if (groupNode) {
            return groupNode;
        }
    }

    return std::const_pointer_cast<Node>(node->shared_from_this());
}

DataKindEnum
Node::getEffectiveOutputDataKind(bool* isAmbiguous) const
{
    if (isAmbiguous) {
        *isAmbiguous = false;
    }

    DataKindEnum declared = _imp->effect->getOutputDataKind();

    if (declared != eDataKindPolymorphic) {
        return declared;
    }

    {
        QMutexLocker l(&_imp->effectiveDataKindMutex);
        if (_imp->effectiveDataKindCacheSet) {
            if (isAmbiguous) {
                *isAmbiguous = _imp->effectiveDataKindCacheAmbiguous;
            }

            return _imp->effectiveDataKindCache;
        }
    }

    for (std::vector<DataKindResolutionFrame>::const_iterator it = dataKindResolutionStack.begin(); it != dataKindResolutionStack.end(); ++it) {
        if (it->node == this) {
            markDataKindResolutionCycle();

            return eDataKindPolymorphic;
        }
    }
    dataKindResolutionStack.push_back(DataKindResolutionFrame(this));
    bool ambiguous = false;
    DataKindEnum resolved;
    NativeEffectBase* isNative = dynamic_cast<NativeEffectBase*>(_imp->effect.get());
    if (isNative) {
        resolved = isNative->resolveOutputDataKind(&ambiguous);
    } else {
        resolved = resolveStructuralOutputDataKind(&ambiguous);
    }
    const bool reachedThroughCycle = dataKindResolutionStack.back().reachedThroughCycle;
    dataKindResolutionStack.pop_back();

    if (!reachedThroughCycle) {
        QMutexLocker l(&_imp->effectiveDataKindMutex);
        _imp->effectiveDataKindCache = resolved;
        _imp->effectiveDataKindCacheAmbiguous = ambiguous;
        _imp->effectiveDataKindCacheSet = true;
    }

    if (isAmbiguous) {
        *isAmbiguous = ambiguous;
    }

    return resolved;
} // getEffectiveOutputDataKind

void
Node::DataKindConstraint::merge(const Node::DataKindConstraint& other)
{
    if (ambiguous) {
        return;
    }
    if (other.ambiguous) {
        ambiguous = true;
        kind = eDataKindPolymorphic;

        return;
    }
    if (other.kind == eDataKindPolymorphic) {
        return;
    }
    if (kind == eDataKindPolymorphic) {
        kind = other.kind;

        return;
    }
    if (kind != other.kind) {
        ambiguous = true;
        kind = eDataKindPolymorphic;
    }
} // Node::DataKindConstraint::merge

Node::DataKindConstraint
Node::effectiveDataKindConstraintOf(const NodePtr& node)
{
    DataKindConstraint constraint;

    if (node) {
        bool ambiguous = false;
        constraint.kind = node->getEffectiveOutputDataKind(&ambiguous);
        constraint.ambiguous = ambiguous;
    }

    return constraint;
}

DataKindEnum
Node::resolveStructuralOutputDataKind(bool* isAmbiguous) const
{
    DataKindConstraint constraint = resolveDataKindConstraint(-1, DataKindConstraint());

    if (isAmbiguous) {
        *isAmbiguous = constraint.ambiguous;
    }

    return constraint.kind;
}

Node::DataKindConstraint
Node::resolveDataKindConstraint(int overrideInputNb,
                                const DataKindConstraint& overrideConstraint) const
{
    DataKindConstraint constraint;

    collectUpstreamDataKindConstraint(overrideInputNb, overrideConstraint, &constraint);
    collectDownstreamDataKindRequirement(&constraint);

    return constraint;
}

void
Node::collectUpstreamDataKindConstraint(int overrideInputNb,
                                        const DataKindConstraint& overrideConstraint,
                                        DataKindConstraint* constraint) const
{
    // A group renders nothing itself: what leaves it is what reaches its GroupOutput, so its own
    // inputs only count through what they feed inside.
    NodeGroup* isGroup = isEffectGroup();
    if (isGroup) {
        NodePtr outputNode = isGroup->getOutputNode(false);
        if (outputNode) {
            constraint->merge(effectiveDataKindConstraintOf(outputNode->getRealInput(0)));
        }

        return;
    }

    int nInputs = getNInputs();

    for (int i = 0; i < nInputs; ++i) {
        // An input declaring a concrete kind says what may connect to it; it never decides what a
        // polymorphic output carries, or a mask declared eDataKindImage would type the whole node.
        if (_imp->effect->getInputDataKind(i) != eDataKindPolymorphic) {
            continue;
        }
        if (i == overrideInputNb) {
            constraint->merge(overrideConstraint);
            continue;
        }
        constraint->merge(effectiveDataKindConstraintOf(getRealInput(i)));
    }

    // A GroupInput has no real inputs of its own: what feeds it is whatever is connected to the
    // corresponding input of the enclosing Group node, one graph up.
    GroupInput* isGroupInput = dynamic_cast<GroupInput*>(_imp->effect.get());
    if (isGroupInput) {
        NodeGroup* group = dynamic_cast<NodeGroup*>(getGroup().get());
        if (group) {
            NodePtr thisShared = std::const_pointer_cast<Node>(shared_from_this());
            NodePtr realInput = group->getRealInputForInput(false, thisShared);
            if (realInput && (realInput.get() != this)) {
                constraint->merge(effectiveDataKindConstraintOf(realInput));
            }
        }
    }
} // collectUpstreamDataKindConstraint

void
Node::collectDownstreamDataKindRequirement(DataKindConstraint* constraint) const
{
    // Same discipline as the upstream re-entrancy guard: walking consumers can come back through a
    // node already on the walk, and a thread-local stack ends that without threading a visited set
    // through every recursive call.
    static thread_local std::vector<const Node*> requirementStack;
    if (std::find(requirementStack.begin(), requirementStack.end(), this) != requirementStack.end()) {
        markDataKindResolutionCycle();

        return;
    }
    requirementStack.push_back(this);

    NodePtr deliveringNode = dataKindDeliveringNode(this);
    NodesWList outputs;
    deliveringNode->getOutputs_mt_safe(outputs);

    for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
        NodePtr consumer = it->lock();
        if (!consumer) {
            continue;
        }
        int slot = consumer->getInputIndex(deliveringNode.get());
        if (slot < 0) {
            continue;
        }
        // A group's input requires whatever the consumers of its GroupInput inside require.
        NodeGroup* consumerGroup = consumer->isEffectGroup();
        if (consumerGroup) {
            NodePtr groupInput = consumerGroup->getGroupInputNode(slot);
            if (groupInput) {
                groupInput->collectDownstreamDataKindRequirement(constraint);
            }
            continue;
        }
        DataKindEnum required = consumer->getEffectInstance()->getInputDataKind(slot);
        if (required != eDataKindPolymorphic) {
            // A consumer reached by an adapter requires nothing of this node: the conversion is
            // what satisfies it, so merging its declared kind in would make every pass-through
            // between a deep source and such a consumer hold two kinds at once and go ambiguous.
            if (!constraint->ambiguous && inputAcceptsViaAdapter(consumer->getEffectInstance(), slot, constraint->kind, required)) {
                continue;
            }
            constraint->merge(DataKindConstraint(required));
            continue;
        }
        // A consumer that accepts anything requires nothing of its own, but what it must in turn
        // deliver still reaches back through it: that is what resolves a whole chain of
        // pass-throughs feeding one concrete consumer. Its other inputs are deliberately not
        // consulted -- they are its business, not this node's.
        if (consumer->getEffectInstance()->getOutputDataKind() != eDataKindPolymorphic) {
            continue;
        }
        // Unless the consumer's own policy does not carry this input through to its output, in
        // which case what the consumer must deliver says nothing about what reaches that input.
        if (!nodePropagatesDataKindThroughInput(consumer, slot)) {
            continue;
        }
        consumer->collectDownstreamDataKindRequirement(constraint);
    }

    requirementStack.pop_back();
} // collectDownstreamDataKindRequirement

bool
Node::findDataKindConflictDownstream(const DataKindConstraint& constraint,
                                     NodePtr* conflictingNode) const
{
    NodePtr deliveringNode = dataKindDeliveringNode(this);
    NodesWList outputs;

    deliveringNode->getOutputs_mt_safe(outputs);

    for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
        NodePtr consumer = it->lock();
        if (!consumer) {
            continue;
        }
        int slot = consumer->getInputIndex(deliveringNode.get());
        if (slot < 0) {
            continue;
        }
        if (consumer->isEffectGroup()) {
            if (consumer->findDataKindConflictInsideGroup(slot, constraint, conflictingNode)) {
                return true;
            }
            continue;
        }
        DataKindEnum required = consumer->getEffectInstance()->getInputDataKind(slot);
        if (required != eDataKindPolymorphic) {
            if (!constraint.ambiguous && inputAcceptsViaAdapter(consumer->getEffectInstance(), slot, constraint.kind, required)) {
                continue;
            }
            if (constraint.ambiguous || ((constraint.kind != eDataKindPolymorphic) && (required != constraint.kind))) {
                if (conflictingNode) {
                    *conflictingNode = consumer;
                }

                return true;
            }
            continue;
        }
        if (consumer->getEffectInstance()->getOutputDataKind() != eDataKindPolymorphic) {
            continue;
        }
        if (!nodePropagatesDataKindThroughInput(consumer, slot)) {
            continue;
        }

        DataKindConstraint consumerSimulated = consumer->resolveDataKindConstraint(slot, constraint);
        if ((consumerSimulated.ambiguous || (consumerSimulated.kind != eDataKindPolymorphic)) && consumer->findDataKindConflictDownstream(consumerSimulated, conflictingNode)) {
            return true;
        }
    }

    return false;
} // findDataKindConflictDownstream

bool
Node::findDataKindConflictInsideGroup(int inputNb,
                                      const DataKindConstraint& constraint,
                                      NodePtr* conflictingNode) const
{
    NodeGroup* isGroup = isEffectGroup();
    NodePtr groupInput = isGroup ? isGroup->getGroupInputNode(inputNb) : NodePtr();

    if (!groupInput) {
        return false;
    }

    // The GroupInput has no inputs to override: constraint stands for what the group's input
    // would carry, and what the GroupInput's consumers require is merged in as it would be for
    // any other pass-through.
    DataKindConstraint simulated = constraint;
    groupInput->collectDownstreamDataKindRequirement(&simulated);

    return (simulated.ambiguous || (simulated.kind != eDataKindPolymorphic)) && groupInput->findDataKindConflictDownstream(simulated, conflictingNode);
}

bool
Node::isInputDataKindUnacceptable(const NodePtr& input,
                                  int inputNumber) const
{
    DataKindEnum requiredKind = _imp->effect->getInputDataKind(inputNumber);

    if (requiredKind == eDataKindPolymorphic) {
        return false;
    }

    DataKindConstraint upstream = effectiveDataKindConstraintOf(input);

    if (upstream.ambiguous) {
        return true;
    }
    if ((upstream.kind == eDataKindPolymorphic) || (requiredKind == upstream.kind)) {
        return false;
    }

    return !inputAcceptsViaAdapter(_imp->effect, inputNumber, upstream.kind, requiredKind);
} // isInputDataKindUnacceptable

Node::CanConnectInputReturnValue
Node::checkDataKindCompatibility(const NodePtr& input,
                                 int inputNumber,
                                 NodePtr* conflictingNode) const
{
    if (isInputDataKindUnacceptable(input, inputNumber)) {
        if (conflictingNode) {
            *conflictingNode = input;
        }

        return eCanConnectInput_incompatibleDataKind;
    }

    if (isEffectGroup()) {
        if (findDataKindConflictInsideGroup(inputNumber, effectiveDataKindConstraintOf(input), conflictingNode)) {
            return eCanConnectInput_incompatibleDataKind;
        }

        return eCanConnectInput_ok;
    }

    if (_imp->effect->getOutputDataKind() == eDataKindPolymorphic) {
        DataKindConstraint simulated = resolveDataKindConstraint(inputNumber, effectiveDataKindConstraintOf(input));
        if ((simulated.ambiguous || (simulated.kind != eDataKindPolymorphic)) && findDataKindConflictDownstream(simulated, conflictingNode)) {
            return eCanConnectInput_incompatibleDataKind;
        }
    }

    return eCanConnectInput_ok;
} // Node::checkDataKindCompatibility

void
Node::invalidateEffectiveOutputDataKindCache()
{
    // Resolution is bidirectional, so a memoized kind may depend on a node on either side of this
    // one; the walk therefore goes both ways, and the visited set is what keeps it finite.
    std::set<Node*> visited;
    std::vector<Node*> pending;
    std::vector<Node*> component;

    visited.insert(this);
    pending.push_back(this);

    while (!pending.empty()) {
        Node* node = pending.back();
        pending.pop_back();
        component.push_back(node);

        {
            QMutexLocker l(&node->_imp->effectiveDataKindMutex);
            node->_imp->effectiveDataKindCacheSet = false;
        }

        // Group nodes and their GroupInput and GroupOutput nodes each resolve a kind of their own,
        // and the redirected outputs step over them, so the direct outputs are walked and the
        // boundary is crossed explicitly. Only past a precomp's output node does the redirection
        // reach consumers that nothing else here does.
        NodesList neighbours;
        NodesWList directOutputs;
        node->getOutputs_mt_safe(directOutputs);
        for (NodesWList::const_iterator it = directOutputs.begin(); it != directOutputs.end(); ++it) {
            neighbours.push_back(it->lock());
        }
        NodeGroup* isGroup = node->isEffectGroup();
        if (isGroup) {
            neighbours.push_back(isGroup->getOutputNode(false));
            std::vector<NodePtr> groupInputs;
            isGroup->getInputs(&groupInputs, false);
            neighbours.insert(neighbours.end(), groupInputs.begin(), groupInputs.end());
        }
        PrecompNodePtr precomp = node->isPartOfPrecomp();
        if (precomp && (precomp->getOutputNode().get() == node)) {
            node->getOutputsWithGroupRedirection(neighbours);
        }
        const EffectInstance* effect = node->_imp->effect.get();
        if (dynamic_cast<const GroupInput*>(effect) || dynamic_cast<const GroupOutput*>(effect)) {
            NodeGroup* enclosing = dynamic_cast<NodeGroup*>(node->getGroup().get());
            if (enclosing) {
                neighbours.push_back(enclosing->getNode());
            }
        }

        for (NodesList::const_iterator it = neighbours.begin(); it != neighbours.end(); ++it) {
            if (*it && visited.insert(it->get()).second) {
                pending.push_back(it->get());
            }
        }

        int nInputs = node->getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            NodePtr input = node->getRealInput(i);
            if (input && visited.insert(input.get()).second) {
                pending.push_back(input.get());
            }
        }
    }

    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    // Mid-restore, half the connections are missing and the answer is meaningless; the whole graph
    // is checked once at the end of the load by Project::reportDataKindConflicts().
    const bool refreshDiagnostic = !project || !project->isLoadingProject();

    // Nothing is read back until every cached kind in the component has been dropped, or a node
    // recomputed early would memoize an answer derived from a neighbour's stale one.
    for (std::vector<Node*>::const_iterator it = component.begin(); it != component.end(); ++it) {
        Node* node = *it;
        if (refreshDiagnostic) {
            node->refreshDataKindConflictMessage();
        }
        Q_EMIT node->dataKindChanged();
    }
} // invalidateEffectiveOutputDataKindCache

void
Node::refreshDataKindConflictMessage()
{
    QStringList unhandled;
    const int nInputs = getNInputs();

    for (int i = 0; i < nInputs; ++i) {
        NodePtr inputNode = getRealInput(i);
        if (!inputNode || !isInputDataKindUnacceptable(inputNode, i)) {
            continue;
        }
        unhandled.push_back(tr("Input \"%1\" is connected to %2, which carries a kind of data this node cannot handle.")
                                .arg(QString::fromUtf8(getInputLabel(i).c_str()))
                                .arg(QString::fromUtf8(inputNode->getScriptName_mt_safe().c_str())));
    }

    const QString message = unhandled.join(QString::fromUtf8("\n"));
    QString previous;
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        previous = _imp->dataKindConflictMessage;
        _imp->dataKindConflictMessage = message;
    }

    if (!message.isEmpty()) {
        setPersistentMessage(eMessageTypeError, message.toStdString());

        return;
    }
    if (previous.isEmpty()) {
        return;
    }

    // A persistent message is a single slot with no record of who posted it, so anything else that
    // reported on this node since has overwritten ours and clearing would delete that instead.
    // Only the exact text this node last posted here is ours to take back.
    QString current;
    int type;
    getPersistentMessage(&current, &type, false);
    if (current == previous) {
        clearPersistentMessage(false);
    }
} // refreshDataKindConflictMessage

bool
Node::setContextCountWarning(const QString& message,
                             U64 renderSequence)
{
    bool log;
    bool post;
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        const bool posted = !_imp->contextCountWarningMessage.isEmpty();
        // Outside a render there is no render to log it once for: it is logged when first posted.
        log = (renderSequence == 0) ? !posted : (renderSequence != _imp->contextCountWarningSequence);
        post = (_imp->persistentMessage != message);
        _imp->contextCountWarningMessage = message;
        if (log) {
            _imp->contextCountWarningSequence = renderSequence;
        }
    }
    if (post) {
        setPersistentMessage(eMessageTypeWarning, message.toStdString());
    }

    return log;
}

void
Node::clearContextCountWarning()
{
    QString previous;
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        if (_imp->contextCountWarningMessage.isEmpty()) {
            return;
        }
        previous = _imp->contextCountWarningMessage;
        _imp->contextCountWarningMessage.clear();
        _imp->contextCountWarningSequence = 0;
    }

    // As refreshDataKindConflictMessage(): only the exact text posted here is ours to take back.
    QString current;
    int type;
    getPersistentMessage(&current, &type, false);
    if (current == previous) {
        clearPersistentMessage(false);
    }
}

void
Node::fromSerialization(const Serialization::NodeSerialization& serialization,
                        bool updateKnobGui)
{
    ///Only called from the main thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->knobsInitialized);

    const std::vector<KnobIPtr> & nodeKnobs = getKnobs();

    ///for all knobs of the node
    for (U32 j = 0; j < nodeKnobs.size(); ++j) {
        loadKnob(nodeKnobs[j], serialization, updateKnobGui);
    }

    ///now restore the roto context if the node has a roto context
    if (serialization.hasRotoContext && _imp->curveContext) {
        _imp->curveContext->fromSerialization(serialization.rotoContext);
    }

    if (serialization.hasTrackerContext && _imp->trackContext) {
        _imp->trackContext->fromSerialization(serialization.trackerContext);
    }

    restoreUserKnobs(serialization);

    if (OfxEffectInstance* isOfxEffect = dynamic_cast<OfxEffectInstance*>(_imp->effect.get())) {
        isOfxEffect->syncMultiplaneTwinsAfterLoad();
    }

    // The serialized age is not read: the node cache key is a content hash, so a saved age carries
    // no information and the hash must be recomputed from the freshly restored knobs and inputs.
    computeHash();

    _imp->effect->onKnobsLoaded();
}

void
Node::restoreSublabel()
{
    //Check if natron custom tags are present and insert them if needed
    /// If the node has a sublabel, restore it in the label
    KnobStringPtr labelKnob = _imp->nodeLabelKnob.lock();

    if (labelKnob) {
        QString labeltext = QString::fromUtf8( labelKnob->getValue().c_str() );
        int foundNatronCustomTag = labeltext.indexOf( QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_START) );
        if (foundNatronCustomTag == -1) {
            KnobIPtr sublabelKnob = getKnobByName(kNatronOfxParamStringSublabelName);
            if (sublabelKnob) {
                KnobString* sublabelKnobIsString = dynamic_cast<KnobString*>( sublabelKnob.get() );
                if (sublabelKnobIsString) {
                    QString sublabel = QString::fromUtf8( sublabelKnobIsString->getValue(0).c_str() );
                    if ( !sublabel.isEmpty() ) {
                        int fontEndTagFound = labeltext.lastIndexOf( QString::fromUtf8(kFontEndTag) );
                        if (fontEndTagFound == -1) {
                            labeltext.append( QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_START) );
                            labeltext.append( QLatin1Char('(') + sublabel + QLatin1Char(')') );
                            labeltext.append( QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_END) );
                        } else {
                            QString toAppend( QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_START) );
                            toAppend += QLatin1Char('(');
                            toAppend += sublabel;
                            toAppend += QLatin1Char(')');
                            toAppend += QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_END);
                            labeltext.insert(fontEndTagFound, toAppend);
                        }
                        labelKnob->setValue( labeltext.toStdString() );
                    }
                }
            }
        }
    }
}

void
Node::loadKnob(const KnobIPtr& knob,
               const Serialization::NodeSerialization& serialization,
               bool /*updateKnobGui*/)
{

    const std::list<Serialization::KnobSerialization>& knobsValues = serialization.knobs;

    for (std::list<Serialization::KnobSerialization>::const_iterator it = knobsValues.begin(); it != knobsValues.end(); ++it) {

        if (NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->name) != knob->getName()) {
            continue;
        }

        // Non persistent knobs are loaded too when the project saved a value for them.
        // A knob might change its type between versions: fromSerialization() then loads nothing.
        if (!knob->fromSerialization(*it)) {
            continue;
        }

        if (knob->getName() == kOfxImageEffectFileParamName) {
            computeFrameRangeForReader( knob.get() );
        }

        break;
    }

} // Node::loadKnob


void
Node::setPagesOrder(const std::list<std::string>& pages)
{
    //re-order the pages
    std::list<KnobIPtr> pagesOrdered;

    for (std::list<std::string>::const_iterator it = pages.begin(); it != pages.end(); ++it) {
        const KnobsVec &knobs = getKnobs();
        for (KnobsVec::const_iterator it2 = knobs.begin(); it2 != knobs.end(); ++it2) {
            if ( (*it2)->getName() == *it ) {
                pagesOrdered.push_back(*it2);
                _imp->effect->removeKnobFromList( it2->get() );
                break;
            }
        }
    }
    int index = 0;
    for (std::list<KnobIPtr>::iterator it =  pagesOrdered.begin(); it != pagesOrdered.end(); ++it, ++index) {
        _imp->effect->insertKnob(index, *it);
    }
}

std::list<std::string>
Node::getPagesOrder() const
{
    const KnobsVec& knobs = getKnobs();
    std::list<std::string> ret;

    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        KnobPage* ispage = dynamic_cast<KnobPage*>( it->get() );
        if (ispage) {
            ret.push_back( ispage->getName() );
        }
    }

    return ret;
}

bool
Node::isRenderingPreview() const
{
    QMutexLocker l(&_imp->computingPreviewMutex);

    return _imp->computingPreview;
}


void
Node::Implementation::abortPreview_non_blocking()
{
    bool computing;
    {
        QMutexLocker locker(&computingPreviewMutex);
        computing = computingPreview;
    }

    if (computing) {
        QMutexLocker l(&mustQuitPreviewMutex);
        ++mustQuitPreview;
    }
}

void
Node::Implementation::abortPreview_blocking(bool allowPreviewRenders)
{
    bool computing;
    {
        QMutexLocker locker(&computingPreviewMutex);
        computing = computingPreview;
        previewThreadQuit = !allowPreviewRenders;
    }

    if (computing) {
        QMutexLocker l(&mustQuitPreviewMutex);
        assert(!mustQuitPreview);
        ++mustQuitPreview;
        while (mustQuitPreview) {
            mustQuitPreviewCond.wait(l.mutex());
        }
    }
}

bool
Node::Implementation::checkForExitPreview()
{
    {
        QMutexLocker locker(&mustQuitPreviewMutex);
        if (mustQuitPreview || previewThreadQuit) {
            mustQuitPreview = 0;
            mustQuitPreviewCond.wakeOne();

            return true;
        }

        return false;
    }
}

bool
Node::areAllProcessingThreadsQuit() const
{
    {
        QMutexLocker locker(&_imp->mustQuitPreviewMutex);
        if (!_imp->previewThreadQuit) {
            return false;
        }
    }

    //If this effect has a RenderEngine, make sure it is finished
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( _imp->effect.get() );

    if (isOutput) {
        if ( isOutput->getRenderEngine()->hasThreadsAlive() ) {
            return false;
        }
    }

    TrackerContextPtr trackerContext = getTrackerContext();
    if (trackerContext) {
        if ( !trackerContext->hasTrackerThreadQuit() ) {
            return false;
        }
    }

    return true;
}

void
Node::quitAnyProcessing_non_blocking()
{
    //If this effect has a RenderEngine, make sure it is finished
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( _imp->effect.get() );

    if (isOutput) {
        isOutput->getRenderEngine()->quitEngine(true);
    }

    //Returns when the preview is done computign
    _imp->abortPreview_non_blocking();

    TrackerContextPtr trackerContext = getTrackerContext();
    if (trackerContext) {
        trackerContext->quitTrackerThread_non_blocking();
    }

    if ( isRotoPaintingNode() ) {
        NodesList rotopaintNodes;
        getRotoContext()->getRotoPaintTreeNodes(&rotopaintNodes);
        for (NodesList::iterator it = rotopaintNodes.begin(); it != rotopaintNodes.end(); ++it) {
            (*it)->quitAnyProcessing_non_blocking();
        }
    }
}

void
Node::quitAnyProcessing_blocking(bool allowThreadsToRestart)
{
    QMutexLocker teardownLocker(&_imp->effectTeardownMutex);

    {
        QMutexLocker k(&_imp->nodeIsDequeuingMutex);
        if (_imp->nodeIsDequeuing) {
            _imp->nodeIsDequeuing = false;

            //Attempt to wake-up  sleeping threads of the thread pool
            _imp->nodeIsDequeuingCond.wakeAll();
        }
    }


    //If this effect has a RenderEngine, make sure it is finished
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( _imp->effect.get() );

    if (isOutput) {
        RenderEnginePtr engine = isOutput->getRenderEngine();
        assert(engine);
        engine->quitEngine(allowThreadsToRestart);
        engine->waitForEngineToQuit_enforce_blocking();
    }

    //Returns when the preview is done computign
    _imp->abortPreview_blocking(allowThreadsToRestart);

    TrackerContextPtr trackerContext = getTrackerContext();
    if (trackerContext) {
        trackerContext->quitTrackerThread_blocking(allowThreadsToRestart);
    }

    if ( isRotoPaintingNode() ) {
        NodesList rotopaintNodes;
        getRotoContext()->getRotoPaintTreeNodes(&rotopaintNodes);
        for (NodesList::iterator it = rotopaintNodes.begin(); it != rotopaintNodes.end(); ++it) {
            (*it)->quitAnyProcessing_blocking(allowThreadsToRestart);
        }
    }
}

void
Node::abortAnyProcessing_non_blocking()
{
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( getEffectInstance().get() );

    if (isOutput) {
        isOutput->getRenderEngine()->abortRenderingNoRestart();
    }

    TrackerContextPtr trackerContext = getTrackerContext();
    if (trackerContext) {
        trackerContext->abortTracking();
    }

    _imp->abortPreview_non_blocking();
}

void
Node::abortAnyProcessing_blocking()
{
    QMutexLocker teardownLocker(&_imp->effectTeardownMutex);

    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( getEffectInstance().get() );

    if (isOutput) {
        RenderEnginePtr engine = isOutput->getRenderEngine();
        assert(engine);
        engine->abortRenderingNoRestart();
        engine->waitForAbortToComplete_enforce_blocking();
    }

    TrackerContextPtr trackerContext = getTrackerContext();
    if (trackerContext) {
        trackerContext->abortTracking_blocking();
    }

    _imp->abortPreview_blocking(false);
}

namespace {

// Lets a timeline move revalidate only the nodes owning a channel-selector error instead of
// every node of the project.
QMutex channelSelectorOwnersMutex;
std::map<const Node*, NodeWPtr> channelSelectorOwners;

} // namespace

Node::~Node()
{
    destroyNode(true, false);

    QMutexLocker k(&channelSelectorOwnersMutex);
    channelSelectorOwners.erase(this);
}



AppInstancePtr
Node::getApp() const
{
    return _imp->app.lock();
}

bool
Node::isActivated() const
{
    QMutexLocker l(&_imp->activatedMutex);

    return _imp->activated;
}

std::string
Node::makeCacheInfo() const
{
    std::size_t ram, disk;

    appPTR->getMemoryStatsForCacheEntryHolder(this, &ram, &disk);
    QString ramSizeStr = printAsRAM( (U64)ram );
    QString diskSizeStr = printAsRAM( (U64)disk );
    std::stringstream ss;
    ss << "<b><font color=\"green\">Cache occupancy:</font></b> RAM: <font color=#c8c8c8>" << ramSizeStr.toStdString() << "</font> / Disk: <font color=#c8c8c8>" << diskSizeStr.toStdString() << "</font>";

    return ss.str();
}

std::string
Node::makeInfoForInput(int inputNumber) const
{
    if ( (inputNumber < -1) || ( inputNumber >= getNInputs() ) ) {
        return "";
    }
    EffectInstancePtr input;
    if (inputNumber != -1) {
        input = _imp->effect->getInput(inputNumber);
        /*if (input) {
            input = input->getNearestNonIdentity( getApp()->getTimeLine()->currentFrame() );
        }*/
    } else {
        input = _imp->effect;
    }

    if (!input) {
        return "";
    }


    double time = getApp()->getTimeLine()->currentFrame();
    std::stringstream ss;
    { // input name
        QString inputName;
        if (inputNumber != -1) {
            inputName = QString::fromUtf8( getInputLabel(inputNumber).c_str() );
        } else {
            inputName = tr("Output");
        }
        ss << "<b><font color=\"orange\">" << tr("%1:").arg(inputName).toStdString() << "</font></b><br />";
    }
    { // image format
        ss << "<b>" << tr("Layers:").toStdString() << "</b> <font color=#c8c8c8>";
        std::list<ImageLayerDesc> availableLayers;
        input->getAvailableLayers(time, ViewIdx(0), -1, &availableLayers); // get the layers in the input's output (thus the -1)!
        std::list<ImageLayerDesc>::iterator next = availableLayers.begin();
        if ( next != availableLayers.end() ) {
            ++next;
        }
        for (std::list<ImageLayerDesc>::iterator it = availableLayers.begin(); it != availableLayers.end(); ++it) {

            ss << " " << it->getUserFacingLabel() << '.' << it->getChannelsLabel();
            if ( next != availableLayers.end() ) {
                ss << ", ";
                ++next;
            }
        }
        ss << "</font><br />";
    }
    {
        ImageBitDepthEnum depth = _imp->effect->getMetadata(time, ViewIdx(0))->getBitDepth(inputNumber);
        QString depthStr = tr("unknown");
        switch (depth) {
            case eImageBitDepthByte:
                depthStr = tr("8u");
                break;
            case eImageBitDepthShort:
                depthStr = tr("16u");
                break;
            case eImageBitDepthFloat:
                depthStr = tr("32fp");
                break;
            case eImageBitDepthHalf:
                depthStr = tr("16fp");
            case eImageBitDepthNone:
                break;
        }
        ss << "<b>" << tr("BitDepth:").toStdString() << "</b> <font color=#c8c8c8>" << depthStr.toStdString() << "</font><br />";
    }
    const NodeMetadataConstPtr inputMetadata = input->getMetadata(time, ViewIdx(0));
    {
        RectI format = inputMetadata->getOutputFormat();
        if ( !format.isNull() ) {
            ss << "<b>" << tr("Format (pixels):").toStdString() << "</b> <font color=#c8c8c8>";
            ss << tr("left = %1 bottom = %2 right = %3 top = %4").arg(format.x1).arg(format.y1).arg(format.x2).arg(format.y2).toStdString() << "</font><br />";
        }
    }
    { // par
        double par = inputMetadata->getPixelAspectRatio(-1);
        ss << "<b>" << tr("Pixel aspect ratio:").toStdString() << "</b> <font color=#c8c8c8>" << par << "</font><br />";
    }
    { // fps
        double fps = inputMetadata->getOutputFrameRate();
        ss << "<b>" << tr("Frame rate:").toStdString() << "</b> <font color=#c8c8c8>" << tr("%1fps").arg(fps).toStdString() << "</font><br />";
    }
    {
        double first = 1., last = 1.;
        input->getFrameRange_public(getHashValue(), &first, &last);
        ss << "<b>" << tr("Frame range:").toStdString() << "</b> <font color=#c8c8c8>" << first << " - " << last << "</font><br />";
    }
    {
        RectD rod;
        bool isProjectFormat;
        StatusEnum stat = input->getRegionOfDefinition_public(getHashValue(),
                                                              time,
                                                              RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
        if (stat != eStatusFailed) {
            ss << "<b>" << tr("Region of Definition (at t=%1):").arg(time).toStdString() << "</b> <font color=#c8c8c8>";
            ss << tr("left = %1 bottom = %2 right = %3 top = %4").arg(rod.x1).arg(rod.y1).arg(rod.x2).arg(rod.y2).toStdString() << "</font><br />";
        }
    }

    return ss.str();
} // Node::makeInfoForInput

void
Node::findPluginFormatKnobs()
{
    findPluginFormatKnobs(getKnobs(), true);
}

void
Node::findRightClickMenuKnob(const KnobsVec& knobs)
{
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if (knobs[i]->getName() == kNatronOfxParamRightClickMenu) {
            KnobIPtr rightClickKnob = knobs[i];
            KnobChoice* isChoice = dynamic_cast<KnobChoice*>( rightClickKnob.get() );
            if (isChoice) {
                QObject::connect( isChoice, SIGNAL(populated()), this, SIGNAL(rightClickMenuKnobPopulated()) );
            }
            break;
        }
    }
}

void
Node::findPluginFormatKnobs(const KnobsVec & knobs,
                            bool loadingSerialization)
{
    ///Try to find a format param and hijack it to handle it ourselves with the project's formats
    KnobIPtr formatKnob;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if (knobs[i]->getName() == kNatronParamFormatChoice) {
            formatKnob = knobs[i];
            break;
        }
    }
    if (formatKnob) {
        KnobIPtr formatSize;
        for (std::size_t i = 0; i < knobs.size(); ++i) {
            if (knobs[i]->getName() == kNatronParamFormatSize) {
                formatSize = knobs[i];
                break;
            }
        }
        KnobIPtr formatPar;
        for (std::size_t i = 0; i < knobs.size(); ++i) {
            if (knobs[i]->getName() == kNatronParamFormatPar) {
                formatPar = knobs[i];
                break;
            }
        }
        if (formatSize && formatPar) {
            _imp->pluginFormatKnobs.formatChoice = std::dynamic_pointer_cast<KnobChoice>(formatKnob);
            formatSize->setEvaluateOnChange(false);
            formatPar->setEvaluateOnChange(false);
            formatSize->setSecret(true);
            formatSize->setSecretByDefault(true);
            formatPar->setSecret(true);
            formatPar->setSecretByDefault(true);
            _imp->pluginFormatKnobs.size = std::dynamic_pointer_cast<KnobInt>(formatSize);
            _imp->pluginFormatKnobs.par = std::dynamic_pointer_cast<KnobDouble>(formatPar);

            std::vector<ChoiceOption> formats;
            int defValue;
            getApp()->getProject()->getProjectFormatEntries(&formats, &defValue);
            refreshFormatParamChoice(formats, defValue, loadingSerialization);
        }
    }
}

void
Node::createNodePage(const KnobPagePtr& settingsPage)
{
    KnobBoolPtr hideInputs = AppManager::createKnob<KnobBool>(_imp->effect.get(), tr("Hide inputs"), 1, false);

    hideInputs->setName("hideInputs");
    hideInputs->setHashExempt(true);
    hideInputs->setDefaultValue(false);
    hideInputs->setAnimationEnabled(false);
    hideInputs->setAddNewLine(false);
    hideInputs->setIsPersistent(true);
    hideInputs->setEvaluateOnChange(false);
    hideInputs->setHintToolTip( tr("When checked, the input arrows of the node in the nodegraph will be hidden") );
    _imp->hideInputs = hideInputs;
    settingsPage->addKnob(hideInputs);


    KnobBoolPtr fCaching = AppManager::createKnob<KnobBool>(_imp->effect.get(), tr("Force caching"), 1, false);
    fCaching->setName("forceCaching");
    fCaching->setHashExempt(true);
    fCaching->setDefaultValue(false);
    fCaching->setAnimationEnabled(false);
    fCaching->setAddNewLine(false);
    fCaching->setIsPersistent(true);
    fCaching->setEvaluateOnChange(false);
    fCaching->setHintToolTip( tr("When checked, the output of this node will always be kept in the RAM cache for fast access of already computed "
                                 "images.") );
    _imp->forceCaching = fCaching;
    settingsPage->addKnob(fCaching);

    KnobBoolPtr previewEnabled = AppManager::createKnob<KnobBool>(_imp->effect.get(), tr("Preview"), 1, false);
    assert(previewEnabled);
    previewEnabled->setDefaultValue( makePreviewByDefault() );
    previewEnabled->setName(kEnablePreviewKnobName);
    previewEnabled->setHashExempt(true);
    previewEnabled->setAnimationEnabled(false);
    previewEnabled->setAddNewLine(false);
    previewEnabled->setIsPersistent(true);
    previewEnabled->setEvaluateOnChange(false);
    previewEnabled->setHintToolTip( tr("Whether to show a preview on the node box in the node-graph.") );
    settingsPage->addKnob(previewEnabled);
    _imp->previewEnabledKnob = previewEnabled;

    KnobBoolPtr disableNodeKnob = AppManager::createKnob<KnobBool>(_imp->effect.get(), tr("Disable"), 1, false);
    assert(disableNodeKnob);
    disableNodeKnob->setIsMetadataSlave(true);
    disableNodeKnob->setName(kDisableNodeKnobName);
    disableNodeKnob->setAddNewLine(false);
    disableNodeKnob->setHintToolTip( tr("When disabled, this node acts as a pass through.") );
    settingsPage->addKnob(disableNodeKnob);
    _imp->disableNodeKnob = disableNodeKnob;



    KnobBoolPtr useFullScaleImagesWhenRenderScaleUnsupported = AppManager::createKnob<KnobBool>(_imp->effect.get(), tr("Render high def. upstream"), 1, false);
    useFullScaleImagesWhenRenderScaleUnsupported->setAnimationEnabled(false);
    useFullScaleImagesWhenRenderScaleUnsupported->setDefaultValue(false);
    useFullScaleImagesWhenRenderScaleUnsupported->setName("highDefUpstream");
    useFullScaleImagesWhenRenderScaleUnsupported->setHintToolTip( tr("This node does not support rendering images at a scale lower than 1, it "
                                                                     "can only render high definition images. When checked this parameter controls "
                                                                     "whether the rest of the graph upstream should be rendered with a high quality too or at "
                                                                     "the most optimal resolution for the current viewer's viewport. Typically checking this "
                                                                     "means that an image will be slow to be rendered, but once rendered it will stick in the cache "
                                                                     "whichever zoom level you are using on the Viewer, whereas when unchecked it will be much "
                                                                     "faster to render but will have to be recomputed when zooming in/out in the Viewer.") );
    if ( ( isRenderScaleSupportEnabledForPlugin() ) && (getEffectInstance()->supportsRenderScaleMaybe() == EffectInstance::eSupportsYes) ) {
        useFullScaleImagesWhenRenderScaleUnsupported->setSecretByDefault(true);
    }
    settingsPage->addKnob(useFullScaleImagesWhenRenderScaleUnsupported);
    _imp->useFullScaleImagesWhenRenderScaleUnsupported = useFullScaleImagesWhenRenderScaleUnsupported;


    KnobIntPtr lifeTimeKnob = AppManager::createKnob<KnobInt>(_imp->effect.get(), tr("Lifetime Range"), 2, false);
    assert(lifeTimeKnob);
    lifeTimeKnob->setAnimationEnabled(false);
    lifeTimeKnob->setIsMetadataSlave(true);
    lifeTimeKnob->setName(kLifeTimeNodeKnobName);
    lifeTimeKnob->setAddNewLine(false);
    lifeTimeKnob->setHintToolTip( tr("This is the frame range during which the node will be active if Enable Lifetime is checked") );
    settingsPage->addKnob(lifeTimeKnob);
    _imp->lifeTimeKnob = lifeTimeKnob;


    KnobBoolPtr enableLifetimeNodeKnob = AppManager::createKnob<KnobBool>(_imp->effect.get(), tr("Enable Lifetime"), 1, false);
    assert(enableLifetimeNodeKnob);
    enableLifetimeNodeKnob->setAnimationEnabled(false);
    enableLifetimeNodeKnob->setDefaultValue(false);
    enableLifetimeNodeKnob->setIsMetadataSlave(true);
    enableLifetimeNodeKnob->setName(kEnableLifeTimeNodeKnobName);
    enableLifetimeNodeKnob->setHintToolTip( tr("When checked, the node is only active during the specified frame range by the Lifetime Range parameter. "
                                               "Outside of this frame range, it behaves as if the Disable parameter is checked") );
    settingsPage->addKnob(enableLifetimeNodeKnob);
    _imp->enableLifeTimeKnob = enableLifetimeNodeKnob;

    PluginOpenGLRenderSupport glSupport = ePluginOpenGLRenderSupportNone;
    if (_imp->plugin && _imp->plugin->isOpenGLEnabled()) {
        glSupport = _imp->plugin->getPluginOpenGLRenderSupport();
    }
    if (glSupport != ePluginOpenGLRenderSupportNone) {
        KnobChoicePtr openglRenderingKnob = AppManager::createKnob<KnobChoice>(_imp->effect.get(), tr("GPU Rendering"), 1, false);
        assert(openglRenderingKnob);
        openglRenderingKnob->setAnimationEnabled(false);
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("Enabled", "", tr("If a plug-in support GPU rendering, prefer rendering using the GPU if possible.").toStdString() ));
            entries.push_back(ChoiceOption("Disabled", "", tr("Disable GPU rendering for all plug-ins.").toStdString()));
            entries.push_back(ChoiceOption("Disabled if background", "", tr("Disable GPU rendering when rendering with NatronRenderer but not in GUI mode.").toStdString()));
            openglRenderingKnob->populateChoices(entries);

        }

        openglRenderingKnob->setName("enableGPURendering");
        openglRenderingKnob->setHintToolTip( tr("Select when to activate GPU rendering for this node. Note that if the GPU Rendering parameter in the Project settings is set to disabled then GPU rendering will not be activated regardless of that value.") );
        settingsPage->addKnob(openglRenderingKnob);
        _imp->openglRenderingEnabledKnob = openglRenderingKnob;
    }


    KnobStringPtr knobChangedCallback = AppManager::createKnob<KnobString>(_imp->effect.get(), tr("After param changed callback"), 1, false);
    knobChangedCallback->setHintToolTip( tr("Name of a Python function to be called at each  "
                                            "parameter change. Either define this function in the Script Editor "
                                            "or in the init.py script or even in the script of a Python group plug-in.\n"
                                            "The signature of the callback is: callback(thisParam, thisNode, thisGroup, app, userEdited) where:\n"
                                            "- thisParam: The parameter which just had its value changed\n"
                                            "- userEdited: A boolean informing whether the change was due to user interaction or "
                                            "because something internally triggered the change.\n"
                                            "- thisNode: The node holding the parameter\n"
                                            "- app: points to the current application instance\n"
                                            "- thisGroup: The group holding thisNode (only if thisNode belongs to a group)") );
    knobChangedCallback->setAnimationEnabled(false);
    knobChangedCallback->setName("onParamChanged");
    knobChangedCallback->setHashExempt(true);
    settingsPage->addKnob(knobChangedCallback);
    _imp->knobChangedCallback = knobChangedCallback;

    KnobStringPtr inputChangedCallback = AppManager::createKnob<KnobString>(_imp->effect.get(), tr("After input changed callback"), 1, false);
    inputChangedCallback->setHintToolTip( tr("Name of a Python function to be called after "
                                             "an input connection of the node is changed. "
                                             "Either define this function in the Script Editor "
                                             "or in the init.py script or even in the script of a Python group plug-in.\n"
                                             "The signature of the callback is: callback(inputIndex, thisNode, thisGroup, app):\n"
                                             "- inputIndex: the index of the input which changed, you can query the node "
                                             "connected to the input by calling the getInput(...) function.\n"
                                             "- thisNode: The node holding the parameter\n"
                                             "- app: points to the current application instance\n"
                                             "- thisGroup: The group holding thisNode (only if thisNode belongs to a group)") );

    inputChangedCallback->setAnimationEnabled(false);
    inputChangedCallback->setName("onInputChanged");
    inputChangedCallback->setHashExempt(true);
    settingsPage->addKnob(inputChangedCallback);
    _imp->inputChangedCallback = inputChangedCallback;

    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( _imp->effect.get() );
    if (isGroup) {
        KnobStringPtr onNodeCreated = AppManager::createKnob<KnobString>(_imp->effect.get(), tr("After Node Created"), 1, false);
        onNodeCreated->setName("afterNodeCreated");
        onNodeCreated->setHashExempt(true);
        onNodeCreated->setHintToolTip( tr("Name of a Python function to be called each time a node "
                                          "is created in the group. This is called in addition to the After Node Created "
                                          " callback of the project for the group node and all nodes within it (not recursively).\n"
                                          "The boolean variable userEdited is set to True if the node was created "
                                          "by the user or False otherwise (such as when loading a project, or pasting a node).\n"
                                          "The signature of the callback is: callback(thisNode, app, userEdited) where:\n"
                                          "- thisNode: the node which has just been created\n"
                                          "- userEdited: a boolean indicating whether the node was created by user interaction or from "
                                          "a script/project load/copy-paste\n"
                                          "- app: points to the current application instance.") );
        onNodeCreated->setAnimationEnabled(false);
        _imp->nodeCreatedCallback = onNodeCreated;
        settingsPage->addKnob(onNodeCreated);

        KnobStringPtr onNodeDeleted = AppManager::createKnob<KnobString>(_imp->effect.get(), tr("Before Node Removal"), 1, false);
        onNodeDeleted->setName("beforeNodeRemoval");
        onNodeDeleted->setHashExempt(true);
        onNodeDeleted->setHintToolTip( tr("Add here the name of a Python-defined function that will be called each time a node "
                                          "is about to be deleted. This will be called in addition to the Before Node Removal "
                                          " callback of the project for the group node and all nodes within it (not recursively).\n"
                                          "This function will not be called when the project is closing.\n"
                                          "The signature of the callback is: callback(thisNode, app) where:\n"
                                          "- thisNode: the node about to be deleted\n"
                                          "- app: points to the current application instance.") );
        onNodeDeleted->setAnimationEnabled(false);
        _imp->nodeRemovalCallback = onNodeDeleted;
        settingsPage->addKnob(onNodeDeleted);
    }
    if (_imp->effect->isWriter()) {
        KnobStringPtr beforeFrameRender =  AppManager::createKnob<KnobString>(_imp->effect.get(), tr("Before frame render"), 1, false);

        beforeFrameRender->setName("beforeFrameRender");
        beforeFrameRender->setHashExempt(true);
        beforeFrameRender->setAnimationEnabled(false);
        beforeFrameRender->setHintToolTip( tr("Add here the name of a Python defined function that will be called before rendering "
                                              "any frame.\n "
                                              "The signature of the callback is: callback(frame, thisNode, app) where:\n"
                                              "- frame: the frame to be rendered\n"
                                              "- thisNode: points to the writer node\n"
                                              "- app: points to the current application instance") );
        settingsPage->addKnob(beforeFrameRender);
        _imp->beforeFrameRender = beforeFrameRender;

        KnobStringPtr beforeRender =  AppManager::createKnob<KnobString>(_imp->effect.get(), tr("Before render"), 1, false);
        beforeRender->setName("beforeRender");
        beforeRender->setHashExempt(true);
        beforeRender->setAnimationEnabled(false);
        beforeRender->setHintToolTip( tr("Add here the name of a Python defined function that will be called once when "
                                         "starting rendering.\n "
                                         "The signature of the callback is: callback(thisNode, app) where:\n"
                                         "- thisNode: points to the writer node\n"
                                         "- app: points to the current application instance") );
        settingsPage->addKnob(beforeRender);
        _imp->beforeRender = beforeRender;

        KnobStringPtr afterFrameRender =  AppManager::createKnob<KnobString>(_imp->effect.get(), tr("After frame render"), 1, false);
        afterFrameRender->setName("afterFrameRender");
        afterFrameRender->setHashExempt(true);
        afterFrameRender->setAnimationEnabled(false);
        afterFrameRender->setHintToolTip( tr("Add here the name of a Python defined function that will be called after rendering "
                                             "any frame.\n "
                                             "The signature of the callback is: callback(frame, thisNode, app) where:\n"
                                             "- frame: the frame that has been rendered\n"
                                             "- thisNode: points to the writer node\n"
                                             "- app: points to the current application instance") );
        settingsPage->addKnob(afterFrameRender);
        _imp->afterFrameRender = afterFrameRender;

        KnobStringPtr afterRender =  AppManager::createKnob<KnobString>(_imp->effect.get(), tr("After render"), 1, false);
        afterRender->setName("afterRender");
        afterRender->setHashExempt(true);
        afterRender->setAnimationEnabled(false);
        afterRender->setHintToolTip( tr("Add here the name of a Python defined function that will be called once when the rendering "
                                        "is finished.\n "
                                        "The signature of the callback is: callback(aborted, thisNode, app) where:\n"
                                        "- aborted: True if the render ended because it was aborted, False upon completion\n"
                                        "- thisNode: points to the writer node\n"
                                        "- app: points to the current application instance") );
        settingsPage->addKnob(afterRender);
        _imp->afterRender = afterRender;
    }
} // Node::createNodePage

void
Node::createInfoPage()
{
    KnobPagePtr infoPage = AppManager::createKnob<KnobPage>(_imp->effect.get(), tr("Info").toStdString(), 1, false);

    infoPage->setName(NATRON_PARAMETER_PAGE_NAME_INFO);
    _imp->infoPage = infoPage;

    KnobStringPtr nodeInfos = AppManager::createKnob<KnobString>(_imp->effect.get(), std::string(), 1, false);
    nodeInfos->setName("nodeInfos");
    nodeInfos->setHashExempt(true);
    nodeInfos->setAnimationEnabled(false);
    nodeInfos->setIsPersistent(false);
    nodeInfos->setAsMultiLine();
    nodeInfos->setAsCustomHTMLText(true);
    nodeInfos->setEvaluateOnChange(false);
    nodeInfos->setHintToolTip( tr("Input and output information, press Refresh to update them with current values") );
    infoPage->addKnob(nodeInfos);
    _imp->nodeInfos = nodeInfos;


    KnobButtonPtr refreshInfoButton = AppManager::createKnob<KnobButton>(_imp->effect.get(), tr("Refresh Info"), 1, false);
    refreshInfoButton->setName("refreshButton");
    refreshInfoButton->setEvaluateOnChange(false);
    infoPage->addKnob(refreshInfoButton);
    _imp->refreshInfoButton = refreshInfoButton;
}

void
Node::createHostMixKnob(const KnobPagePtr& mainPage)
{
    KnobDoublePtr mixKnob = AppManager::createKnob<KnobDouble>(_imp->effect.get(), tr("Mix"), 1, false);

    mixKnob->setName("hostMix");
    mixKnob->setHintToolTip( tr("Mix between the source image at 0 and the full effect at 1.") );
    mixKnob->setMinimum(0.);
    mixKnob->setMaximum(1.);
    mixKnob->setDefaultValue(1.);
    if (mainPage) {
        mainPage->addKnob(mixKnob);
    }
    _imp->mixWithSource = mixKnob;
}

void
Node::createStageFooterSeparator(const KnobPagePtr& mainPage)
{
    if (!mainPage) {
        return;
    }
    const KnobsVec children = mainPage->getChildren();
    if (children.empty() || dynamic_cast<KnobSeparator*>(children.back().get())) {
        return;
    }
    KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(_imp->effect.get(), std::string(), 1, false);
    sep->setName("advancedSep");
    mainPage->addKnob(sep);
}

void
Node::createUnPremultSelector(const KnobPagePtr& mainPage)
{
    // openfx-misc's colour family declares "(Un)premult by" as a bool plus a fixed R/G/B/A
    // choice, and its ofxsUnPremult()/ofxsPremult() pair can only ever divide by a channel of
    // the very plane it is handed -- never by another layer's. The host owns the channel
    // routing, so it owns this convenience too: the plug-in's pair is switched off and hidden,
    // and one channel selector over every layer of the source takes their place. The divide and
    // the multiply are then done either side of the plug-in's render action, by
    // OfxClipInstance::getInputImageInternal() and EffectInstance::tiledRenderingFunctor().
    // An effect asking through wantsHostUnPremultSelector() has no such pair to hide, and does
    // the divide and the multiply in its own render.
    KnobBoolPtr pluginEnabled = _imp->effect->getKnobByNameAndType<KnobBool>(kUnPremultByPluginKnobName);
    KnobChoicePtr pluginChannel = _imp->effect->getKnobByNameAndType<KnobChoice>(kUnPremultByChannelPluginKnobName);
    const bool pluginDeclaresPair = pluginEnabled && pluginChannel;

    if (!pluginDeclaresPair && !_imp->effect->wantsHostUnPremultSelector()) {
        return;
    }

    if (pluginDeclaresPair) {
        pluginEnabled->setValue(false);
        pluginEnabled->setSecret(true);
        pluginEnabled->setSecretLocked(true);
        pluginEnabled->setIsPersistent(false);
        pluginChannel->setSecret(true);
        pluginChannel->setSecretLocked(true);
        pluginChannel->setIsPersistent(false);
    }

    // A Divisor stage hands the channel to the node's own maths: a Premult or Unpremult node,
    // whose whole work is the multiply or the divide, and which starts on alpha.
    const StagedImageEffect* staged = dynamic_cast<const StagedImageEffect*>(_imp->effect.get());
    const bool divisorToOp = !pluginDeclaresPair && staged && staged->imagePipelineSpec().stages.has(Stage::Divisor);
    const char* label = (divisorToOp && staged->imagePipelineSpec().divisorLabel) ? staged->imagePipelineSpec().divisorLabel : kUnPremultByKnobLabel;
    KnobChannelSelectPtr channel = _imp->effect->createChannelSelectKnob(kUnPremultByKnobName, tr(label).toStdString(), false);
    channel->setAnimationEnabled(false);
    if (divisorToOp) {
        channel->setDefaultValue(channel->encode(std::string(kNatronColorViewRGBA) + ".A"));
        channel->setHintToolTip(tr("The channel this node multiplies or divides the processed channels by. Any "
                                   "layer.channel of the source, or None to leave the image as it is."));
    } else {
        // A KnobChannelSelect left empty reads as rgba.A, which is the right default for a mask
        // footer but not here: the plug-in's own bool defaulted to off, and quietly
        // unpremultiplying every colour node by alpha is not something to turn on behind the
        // user's back.
        channel->setDefaultValue(channel->encode(std::string()));
        channel->setHintToolTip(tr("Divide the image by this channel of the source before processing, and multiply it "
                                   "back afterwards. Any layer.channel of the source, or None for no (un)premult. Use it "
                                   "where the values being processed are premultiplied, as any correction that moves the "
                                   "black point has to be done unpremultiplied."));
    }
    _imp->layerKnobSources[channel.get()] = LayerKnobSource(channel, LayerKnobSource::kPreferredInput, LayerKnobSpec::eRoleInputBound);
    if (mainPage) {
        mainPage->addKnob(channel);
    }
    _imp->unPremultBySelector = channel;
} // Node::createUnPremultSelector

KnobChannelSelectPtr
Node::getUnPremultBySelector() const
{
    return _imp->unPremultBySelector.lock();
}

int
Node::getUnPremultSkipChannel(const ImageLayerDesc& plane,
                              const ImageLayerDesc& divisorLayer,
                              int divisorChannel)
{
    if (!divisorLayer.isColorLayer()) {
        return (plane.getLayerID() == divisorLayer.getLayerID()) ? divisorChannel : -1;
    }
    if (!plane.isColorLayer()) {
        return -1;
    }

    // The divisor may have been fetched in a wider colour layout than plane (see
    // KnobChannelSelect::resolve()), so match the channel by its colour bit, not its index.
    const int bit = ImageLayerDesc::colorViewChannelBit(ImageLayerDesc::colorViewForNComps(divisorLayer.getNumComponents()).getLayerID(), divisorChannel);

    return ImageLayerDesc::colorViewChannelIndex(ImageLayerDesc::colorViewForNComps(plane.getNumComponents()).getLayerID(), bit);
}

int
Node::getUnPremultChannel(const std::list<ImageLayerDesc>& availableLayers,
                          ImageLayerDesc* comps) const
{
    *comps = ImageLayerDesc::getNoneComponents();

    KnobChannelSelectPtr channel = _imp->unPremultBySelector.lock();
    if (!channel) {
        return -1;
    }
    int channelIndex = -1;
    ImageLayerDesc layer;
    if (!channel->resolve(availableLayers, &layer, &channelIndex)) {
        return -1;
    }
    *comps = layer;

    return channelIndex;
}

void
Node::createMaskSelectors(const std::vector<std::pair<bool, bool> >& hasMaskChannelSelector,
                          const std::vector<std::string>& inputLabels,
                          const KnobPagePtr& mainPage,
                          bool addNewLineOnLastMask,
                          KnobIPtr* lastKnobCreated)
{
    assert( hasMaskChannelSelector.size() == inputLabels.size() );

    for (std::size_t i = 0; i < hasMaskChannelSelector.size(); ++i) {
        if (!hasMaskChannelSelector[i].first) {
            continue;
        }


        MaskSelector sel;
        KnobBoolPtr enabled = AppManager::createKnob<KnobBool>(_imp->effect.get(), inputLabels[i], 1, false);

        enabled->setDefaultValue(false, 0);
        enabled->setAddNewLine(false);
        if (hasMaskChannelSelector[i].second) {
            std::string enableMaskName(std::string(kEnableMaskKnobName) + "_" + inputLabels[i]);
            enabled->setName(enableMaskName);
            enabled->setHintToolTip( tr("Enable the mask to come from the channel named by the choice parameter on the right. "
                                        "Turning this off will act as though the mask was disconnected.") );
        } else {
            std::string enableMaskName(std::string(kEnableInputKnobName) + "_" + inputLabels[i]);
            enabled->setName(enableMaskName);
            enabled->setHintToolTip( tr("Enable the image to come from the channel named by the choice parameter on the right. "
                                        "Turning this off will act as though the input was disconnected.") );
        }
        enabled->setAnimationEnabled(false);
        if (mainPage) {
            mainPage->addKnob(enabled);
        }


        sel.enabled = enabled;

        std::string channelKnobName = std::string(hasMaskChannelSelector[i].second ? kMaskChannelKnobName : kInputChannelKnobName) + "_" + inputLabels[i];
        KnobChannelSelectPtr channel = _imp->effect->createChannelSelectKnob(channelKnobName, std::string(), false);
        channel->setAnimationEnabled(false);
        channel->setHintToolTip(tr("One layer.channel of this input, or None. A selected channel this input no longer has "
                                   "is kept and marked \"(not in input)\". Setting this to None is the same as disconnecting "
                                   "the input."));
        sel.channel = channel;
        _imp->layerKnobSources[channel.get()] = LayerKnobSource(channel, i, LayerKnobSpec::eRoleInputBound);
        if (mainPage) {
            mainPage->addKnob(channel);
        }

        //Make sure the first default param in the vector is MaskInvert

        if (!addNewLineOnLastMask) {
            //If there is a MaskInvert parameter, make it on the same line as the Mask channel parameter
            channel->setAddNewLine(false);
        }
        if (!*lastKnobCreated) {
            *lastKnobCreated = enabled;
        }

        _imp->maskSelectors[i] = sel;
    } // for (int i = 0; i < inputsCount; ++i) {
} // Node::createMaskSelectors

KnobPagePtr
Node::getOrCreateMainPage()
{
    const KnobsVec & knobs = _imp->effect->getKnobs();
    KnobPagePtr mainPage;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobPagePtr p = std::dynamic_pointer_cast<KnobPage>(knobs[i]);
        if ( p && (p->getLabel() != NATRON_PARAMETER_PAGE_NAME_INFO) &&
             (p->getLabel() != NATRON_PARAMETER_PAGE_NAME_EXTRA) ) {
            mainPage = p;
            break;
        }
    }
    if (!mainPage) {
        mainPage = AppManager::createKnob<KnobPage>( _imp->effect.get(), tr("Settings") );
    }

    return mainPage;
}

void
Node::createLabelKnob(const KnobPagePtr& settingsPage,
                      const std::string& label)
{
    KnobStringPtr nodeLabel = AppManager::createKnob<KnobString>(_imp->effect.get(), label, 1, false);

    assert(nodeLabel);
    nodeLabel->setName(kUserLabelKnobName);
    nodeLabel->setHashExempt(true);
    nodeLabel->setAnimationEnabled(false);
    nodeLabel->setEvaluateOnChange(false);
    nodeLabel->setAsMultiLine();
    nodeLabel->setUsesRichText(true);
    nodeLabel->setHintToolTip( tr("This label gets appended to the node name on the node graph.") );
    settingsPage->addKnob(nodeLabel);
    _imp->nodeLabelKnob = nodeLabel;
}

void
Node::adoptChannelQuad()
{
    // KeyMix's quad picks source A vs source B per channel, DenoiseSharpen's collapses R/G/B
    // into a single "process chroma" flag, and ClipTest's ORs the selected channels into one
    // zebra-stripe decision: none of the three is a per-channel output mask, so forcing the
    // quad true and letting the host mask instead would silently change what they compute.
    static const std::set<std::string> pluginsOwningChannelMask = {
        "net.sf.openfx.KeyMix",
        "net.sf.openfx.DenoiseSharpen",
        "net.sf.openfx.ClipTestPlugin"
    };

    KnobIPtr layerKnob = _imp->layerKnob.lock();
    KnobChannelSet* isChannelSet = dynamic_cast<KnobChannelSet*>(layerKnob.get());
    KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>(layerKnob.get());
    const bool defaultsToAll = isChannelSet && _imp->effect->defaultProcessesAllLayers();

    if (defaultsToAll) {
        std::vector<ChannelSetRow> allRows(1);
        allRows[0].mode = ChannelSetRow::eModeAll;
        isChannelSet->setDefaultValue(isChannelSet->encodeRows(allRows));
    }

    if (pluginsOwningChannelMask.count(getPluginID()) > 0) {
        _imp->pluginOwnsChannelMask = true;
        return;
    }

    static const std::string channelNames[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    static const std::string channelShortNames[4] = { "R", "G", "B", "A" };
    KnobBoolPtr foundEnabled[4];
    const KnobsVec& knobs = _imp->effect->getKnobs();

    for (int i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < knobs.size(); ++j) {
            if (knobs[j]->getOriginalName() == channelNames[i]) {
                foundEnabled[i] = std::dynamic_pointer_cast<KnobBool>(knobs[j]);
                break;
            }
        }
    }

    bool foundAll = foundEnabled[0] && foundEnabled[1] && foundEnabled[2] && foundEnabled[3];
    bool pluginDefaultPref[4];
    bool hostChannelSelectorEnabled = _imp->effect->isHostChannelSelectorSupported(&pluginDefaultPref[0], &pluginDefaultPref[1], &pluginDefaultPref[2], &pluginDefaultPref[3]);

    bool enabledByDefault[4] = { true, true, true, true };
    if (foundAll) {
        for (int i = 0; i < 4; ++i) {
            enabledByDefault[i] = foundEnabled[i]->getDefaultValue(0);
            // The layer knob now owns the channel choice; the plug-in's own quad stays fully
            // on so the host decides which channels reach the output.
            foundEnabled[i]->setSecretByDefault(true);
            foundEnabled[i]->setIsPersistent(false);
            foundEnabled[i]->setValue(true);
        }
    } else if (hostChannelSelectorEnabled) {
        for (int i = 0; i < 4; ++i) {
            enabledByDefault[i] = pluginDefaultPref[i];
        }
    }

    std::vector<std::string> enabledChannels;
    for (int i = 0; i < 4; ++i) {
        if (enabledByDefault[i]) {
            enabledChannels.push_back(channelShortNames[i]);
        }
    }

    // All four channels already enabled by default: the layer knob's "every channel" state
    // already matches, so there is nothing to seed on it.
    if (enabledChannels.size() != 4) {
        if (isChannelSet) {
            std::vector<ChannelSetRow> rows = KnobChannelSet::defaultRows();
            if (enabledChannels.empty()) {
                rows[0].mode = ChannelSetRow::eModeNone;
            } else {
                rows[0].channels = enabledChannels;
            }
            if (!defaultsToAll) {
                isChannelSet->setDefaultValue(isChannelSet->encodeRows(rows));
            }
        } else if (isLayerSelect && isLayerSelect->getWithChannelButtons()) {
            isLayerSelect->setChannels(enabledChannels);
            isLayerSelect->setDefaultValue(isLayerSelect->getValue());
        }
    }

    if (isLayerSelect && isLayerSelect->getWithChannelButtons()) {
        // The layer select now owns which channels the generator writes; the plug-in's own
        // outputComponents choice would otherwise stay on the panel as a second, redundant
        // control, and openfx-misc generators read it unconditionally (unlike GenericWriter,
        // there is no secret gate to piggyback on).
        KnobChoicePtr outputComponents = _imp->effect->getKnobByNameAndType<KnobChoice>(kParamGeneratorOutputComponents);
        if (outputComponents) {
            outputComponents->setSecret(true);
            outputComponents->setSecretLocked(true);
            outputComponents->setIsPersistent(false);
        }
        refreshGeneratorOutputComponentsKnob();
    }
} // Node::adoptChannelQuad

void
Node::refreshGeneratorOutputComponentsKnob()
{
    KnobIPtr layerKnob = _imp->layerKnob.lock();
    KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>(layerKnob.get());
    if (!isLayerSelect || !isLayerSelect->getWithChannelButtons()) {
        return;
    }
    KnobChoicePtr outputComponents = _imp->effect->getKnobByNameAndType<KnobChoice>(kParamGeneratorOutputComponents);
    if (!outputComponents) {
        return;
    }

    // The layer select's buttons choose which channels the plug-in writes; the rest pass
    // through unchanged (or are zero with no source), so the stream itself must always carry
    // all four channels, whatever the selection: outputComponents is pinned to RGBA.
    std::vector<ChoiceOption> entries = outputComponents->getEntries_mt_safe();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id != "RGBA") {
            continue;
        }
        if (outputComponents->getValue() != (int)i) {
            outputComponents->setValue((int)i);
        }
        break;
    }
} // Node::refreshGeneratorOutputComponentsKnob

void
Node::createLayerKnob(const LayerKnobSpec& spec,
                      const KnobPagePtr& mainPage)
{
    KnobIPtr knob;

    if (spec.kind == LayerKnobSpec::eKindChannelSet) {
        KnobChannelSetPtr channelSet = _imp->effect->createChannelSetKnob(kNodeParamChannelSet, tr(kNodeParamChannelSetLabel).toStdString(), false);
        channelSet->setHintToolTip(tr("The layers and channels this node processes in place; every other channel of every layer "
                                      "passes through unchanged. Row 0 can be None, All, a regex over layer names, or a layer; "
                                      "more rows add layers. A selected layer that the input no longer has is kept and marked "
                                      "\"(not in input)\"."));
        knob = channelSet;
    } else {
        KnobLayerSelectPtr layerSelect = _imp->effect->createLayerSelectKnob(kNodeParamLayerSelect, tr(kNodeParamLayerSelectLabel).toStdString(), spec.withChannelButtons, false);
        layerSelect->setHintToolTip(spec.role == LayerKnobSpec::eRoleTarget ? tr("The layer this node writes into. Choosing \"New layer...\" creates a project layer. "
                                                                                 "A selected layer no longer in the project is kept and marked \"(not in project)\".")
                                                                            : tr("The layer of the input this node reads. A selected layer the input no longer has is "
                                                                                 "kept and marked \"(not in input)\"."));
        knob = layerSelect;
    }
    knob->setAnimationEnabled(false);
    knob->setIsMetadataSlave(true);
    knob->setAddNewLine(true);
    mainPage->insertKnob(0, knob);

    KnobSeparatorPtr separator = AppManager::createKnob<KnobSeparator>(_imp->effect.get(), std::string(), 1, false);
    separator->setName(kNodeParamLayerSeparator);
    separator->setIsPersistent(false);
    mainPage->insertKnob(1, separator);

    int inputNb = spec.role == LayerKnobSpec::eRoleTarget ? -1 : LayerKnobSource::kPreferredInput;
    _imp->layerKnob = knob;
    _imp->layerKnobSpec = spec;
    _imp->layerKnobSources[knob.get()] = LayerKnobSource(knob, inputNb, spec.role);
} // Node::createLayerKnob

void
Node::initializeDefaultKnobs(bool loadingSerialization)
{
    //Add the "Node" page
    KnobPagePtr settingsPage = AppManager::createKnob<KnobPage>(_imp->effect.get(), tr(NATRON_PARAMETER_PAGE_NAME_EXTRA), 1, false);
    _imp->nodeSettingsPage = settingsPage;

    //Create the "Label" knob
    Backdrop* isBackdropNode = dynamic_cast<Backdrop*>( _imp->effect.get() );
    QString labelKnobLabel = isBackdropNode ? tr("Name label") : tr("Label");
    createLabelKnob( settingsPage, labelKnobLabel.toStdString() );

    if (isBackdropNode) {
        //backdrops just have a label
        return;
    }


    ///find in all knobs a page param to set this param into
    const KnobsVec & knobs = _imp->effect->getKnobs();

    findPluginFormatKnobs(knobs, loadingSerialization);
    findRightClickMenuKnob(knobs);

    KnobPagePtr mainPage;


    // Scan all inputs to find masks and get inputs labels
    //Pair hasMaskChannelSelector, isMask
    int inputsCount = getNInputs();
    std::vector<std::pair<bool, bool> > hasMaskChannelSelector(inputsCount);
    std::vector<std::string> inputLabels(inputsCount);
    for (int i = 0; i < inputsCount; ++i) {
        inputLabels[i] = _imp->effect->getInputLabel(i);

        assert( i < (int)_imp->inputsComponents.size() );
        bool isMask = _imp->effect->isInputMask(i);
        bool supportsOnlyAlpha = isInputOnlyAlpha(i);

        hasMaskChannelSelector[i].first = false;
        hasMaskChannelSelector[i].second = isMask;

        if ( (isMask || supportsOnlyAlpha) &&
             !_imp->effect->isInputRotoBrush(i) ) {
            hasMaskChannelSelector[i].first = true;
            if (!mainPage) {
                mainPage = getOrCreateMainPage();
            }
        }
    }

    LayerKnobSpec layerKnobSpec = _imp->effect->getLayerKnobSpec();
    KnobIPtr lastKnobBeforeAdvancedOption;

    if (!mainPage) {
        mainPage = getOrCreateMainPage();
    }
    if (layerKnobSpec.kind != LayerKnobSpec::eKindNone) {
        createLayerKnob(layerKnobSpec, mainPage);
        adoptChannelQuad();
    }

    ///Find in the plug-in the Mask/Mix related parameter to re-order them so it is consistent across nodes
    std::vector<std::pair<std::string, KnobIPtr> > foundPluginDefaultKnobsToReorder;
    foundPluginDefaultKnobsToReorder.push_back( std::make_pair( kOfxMaskInvertParamName, KnobIPtr() ) );
    foundPluginDefaultKnobsToReorder.push_back( std::make_pair( kOfxMixParamName, KnobIPtr() ) );

    ///Insert auto-added knobs before mask invert if found
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        for (std::size_t j = 0; j < foundPluginDefaultKnobsToReorder.size(); ++j) {
            if (knobs[i]->getName() == foundPluginDefaultKnobsToReorder[j].first) {
                foundPluginDefaultKnobsToReorder[j].second = knobs[i];
            }
        }
    }


    assert(foundPluginDefaultKnobsToReorder.size() > 0 && foundPluginDefaultKnobsToReorder[0].first == kOfxMaskInvertParamName);

    // Spec'd nodes put the whole footer under one separator; the generic one below would split
    // (Un)premult by from the mask rows.
    const StagedImageEffect* stagedEffect = dynamic_cast<const StagedImageEffect*>(_imp->effect.get());
    if (stagedEffect) {
        const bool hasMaskSelector = std::any_of(hasMaskChannelSelector.begin(), hasMaskChannelSelector.end(),
                                                 [](const std::pair<bool, bool>& p) { return p.first; });
        if (hasMaskSelector || stagedEffect->wantsHostUnPremultSelector() || stagedEffect->imagePipelineSpec().stages.has(Stage::MaskMix)) {
            createStageFooterSeparator(mainPage);
        }
    }
    createUnPremultSelector(mainPage);

    OfxEffectInstance* isOfxEffect = dynamic_cast<OfxEffectInstance*>(_imp->effect.get());
    if (isOfxEffect && isOfxEffect->isMultiPlanar()) {
        isOfxEffect->takeOverMultiplaneChoices(mainPage);
    }

    createMaskSelectors(hasMaskChannelSelector, inputLabels, mainPage, !foundPluginDefaultKnobsToReorder[0].second.get(), &lastKnobBeforeAdvancedOption);


    //Create the host mix if needed
    if ( _imp->effect->isHostMixingEnabled() ) {
        if (!mainPage) {
            mainPage = getOrCreateMainPage();
        }
        createHostMixKnob(mainPage);
    }


    /*
     * Reposition the MaskInvert and Mix parameters declared by the plug-in
     */
    for (std::size_t i = 0; i < foundPluginDefaultKnobsToReorder.size(); ++i) {
        if (foundPluginDefaultKnobsToReorder[i].second) {
            if (!mainPage) {
                mainPage = getOrCreateMainPage();
            }
            if (foundPluginDefaultKnobsToReorder[i].second->getParentKnob() == mainPage) {
                mainPage->removeKnob( foundPluginDefaultKnobsToReorder[i].second.get() );
                mainPage->addKnob(foundPluginDefaultKnobsToReorder[i].second);
            }
        }
    }

    if (stagedEffect) {
        lastKnobBeforeAdvancedOption.reset();
    }

    if (lastKnobBeforeAdvancedOption && mainPage) {

        KnobsVec mainPageChildren = mainPage->getChildren();
        int i = 0;
        for (KnobsVec::iterator it = mainPageChildren.begin(); it != mainPageChildren.end(); ++it, ++i) {
            if (*it == lastKnobBeforeAdvancedOption) {
                if (i > 0) {
                    int j = i - 1;
                    KnobsVec::iterator prev = it;
                    --prev;
                    while (j >= 0 && (*prev)->getIsSecret()) {
                        --j;
                        --prev;
                    }

                    if ( j >= 0 && !dynamic_cast<KnobSeparator*>( prev->get() ) ) {
                        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(_imp->effect.get(), std::string(), 1, false);
                        sep->setName("advancedSep");
                        mainPage->insertKnob(i, sep);
                    }
                }

                break;
            }
        }
    }


    createNodePage(settingsPage);

    NodeGroup* isGroup = dynamic_cast<NodeGroup*>(_imp->effect.get());
    if (!isGroup && !isBackdropNode) {
        createInfoPage();
    }

    if (_imp->effect->isWriter()) {
        if (!mainPage) {
            mainPage = getOrCreateMainPage();
        }

        KnobButtonPtr renderButton = AppManager::createKnob<KnobButton>(_imp->effect.get(), tr("Render"), 1, false);
        renderButton->setHintToolTip( tr("Starts rendering the specified frame range.") );
        renderButton->setAsRenderButton();
        renderButton->setName(kNatronWriteParamStartRender);
        renderButton->setEvaluateOnChange(false);
        _imp->renderButton = renderButton;
        mainPage->addKnob(renderButton);
    }
} // Node::initializeDefaultKnobs

void
Node::initializeKnobs(bool loadingSerialization)
{
    ////Only called by the main-thread


    _imp->effect->beginChanges();

    assert( QThread::currentThread() == qApp->thread() );
    assert(!_imp->knobsInitialized);

    ///For groups, declare the plugin knobs after the node knobs because we want to use the Node page
    bool effectIsGroup = getPluginID() == PLUGINID_NATRON_GROUP;

    if (!effectIsGroup) {
        //Initialize plug-in knobs
        _imp->effect->initializeKnobsPublic();
    }

    InitializeKnobsFlag_RAII __isInitializingKnobsFlag__( _imp->effect.get() );

    if ( _imp->effect->getMakeSettingsPanel() ) {
        //initialize default knobs added by Natron
        initializeDefaultKnobs(loadingSerialization);
    }

    if (effectIsGroup) {
        _imp->effect->initializeKnobsPublic();
    }
    _imp->effect->endChanges();

    _imp->knobsInitialized = true;

    Q_EMIT knobsInitialized();
} // initializeKnobs

int
Node::getFrameStepKnobValue() const
{
    KnobIPtr knob = getKnobByName(kNatronWriteParamFrameStep);
    if (!knob) {
        return 1;
    }
    KnobInt* k = dynamic_cast<KnobInt*>(knob.get());

    if (!k) {
        return 1;
    } else {
        int v = k->getValue();

        return std::max(1, v);
    }
}

bool
Node::handleFormatKnob(KnobI* knob)
{
    KnobChoicePtr choice = _imp->pluginFormatKnobs.formatChoice.lock();

    if (!choice) {
        return false;
    }

    if ( knob != choice.get() ) {
        return false;
    }
    if (choice->getIsSecret()) {
        return true;
    }
    int curIndex = choice->getValue();
    Format f;
    if ( !getApp()->getProject()->getProjectFormatAtIndex(curIndex, &f) ) {
        assert(false);

        return true;
    }

    KnobIntPtr size = _imp->pluginFormatKnobs.size.lock();
    KnobDoublePtr par = _imp->pluginFormatKnobs.par.lock();
    assert(size && par);

    _imp->effect->beginChanges();
    size->blockValueChanges();
    size->setValues(f.width(), f.height(), ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    size->unblockValueChanges();
    par->blockValueChanges();
    par->setValue( f.getPixelAspectRatio() );
    par->unblockValueChanges();

    _imp->effect->endChanges();

    return true;
}

void
Node::refreshFormatParamChoice(const std::vector<ChoiceOption>& entries,
                               int defValue,
                               bool loadingProject)
{
    KnobChoicePtr choice = _imp->pluginFormatKnobs.formatChoice.lock();

    if (!choice) {
        return;
    }
    int curIndex = choice->getValue();
    choice->populateChoices(entries);
    choice->beginChanges();
    choice->setDefaultValueWithoutApplying(defValue);
    if (!loadingProject) {
        //changedKnob was not called because we are initializing knobs
        handleFormatKnob( choice.get() );
    } else {
        if ( curIndex < (int)entries.size() ) {
            choice->setValue(curIndex);
        }
    }

    choice->endChanges();
}

void
Node::refreshPreviewsAfterProjectLoad()
{
    computePreviewImage( getApp()->getTimeLine()->currentFrame() );
    Q_EMIT s_refreshPreviewsAfterProjectLoadRequested();
}


bool
Node::isForceCachingEnabled() const
{
    KnobBoolPtr b = _imp->forceCaching.lock();

    return b ? b->getValue() : false;
}

void
Node::onSetSupportRenderScaleMaybeSet(int support)
{
    if ( (EffectInstance::SupportsEnum)support == EffectInstance::eSupportsYes ) {
        KnobBoolPtr b = _imp->useFullScaleImagesWhenRenderScaleUnsupported.lock();
        if (b) {
            b->setSecretByDefault(true);
            b->setSecret(true);
        }
    }
}

bool
Node::useScaleOneImagesWhenRenderScaleSupportIsDisabled() const
{
    KnobBoolPtr b =  _imp->useFullScaleImagesWhenRenderScaleUnsupported.lock();

    return b ? b->getValue() : false;
}

void
Node::beginEditKnobs()
{
    _imp->effect->beginEditKnobs();
}

void
Node::setEffect(const EffectInstancePtr& effect)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    _imp->effect = effect;
    _imp->effect->initializeData();
}

EffectInstancePtr
Node::getEffectInstance() const
{
    ///Thread safe as it never changes
    return _imp->effect;
}

namespace {
// Depth first through the outputs, visiting each node once; stop decides at a node whether to
// collect it and go no further below it. Every edit runs it over everything downstream of it, so
// the visited test is a hash lookup and the outputs are gathered into reused vectors.
template <typename Stop>
void
walkDownstreamUntil(const Node* start,
                    Stop stop)
{
    std::unordered_set<const Node*> visited;
    std::vector<NodePtr> outputs;
    std::vector<NodePtr> stack;

    visited.insert(start);
    if (stop(start)) {
        return;
    }
    start->appendOutputsWithGroupRedirection(&outputs);
    stack.insert(stack.end(), outputs.rbegin(), outputs.rend());
    while (!stack.empty()) {
        NodePtr node = std::move(stack.back());
        stack.pop_back();
        assert(node);
        if (!visited.insert(node.get()).second || stop(node.get())) {
            continue;
        }
        outputs.clear();
        node->appendOutputsWithGroupRedirection(&outputs);
        stack.insert(stack.end(), outputs.rbegin(), outputs.rend());
    }
}
} // anonymous namespace

void
Node::hasOutputNodesConnected(std::list<OutputEffectInstance*>* writers) const
{
    walkDownstreamUntil(this, [writers](const Node* node) {
        OutputEffectInstance* writer = dynamic_cast<OutputEffectInstance*>(node->getEffectInstance().get());
        if (writer && writer->isOutput() && !dynamic_cast<GroupOutput*>(writer)) {
            writers->push_back(writer);

            return true;
        }

        return false;
    });
}

void
Node::hasViewersConnected(std::list<ViewerInstance* >* viewers) const
{
    walkDownstreamUntil(this, [viewers](const Node* node) {
        ViewerInstance* viewer = dynamic_cast<ViewerInstance*>(node->getEffectInstance().get());
        if (viewer) {
            viewers->push_back(viewer);

            return true;
        }

        return false;
    });
}

int
Node::getMajorVersion() const
{
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if (_imp->pyPlugInfo.isPyPlug) {
            return _imp->pyPlugInfo.pyPlugVersion;
        }
    }
    if (!_imp->plugin) {
        return 0;
    }

    return _imp->plugin->getMajorVersion();
}

int
Node::getMinorVersion() const
{
    ///Thread safe as it never changes
    if (!_imp->plugin) {
        return 0;
    }
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if ( _imp->pyPlugInfo.isPyPlug ) {
            return 0;
        }
    }

    return _imp->plugin->getMinorVersion();
}




void
Node::clearLastRenderedImage()
{
    _imp->effect->clearLastRenderedImage();
}

/*After this call this node still knows the link to the old inputs/outputs
   but no other node knows this node.*/
void
Node::deactivate(const std::list<NodePtr> & outputsToDisconnect,
                 bool disconnectAll,
                 bool reconnect,
                 bool hideGui,
                 bool triggerRender,
                 bool unslaveKnobs)
{

    if ( !_imp->effect || !isActivated() ) {
        return;
    }
    //first tell the gui to clear any persistent message linked to this node
    clearPersistentMessage(false);



    bool beingDestroyed;
    {
        QMutexLocker k(&_imp->isBeingDestroyedMutex);
        beingDestroyed = _imp->isBeingDestroyed;
    }


    ///kill any thread it could have started
    ///Commented-out: If we were to undo the deactivate we don't want all threads to be
    ///exited, just exit them when the effect is really deleted instead
    //quitAnyProcessing();
    if (!beingDestroyed) {
        _imp->effect->abortAllRendersReadingThis();
        _imp->abortPreview_non_blocking();
    }

    NodeCollectionPtr parentCol = getGroup();


    if (unslaveKnobs) {
        ///For all knobs that have listeners, invalidate expressions
        NodeGroup* isParentGroup = dynamic_cast<NodeGroup*>( parentCol.get() );
        // The effect holding a knob that reads this node from outside it and its enclosing group,
        // or null.
        const auto outsideReader = [this, isParentGroup](const KnobIPtr& listener) -> EffectInstance* {
            KnobHolder* holder = listener->getHolder();
            if (!holder || (holder == _imp->effect.get()) || (holder == isParentGroup)) {
                return 0;
            }
            EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder);
            if (!isEffect) {
                return 0;
            }
            NodeCollectionPtr effectParent = isEffect->getNode()->getGroup();
            if (!effectParent) {
                return 0;
            }
            NodeGroup* isEffectParentGroup = dynamic_cast<NodeGroup*>(effectParent.get());
            if (isEffectParentGroup && (isEffectParentGroup == _imp->effect.get())) {
                return 0;
            }

            return isEffect;
        };
        std::stringstream missingNode;
        missingNode << tr("Missing node ").toStdString();
        missingNode << getFullyQualifiedName();
        missingNode << ' ';
        missingNode << tr("in expression.").toStdString();

        KnobsVec  knobs = _imp->effect->getKnobs_mt_safe();
        for (U32 i = 0; i < knobs.size(); ++i) {
            KnobI::ListenerDimsMap listeners;
            knobs[i]->getListeners(listeners);
            for (KnobI::ListenerDimsMap::iterator it = listeners.begin(); it != listeners.end(); ++it) {
                KnobIPtr listener = it->first.lock();
                if (!listener) {
                    continue;
                }
                EffectInstance* isEffect = outsideReader(listener);
                if (!isEffect) {
                    continue;
                }

                isEffect->beginChanges();
                for (int dim = 0; dim < listener->getDimension(); ++dim) {
                    std::pair<int, KnobIPtr> master = listener->getMaster(dim);
                    if (master.second == knobs[i]) {
                        listener->unSlave(dim, true);
                    }

                    std::string hasExpr = listener->getExpression(dim);
                    if ( !hasExpr.empty() ) {
                        listener->setExpressionInvalid(dim, false, missingNode.str());
                    }
                }
                isEffect->endChanges(true);
            }
        }

        // Their cached results were derived from this node's metadata.
        std::list<std::pair<KnobIPtr, int>> metadataReaders;
        getMetadataReaders(&metadataReaders);
        for (std::list<std::pair<KnobIPtr, int>>::const_iterator it = metadataReaders.begin(); it != metadataReaders.end(); ++it) {
            EffectInstance* isEffect = outsideReader(it->first);
            if (!isEffect || it->first->getExpression(it->second).empty()) {
                continue;
            }
            isEffect->beginChanges();
            it->first->clearExpressionsResults(it->second);
            it->first->setExpressionInvalid(it->second, false, missingNode.str());
            isEffect->endChanges(true);
        }
    }

    ///if the node has 1 non-optional input, attempt to connect the outputs to the input of the current node
    ///this node is the node the outputs should attempt to connect to
    NodePtr inputToConnectTo;
    NodePtr firstOptionalInput;
    int firstNonOptionalInput = -1;
    if (reconnect) {
        bool hasOnlyOneInputConnected = false;

        ///No need to lock guiInputs is only written to by the mainthread
        for (std::size_t i = 0; i < _imp->guiInputs.size(); ++i) {
            NodePtr input = _imp->guiInputs[i].lock();
            if (input) {
                if ( !_imp->effect->isInputOptional(i) ) {
                    if (firstNonOptionalInput == -1) {
                        firstNonOptionalInput = i;
                        hasOnlyOneInputConnected = true;
                    } else {
                        hasOnlyOneInputConnected = false;
                    }
                } else if (!firstOptionalInput) {
                    firstOptionalInput = input;
                    if (hasOnlyOneInputConnected) {
                        hasOnlyOneInputConnected = false;
                    } else {
                        hasOnlyOneInputConnected = true;
                    }
                }
            }
        }

        if (hasOnlyOneInputConnected) {
            if (firstNonOptionalInput != -1) {
                inputToConnectTo = getRealGuiInput(firstNonOptionalInput);
            } else if (firstOptionalInput) {
                inputToConnectTo = firstOptionalInput;
            }
        }
    }
    /*Removing this node from the output of all inputs*/
    _imp->deactivatedState.clear();


    std::vector<NodePtr> inputsQueueCopy;


    ///For multi-instances, if we deactivate the main instance without hiding the GUI (the default state of the tracker node)
    ///then don't remove it from outputs of the inputs
    if (hideGui || !_imp->isMultiInstance) {
        for (std::size_t i = 0; i < _imp->guiInputs.size(); ++i) {
            NodePtr input = _imp->guiInputs[i].lock();
            if (input) {
                input->disconnectOutput(false, this);
            }
        }
    }


    ///For each output node we remember that the output node  had its input number inputNb connected
    ///to this node
    NodesWList outputsQueueCopy;
    {
        QMutexLocker l(&_imp->outputsMutex);
        outputsQueueCopy = _imp->guiOutputs;
    }


    for (NodesWList::iterator it = outputsQueueCopy.begin(); it != outputsQueueCopy.end(); ++it) {
        NodePtr output = it->lock();
        if (!output) {
            continue;
        }
        bool dc = false;
        if (disconnectAll) {
            dc = true;
        } else {
            for (NodesList::const_iterator found = outputsToDisconnect.begin(); found != outputsToDisconnect.end(); ++found) {
                if (*found == output) {
                    dc = true;
                    break;
                }
            }
        }
        if (dc) {
            int inputNb = output->getInputIndex(this);
            if (inputNb != -1) {
                _imp->deactivatedState.insert( make_pair(*it, inputNb) );

                ///reconnect if inputToConnectTo is not null
                if (inputToConnectTo) {
                    output->replaceInputInternal(inputToConnectTo, inputNb, false);
                } else {
                    ignore_result( output->disconnectInputInternal(this, false) );
                }
            }
        }
    }

    // If the effect was doing OpenGL rendering and had context(s) bound, detach them.
    _imp->effect->dettachAllOpenGLContexts();

    ///Free all memory used by the plug-in.

    ///COMMENTED-OUT: Don't do this, the node may still be rendering here since the abort call is not blocking.
    ///_imp->effect->clearPluginMemoryChunks();
    clearLastRenderedImage();

    if (parentCol && !beingDestroyed) {
        parentCol->notifyNodeDeactivated( shared_from_this() );
    }

    if (hideGui && !beingDestroyed) {
        Q_EMIT deactivated(triggerRender);
    }
    {
        QMutexLocker l(&_imp->activatedMutex);
        _imp->activated = false;
    }
    emitLinksChanged(NodesList());

    ///If the node is a group, deactivate all nodes within the group
    NodeGroup* isGrp = dynamic_cast<NodeGroup*>( _imp->effect.get() );
    if (isGrp) {
        isGrp->setIsDeactivatingGroup(true);
        NodesList nodes = isGrp->getNodes();
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            (*it)->deactivate(std::list<NodePtr>(), false, false, true, false);
        }
        isGrp->setIsDeactivatingGroup(false);
    }

    ///If the node has children (i.e it is a multi-instance), deactivate its children
    for (NodesWList::iterator it = _imp->children.begin(); it != _imp->children.end(); ++it) {
        it->lock()->deactivate(std::list<NodePtr>(), false, false, true, false);
    }


    AppInstancePtr app = getApp();
    if ( app && !app->getProject()->isProjectClosing() ) {
        _imp->runOnNodeDeleteCB();
    }

    deleteNodeVariableToPython( getFullyQualifiedName() );
    _imp->notifyLayerReferencesChanged();
} // deactivate

void
Node::activate(const std::list<NodePtr> & outputsToRestore,
               bool restoreAll,
               bool triggerRender)
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !_imp->effect || isActivated() ) {
        return;
    }


    ///No need to lock, guiInputs is only written to by the main-thread
    NodePtr thisShared = shared_from_this();

    ///for all inputs, reconnect their output to this node
    for (std::size_t i = 0; i < _imp->inputs.size(); ++i) {
        NodePtr input = _imp->inputs[i].lock();
        if (input) {
            input->connectOutput(false, thisShared);
        }
    }


    ///Restore all outputs that was connected to this node
    for (DeactivatedState::iterator it = _imp->deactivatedState.begin();
         it != _imp->deactivatedState.end(); ++it) {
        NodePtr output = it->first.lock();
        if (!output) {
            continue;
        }

        bool restore = false;
        if (restoreAll) {
            restore = true;
        } else {
            for (NodesList::const_iterator found = outputsToRestore.begin(); found != outputsToRestore.end(); ++found) {
                if (*found == output) {
                    restore = true;
                    break;
                }
            }
        }

        if (restore) {
            ///before connecting the outputs to this node, disconnect any link that has been made
            ///between the outputs by the user. This should normally never happen as the undo/redo
            ///stack follow always the same order.
            NodePtr outputHasInput = output->getInput(it->second);
            if (outputHasInput) {
                bool ok = getApp()->getProject()->disconnectNodes(outputHasInput, output);
                assert(ok);
                Q_UNUSED(ok);
            }

            ///and connect the output to this node
            output->connectInput(thisShared, it->second);
        }
    }

    {
        QMutexLocker l(&_imp->activatedMutex);
        _imp->activated = true; //< flag it true before notifying the GUI because the gui rely on this flag (especially the Viewer)
    }

    NodeCollectionPtr group = getGroup();
    if (group) {
        group->notifyNodeActivated( shared_from_this() );
    }
    Q_EMIT activated(triggerRender);
    emitLinksChanged(NodesList());

    declareAllPythonAttributes();
    getApp()->recheckInvalidExpressions();


    ///If the node is a group, activate all nodes within the group first
    NodeGroup* isGrp = dynamic_cast<NodeGroup*>( _imp->effect.get() );
    if (isGrp) {
        isGrp->setIsActivatingGroup(true);
        NodesList nodes = isGrp->getNodes();
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            (*it)->activate(std::list<NodePtr>(), false, false);
        }
        isGrp->setIsActivatingGroup(false);
    }

    ///If the node has children (i.e it is a multi-instance), activate its children
    for (NodesWList::iterator it = _imp->children.begin(); it != _imp->children.end(); ++it) {
        it->lock()->activate(std::list<NodePtr>(), false, false);
    }

    _imp->runOnNodeCreatedCB(true);
    _imp->notifyLayerReferencesChanged();
} // activate





KnobIPtr
Node::getKnobByName(const std::string & name) const
{
    ///MT-safe, never changes
    assert(_imp->knobsInitialized);
    if (!_imp->effect) {
        return KnobIPtr();
    }

    return _imp->effect->getKnobByName(name);
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

///output is always RGBA with alpha = 255
template <typename PIX, int maxValue, int srcNComps>
void
renderPreview(const Image& srcImg,
              int* dstWidth,
              int* dstHeight,
              const ProjectColorManagement::DisplayProcessor* processor,
              unsigned int* dstPixels)
{
#if defined(DEBUG) && !defined(DEBUG_NAN)
    // Some plugins generate FP exceptions
    boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
    ///recompute it after the rescaling
    const RectI & srcBounds = srcImg.getBounds();
    double yZoomFactor = *dstHeight / (double)srcBounds.height();
    double xZoomFactor = *dstWidth / (double)srcBounds.width();
    double zoomFactor;

    if (xZoomFactor < yZoomFactor) {
        zoomFactor = xZoomFactor;
        *dstHeight = srcBounds.height() * zoomFactor;
    } else {
        zoomFactor = yZoomFactor;
        *dstWidth = srcBounds.width() * zoomFactor;
    }

    Image::ReadAccess acc = srcImg.getReadRights();

    std::vector<float> scanline((std::size_t)*dstWidth * 4);
    std::vector<char> inBounds((std::size_t)*dstWidth);

    for (int i = 0; i < *dstHeight; ++i) {
        double y = (i - *dstHeight / 2.) / zoomFactor + (srcBounds.y1 + srcBounds.y2) / 2.;
        int yi = std::floor(y + 0.5);
        U32 *dst_pixels = dstPixels + *dstWidth * (*dstHeight - 1 - i);
        const PIX* src_pixels = (const PIX*)acc.pixelAt(srcBounds.x1, yi);
        std::fill(inBounds.begin(), inBounds.end(), 0);
        if (src_pixels) {
            for (int j = 0; j < *dstWidth; ++j) {
                // bilinear interpolation is pointless when downscaling a lot, and this is a preview anyway.
                // just use nearest neighbor
                double x = (j - *dstWidth / 2.) / zoomFactor + (srcBounds.x1 + srcBounds.x2) / 2.;
                int xi = std::floor(x + 0.5) - srcBounds.x1;     // round to nearest
                if ( (xi < 0) || ( xi >= (srcBounds.x2 - srcBounds.x1) ) ) {
                    continue;
                }
                float* px = &scanline[(std::size_t)j * 4];
                const PIX* src = src_pixels + xi * srcNComps;
                if (maxValue == 1) {
                    px[0] = (float)src[0];
                    px[1] = srcNComps < 2 ? px[0] : (float)src[1];
                    px[2] = srcNComps < 3 ? (srcNComps == 1 ? px[0] : 0.f) : (float)src[2];
                } else {
                    px[0] = Color::intToFloat<maxValue + 1>((int)src[0]);
                    px[1] = srcNComps < 2 ? px[0] : Color::intToFloat<maxValue + 1>((int)src[1]);
                    px[2] = srcNComps < 3 ? (srcNComps == 1 ? px[0] : 0.f) : Color::intToFloat<maxValue + 1>((int)src[2]);
                }
                px[3] = 1.f;
                inBounds[j] = 1;
            }
            if (processor) {
                applyViewerDisplayTransform(*processor, &scanline[0], *dstWidth, 1., 0., 1.);
            }
        }
        for (int j = 0; j < *dstWidth; ++j) {
            if (!inBounds[j]) {
#ifndef __NATRON_WIN32__
                dst_pixels[j] = toBGRA(0, 0, 0, 0);
#else
                dst_pixels[j] = toBGRA(0, 0, 0, 255);
#endif
            } else {
                const float* px = &scanline[(std::size_t)j * 4];
                dst_pixels[j] = toBGRA(Color::floatToInt<256>(px[0]),
                                       Color::floatToInt<256>(px[1]),
                                       Color::floatToInt<256>(px[2]),
                                       255);
            }
        }
    }
}     // renderPreview

///output is always RGBA with alpha = 255
template <typename PIX, int maxValue>
void
renderPreviewForDepth(const Image& srcImg,
                      int elemCount,
                      int* dstWidth,
                      int* dstHeight,
                      const ProjectColorManagement::DisplayProcessor* processor,
                      unsigned int* dstPixels)
{
    switch (elemCount) {
    case 0:

        return;
    case 1:
        renderPreview<PIX, maxValue, 1>(srcImg, dstWidth, dstHeight, processor, dstPixels);
        break;
    case 2:
        renderPreview<PIX, maxValue, 2>(srcImg, dstWidth, dstHeight, processor, dstPixels);
        break;
    case 3:
        renderPreview<PIX, maxValue, 3>(srcImg, dstWidth, dstHeight, processor, dstPixels);
        break;
    case 4:
        renderPreview<PIX, maxValue, 4>(srcImg, dstWidth, dstHeight, processor, dstPixels);
        break;
    default:
        break;
    }
}     // renderPreviewForDepth

NATRON_NAMESPACE_ANONYMOUS_EXIT


class ComputingPreviewSetter_RAII
{
    Node::Implementation* _imp;

public:
    ComputingPreviewSetter_RAII(Node::Implementation* imp)
        : _imp(imp)
    {
        _imp->setComputingPreview(true);
    }

    ~ComputingPreviewSetter_RAII()
    {
        _imp->setComputingPreview(false);

        bool mustQuitPreview = _imp->checkForExitPreview();
        Q_UNUSED(mustQuitPreview);
    }
};

bool
Node::makePreviewImage(SequenceTime time,
                       int *width,
                       int *height,
                       unsigned int* buf)
{
    assert(_imp->knobsInitialized);


    {
        QMutexLocker k(&_imp->isBeingDestroyedMutex);
        if (_imp->isBeingDestroyed) {
            return false;
        }
    }

    if ( _imp->checkForExitPreview() ) {
        return false;
    }

    /// prevent 2 previews to occur at the same time since there's only 1 preview instance
    ComputingPreviewSetter_RAII computingPreviewRAII( _imp.get() );
    RectD rod;
    bool isProjectFormat;
    EffectInstance* effect = 0;
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( _imp->effect.get() );
    if (isGroup) {
        effect = isGroup->getOutputNode(false)->getEffectInstance().get();
    } else {
        effect = _imp->effect.get();
    }

    if (!_imp->effect) {
        return false;
    }
    // A group's hash does not cover its inner graph, so the output node's own hash must key its
    // actions cache here.
    U64 nodeHash = effect->getHash();

    effect->getNode()->clearPersistentMessageForPreview();

    StatusEnum stat = effect->getRegionOfDefinition_public(nodeHash, time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
    if ( (stat == eStatusFailed) || rod.isNull() ) {
        return false;
    }
    assert( !rod.isNull() );
    double yZoomFactor = (double)*height / (double)rod.height();
    double xZoomFactor = (double)*width / (double)rod.width();
    double closestPowerOf2X = xZoomFactor >= 1 ? 1 : ipow( 2, (int)-std::ceil( std::log(xZoomFactor) / M_LN2 ) );
    double closestPowerOf2Y = yZoomFactor >= 1 ? 1 : ipow( 2, (int)-std::ceil( std::log(yZoomFactor) / M_LN2 ) );
    int closestPowerOf2 = std::max(closestPowerOf2X, closestPowerOf2Y);
    unsigned int mipmapLevel = std::min(std::log( (double)closestPowerOf2 ) / std::log(2.), 5.);

    const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);

    const NodeMetadataConstPtr metadata = effect->getMetadata(time, ViewIdx(0));
    const double par = metadata->getPixelAspectRatio(-1);
    const RectI renderWindow = rod.toPixelEnclosing(mipmapLevel, par);

    NodePtr thisNode = shared_from_this();
    RenderingFlagSetter flagIsRendering(thisNode);


    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        const bool isRenderUserInteraction = true;
        const bool isSequentialRender = false;
        AbortableThread* isAbortable = dynamic_cast<AbortableThread*>( QThread::currentThread() );
        if (isAbortable) {
            isAbortable->setAbortInfo( isRenderUserInteraction, abortInfo, thisNode->getEffectInstance() );
        }
        ParallelRenderArgsSetter frameRenderArgs( time,
                                                  ViewIdx(0), //< preview only renders view 0 (left)
                                                  isRenderUserInteraction, //<isRenderUserInteraction
                                                  isSequentialRender, //isSequential
                                                  abortInfo, // abort info
                                                  thisNode, // viewer requester
                                                  0, //texture index
                                                  getApp()->getTimeLine().get(), // timeline
                                                  NodePtr(), //rotoPaint node
                                                  false, // isAnalysis
                                                  true, // isDraft
                                                  RenderStatsPtr() );
        FrameRequestMap request;
        stat = EffectInstance::computeRequestPass(time, ViewIdx(0), mipmapLevel, rod, thisNode, request);
        if (stat == eStatusFailed) {
            return false;
        }

        frameRenderArgs.updateNodesRequest(request);

        std::list<ImageLayerDesc> requestedComps;
        ImageBitDepthEnum depth = metadata->getBitDepth(-1);
        {
            ImageLayerDesc layer, pairedLayer;
            EffectInstance::getLayersFromMetadata(*metadata, -1, &layer, &pairedLayer);
            requestedComps.push_back(layer);
        }


        // Exceptions are caught because the program can run without a preview,
        // but any exception in renderROI is probably fatal.
        std::map<ImageLayerDesc, ImagePtr> layers;
        try {
            std::unique_ptr<EffectInstance::RenderRoIArgs> renderArgs( new EffectInstance::RenderRoIArgs(time,
                                                                                                           scale,
                                                                                                           mipmapLevel,
                                                                                                           ViewIdx(0), //< preview only renders view 0 (left)
                                                                                                           false,
                                                                                                           renderWindow,
                                                                                                           rod,
                                                                                                           requestedComps, //< preview is always rgb...
                                                                                                           depth,
                                                                                                           false,
                                                                                                           effect,
                                                                                                           eStorageModeRAM /*returnStorage*/,
                                                                                                           time /*callerRenderTime*/) );
            EffectInstance::RenderRoIRetCode retCode;
            retCode = effect->renderRoI(*renderArgs, &layers);
            if (retCode != EffectInstance::eRenderRoIRetCodeOk) {
                return false;
            }
        } catch (...) {
            return false;
        }

        if (layers.empty()) {
            return false;
        }

        const ImagePtr& img = layers.begin()->second;
        const ImageLayerDesc& components = img->getComponents();
        int elemCount = components.getNumComponents();

        ProjectColorManagement::DisplayProcessorPtr processor;
        {
            const ProjectPtr project = getApp()->getProject();
            const ProjectColorManagementPtr colorManagement = project->getColorManagement();
            std::string display, view;
            project->getDefaultDisplayView(&display, &view);
            processor = colorManagement->getDisplayProcessor(project->getWorkingColorSpace(), display, view, std::string());
        }

        switch ( img->getBitDepth() ) {
        case eImageBitDepthByte: {
            renderPreviewForDepth<unsigned char, 255>(*img, elemCount, width, height, processor.get(), buf);
            break;
        }
        case eImageBitDepthShort: {
            renderPreviewForDepth<unsigned short, 65535>(*img, elemCount, width, height, processor.get(), buf);
            break;
        }
        case eImageBitDepthHalf:
            break;
        case eImageBitDepthFloat: {
            renderPreviewForDepth<float, 1>(*img, elemCount, width, height, processor.get(), buf);
            break;
        }
        case eImageBitDepthNone:
            break;
        }
    } // ParallelRenderArgsSetter

    ///Exit of the thread
    appPTR->getAppTLS()->cleanupTLSForThread();

    return true;
} // makePreviewImage

bool
Node::isInputNode() const
{
    ///MT-safe, never changes
    return _imp->effect->isGenerator() && ! _imp->effect->isFilter();
}

bool
Node::isOutputNode() const
{   ///MT-safe, never changes
    return _imp->effect->isOutput();
}

bool
Node::isOpenFXNode() const
{
    ///MT-safe, never changes
    return _imp->effect->isOpenFX();
}

bool
Node::isRotoNode() const
{
    ///Runs only in the main thread (checked by getName())

    ///Crude way to distinguish between Rotoscoping and Rotopainting nodes.
    return getPluginID() == PLUGINID_OFX_ROTO;
}

/**
 * @brief Returns true if the node is a rotopaint node
 **/
bool
Node::isRotoPaintingNode() const
{
    return _imp->effect ? _imp->effect->isRotoPaintNode() : false;
}

ViewerInstance*
Node::isEffectViewer() const
{
    return dynamic_cast<ViewerInstance*>( _imp->effect.get() );
}

NodeGroup*
Node::isEffectGroup() const
{
    return dynamic_cast<NodeGroup*>( _imp->effect.get() );
}

RotoContextPtr
Node::getRotoContext() const
{
    return _imp->rotoContext;
}

RotoCurveContextPtr
Node::getCurveContext() const
{
    return _imp->curveContext;
}

TrackerContextPtr
Node::getTrackerContext() const
{
    return _imp->trackContext;
}

U64
Node::getRotoAge() const
{
    if (_imp->curveContext) {
        return _imp->curveContext->getAge();
    }

    RotoDrawableItemPtr item = _imp->paintStroke.lock();
    if (item) {
        return item->getContext()->getAge();
    }

    return 0;
}

const KnobsVec &
Node::getKnobs() const
{
    ///MT-safe from EffectInstance::getKnobs()
    return _imp->effect->getKnobs();
}

void
Node::setKnobsFrozen(bool frozen)
{
    ///MT-safe from EffectInstance::setKnobsFrozen
    _imp->effect->setKnobsFrozen(frozen);

    QMutexLocker l(&_imp->inputsMutex);
    for (std::size_t i = 0; i < _imp->inputs.size(); ++i) {
        NodePtr input = _imp->inputs[i].lock();
        if (input) {
            input->setKnobsFrozen(frozen);
        }
    }
}

std::string
Node::getPluginIconFilePath() const
{
    if (!_imp->plugin) {
        return std::string();
    }

    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if ( !_imp->pyPlugInfo.pyPlugIconFilePath.empty() ) {
            return _imp->pyPlugInfo.pyPlugIconFilePath;
        }
    }
    return _imp->plugin->getIconFilePath().toStdString();
}

std::string
Node::getPluginID() const
{
    if (!_imp->plugin) {
        return std::string();
    }

    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if ( _imp->pyPlugInfo.isPyPlug ) {
            return _imp->pyPlugInfo.pyPlugID;
        }
    }

    return _imp->plugin->getPluginID().toStdString();
}

std::string
Node::getPluginLabel() const
{
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if ( !_imp->pyPlugInfo.pyPlugLabel.empty() ) {
            return _imp->pyPlugInfo.pyPlugLabel;
        }
    }

    return _imp->effect->getPluginLabel();
}

std::string
Node::getPluginResourcesPath() const
{
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if ( !_imp->pyPlugInfo.pluginPythonModule.empty() ) {
            std::size_t foundSlash = _imp->pyPlugInfo.pluginPythonModule.find_last_of("/");
            if (foundSlash != std::string::npos) {
                return _imp->pyPlugInfo.pluginPythonModule.substr(0, foundSlash);
            }
        }
    }

    return _imp->plugin->getResourcesPath().toStdString();
}

std::string
Node::getPluginDescription() const
{
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if ( _imp->pyPlugInfo.isPyPlug ) {
            return _imp->pyPlugInfo.pyPlugDesc;
        }
    }

    return _imp->effect->getPluginDescription();
}

void
Node::getPluginGrouping(std::list<std::string>* grouping) const
{
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        if ( !_imp->pyPlugInfo.pyPlugGrouping.empty() ) {
            *grouping =  _imp->pyPlugInfo.pyPlugGrouping;
            return;
        }
    }
    _imp->effect->getPluginGrouping(grouping);
}

NodeCategoryEnum
Node::getNodeCategory() const
{
    EffectInstancePtr effect = getEffectInstance();
    if (!effect) {
        return eNodeCategoryOther;
    }

    std::list<std::string> grouping;
    getPluginGrouping(&grouping);

    // Domain categories carry their own colour identity and win over Reader/Writer/Generator:
    // this is what makes DeepWrite (which also sets isWriter()) and DeepRead (which does not)
    // agree on being coloured as Deep, rather than one of them falling through to Writer grey.
    NodeCategoryEnum domainCategory;
    if (domainCategoryFromGrouping(grouping, &domainCategory)) {
        return domainCategory;
    }

    if (effect->isReader()) {
        return eNodeCategoryRead;
    }
    if (effect->isWriter()) {
        return eNodeCategoryWrite;
    }
    if (effect->isGenerator()) {
        return eNodeCategoryGenerator;
    }

    return categoryFromGroupingAndLabel(grouping.empty() ? std::string() : grouping.front(), getPluginLabel());
}

NodeCategoryEnum
Node::categoryForPlugin(const Plugin& plugin)
{
    const QStringList& qgrouping = plugin.getGrouping();
    std::list<std::string> grouping;
    for (QStringList::const_iterator it = qgrouping.begin(); it != qgrouping.end(); ++it) {
        grouping.push_back(it->toStdString());
    }

    NodeCategoryEnum domainCategory;
    if (domainCategoryFromGrouping(grouping, &domainCategory)) {
        return domainCategory;
    }

    if (plugin.isReader()) {
        return eNodeCategoryRead;
    }
    if (plugin.isWriter()) {
        return eNodeCategoryWrite;
    }

    return categoryFromGroupingAndLabel(grouping.empty() ? std::string() : grouping.front(), plugin.getPluginLabel().toStdString());
}

bool
Node::domainCategoryFromGrouping(const std::list<std::string>& grouping,
                                 NodeCategoryEnum* category)
{
    if (grouping.empty()) {
        return false;
    }
    const std::string& major = grouping.front();
    if (major == PLUGIN_GROUP_DEEP) {
        *category = eNodeCategoryDeep;

        return true;
    }
    if (major == PLUGIN_GROUP_3D) {
        std::list<std::string>::const_iterator minor = grouping.begin();
        ++minor;
        *category = (minor != grouping.end() && PLUGIN_GROUP_3D + std::string("/") + *minor == PLUGIN_GROUP_3D_USD) ? eNodeCategoryUsd3D : eNodeCategoryNative3D;

        return true;
    }

    return false;
}

// Splits on non-alphanumeric characters, letter/digit transitions and camel case, so that
// "ZBlur", "Roto_Paint2" or "KeyMix" yield whole words a keyword must equal.
static std::vector<std::string>
labelWords(const std::string& label)
{
    std::vector<std::string> words;
    std::string current;
    const std::size_t n = label.size();

    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(label[i]);
        if (!std::isalnum(c)) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        if (!current.empty()) {
            const unsigned char prev = static_cast<unsigned char>(label[i - 1]);
            const bool lowerToUpper = std::islower(prev) && std::isupper(c);
            const bool acronymEnd = std::isupper(prev) && std::isupper(c) && (i + 1 < n) && std::islower(static_cast<unsigned char>(label[i + 1]));
            const bool digitBoundary = (std::isdigit(prev) != 0) != (std::isdigit(c) != 0);
            if (lowerToUpper || acronymEnd || digitBoundary) {
                words.push_back(current);
                current.clear();
            }
        }
        current.push_back(static_cast<char>(std::tolower(c)));
    }
    if (!current.empty()) {
        words.push_back(current);
    }

    return words;
}

NodeCategoryEnum
Node::categoryFromGroupingAndLabel(const std::string& majorGrouping,
                                   const std::string& label)
{
    if (majorGrouping == PLUGIN_GROUP_COLOR) {
        return eNodeCategoryColor;
    }
    if (majorGrouping == PLUGIN_GROUP_FILTER) {
        return eNodeCategoryFilter;
    }
    if (majorGrouping == PLUGIN_GROUP_CHANNEL) {
        return eNodeCategoryChannel;
    }
    if (majorGrouping == PLUGIN_GROUP_KEYER) {
        return eNodeCategoryKeyer;
    }
    if (majorGrouping == PLUGIN_GROUP_MERGE) {
        return eNodeCategoryMerge;
    }
    if (majorGrouping == PLUGIN_GROUP_PAINT) {
        return eNodeCategoryDraw;
    }
    if (majorGrouping == PLUGIN_GROUP_TIME) {
        return eNodeCategoryTime;
    }
    if (majorGrouping == PLUGIN_GROUP_TRANSFORM) {
        return eNodeCategoryTransform;
    }
    if (majorGrouping == PLUGIN_GROUP_MULTIVIEW) {
        return eNodeCategoryViews;
    }

    // The label is the last signal available for a third-party OFX plugin with an arbitrary
    // grouping string. Earlier entries win, which is why "mix" precedes "key": a KeyMix
    // combines two inputs through a mask, it does not pull a key.
    struct KeywordEntry {
        const char* keyword;
        NodeCategoryEnum category;
    };
    static const KeywordEntry keywordTable[] = {
        { "blur", eNodeCategoryFilter },
        { "defocus", eNodeCategoryFilter },
        { "merge", eNodeCategoryMerge },
        { "over", eNodeCategoryMerge },
        { "plus", eNodeCategoryMerge },
        { "mix", eNodeCategoryMerge },
        { "key", eNodeCategoryKeyer },
        { "keyer", eNodeCategoryKeyer },
        { "grade", eNodeCategoryColor },
        { "color", eNodeCategoryColor },
        { "colour", eNodeCategoryColor },
        { "hue", eNodeCategoryColor },
        { "sat", eNodeCategoryColor },
        { "saturation", eNodeCategoryColor },
        { "transform", eNodeCategoryTransform },
        { "crop", eNodeCategoryTransform },
        { "reformat", eNodeCategoryTransform },
        { "warp", eNodeCategoryTransform },
        { "time", eNodeCategoryTime },
        { "frame", eNodeCategoryTime },
        { "paint", eNodeCategoryDraw },
        { "roto", eNodeCategoryDraw },
        { "draw", eNodeCategoryDraw },
        { "shuffle", eNodeCategoryChannel },
        { "channel", eNodeCategoryChannel },
        { "copy", eNodeCategoryChannel },
    };

    const std::vector<std::string> words = labelWords(label);

    for (std::size_t i = 0; i < sizeof(keywordTable) / sizeof(keywordTable[0]); ++i) {
        if (std::find(words.begin(), words.end(), keywordTable[i].keyword) != words.end()) {
            return keywordTable[i].category;
        }
    }

    return eNodeCategoryOther;
}

std::string
Node::getPyPlugID() const
{
    return _imp->pyPlugInfo.pyPlugID;
}

std::string
Node::getPyPlugLabel() const
{
    return _imp->pyPlugInfo.pyPlugLabel;
}

std::string
Node::getPyPlugDescription() const
{
    return _imp->pyPlugInfo.pyPlugDesc;
}

void
Node::getPyPlugGrouping(std::list<std::string>* grouping) const
{
    *grouping = _imp->pyPlugInfo.pyPlugGrouping;
}


std::string
Node::getPyPlugIconFilePath() const
{
    return _imp->pyPlugInfo.pyPlugIconFilePath;
}

int
Node::getPyPlugMajorVersion() const
{
    return _imp->pyPlugInfo.pyPlugVersion;
}

bool
Node::makePreviewByDefault() const
{
    ///MT-safe, never changes
    assert(_imp->effect);

    return _imp->effect->makePreviewByDefault();
}

void
Node::togglePreview()
{
    ///MT-safe from Knob
    assert(_imp->knobsInitialized);
    KnobBoolPtr b = _imp->previewEnabledKnob.lock();
    if (!b) {
        return;
    }
    b->setValue( !b->getValue() );
}

bool
Node::isPreviewEnabled() const
{
    ///MT-safe from EffectInstance
    if (!_imp->knobsInitialized) {
        qDebug() << "Node::isPreviewEnabled(): knobs not initialized (including previewEnabledKnob)";
    }
    KnobBoolPtr b = _imp->previewEnabledKnob.lock();
    if (!b) {
        return false;
    }

    return b->getValue();
}

bool
Node::aborted() const
{
    ///MT-safe from EffectInstance
    assert(_imp->effect);

    return _imp->effect->aborted();
}

bool
Node::message(MessageTypeEnum type,
              const std::string & content) const
{
    ///If the node was aborted, don't transmit any message because we could cause a deadlock
    if ( _imp->effect->aborted() || (!_imp->nodeCreated && _imp->wasCreatedSilently)) {
        std::cerr << getScriptName_mt_safe() << " message: " << content << std::endl;
        return false;
    }

    // See https://github.com/MrKepzie/Natron/issues/1313
    // Messages posted from a separate thread should be logged and not show a pop-up
    if ( QThread::currentThread() != qApp->thread() ) {

        LogEntry::LogEntryColor c;
        if (getColor(&c.r, &c.g, &c.b)) {
            c.colorSet = true;
        }
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8( getLabel_mt_safe().c_str() ), QDateTime::currentDateTime(), QString::fromUtf8( content.c_str() ), false, c);
        if (type == eMessageTypeError) {
            appPTR->showErrorLog();
        }
        return true;
    }

    // Some nodes may be hidden from the user but may still report errors (such that the group is in fact hidden to the user)
    if ( !isPartOfProject() && getGroup() ) {
        NodeGroup* isGroup = dynamic_cast<NodeGroup*>( getGroup().get() );
        if (isGroup) {
            isGroup->message(type, content);
        }
    }

    switch (type) {
    case eMessageTypeInfo:
        Dialogs::informationDialog(getLabel_mt_safe(), content);

        return true;
    case eMessageTypeWarning:
        Dialogs::warningDialog(getLabel_mt_safe(), content);

        return true;
    case eMessageTypeError:
        Dialogs::errorDialog(getLabel_mt_safe(), content);

        return true;
    case eMessageTypeQuestion:

        return Dialogs::questionDialog(getLabel_mt_safe(), content, false) == eStandardButtonYes;
    default:

        return false;
    }
}

void
Node::setPersistentMessage(MessageTypeEnum type,
                           const std::string & content)
{
    postPersistentMessage(type, content, false, 0, NULL);
}

void
Node::setChannelSelectorMessage(const std::string& content,
                                U64 renderSequence)
{
    postPersistentMessage(eMessageTypeError, content, true, renderSequence, NULL);
}

void
Node::setChannelSelectorMessageFromRender(const std::string& content,
                                          const AbortableRenderInfoPtr& render)
{
    postPersistentMessage(eMessageTypeError, content, true, render ? render->getRenderSequence() : 0, render.get());
}

bool
Node::storePersistentMessage(MessageTypeEnum type,
                             const std::string& content,
                             bool fromChannelSelector,
                             U64 renderSequence,
                             const AbortableRenderInfo* render)
{
    QMutexLocker k(&_imp->persistentMessageMutex);

    if (fromChannelSelector && (renderSequence != 0) && (renderSequence <= _imp->channelSelectorResolvedSequence)) {
        return false;
    }
    if (fromChannelSelector && render && (render->isAborted() || render->isSuperseded())) {
        return false;
    }
    QString mess = QString::fromUtf8(content.c_str());
    if ((mess == _imp->persistentMessage) && ((int)type == _imp->persistentMessageType)) {
        // The same text posted by the other kind of poster keeps the current owner, so a
        // generic message cannot take a channel error out of reach of its clear, nor hand a
        // generic one to it.
        if (fromChannelSelector && _imp->persistentMessageFromChannelSelector) {
            _imp->persistentMessageRenderSequence = std::max(_imp->persistentMessageRenderSequence, renderSequence);
        }

        return false;
    }
    setChannelSelectorOwnership(fromChannelSelector);
    _imp->persistentMessageRenderSequence = fromChannelSelector ? renderSequence : 0;
    _imp->persistentMessageType = (int)type;
    _imp->persistentMessage = mess;

    return true;
}

void
Node::postPersistentMessage(MessageTypeEnum type,
                            const std::string& content,
                            bool fromChannelSelector,
                            U64 renderSequence,
                            const AbortableRenderInfo* render)
{
    if (!_imp->nodeCreated && _imp->wasCreatedSilently) {
        std::cerr << getScriptName_mt_safe() << " message: " << content << std::endl;
        return;
    }

    if (!appPTR->isBackground()) {
        // Some nodes may be hidden from the user but may still report errors (such that the group is in fact hidden to the user)
        if ( !isPartOfProject() && getGroup() ) {
            NodeGroup* isGroup = dynamic_cast<NodeGroup*>( getGroup().get() );
            if (isGroup) {
                isGroup->setPersistentMessage(type, content);
            }
        }

        if (type == eMessageTypeInfo) {
            message(type, content);

            return;
        }

        if (storePersistentMessage(type, content, fromChannelSelector, renderSequence, render)) {
            Q_EMIT persistentMessageChanged();
        }
    } else {
        std::cout << "Persistent message: " << content << std::endl;

        storePersistentMessage(type, content, fromChannelSelector, renderSequence, render);
    }
}

bool
Node::hasPersistentMessage() const
{
    QMutexLocker k(&_imp->persistentMessageMutex);

    return !_imp->persistentMessage.isEmpty();
}

bool
Node::hasAnyPersistentMessage() const
{
    QMutexLocker k(&_imp->persistentMessageMutex);
    return !_imp->persistentMessage.isEmpty();
}

void
Node::getPersistentMessage(QString* message,
                           int* type,
                           bool prefixLabelAndType) const
{
    QMutexLocker k(&_imp->persistentMessageMutex);

    *type = _imp->persistentMessageType;

    if ( prefixLabelAndType && !_imp->persistentMessage.isEmpty() ) {
        message->append( QString::fromUtf8( getLabel_mt_safe().c_str() ) );
        if (*type == eMessageTypeError) {
            message->append( QString::fromUtf8(" error: ") );
        } else if (*type == eMessageTypeWarning) {
            message->append( QString::fromUtf8(" warning: ") );
        }
    }
    message->append(_imp->persistentMessage);
}

void
Node::clearPersistentMessageRecursive(std::list<Node*>& markedNodes)
{
    if ( std::find(markedNodes.begin(), markedNodes.end(), this) != markedNodes.end() ) {
        return;
    }
    markedNodes.push_back(this);
    clearPersistentMessageInternal();

    int nInputs = getNInputs();
    ///No need to lock, guiInputs is only written to by the main-thread
    for (int i = 0; i < nInputs; ++i) {
        NodePtr input = getInput(i);
        if (input) {
            input->clearPersistentMessageRecursive(markedNodes);
        }
    }
}

void
Node::clearPersistentMessageInternal()
{
    bool changed;
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        setChannelSelectorOwnership(false);
        _imp->persistentMessageRenderSequence = 0;
        changed = !_imp->persistentMessage.isEmpty();
        if (changed) {
            _imp->persistentMessage.clear();
        }
    }

    if (changed) {
        Q_EMIT persistentMessageChanged();
    }
}

void
Node::clearChannelSelectorMessage()
{
    if (!getApp()) {
        return;
    }
    bool changed = false;
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        _imp->channelSelectorResolvedSequence = std::max(_imp->channelSelectorResolvedSequence, AbortableRenderInfo::getLatestRenderSequence());
        if (_imp->persistentMessageFromChannelSelector) {
            setChannelSelectorOwnership(false);
            _imp->persistentMessageRenderSequence = 0;
            changed = !_imp->persistentMessage.isEmpty();
            _imp->persistentMessage.clear();
        }
    }

    if (changed) {
        Q_EMIT persistentMessageChanged();
    }
}

void
Node::clearChannelSelectorMessageFromRender(U64 renderSequence,
                                            bool renderAborted)
{
    if (renderAborted || !getApp()) {
        return;
    }
    bool changed = false;
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        _imp->channelSelectorResolvedSequence = std::max(_imp->channelSelectorResolvedSequence, renderSequence);
        if (_imp->persistentMessageFromChannelSelector && (renderSequence > _imp->persistentMessageRenderSequence)) {
            setChannelSelectorOwnership(false);
            _imp->persistentMessageRenderSequence = 0;
            changed = !_imp->persistentMessage.isEmpty();
            _imp->persistentMessage.clear();
        }
    }

    if (changed) {
        Q_EMIT persistentMessageChanged();
    }
}

void
Node::refreshChannelSelectorMessageAtTime(double time,
                                          const std::list<ViewIdx>& views)
{
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        if (!_imp->persistentMessageFromChannelSelector || _imp->persistentMessage.isEmpty()) {
            return;
        }
    }
    for (std::list<ViewIdx>::const_iterator it = views.begin(); it != views.end(); ++it) {
        std::string message;
        if (!checkSelectedChannelsPresent(time, *it, &message)) {
            return;
        }
    }
    clearChannelSelectorMessage();
}

void
Node::refreshChannelSelectorMessageAtTime(double time,
                                          ViewIdx view)
{
    refreshChannelSelectorMessageAtTime(time, std::list<ViewIdx>(1, view));
}

void
Node::setChannelSelectorOwnership(bool owned)
{
    if (_imp->persistentMessageFromChannelSelector == owned) {
        return;
    }
    _imp->persistentMessageFromChannelSelector = owned;

    QMutexLocker k(&channelSelectorOwnersMutex);
    if (owned) {
        channelSelectorOwners[this] = weak_from_this();
    } else {
        channelSelectorOwners.erase(this);
    }
}

void
Node::getNodesOwningChannelSelectorMessage(NodesList* nodes)
{
    QMutexLocker k(&channelSelectorOwnersMutex);

    for (std::map<const Node*, NodeWPtr>::const_iterator it = channelSelectorOwners.begin(); it != channelSelectorOwners.end(); ++it) {
        NodePtr node = it->second.lock();
        if (node) {
            nodes->push_back(node);
        }
    }
}

void
Node::clearPersistentMessageForPreview()
{
    if (!getApp()) {
        return;
    }
    bool changed = false;
    {
        QMutexLocker k(&_imp->persistentMessageMutex);
        if (!_imp->persistentMessageFromChannelSelector) {
            changed = !_imp->persistentMessage.isEmpty();
            _imp->persistentMessage.clear();
        }
    }

    if (changed) {
        Q_EMIT persistentMessageChanged();
    }
}

void
Node::getNodesWithPersistentMessageUpstream(const NodesList& roots,
                                            NodesList* nodes)
{
    std::set<Node*> visited;
    std::list<NodePtr> queue(roots.begin(), roots.end());

    while (!queue.empty()) {
        NodePtr node = queue.front();
        queue.pop_front();
        if (!node || !visited.insert(node.get()).second) {
            continue;
        }
        if (node->hasAnyPersistentMessage()) {
            nodes->push_back(node);
        }
        int nInputs = node->getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            // The real input keeps a group (which reports errors of hidden nodes) in the walk,
            // the redirected one steps into it or out of a GroupInput.
            queue.push_back(node->getRealInput(i));
            queue.push_back(node->getInput(i));
        }
    }
}

void
Node::clearPersistentMessage(bool recurse)
{
    AppInstancePtr app = getApp();

    if (!app) {
        return;
    }
    if (recurse) {
        std::list<Node*> markedNodes;
        clearPersistentMessageRecursive(markedNodes);
    } else {
        clearPersistentMessageInternal();
    }
}

void
Node::purgeAllInstancesCaches()
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->effect);
    _imp->effect->purgeCaches();
    // Native effects leave purgeCaches() empty, and their ActionsCache is keyed by content hash
    // and never dropped on a hash change, so the only thing that empties it is this.
    _imp->effect->clearActionsCache();
    // The paint tree's nodes are not in any node collection.
    RotoContextPtr roto = getRotoContext();
    if (roto) {
        NodesList tree;
        roto->getRotoPaintTreeNodes(&tree);
        for (NodesList::const_iterator it = tree.begin(); it != tree.end(); ++it) {
            const EffectInstancePtr treeEffect = (*it)->getEffectInstance();
            if (treeEffect) {
                treeEffect->purgeCaches();
                treeEffect->clearActionsCache();
            }
        }
    }
}

bool
Node::notifyInputNIsRendering(int inputNb)
{
    if ( !getApp() || getApp()->isGuiFrozen() ) {
        return false;
    }

    timeval now;


    gettimeofday(&now, 0);

    QMutexLocker l(&_imp->lastRenderStartedMutex);
    double t =  now.tv_sec  - _imp->lastInputNRenderStartedSlotCallTime.tv_sec +
               (now.tv_usec - _imp->lastInputNRenderStartedSlotCallTime.tv_usec) * 1e-6f;

    assert( inputNb >= 0 && inputNb < (int)_imp->inputIsRenderingCounter.size() );

    if ( (t > NATRON_RENDER_GRAPHS_HINTS_REFRESH_RATE_SECONDS) || (_imp->inputIsRenderingCounter[inputNb] == 0) ) {
        _imp->lastInputNRenderStartedSlotCallTime = now;
        ++_imp->inputIsRenderingCounter[inputNb];

        l.unlock();

        Q_EMIT inputNIsRendering(inputNb);

        return true;
    }

    return false;
}

void
Node::notifyInputNIsFinishedRendering(int inputNb)
{
    {
        QMutexLocker l(&_imp->lastRenderStartedMutex);
        assert( inputNb >= 0 && inputNb < (int)_imp->inputIsRenderingCounter.size() );
        --_imp->inputIsRenderingCounter[inputNb];
    }
    Q_EMIT inputNIsFinishedRendering(inputNb);
}

bool
Node::notifyRenderingStarted()
{
    if ( !getApp() || getApp()->isGuiFrozen() ) {
        return false;
    }

    timeval now;

    gettimeofday(&now, 0);


    QMutexLocker l(&_imp->lastRenderStartedMutex);
    double t =  now.tv_sec  - _imp->lastRenderStartedSlotCallTime.tv_sec +
               (now.tv_usec - _imp->lastRenderStartedSlotCallTime.tv_usec) * 1e-6f;

    if ( (t > NATRON_RENDER_GRAPHS_HINTS_REFRESH_RATE_SECONDS) || (_imp->renderStartedCounter == 0) ) {
        _imp->lastRenderStartedSlotCallTime = now;
        ++_imp->renderStartedCounter;


        l.unlock();

        Q_EMIT renderingStarted();

        return true;
    }

    return false;
}

void
Node::notifyRenderingEnded()
{
    {
        QMutexLocker l(&_imp->lastRenderStartedMutex);
        --_imp->renderStartedCounter;
    }
    Q_EMIT renderingEnded();
}

int
Node::getIsInputNRenderingCounter(int inputNb) const
{
    QMutexLocker l(&_imp->lastRenderStartedMutex);

    assert( inputNb >= 0 && inputNb < (int)_imp->inputIsRenderingCounter.size() );

    return _imp->inputIsRenderingCounter[inputNb];
}

int
Node::getIsNodeRenderingCounter() const
{
    QMutexLocker l(&_imp->lastRenderStartedMutex);

    return _imp->renderStartedCounter;
}

void
Node::setOutputFilesForWriter(const std::string & pattern)
{
    assert(_imp->effect);
    _imp->effect->setOutputFilesForWriter(pattern);
}

void
Node::registerPluginMemory(size_t nBytes)
{
    {
        QMutexLocker l(&_imp->memoryUsedMutex);
        _imp->pluginInstanceMemoryUsed += nBytes;
    }
    Q_EMIT pluginMemoryUsageChanged(nBytes);
}

void
Node::unregisterPluginMemory(size_t nBytes)
{
    {
        QMutexLocker l(&_imp->memoryUsedMutex);
        _imp->pluginInstanceMemoryUsed -= nBytes;
    }
    Q_EMIT pluginMemoryUsageChanged(-nBytes);
}

QMutex &
Node::getRenderInstancesSharedMutex()
{
    return _imp->renderInstancesSharedMutex;
}

static void
refreshPreviewsRecursivelyUpstreamInternal(double time,
                                           Node* node,
                                           std::unordered_set<Node*>& marked)
{
    if (!marked.insert(node).second) {
        return;
    }

    if ( node->isPreviewEnabled() ) {
        node->refreshPreviewImage( time );
    }

    std::vector<NodeWPtr> inputs = node->getInputs_copy();

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        NodePtr input = inputs[i].lock();
        if (input) {
            refreshPreviewsRecursivelyUpstreamInternal(time, input.get(), marked);
        }
    }
}

void
Node::refreshPreviewsRecursivelyUpstream(double time)
{
    std::unordered_set<Node*> marked;

    refreshPreviewsRecursivelyUpstreamInternal(time, this, marked);
}

// The set is shared by the whole walk: a node below several paths is visited once, where a set per
// call made the walk exponential in the number of merges below an edit.
static void
refreshPreviewsRecursivelyDownstreamInternal(double time,
                                             Node* node,
                                             std::unordered_set<Node*>& marked)
{
    if (!node->getNodeGui() || !marked.insert(node).second) {
        return;
    }

    if ( node->isPreviewEnabled() ) {
        node->refreshPreviewImage( time );
    }

    NodesWList outputs;
    node->getOutputs_mt_safe(outputs);
    for (NodesWList::iterator it = outputs.begin(); it != outputs.end(); ++it) {
        NodePtr output = it->lock();
        if (output) {
            refreshPreviewsRecursivelyDownstreamInternal(time, output.get(), marked);
        }
    }
}

void
Node::refreshPreviewsRecursivelyDownstream(double time)
{
    std::unordered_set<Node*> marked;
    refreshPreviewsRecursivelyDownstreamInternal(time, this, marked);
}

void
Node::onKnobSlaved(const KnobIPtr& slave,
                   const KnobIPtr& master,
                   int dimension,
                   bool isSlave,
                   bool isExpression)
{
    assert( master->getHolder() );


    ///If the holder isn't an effect, ignore it too
    EffectInstance* isEffect = dynamic_cast<EffectInstance*>( master->getHolder() );
    NodePtr parentNode;
    if (!isEffect) {
        TrackMarker* isMarker = dynamic_cast<TrackMarker*>( master->getHolder() );
        if (isMarker) {
            parentNode = isMarker->getContext()->getNode();
        }
    } else {
        parentNode = isEffect->getNode();
    }

    const KnobLinkKindEnum kind = isExpression ? eKnobLinkKindExpression : eKnobLinkKindSlave;
    bool changed = false;
    {
        QMutexLocker l(&_imp->nodeLinksMutex);
        KnobLinkList::iterator found = _imp->nodeLinks.end();
        for (KnobLinkList::iterator it = _imp->nodeLinks.begin(); it != _imp->nodeLinks.end(); ++it) {
            if ((it->kind == kind) && (it->dimension == dimension) && (it->slave.lock() == slave) && (it->master.lock() == master)) {
                found = it;
                break;
            }
        }

        if (isSlave && (found == _imp->nodeLinks.end())) {
            KnobLink link;
            link.masterNode = parentNode;
            link.slave = slave;
            link.master = master;
            link.dimension = dimension;
            link.kind = kind;
            _imp->nodeLinks.push_back(link);
            changed = true;
        } else if (!isSlave && (found != _imp->nodeLinks.end())) {
            _imp->nodeLinks.erase(found);
            changed = true;
        }
    }
    if (changed) {
        Q_EMIT knobsLinksChanged();
    }
} // onKnobSlaved

void
Node::getKnobsLinks(std::list<Node::KnobLink> & links) const
{
    {
        QMutexLocker l(&_imp->nodeLinksMutex);

        links = _imp->nodeLinks;
    }

    EffectInstancePtr effect = _imp->effect;
    if (!effect) {
        return;
    }
    const KnobsVec& knobs = effect->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        std::vector<KnobLinkedDimension> linked = (*it)->getLinkedKnobs();
        for (std::vector<KnobLinkedDimension>::const_iterator l = linked.begin(); l != linked.end(); ++l) {
            NodePtr node;
            KnobHolder* holder = l->knob->getHolder();
            if (EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder)) {
                node = isEffect->getNode();
            } else if (TrackMarker* isMarker = dynamic_cast<TrackMarker*>(holder)) {
                node = isMarker->getContext()->getNode();
            }
            if (!node || !node->isActivated()) {
                continue;
            }
            KnobLink link;
            link.slave = *it;
            link.master = l->knob;
            link.masterNode = node;
            link.dimension = l->dimension;
            link.kind = eKnobLinkKindLinkGroup;
            links.push_back(link);
        }
    }
}

static bool
isLinkedNodeKnob(const KnobIPtr& knob)
{
    if (!knob->isDeclaredByPlugin() && !knob->isUserKnob()) {
        return false;
    }

    return !dynamic_cast<KnobButton*>(knob.get()) && !dynamic_cast<KnobPage*>(knob.get()) && !dynamic_cast<KnobGroup*>(knob.get()) && !dynamic_cast<KnobSeparator*>(knob.get());
}

bool
Node::canPluginBeLinkedNode(const std::string& pluginID)
{
    return pluginID != PLUGINID_NATRON_VIEWER;
}

bool
Node::canBeLinkedNode() const
{
    return getEffectInstance() && !isEffectViewer() && canPluginBeLinkedNode(getPluginID()) && !isMultiInstance() && !getParentMultiInstance();
}

bool
Node::linkAllKnobsTo(const NodePtr& other)
{
    assert(QThread::currentThread() == qApp->thread());
    if (!other || (other.get() == this) || !canBeLinkedNode() || !other->canBeLinkedNode() || (getPluginID() != other->getPluginID())) {
        return false;
    }

    // Groups this node leaves by joining other's ones must notify their remaining members too.
    NodesList previouslyLinked;
    collectLinkedNodes(false, false, &previouslyLinked);

    bool linked = false;
    const KnobsVec& otherKnobs = other->getEffectInstance()->getKnobs();
    _imp->effect->beginChanges();
    for (KnobsVec::const_iterator it = otherKnobs.begin(); it != otherKnobs.end(); ++it) {
        if (!isLinkedNodeKnob(*it)) {
            continue;
        }
        KnobIPtr thisKnob = _imp->effect->getKnobByName((*it)->getName());
        if (!thisKnob || !thisKnob->linkTo(-1, *it, -1)) {
            continue;
        }
        // linkTo(-1, other, -1) linked the dimensions both knobs have one-to-one. A Paste Link group that
        // either knob was already in is joined, not taken over: only these two members become node links.
        const int nDims = std::min(thisKnob->getDimension(), (*it)->getDimension());
        for (int i = 0; i < nDims; ++i) {
            KnobLinkGroupPtr group = thisKnob->getLinkGroup(i);
            if (group && (group == (*it)->getLinkGroup(i))) {
                group->setMemberNodeLink(thisKnob.get(), i, true);
                group->setMemberNodeLink(it->get(), i, true);
            }
        }
        linked = true;
    }
    _imp->effect->endChanges();

    if (linked) {
        emitLinksChanged(previouslyLinked);
    }

    return linked;
}

bool
Node::unlinkAllKnobs(KnobsVec* unlinkedKnobs)
{
    assert(QThread::currentThread() == qApp->thread());
    if (!_imp->effect) {
        return false;
    }

    NodesList previouslyLinked;
    collectLinkedNodes(false, false, &previouslyLinked);

    bool unlinked = false;
    const KnobsVec& knobs = _imp->effect->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if ((*it)->unlinkFromGroups()) {
            unlinked = true;
            if (unlinkedKnobs) {
                unlinkedKnobs->push_back(*it);
            }
        }
    }

    if (unlinked) {
        emitLinksChanged(previouslyLinked);
    }

    return unlinked;
}

bool
Node::relinkKnobs(const KnobsVec& knobs)
{
    assert(QThread::currentThread() == qApp->thread());
    if (!_imp->effect) {
        return false;
    }

    bool relinked = false;
    _imp->effect->beginChanges();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if ((*it)->getHolder() == _imp->effect.get() && (*it)->relinkToGroups()) {
            relinked = true;
        }
    }
    _imp->effect->endChanges();

    if (relinked) {
        emitLinksChanged(NodesList());
    }

    return relinked;
}

bool
Node::isLinkedNode() const
{
    NodesList nodes;

    collectLinkedNodes(true, true, &nodes);

    return !nodes.empty();
}

void
Node::getLinkedNodes(NodesList* nodes) const
{
    collectLinkedNodes(true, true, nodes);
}

void
Node::getKnobLinkedNodes(NodesList* nodes) const
{
    collectLinkedNodes(false, true, nodes);
}

void
Node::collectLinkedNodes(bool nodeLinksOnly,
                         bool activeOnly,
                         NodesList* nodes) const
{
    EffectInstancePtr effect = _imp->effect;
    if (!effect) {
        return;
    }

    const KnobsVec& knobs = effect->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        std::vector<KnobLinkedDimension> linked = (*it)->getLinkedKnobs();
        for (std::vector<KnobLinkedDimension>::const_iterator l = linked.begin(); l != linked.end(); ++l) {
            if (nodeLinksOnly) {
                KnobLinkGroupPtr group = (*it)->getLinkGroup(l->dimension);
                if (!group || !group->isMemberNodeLink(it->get(), l->dimension) || !group->isMemberNodeLink(l->knob.get(), l->otherDimension)) {
                    continue;
                }
            }
            NodePtr node;
            KnobHolder* holder = l->knob->getHolder();
            if (EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder)) {
                node = isEffect->getNode();
            } else if (TrackMarker* isMarker = dynamic_cast<TrackMarker*>(holder)) {
                node = isMarker->getContext()->getNode();
            }
            if (!node || (node.get() == this) || (activeOnly && !node->isActivated())) {
                continue;
            }
            if (std::find(nodes->begin(), nodes->end(), node) == nodes->end()) {
                nodes->push_back(node);
            }
        }
    }
}

void
Node::emitLinksChanged(const NodesList& alsoNotify)
{
    NodesList nodes = alsoNotify;

    collectLinkedNodes(false, false, &nodes);
    Q_EMIT linksChanged();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (it->get() != this) {
            Q_EMIT(*it)->linksChanged();
        }
    }
}

ImageLayerDesc
Node::findClosestInList(const ImageLayerDesc& comp,
                        const std::list<ImageLayerDesc>& components,
                        bool multiPlanar)
{
    if ( components.empty() ) {
        return ImageLayerDesc::getNoneComponents();
    }

    // A colour layout the clip lacks goes to the narrowest supported layout holding all of its
    // channels, so none is dropped: Alpha on an RGB/RGBA clip is RGBA, not RGB.
    if (comp.isColorLayer() && (comp.getNumComponents() > 0)) {
        const std::bitset<4> storageBits = ImageLayerDesc::colorStorageBits(comp);
        bool exactMatch = false;
        std::list<ImageLayerDesc>::const_iterator covering = components.end();
        for (std::list<ImageLayerDesc>::const_iterator it = components.begin(); it != components.end(); ++it) {
            if (it->getNumComponents() == comp.getNumComponents()) {
                exactMatch = true;
                break;
            }
            if (!it->isColorLayer() || (storageBits & ~ImageLayerDesc::colorStorageBits(*it)).any()) {
                continue;
            }
            if ((covering == components.end()) || (it->getNumComponents() < covering->getNumComponents())) {
                covering = it;
            }
        }
        if (!exactMatch && (covering != components.end())) {
            return *covering;
        }
    }

    std::list<ImageLayerDesc>::const_iterator closestComp = components.end();
    for (std::list<ImageLayerDesc>::const_iterator it = components.begin(); it != components.end(); ++it) {
        if ( closestComp == components.end() ) {
            if ( multiPlanar && ( it->getNumComponents() == comp.getNumComponents() ) ) {
                return comp;
            }
            closestComp = it;
        } else {
            if ( it->getNumComponents() == comp.getNumComponents() ) {
                if (multiPlanar) {
                    return comp;
                }
                closestComp = it;
                break;
            } else {
                int diff = it->getNumComponents() - comp.getNumComponents();
                int diffSoFar = closestComp->getNumComponents() - comp.getNumComponents();
                if ( (diff > 0) && (diff < diffSoFar) ) {
                    closestComp = it;
                }
            }
        }
    }
    if ( closestComp == components.end() ) {
        return ImageLayerDesc::getNoneComponents();
    }

    return *closestComp;
}

ImageLayerDesc
Node::findClosestSupportedComponents(int inputNb,
                                     const ImageLayerDesc& comp) const
{
    // Searched in place: this runs for every input of every node each frame, and the lists are
    // only replaced, under the same lock, when the inputs are initialized.
    const bool multiPlanar = _imp->effect->isMultiPlanar();
    QMutexLocker l(&_imp->inputsMutex);

    if (inputNb >= 0) {
        assert(inputNb < (int)_imp->inputsComponents.size());

        return findClosestInList(comp, _imp->inputsComponents[inputNb], multiPlanar);
    }
    assert(inputNb == -1);

    return findClosestInList(comp, _imp->outputComponents, multiPlanar);
}

int
Node::isMaskChannelKnob(const KnobI* knob) const
{
    for (std::map<int, MaskSelector >::const_iterator it = _imp->maskSelectors.begin(); it != _imp->maskSelectors.end(); ++it) {
        if (it->second.channel.lock().get() == knob) {
            return it->first;
        }
    }

    return -1;
}

bool
Node::isMaskEnabled(int inputNb) const
{
    std::map<int, MaskSelector >::const_iterator it = _imp->maskSelectors.find(inputNb);

    if ( it != _imp->maskSelectors.end() ) {
        return it->second.enabled.lock()->getValue();
    } else {
        return true;
    }
}

bool
Node::checkSelectedChannelsPresent(std::string* message) const
{
    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;

    return checkSelectedChannelsPresent(time, ViewIdx(0), message);
}

bool
Node::checkSelectedChannelsPresent(double time,
                                   ViewIdx view,
                                   std::string* message) const
{
    for (std::map<int, MaskSelector>::const_iterator it = _imp->maskSelectors.begin(); it != _imp->maskSelectors.end(); ++it) {
        int inputNb = it->first;
        if (!getInput(inputNb)) {
            continue;
        }
        if (!isMaskEnabled(inputNb)) {
            continue;
        }
        KnobChannelSelectPtr channel = it->second.channel.lock();
        if (!channel || channel->isNone()) {
            continue;
        }
        std::list<ImageLayerDesc> present;
        listLayersForKnob(channel, time, view, &present);
        if (channel->resolve(present, 0, 0)) {
            continue;
        }
        if (message) {
            *message = "Mask channel " + channel->get() + " is not in the " + getInputLabel(inputNb) + " input";
        }

        return false;
    }

    // A divisor the source no longer carries would otherwise leave the node rendering
    // premultiplied values with no sign that the selection was dropped.
    KnobChannelSelectPtr unPremultBy = _imp->unPremultBySelector.lock();
    if (unPremultBy && !unPremultBy->isNone()) {
        const int inputNb = getPreferredInput();
        std::list<ImageLayerDesc> present;
        listLayersForKnob(unPremultBy, time, view, &present);
        if ((inputNb >= 0) && getInput(inputNb) && !unPremultBy->resolve(present, 0, 0)) {
            if (message) {
                *message = "(Un)premult by channel " + unPremultBy->get() + " is not in the " + getInputLabel(inputNb) + " input";
            }

            return false;
        }
    }

    if (_imp->effect && !_imp->effect->checkExtraChannelsPresent(time, view, message)) {
        return false;
    }

    return true;
} // Node::checkSelectedChannelsPresent

void
Node::lock(const ImagePtr & image)
{
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);
    std::list<ImagePtr>::iterator it =
        std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);

    while ( it != _imp->imagesBeingRendered.end() ) {
        _imp->imagesBeingRenderedCond.wait(l.mutex());
        it = std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);
    }
    ///Okay the image is not used by any other thread, claim that we want to use it
    assert( it == _imp->imagesBeingRendered.end() );
    _imp->imagesBeingRendered.push_back(image);
}

bool
Node::tryLock(const ImagePtr & image)
{
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);
    std::list<ImagePtr>::iterator it =
        std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);

    if ( it != _imp->imagesBeingRendered.end() ) {
        return false;
    }
    ///Okay the image is not used by any other thread, claim that we want to use it
    assert( it == _imp->imagesBeingRendered.end() );
    _imp->imagesBeingRendered.push_back(image);

    return true;
}

void
Node::unlock(const ImagePtr & image)
{
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);
    std::list<ImagePtr>::iterator it =
        std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);

    ///The image must exist, otherwise this is a bug
    assert( it != _imp->imagesBeingRendered.end() );
    _imp->imagesBeingRendered.erase(it);
    ///Notify all waiting threads that we're finished
    _imp->imagesBeingRenderedCond.wakeAll();
}

ImagePtr
Node::getImageBeingRendered(double time,
                            unsigned int mipmapLevel,
                            ViewIdx view)
{
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);

    for (std::list<ImagePtr>::iterator it = _imp->imagesBeingRendered.begin();
         it != _imp->imagesBeingRendered.end(); ++it) {
        const ImageKey &key = (*it)->getKey();
        if ( (key._view == view) && ( (*it)->getMipmapLevel() == mipmapLevel ) && (key._time == time) ) {
            return *it;
        }
    }

    return ImagePtr();
}


void
Node::onParentMultiInstanceInputChanged(int input)
{
    ++_imp->inputModifiedRecursion;
    _imp->effect->onInputChanged(input);
    --_imp->inputModifiedRecursion;
}

void
Node::onFileNameParameterChanged(KnobI* fileKnob)
{
    if (_imp->effect->isReader()) {
        ///union the project frame range if not locked with the reader frame range
        bool isLocked = getApp()->getProject()->isFrameRangeLocked();
        if (!isLocked) {
            double leftBound = std::numeric_limits<int>::min(), rightBound = std::numeric_limits<int>::max();
            _imp->effect->getFrameRange_public(getHashValue(), &leftBound, &rightBound, true);

            if ( (leftBound != std::numeric_limits<int>::min()) && (rightBound != std::numeric_limits<int>::max()) ) {
                if (getGroup()) {
                    getApp()->getProject()->unionFrameRangeWith(leftBound, rightBound);
                }
            }
        }
    } else if (_imp->effect->isWriter()) {
        KnobIPtr sublabelKnob = getKnobByName(kNatronOfxParamStringSublabelName);
        KnobOutputFile* isFile = dynamic_cast<KnobOutputFile*>(fileKnob);
        if (isFile && sublabelKnob) {
            KnobStringBase* isString = dynamic_cast<KnobStringBase*>( sublabelKnob.get() );

            std::string pattern = isFile->getValue();
            if (isString) {
                std::size_t foundSlash = pattern.find_last_of("/");
                if (foundSlash != std::string::npos) {
                    pattern = pattern.substr(foundSlash + 1);
                }

                isString->setValue(pattern);
            }
        }

        /*
           Check if the filename param has a %V in it, in which case make sure to hide the Views parameter
         */
        KnobOutputFile* fileParam = dynamic_cast<KnobOutputFile*>(fileKnob);
        if (fileParam) {
            std::string pattern = fileParam->getValue();
            if ((pattern.find("%v") != std::string::npos) || (pattern.find("%V") != std::string::npos)) {
                //We found view pattern
                KnobIPtr viewsKnob = getKnobByName(kWriteOIIOParamViewsSelector);
                if (viewsKnob) {
                    KnobChoice* viewsSelector = dynamic_cast<KnobChoice*>( viewsKnob.get() );
                    if (viewsSelector) {
                        viewsSelector->setSecret(true);
                    }
                }
            }
        }
    }
} // Node::onFileNameParameterChanged

void
Node::getOriginalFrameRangeForReader(const std::string& /*pluginID*/,
                                     const std::string& canonicalFileName,
                                     int* firstFrame,
                                     int* lastFrame)
{
    SequenceParsing::SequenceFromPattern seq;
    FileSystemModel::filesListFromPattern(canonicalFileName, &seq);
    // SequenceParsing files the matches of a pattern without a frame number under frame -1, and a
    // frame number it parses has no sign, so a pattern that matches a single frame keeps its number.
    if (seq.empty() || (seq.begin()->first < 0)) {
        *firstFrame = 1;
        *lastFrame = 1;
    } else {
        *firstFrame = seq.begin()->first;
        *lastFrame = seq.rbegin()->first;
    }
}

void
Node::computeFrameRangeForReader(KnobI* fileKnob)
{
    /*
       We compute the original frame range of the sequence for the plug-in
       because the plug-in does not have access to the exact original pattern
       hence may not exactly end-up with the same file sequence as what the user
       selected from the file dialog.
     */
    int leftBound = std::numeric_limits<int>::min();
    int rightBound = std::numeric_limits<int>::max();
    ///Set the originalFrameRange parameter of the reader if it has one.
    KnobIPtr knob = getKnobByName(kReaderParamNameOriginalFrameRange);
    if (knob) {
        KnobInt* originalFrameRange = dynamic_cast<KnobInt*>( knob.get() );
        if ( originalFrameRange && (originalFrameRange->getDimension() == 2) ) {
            KnobFile* isFile = dynamic_cast<KnobFile*>(fileKnob);
            assert(isFile);
            if (!isFile) {
                throw std::logic_error("Node::computeFrameRangeForReader");
            }

            std::string pattern = isFile->getValue();
            getApp()->getProject()->canonicalizePath(pattern);
            getOriginalFrameRangeForReader(getPluginID(), pattern, &leftBound, &rightBound);
            originalFrameRange->setValues(leftBound, rightBound, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        }
    }
}


void
Node::setPluginIDAndVersionForGui(const std::list<std::string>& grouping,
                                  const std::string& pluginLabel,
                                  const std::string& pluginID,
                                  const std::string& pluginDesc,
                                  const std::string& pluginIconFilePath,
                                  unsigned int version)
{
    assert( QThread::currentThread() == qApp->thread() );
    NodeGuiIPtr nodeGui = getNodeGui();

    setPyPlugEdited(false);
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);
        _imp->pyPlugInfo.pyPlugVersion = version;
        _imp->pyPlugInfo.pyPlugID = pluginID;
        _imp->pyPlugInfo.pyPlugDesc = pluginDesc;
        _imp->pyPlugInfo.pyPlugLabel = pluginLabel;
        _imp->pyPlugInfo.pyPlugGrouping = grouping;
        _imp->pyPlugInfo.pyPlugIconFilePath = pluginIconFilePath;
    }

    if (!nodeGui) {
        return;
    }

    nodeGui->setPluginIDAndVersion(grouping, pluginLabel, pluginID, pluginDesc, pluginIconFilePath, version);
}

bool
Node::hasPyPlugBeenEdited() const
{
    QMutexLocker k(&_imp->pyPluginInfoMutex);

    return !_imp->pyPlugInfo.isPyPlug || _imp->pyPlugInfo.pluginPythonModule.empty();
}

void
Node::setPyPlugEdited(bool edited)
{
    {
        QMutexLocker k(&_imp->pyPluginInfoMutex);

        _imp->pyPlugInfo.isPyPlug = !edited;
    }
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>(_imp->effect.get());
    if (isGroup) {
        KnobButtonPtr convertToGroupButton = isGroup->getConvertToGroupButton();
        KnobButtonPtr exportPyPlugButton = isGroup->getExportAsPyPlugButton();
        if (convertToGroupButton) {
            convertToGroupButton->setSecret(edited);
        }
        if (exportPyPlugButton) {
            exportPyPlugButton->setSecret(!edited);
        }

    }
}

bool
Node::isPyPlug() const
{
    return _imp->pyPlugInfo.isPyPlug;
}

void
Node::setPluginPythonModule(const std::string& pythonModule)
{
    QMutexLocker k(&_imp->pyPluginInfoMutex);

    _imp->pyPlugInfo.pluginPythonModule = pythonModule;
}

std::string
Node::getPluginPythonModule() const
{
    QMutexLocker k(&_imp->pyPluginInfoMutex);

    return _imp->pyPlugInfo.pluginPythonModule;
}


void
Node::pushUndoCommand(const UndoCommandPtr& command)
{
    NodeGuiIPtr nodeGui = getNodeGui();

    if (nodeGui) {
        nodeGui->pushUndoCommand(command);
    } else {
        command->redo();
    }
}



const std::vector<std::string>&
Node::getCreatedViews() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->createdViews;
}

void
Node::refreshCreatedViews(bool silent)
{
    KnobIPtr knob = getKnobByName(kReadOIIOAvailableViewsKnobName);

    if (knob) {
        refreshCreatedViews( knob.get(), silent );
    }
}

void
Node::refreshCreatedViews(KnobI* knob, bool silent)
{
    assert( QThread::currentThread() == qApp->thread() );

    KnobString* availableViewsKnob = dynamic_cast<KnobString*>(knob);
    if (!availableViewsKnob) {
        return;
    }
    QString value = QString::fromUtf8( availableViewsKnob->getValue().c_str() );
    QStringList views = value.split( QLatin1Char(',') );

    _imp->createdViews.clear();

    const std::vector<std::string>& projectViews = getApp()->getProject()->getProjectViewNames();
    QStringList qProjectViews;
    for (std::size_t i = 0; i < projectViews.size(); ++i) {
        qProjectViews.push_back( QString::fromUtf8( projectViews[i].c_str() ) );
    }

    QStringList missingViews;
    for (QStringList::Iterator it = views.begin(); it != views.end(); ++it) {
        if ( !qProjectViews.contains(*it, Qt::CaseInsensitive) && !it->isEmpty() ) {
            missingViews.push_back(*it);
        }
        _imp->createdViews.push_back( it->toStdString() );
    }

    if ( !missingViews.isEmpty() && !silent ) {
        KnobIPtr fileKnob = getKnobByName(kOfxImageEffectFileParamName);
        KnobFile* inputFileKnob = dynamic_cast<KnobFile*>( fileKnob.get() );
        if (inputFileKnob) {
            std::string filename = inputFileKnob->getValue();
            std::stringstream ss;
            for (int i = 0; i < missingViews.size(); ++i) {
                ss << missingViews[i].toStdString();
                if (i < missingViews.size() - 1) {
                    ss << ", ";
                }
            }
            ss << std::endl;
            ss << std::endl;
            ss << tr("These views are in %1 but do not exist in the project.\nWould you like to create them?").arg( QString::fromUtf8( filename.c_str() ) ).toStdString();
            std::string question  = ss.str();
            StandardButtonEnum rep = Dialogs::questionDialog(tr("Views available").toStdString(), question, false, StandardButtons(eStandardButtonYes | eStandardButtonNo), eStandardButtonYes);
            if (rep == eStandardButtonYes) {
                std::vector<std::string> viewsToCreate;
                for (QStringList::Iterator it = missingViews.begin(); it != missingViews.end(); ++it) {
                    viewsToCreate.push_back( it->toStdString() );
                }
                getApp()->getProject()->createProjectViews(viewsToCreate);
            }
        }
    }

    Q_EMIT availableViewsChanged();
} // Node::refreshCreatedViews

bool
Node::getHideInputsKnobValue() const
{
    KnobBoolPtr k = _imp->hideInputs.lock();

    if (!k) {
        return false;
    }

    return k->getValue();
}

void
Node::setHideInputsKnobValue(bool hidden)
{
    KnobBoolPtr k = _imp->hideInputs.lock();

    if (!k) {
        return;
    }
    k->setValue(hidden);
}

void
Node::onRefreshIdentityStateRequestReceived()
{
    assert( QThread::currentThread() == qApp->thread() );
    if ( (_imp->refreshIdentityStateRequestsCount == 0) || !_imp->effect ) {
        //was already processed
        return;
    }
    _imp->refreshIdentityStateRequestsCount = 0;

    ProjectPtr project = getApp()->getProject();
    double time = project->currentFrame();
    double inputTime = 0;
    U64 hash = getHashValue();
    bool viewAware =  _imp->effect->isViewAware();
    int nViews = !viewAware ? 1 : project->getProjectViewsCount();
    // Do not test identity on the project format! intermediate images may be much bigger!
    // see https://github.com/MrKepzie/Natron/issues/1578#issuecomment-377640892
    //Format frmt;
    //project->getProjectDefaultFormat(&frmt);
    RectI frmt;
    frmt.x1 = frmt.y1 = kOfxFlagInfiniteMin;
    frmt.x2 = frmt.y2 = kOfxFlagInfiniteMax;

    //The one view node might report it is identity, but we do not want it to display it


    bool isIdentity = false;
    int inputNb = -1;
    OneViewNode* isOneView = dynamic_cast<OneViewNode*>( _imp->effect.get() );
    if (!isOneView) {
        for (int i = 0; i < nViews; ++i) {
            int identityInputNb = -1;
            ViewIdx identityView;
            bool isViewIdentity = _imp->effect->isIdentity_public(true, hash, time, RenderScale::identity, frmt, ViewIdx(i), &inputTime, &identityView, &identityInputNb);
            if ( (i > 0) && ( (isViewIdentity != isIdentity) || (identityInputNb != inputNb) || (identityView.value() != i) ) ) {
                isIdentity = false;
                inputNb = -1;
                break;
            }
            isIdentity |= isViewIdentity;
            inputNb = identityInputNb;
            if (!isIdentity) {
                break;
            }
        }
    }

    //Check for consistency across views or then say the effect is not identity since the UI cannot display 2 different states
    //depending on the view


    NodeGuiIPtr nodeUi = _imp->guiPointer.lock();
    assert(nodeUi);
    nodeUi->onIdentityStateChanged(isIdentity ? inputNb : -1);
} // Node::onRefreshIdentityStateRequestReceived

void
Node::refreshIdentityState()
{
    assert( QThread::currentThread() == qApp->thread() );

    if ( !_imp->guiPointer.lock() ) {
        return;
    }

    //Post a new request
    ++_imp->refreshIdentityStateRequestsCount;
    Q_EMIT refreshIdentityStateRequested();
}

/*
   This is called AFTER the instanceChanged action has been called on the plug-in
 */
bool
Node::onEffectKnobValueChanged(KnobI* what,
                               ValueChangedReasonEnum reason)
{
    if (!what) {
        return false;
    }
    for (std::map<int, MaskSelector >::iterator it = _imp->maskSelectors.begin(); it != _imp->maskSelectors.end(); ++it) {
        if (it->second.channel.lock().get() == what) {
            _imp->onMaskSelectorChanged(it->first, it->second);
            break;
        }
    }

    // Every declared layer knob is a reference counted in the project's "Used by" column,
    // whatever its role; a target-role knob changes no input, so nothing else refreshes it.
    if (_imp->layerKnobSources.find(what) != _imp->layerKnobSources.end()) {
        _imp->notifyLayerReferencesChanged();
    }

    bool ret = true;
    if ( what == _imp->previewEnabledKnob.lock().get() ) {
        if ( (reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonNatronInternalEdited) ) {
            Q_EMIT previewKnobToggled();
        }
    } else if ( what == _imp->renderButton.lock().get() ) {
        if (getEffectInstance()->isWriter() && !_imp->renderButton.lock()->isTriggeringCallbacksOnly()) {
            /*if this is a button and it is a render button,we're safe to assume the plug-ins wants to start rendering.*/
            AppInstance::RenderWork w;
            w.writer = dynamic_cast<OutputEffectInstance*>( _imp->effect.get() );
            w.firstFrame = std::numeric_limits<int>::min();
            w.lastFrame = std::numeric_limits<int>::max();
            w.frameStep = std::numeric_limits<int>::min();
            w.useRenderStats = getApp()->isRenderStatsActionChecked();
            std::list<AppInstance::RenderWork> works;
            works.push_back(w);
            getApp()->renderForeground(works);
        }
    } else if ( ( what == _imp->disableNodeKnob.lock().get() ) && !_imp->isMultiInstance && !_imp->multiInstanceParent.lock() ) {
        Q_EMIT disabledKnobToggled( _imp->disableNodeKnob.lock()->getValue() );
        if ( QThread::currentThread() == qApp->thread() ) {
            getApp()->redrawAllViewers();
        }
    } else if ( what == _imp->nodeLabelKnob.lock().get() ) {
        Q_EMIT nodeExtraLabelChanged( QString::fromUtf8( _imp->nodeLabelKnob.lock()->getValue().c_str() ) );
    } else if (what->getName() == kNatronOfxParamStringSublabelName) {
        //special hack for the merge node and others so we can retrieve the sublabel and display it in the node's label
        KnobString* strKnob = dynamic_cast<KnobString*>(what);
        if (strKnob) {
            QString operation = QString::fromUtf8( strKnob->getValue().c_str() );
            if ( !operation.isEmpty() ) {
                operation.prepend( QString::fromUtf8("(") );
                operation.append( QString::fromUtf8(")") );
            }
            replaceCustomDataInlabel(operation);
        }
    } else if ( what == _imp->hideInputs.lock().get() ) {
        Q_EMIT hideInputsKnobChanged( _imp->hideInputs.lock()->getValue() );
    } else if ( _imp->effect->isReader() && (what->getName() == kReadOIIOAvailableViewsKnobName) ) {
        refreshCreatedViews(what, false /*silent*/);
    } else if ( what == _imp->refreshInfoButton.lock().get() ||
               (what == _imp->infoPage.lock().get() && reason == eValueChangedReasonUserEdited) ) {
        std::stringstream ssinfo;
        int maxinputs = getNInputs();
        for (int i = 0; i < maxinputs; ++i) {
            std::string inputInfo = makeInfoForInput(i);
            if ( !inputInfo.empty() ) {
                ssinfo << inputInfo << "<br />";
            }
        }
        std::string outputInfo = makeInfoForInput(-1);
        ssinfo << outputInfo << "<br />";
        std::string cacheInfo = makeCacheInfo();
        ssinfo << cacheInfo << "<br />";
        ssinfo << "<b>" << tr("Supports tiles:").toStdString() << "</b> <font color=#c8c8c8>";
        ssinfo << ( getCurrentSupportTiles() ? tr("Yes") : tr("No") ).toStdString() << "</font><br />";
        if (_imp->effect) {
            ssinfo << "<b>" << tr("Supports multiresolution:").toStdString() << "</b> <font color=#c8c8c8>";
            ssinfo << ( _imp->effect->supportsMultiResolution() ? tr("Yes") : tr("No") ).toStdString() << "</font><br />";
            ssinfo << "<b>" << tr("Supports renderscale:").toStdString() << "</b> <font color=#c8c8c8>";
            switch ( _imp->effect->supportsRenderScaleMaybe() ) {
                case EffectInstance::eSupportsMaybe:
                    ssinfo << tr("Maybe").toStdString();
                    break;

                case EffectInstance::eSupportsNo:
                    ssinfo << tr("No").toStdString();
                    break;

                case EffectInstance::eSupportsYes:
                    ssinfo << tr("Yes").toStdString();
                    break;
            }
            ssinfo << "</font><br />";
            ssinfo << "<b>" << tr("Supports multiple clip PARs:").toStdString() << "</b> <font color=#c8c8c8>";
            ssinfo << ( _imp->effect->supportsMultipleClipPARs() ? tr("Yes") : tr("No") ).toStdString() << "</font><br />";
            ssinfo << "<b>" << tr("Supports multiple clip depths:").toStdString() << "</b> <font color=#c8c8c8>";
            ssinfo << ( _imp->effect->supportsMultipleClipDepths() ? tr("Yes") : tr("No") ).toStdString() << "</font><br />";
        }
        ssinfo << "<b>" << tr("Render thread safety:").toStdString() << "</b> <font color=#c8c8c8>";
        switch ( getCurrentRenderThreadSafety() ) {
            case eRenderSafetyUnsafe:
                ssinfo << tr("Unsafe").toStdString();
                break;

            case eRenderSafetyInstanceSafe:
                ssinfo << tr("Safe").toStdString();
                break;

            case eRenderSafetyFullySafe:
                ssinfo << tr("Fully safe").toStdString();
                break;

            case eRenderSafetyFullySafeFrame:
                ssinfo << tr("Fully safe frame").toStdString();
                break;
        }
        ssinfo << "</font><br />";
        ssinfo << "<b>" << tr("OpenGL Rendering Support:").toStdString() << "</b>: <font color=#c8c8c8>";
        PluginOpenGLRenderSupport glSupport = getCurrentOpenGLRenderSupport();
        switch (glSupport) {
            case ePluginOpenGLRenderSupportNone:
                ssinfo << tr("No").toStdString();
                break;
            case ePluginOpenGLRenderSupportNeeded:
                ssinfo << tr("Yes but CPU rendering is not supported").toStdString();
                break;
            case ePluginOpenGLRenderSupportYes:
                ssinfo << tr("Yes").toStdString();
                break;
            default:
                break;
        }
        ssinfo << "</font>";
        _imp->nodeInfos.lock()->setValue( ssinfo.str() );
    } else if ( what == _imp->openglRenderingEnabledKnob.lock().get() ) {
        // Do nothing. Knob value will be checked in getCurrentOpenGLRenderSupport() calls.
    } else {
        ret = false;
    }

    if (!ret) {
        KnobGroup* isGroup = dynamic_cast<KnobGroup*>(what);
        if ( isGroup && isGroup->getIsDialog() ) {
            NodeGuiIPtr gui = getNodeGui();
            if (gui) {
                gui->showGroupKnobAsDialog(isGroup);
                ret = true;
            }
        }
    }

    if (!ret) {
        OfxEffectInstance* isOfxEffect = dynamic_cast<OfxEffectInstance*>(_imp->effect.get());
        if (isOfxEffect && isOfxEffect->onMultiplaneTwinChanged(what)) {
            _imp->notifyLayerReferencesChanged();
            ret = true;
        }
    }

    if (!ret && (what == _imp->layerKnob.lock().get())) {
        if (_imp->rotoContext) {
            _imp->rotoContext->retargetRotoPaintTree();
        }
        s_layerSelectionChanged();
        ret = true;
    }

    if (!ret) {
        GroupInput* isInput = dynamic_cast<GroupInput*>(_imp->effect.get());
        if (isInput) {
            if ( (what->getName() == kNatronGroupInputIsOptionalParamName)
                 || ( what->getName() == kNatronGroupInputIsMaskParamName) ) {
                NodeCollectionPtr col = isInput->getNode()->getGroup();
                assert(col);
                // An Input node can be created at the top level of the project, where there is no
                // group whose inputs it drives.
                NodeGroup* isGrp = dynamic_cast<NodeGroup*>(col.get());
                if (isGrp) {
                    ///Refresh input arrows of the node to reflect the state
                    isGrp->getNode()->initializeInputs();
                    ret = true;
                }
            }
        }
    }

    return ret;
} // Node::onEffectKnobValueChanged

void
Node::onOpenGLEnabledKnobChangedOnProject(bool activated)
{
    KnobChoicePtr k = _imp->openglRenderingEnabledKnob.lock();
    if (k) {
        // Make the enable state for the node knob match the
        // state of the project level GPU rendering state.
        // This makes it so you can't change node level GPU
        // rendering state if the project has GPU rendering disabled.
        k->setAllDimensionsEnabled(activated);
    }
}

void
Node::getReferencedLayerIDs(std::set<std::string>* ids) const
{
    // Walks every declared layer/channel knob (the host's own layerKnob, its mask and
    // unpremult-by channel selectors, and any plugin- or node-owned knob added through
    // declareLayerKnob()), not just the host's single layerKnob: each is an independent
    // reference to the project layer registry unless a container drives its value.
    for (std::map<const KnobI*, LayerKnobSource>::const_iterator it = _imp->layerKnobSources.begin(); it != _imp->layerKnobSources.end(); ++it) {
        if (it->second.drivenByContainer) {
            // A container (e.g. RotoPaint) drives this knob's value; it is not a separate reference.
            continue;
        }
        KnobIPtr knob = it->second.knob.lock();
        if (!knob) {
            continue;
        }
        if (KnobChannelSet* isChannelSet = dynamic_cast<KnobChannelSet*>(knob.get())) {
            isChannelSet->getReferencedLayerIDs(ids);
        } else if (KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>(knob.get())) {
            isLayerSelect->getReferencedLayerIDs(ids);
        } else if (KnobChannelSelect* isChannelSelect = dynamic_cast<KnobChannelSelect*>(knob.get())) {
            isChannelSelect->getReferencedLayerIDs(ids);
        }
    }
} // Node::getReferencedLayerIDs

KnobIPtr
Node::getLayerKnob() const
{
    return _imp->layerKnob.lock();
}

// An alias slave reads its value from the master but its image stream is its own node's
// input, so layers are always resolved on the slave's node. A master (typically a group's
// user param) is not declared on its own node and has no stream: it answers through the
// inner node that holds its alias slave.
static KnobIPtr
findAliasSlaveOnOtherNode(const KnobIPtr& master,
                          const Node* self,
                          NodePtr* slaveNode)
{
    KnobsVec aliases = master->getAliasesOfThis();
    for (KnobsVec::const_iterator it = aliases.begin(); it != aliases.end(); ++it) {
        const KnobIPtr& listener = *it;
        EffectInstance* effect = dynamic_cast<EffectInstance*>(listener->getHolder());
        NodePtr node = effect ? effect->getNode() : NodePtr();
        if (node && (node.get() != self)) {
            *slaveNode = node;

            return listener;
        }
    }

    return KnobIPtr();
}

// The layers an image stream on inputNb carries. Every image stream carries a Color plane, so an
// unconnected input still lists it.
static std::shared_ptr<const std::list<ImageLayerDesc>>
getInputStreamLayers(const EffectInstancePtr& effect,
                     int inputNb,
                     double time,
                     ViewIdx view)
{
    std::shared_ptr<const std::list<ImageLayerDesc>> present;
    if (effect && (inputNb >= 0)) {
        present = effect->getPresentLayersShared(time, view, inputNb);
    }
    if (present) {
        for (std::list<ImageLayerDesc>::const_iterator it = present->begin(); it != present->end(); ++it) {
            if (it->isColorLayer()) {
                return present;
            }
        }
    }
    std::shared_ptr<std::list<ImageLayerDesc>> layers = std::make_shared<std::list<ImageLayerDesc>>(1, ImageLayerDesc::getRGBAComponents());
    if (present) {
        layers->insert(layers->end(), present->begin(), present->end());
    }

    return layers;
}

static void
appendInputStreamLayers(const EffectInstancePtr& effect,
                        int inputNb,
                        double time,
                        ViewIdx view,
                        std::list<ImageLayerDesc>* layers)
{
    std::shared_ptr<const std::list<ImageLayerDesc>> present = getInputStreamLayers(effect, inputNb, time, view);
    layers->insert(layers->end(), present->begin(), present->end());
}

void
Node::listLayersForKnob(const KnobIPtr& knob,
                        std::list<ImageLayerDesc>* layers) const
{
    AppInstancePtr app = getApp();
    double time = app ? app->getTimeLine()->currentFrame() : 0.;

    listLayersForKnob(knob, time, ViewIdx(0), layers);
}

void
Node::listLayersForKnob(const KnobIPtr& knob,
                        double time,
                        ViewIdx view,
                        std::list<ImageLayerDesc>* layers) const
{
    if (!knob) {
        return;
    }

    std::map<const KnobI*, LayerKnobSource>::const_iterator found = _imp->layerKnobSources.find(knob.get());
    if (found == _imp->layerKnobSources.end()) {
        NodePtr slaveNode;
        KnobIPtr slave = findAliasSlaveOnOtherNode(knob, this, &slaveNode);
        if (slave) {
            slaveNode->listLayersForKnob(slave, time, view, layers);
        }

        return;
    }

    if (found->second.role == LayerKnobSpec::eRoleTarget) {
        AppInstancePtr app = getApp();
        ProjectPtr project = app ? app->getProject() : ProjectPtr();
        if (!project) {
            return;
        }
        std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();
        std::list<ImageLayerDesc> storagePlanes;
        LayerRegistry::toStoragePlanes(snapshot, &storagePlanes);
        layers->insert(layers->end(), storagePlanes.begin(), storagePlanes.end());

        return;
    }

    int inputNb = found->second.inputNb;
    double inputTime = time;
    ViewIdx inputView = view;
    if (inputNb == LayerKnobSource::kPreferredInput) {
        if (_imp->effect) {
            _imp->effect->getLayersPassThroughInput(time, view, &inputNb, &inputTime, &inputView);
        } else {
            inputNb = -1;
        }
    }
    appendInputStreamLayers(_imp->effect, inputNb, inputTime, inputView, layers);
} // Node::listLayersForKnob

void
Node::listLayerViewsForKnob(const KnobIPtr& knob,
                            std::list<ImageLayerDesc>* layers) const
{
    std::list<ImageLayerDesc> storage;
    listLayersForKnob(knob, &storage);
    ImageLayerDesc::expandColorViews(&storage);
    layers->insert(layers->end(), storage.begin(), storage.end());
}

void
Node::listLayerViewsForKnob(const KnobIPtr& knob,
                            double time,
                            ViewIdx view,
                            std::list<ImageLayerDesc>* layers) const
{
    std::list<ImageLayerDesc> storage;
    listLayersForKnob(knob, time, view, &storage);
    ImageLayerDesc::expandColorViews(&storage);
    layers->insert(layers->end(), storage.begin(), storage.end());
}

void
Node::listChannelViewsForKnob(const KnobIPtr& knob,
                              std::list<ImageLayerDesc>* layers) const
{
    std::list<ImageLayerDesc> storage;
    listLayersForKnob(knob, &storage);
    ImageLayerDesc::collapseColorToLayoutView(&storage);
    layers->insert(layers->end(), storage.begin(), storage.end());
}

bool
Node::isTargetLayerKnob(const KnobIPtr& knob) const
{
    if (!knob) {
        return false;
    }

    std::map<const KnobI*, LayerKnobSource>::const_iterator found = _imp->layerKnobSources.find(knob.get());
    if (found == _imp->layerKnobSources.end()) {
        NodePtr slaveNode;
        KnobIPtr slave = findAliasSlaveOnOtherNode(knob, this, &slaveNode);

        return slave && slaveNode->isTargetLayerKnob(slave);
    }

    return found->second.role == LayerKnobSpec::eRoleTarget;
}

static void
resolveAgainstPresentLayers(const KnobChannelSet* channelSet,
                            const KnobLayerSelect* layerSelect,
                            const std::list<ImageLayerDesc>& present,
                            std::vector<ResolvedLayer>* selected)
{
    if (channelSet) {
        *selected = channelSet->resolve(present);
    } else if (layerSelect) {
        ResolvedLayer one;
        if (layerSelect->resolve(present, &one)) {
            selected->push_back(one);
        }
    }
}

bool
Node::resolveLayerKnob(double time,
                       ViewIdx view,
                       std::vector<ResolvedLayer>* selected) const
{
    selected->clear();

    KnobIPtr layerKnob = getLayerKnob();
    KnobChannelSet* channelSet = dynamic_cast<KnobChannelSet*>(layerKnob.get());
    KnobLayerSelect* layerSelect = dynamic_cast<KnobLayerSelect*>(layerKnob.get());
    if (!channelSet && !layerSelect) {
        return false;
    }

    std::map<const KnobI*, LayerKnobSource>::const_iterator found = _imp->layerKnobSources.find(layerKnob.get());
    if ((found != _imp->layerKnobSources.end()) && (found->second.role != LayerKnobSpec::eRoleTarget)) {
        int inputNb = found->second.inputNb;
        double inputTime = time;
        ViewIdx inputView = view;
        if (inputNb == LayerKnobSource::kPreferredInput) {
            if (_imp->effect) {
                _imp->effect->getLayersPassThroughInput(time, view, &inputNb, &inputTime, &inputView);
            } else {
                inputNb = -1;
            }
        }
        resolveAgainstPresentLayers(channelSet, layerSelect, *getInputStreamLayers(_imp->effect, inputNb, inputTime, inputView), selected);

        return true;
    }

    std::list<ImageLayerDesc> present;
    listLayersForKnob(layerKnob, time, view, &present);
    resolveAgainstPresentLayers(channelSet, layerSelect, present, selected);

    return true;
}

bool
Node::resolveLayerKnob(double time,
                       ViewIdx view,
                       int passThroughInputNb,
                       double passThroughTime,
                       ViewIdx passThroughView,
                       std::vector<ResolvedLayer>* selected) const
{
    selected->clear();

    KnobIPtr layerKnob = getLayerKnob();
    KnobChannelSet* channelSet = dynamic_cast<KnobChannelSet*>(layerKnob.get());
    KnobLayerSelect* layerSelect = dynamic_cast<KnobLayerSelect*>(layerKnob.get());
    if (!channelSet && !layerSelect) {
        return false;
    }

    std::map<const KnobI*, LayerKnobSource>::const_iterator found = _imp->layerKnobSources.find(layerKnob.get());
    if ((found != _imp->layerKnobSources.end()) && (found->second.role != LayerKnobSpec::eRoleTarget) && (found->second.inputNb == LayerKnobSource::kPreferredInput)) {
        resolveAgainstPresentLayers(channelSet, layerSelect, *getInputStreamLayers(_imp->effect, passThroughInputNb, passThroughTime, passThroughView), selected);
    } else {
        std::list<ImageLayerDesc> present;
        listLayersForKnob(layerKnob, time, view, &present);
        resolveAgainstPresentLayers(channelSet, layerSelect, present, selected);
    }

    return true;
}

void
Node::retargetLayerKnob(const std::string& layerID)
{
    KnobIPtr layerKnob = _imp->layerKnob.lock();

    if (!layerKnob) {
        return;
    }
    _imp->layerKnobSpec.role = LayerKnobSpec::eRoleTarget;
    LayerKnobSource source(layerKnob, -1, LayerKnobSpec::eRoleTarget);
    source.drivenByContainer = true;
    _imp->layerKnobSources[layerKnob.get()] = source;

    if (KnobChannelSet* channelSet = dynamic_cast<KnobChannelSet*>(layerKnob.get())) {
        std::vector<ChannelSetRow> rows(1);
        rows[0].mode = ChannelSetRow::eModeLayer;
        rows[0].layerOrPattern = layerID;
        if (channelSet->getRows() != rows) {
            channelSet->setRows(rows);
        }
    } else if (KnobLayerSelect* layerSelect = dynamic_cast<KnobLayerSelect*>(layerKnob.get())) {
        if (layerSelect->getLayer() != layerID || !layerSelect->getChannels().empty()) {
            layerSelect->setLayer(layerID);
        }
    }
}

void
Node::declareLayerKnob(const KnobIPtr& knob,
                       int inputNb,
                       LayerKnobSpec::RoleEnum role)
{
    if (!knob) {
        return;
    }
    _imp->layerKnobSources[knob.get()] = LayerKnobSource(knob, inputNb, role);
}

void
Node::setLayerKnobInput(const KnobIPtr& knob,
                        int inputNb)
{
    if (!knob) {
        return;
    }
    std::map<const KnobI*, LayerKnobSource>::iterator found = _imp->layerKnobSources.find(knob.get());
    if (found == _imp->layerKnobSources.end()) {
        return;
    }
    found->second.inputNb = inputNb;
    Q_EMIT layerListRefreshed();
}

void
Node::setLayerKnobDrivenByContainer(const KnobIPtr& knob)
{
    if (!knob) {
        return;
    }
    std::map<const KnobI*, LayerKnobSource>::iterator found = _imp->layerKnobSources.find(knob.get());
    if (found == _imp->layerKnobSources.end()) {
        return;
    }
    found->second.drivenByContainer = true;
}

void
Node::registerProducedLayers()
{
    assert(QThread::currentThread() == qApp->thread());
    if (!_imp->effect || !isActivated()) {
        return;
    }
    {
        QMutexLocker k(&_imp->isBeingDestroyedMutex);
        if (_imp->isBeingDestroyed) {
            return;
        }
    }

    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project) {
        return;
    }

    double time = app->getTimeLine()->currentFrame();
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime;
    int passThroughView;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInputNb;

    _imp->effect->getComponentsNeededAndProduced_public(getHashValue(), time, ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

    EffectInstance::ComponentsNeededMap::const_iterator foundOutput = comps.find(-1);
    if (foundOutput == comps.end()) {
        return;
    }

    LayerRegistryEntry::OriginEnum origin = _imp->effect->isReader() ? LayerRegistryEntry::eOriginFile : LayerRegistryEntry::eOriginPlugin;
    for (std::list<ImageLayerDesc>::const_iterator it = foundOutput->second.begin(); it != foundOutput->second.end(); ++it) {
        if (it->isColorLayer()) {
            continue;
        }
        std::string error;
        project->addLayer(*it, origin, &error);
    }
} // Node::registerProducedLayers

void
Node::Implementation::onMaskSelectorChanged(int inputNb,
                                            const MaskSelector& selector)
{
    KnobChannelSelectPtr channel = selector.channel.lock();
    KnobBoolPtr enabled = selector.enabled.lock();

    if (channel->isNone() && enabled->isEnabled(0)) {
        enabled->setValue(false);
        enabled->setEnabled(0, false);
    } else if (!enabled->isEnabled(0)) {
        enabled->setEnabled(0, true);
        if ( _publicInterface->getInput(inputNb) ) {
            enabled->setValue(true);
        }
    }

    if (!isRefreshingInputRelatedData) {
        ///Clip preferences have changed
        effect->refreshMetadataSideEffects();
    }
}

void
Node::Implementation::notifyLayerReferencesChanged()
{
    if (QThread::currentThread() != qApp->thread()) {
        return;
    }
    AppInstancePtr app = _publicInterface->getApp();
    if (!app) {
        return;
    }
    ProjectPtr project = app->getProject();
    if (!project || project->isLoadingProject() || project->isProjectClosing()) {
        return;
    }
    project->refreshLayersKnob();
}

bool
Node::pluginOwnsChannelMask() const
{
    return _imp->pluginOwnsChannelMask;
}

bool
Node::hasAtLeastOneChannelToProcess(double time,
                                    ViewIdx view) const
{
    std::vector<ResolvedLayer> selected;
    if (!resolveLayerKnob(time, view, &selected)) {
        return true;
    }
    for (std::vector<ResolvedLayer>::const_iterator it = selected.begin(); it != selected.end(); ++it) {
        if (it->channels.any()) {
            return true;
        }
    }

    return false;
}

void
Node::replaceCustomDataInlabel(const QString & data)
{
    assert( QThread::currentThread() == qApp->thread() );
    KnobStringPtr labelKnob = _imp->nodeLabelKnob.lock();
    if (!labelKnob) {
        return;
    }
    QString label = QString::fromUtf8( labelKnob->getValue().c_str() );
    ///Since the label is html encoded, find the text's start
    int foundFontTag = label.indexOf( QString::fromUtf8("<font") );
    bool htmlPresent =  (foundFontTag != -1);
    ///we're sure this end tag is the one of the font tag
    QString endFont( QString::fromUtf8("\">") );
    int endFontTag = label.indexOf(endFont, foundFontTag);
    QString customTagStart( QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_START) );
    QString customTagEnd( QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_END) );
    int foundNatronCustomDataTag = label.indexOf(customTagStart, endFontTag == -1 ? 0 : endFontTag);
    if (foundNatronCustomDataTag != -1) {
        ///remove the current custom data
        int foundNatronEndTag = label.indexOf(customTagEnd, foundNatronCustomDataTag);
        assert(foundNatronEndTag != -1);

        foundNatronEndTag += customTagEnd.size();
        label.remove(foundNatronCustomDataTag, foundNatronEndTag - foundNatronCustomDataTag);
    }

    int i = htmlPresent ? endFontTag + endFont.size() : 0;
    label.insert(i, customTagStart);
    label.insert(i + customTagStart.size(), data);
    label.insert(i + customTagStart.size() + data.size(), customTagEnd);
    labelKnob->setValue( label.toStdString() );
}

KnobBoolPtr
Node::getDisabledKnob() const
{
    return _imp->disableNodeKnob.lock();
}

bool
Node::isLifetimeActivated(int *firstFrame,
                          int *lastFrame) const
{
    KnobBoolPtr enableLifetimeKnob = _imp->enableLifeTimeKnob.lock();

    if (!enableLifetimeKnob) {
        return false;
    }
    if ( !enableLifetimeKnob->getValue() ) {
        return false;
    }
    KnobIntPtr lifetimeKnob = _imp->lifeTimeKnob.lock();
    *firstFrame = lifetimeKnob->getValue(0);
    *lastFrame = lifetimeKnob->getValue(1);

    return true;
}

bool
Node::isNodeDisabled() const
{
    KnobBoolPtr b = _imp->disableNodeKnob.lock();
    bool thisDisabled = b ? b->getValue() : false;
    NodeGroup* isContainerGrp = dynamic_cast<NodeGroup*>( getGroup().get() );

    if (isContainerGrp) {
        return thisDisabled || isContainerGrp->getNode()->isNodeDisabled();
    }
    int lifeTimeFirst, lifeTimeEnd;
    bool lifeTimeEnabled = isLifetimeActivated(&lifeTimeFirst, &lifeTimeEnd);
    double curFrame = _imp->effect->getCurrentTime();
    bool enabled = ( !lifeTimeEnabled || (curFrame >= lifeTimeFirst && curFrame <= lifeTimeEnd) ) && !thisDisabled;

    return !enabled;
}

bool
Node::isNodeDisabled(double time) const
{
    KnobBoolPtr b = _imp->disableNodeKnob.lock();
    bool thisDisabled = b ? b->getValueAtTime(time) : false;
    NodeGroup* isContainerGrp = dynamic_cast<NodeGroup*>(getGroup().get());

    if (isContainerGrp) {
        return thisDisabled || isContainerGrp->getNode()->isNodeDisabled(time);
    }
    int lifeTimeFirst, lifeTimeEnd;
    bool lifeTimeEnabled = isLifetimeActivated(&lifeTimeFirst, &lifeTimeEnd);
    bool enabled = (!lifeTimeEnabled || (time >= lifeTimeFirst && time <= lifeTimeEnd)) && !thisDisabled;

    return !enabled;
}

bool
Node::isNodeDisabledAtAllTimes() const
{
    KnobBoolPtr b = _imp->disableNodeKnob.lock();
    bool thisDisabled = b && !b->hasAnimation() && b->getValue();
    NodeGroup* isContainerGrp = dynamic_cast<NodeGroup*>(getGroup().get());

    if (isContainerGrp) {
        return thisDisabled || isContainerGrp->getNode()->isNodeDisabledAtAllTimes();
    }
    return thisDisabled;
}

void
Node::setNodeDisabled(bool disabled)
{
    KnobBoolPtr b = _imp->disableNodeKnob.lock();

    if (b) {
        b->setValue(disabled);

        // Clear the actions cache because if this function is called from another thread, the hash will not be incremented
        _imp->effect->clearActionsCache();
    }
}

void
Node::showKeyframesOnTimeline(bool emitSignal)
{
    assert( QThread::currentThread() == qApp->thread() );
    if ( _imp->keyframesDisplayedOnTimeline || appPTR->isBackground() ) {
        return;
    }
    _imp->keyframesDisplayedOnTimeline = true;
    std::list<SequenceTime> keys;
    getAllKnobsKeyframes(&keys);
    getApp()->addMultipleKeyframeIndicatorsAdded(keys, emitSignal);
}

void
Node::hideKeyframesFromTimeline(bool emitSignal)
{
    assert( QThread::currentThread() == qApp->thread() );
    if ( !_imp->keyframesDisplayedOnTimeline || appPTR->isBackground() ) {
        return;
    }
    _imp->keyframesDisplayedOnTimeline = false;
    std::list<SequenceTime> keys;
    getAllKnobsKeyframes(&keys);
    getApp()->removeMultipleKeyframeIndicator(keys, emitSignal);
}

bool
Node::areKeyframesVisibleOnTimeline() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->keyframesDisplayedOnTimeline;
}

bool
Node::hasAnimatedKnob() const
{
    // Metadata derivation asks this from render threads, so the knob list is copied under its lock.
    const KnobsVec knobs = _imp->effect->getKnobs_mt_safe();
    bool hasAnimation = false;

    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->canAnimate() ) {
            for (int j = 0; j < knobs[i]->getDimension(); ++j) {
                if ( knobs[i]->isAnimated(j) ) {
                    hasAnimation = true;
                    break;
                }
            }
        }
        if (hasAnimation) {
            break;
        }
    }

    return hasAnimation;
}

void
Node::getAllKnobsKeyframes(std::list<SequenceTime>* keyframes)
{
    assert(keyframes);
    const KnobsVec & knobs = getKnobs();

    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->getIsSecret() || !knobs[i]->getIsPersistent() ) {
            continue;
        }
        if ( !knobs[i]->canAnimate() ) {
            continue;
        }
        int dim = knobs[i]->getDimension();
        KnobFile* isFile = dynamic_cast<KnobFile*>( knobs[i].get() );
        if (isFile) {
            ///skip file knobs
            continue;
        }
        for (int j = 0; j < dim; ++j) {
            if ( knobs[i]->canAnimate() && knobs[i]->isAnimated( j, ViewIdx(0) ) ) {
                KeyFrameSet kfs = knobs[i]->getCurve(ViewIdx(0), j)->getKeyFrames_mt_safe();
                for (KeyFrameSet::iterator it = kfs.begin(); it != kfs.end(); ++it) {
                    keyframes->push_back( it->getTime() );
                }
            }
        }
    }
}

std::string
Node::getNodeExtraLabel() const
{
    KnobStringPtr s = _imp->nodeLabelKnob.lock();

    if (s) {
        return s->getValue();
    } else {
        return std::string();
    }
}


bool
Node::isTrackerNodePlugin() const
{
    return _imp->effect->isTrackerNodePlugin();
}

bool
Node::isPointTrackerNode() const
{
    return getPluginID() == PLUGINID_OFX_TRACKERPM;
}

bool
Node::isBackdropNode() const
{
    return getPluginID() == PLUGINID_NATRON_BACKDROP;
}

void
Node::updateEffectLabelKnob(const QString & name)
{
    if (!_imp->effect) {
        return;
    }
    KnobIPtr knob = getKnobByName(kNatronOfxParamStringSublabelName);
    KnobString* strKnob = dynamic_cast<KnobString*>( knob.get() );
    if (strKnob) {
        strKnob->setValue( name.toStdString() );
    }
}

bool
Node::canOthersConnectToThisNode() const
{
    if ( dynamic_cast<Backdrop*>( _imp->effect.get() ) ) {
        return false;
    } else if ( dynamic_cast<GroupOutput*>( _imp->effect.get() ) ) {
        return false;
    } else if ( _imp->effect->isWriter() && (_imp->effect->getSequentialPreference() == eSequentialPreferenceOnlySequential) ) {
        return false;
    }
    ///In debug mode only allow connections to Writer nodes
# ifdef DEBUG

    return dynamic_cast<const ViewerInstance*>( _imp->effect.get() ) == NULL;
# else // !DEBUG
    return dynamic_cast<const ViewerInstance*>( _imp->effect.get() ) == NULL /* && !_imp->effect->isWriter()*/;
# endif // !DEBUG
}

void
Node::setNodeIsRenderingInternal(std::list<NodeWPtr>& markedNodes)
{
    ///If marked, we already set render args
    for (std::list<NodeWPtr>::iterator it = markedNodes.begin(); it != markedNodes.end(); ++it) {
        if (it->lock().get() == this) {
            return;
        }
    }

    ///Wait for the main-thread to be done dequeuing the connect actions queue
    if ( QThread::currentThread() != qApp->thread() ) {
        QMutexLocker k(&_imp->nodeIsDequeuingMutex);
        while ( _imp->nodeIsDequeuing && !aborted() ) {
            _imp->nodeIsDequeuingCond.wait(k.mutex());
        }
    }

    ///Increment the node is rendering counter
    {
        QMutexLocker nrLocker(&_imp->nodeIsRenderingMutex);
        ++_imp->nodeIsRendering;
    }


    ///mark this
    markedNodes.push_back( shared_from_this() );

    ///Call recursively

    int maxInpu = getNInputs();
    for (int i = 0; i < maxInpu; ++i) {
        NodePtr input = getInput(i);
        if (input) {
            input->setNodeIsRenderingInternal(markedNodes);
        }
    }
}

RenderingFlagSetter::RenderingFlagSetter(const NodePtr& n)
    : node(n)
    , nodes()
{
    n->setNodeIsRendering(nodes);
}

RenderingFlagSetter::~RenderingFlagSetter()
{
    for (std::list<NodeWPtr>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodePtr n = it->lock();
        if (!n) {
            continue;
        }
        n->unsetNodeIsRendering();
    }
}

void
Node::setNodeIsRendering(std::list<NodeWPtr>& nodes)
{
    setNodeIsRenderingInternal(nodes);
}

void
Node::unsetNodeIsRendering()
{
    bool mustDequeue;
    {
        int nodeIsRendering;
        ///Decrement the node is rendering counter
        QMutexLocker k(&_imp->nodeIsRenderingMutex);
        if (_imp->nodeIsRendering > 1) {
            --_imp->nodeIsRendering;
        } else {
            _imp->nodeIsRendering = 0;
        }
        nodeIsRendering = _imp->nodeIsRendering;


        mustDequeue = nodeIsRendering == 0 && !appPTR->isBackground();
    }

    if (mustDequeue) {
        Q_EMIT mustDequeueActions();
    }
}

bool
Node::isNodeRendering() const
{
    QMutexLocker k(&_imp->nodeIsRenderingMutex);

    return _imp->nodeIsRendering > 0;
}

void
Node::dequeueActions()
{
    assert( QThread::currentThread() == qApp->thread() );

    ///Flag that the node is dequeuing.
    {
        QMutexLocker k(&_imp->nodeIsDequeuingMutex);
        assert(!_imp->nodeIsDequeuing);
        _imp->nodeIsDequeuing = true;
    }
    bool hasChanged = false;
    if (_imp->effect) {
        hasChanged |= _imp->effect->dequeueValuesSet();
        NodeGroup* isGroup = dynamic_cast<NodeGroup*>( _imp->effect.get() );
        if (isGroup) {
            isGroup->dequeueConnexions();
        }
    }
    if (_imp->curveContext) {
        _imp->curveContext->dequeueGuiActions();
    }

    std::set<int> inputChanges;
    {
        QMutexLocker k(&_imp->inputsMutex);
        assert( _imp->guiInputs.size() == _imp->inputs.size() );

        for (std::size_t i = 0; i < _imp->inputs.size(); ++i) {
            NodePtr inp = _imp->inputs[i].lock();
            NodePtr guiInp = _imp->guiInputs[i].lock();
            if (inp != guiInp) {
                inputChanges.insert(i);
                _imp->inputs[i] = guiInp;
            }
        }
    }
    {
        QMutexLocker k(&_imp->outputsMutex);
        _imp->outputs = _imp->guiOutputs;
    }

    if ( !inputChanges.empty() ) {
        beginInputEdition();
        hasChanged = true;
        for (std::set<int>::iterator it = inputChanges.begin(); it != inputChanges.end(); ++it) {
            onInputChanged(*it);
        }
        endInputEdition(true);
    }
    if (hasChanged) {
        computeHash();
        refreshIdentityState();
    }

    {
        QMutexLocker k(&_imp->nodeIsDequeuingMutex);
        //Another slots in this thread might have aborted the dequeuing
        if (_imp->nodeIsDequeuing) {
            _imp->nodeIsDequeuing = false;

            //There might be multiple threads waiting
            _imp->nodeIsDequeuingCond.wakeAll();
        }
    }
} // Node::dequeueActions

static void
addIdentityNodesRecursively(const Node* caller,
                            const Node* node,
                            double time,
                            ViewIdx view,
                            std::list<const Node*>* outputs,
                            std::list<const Node*>* markedNodes)
{
    if ( std::find(markedNodes->begin(), markedNodes->end(), node) != markedNodes->end() ) {
        return;
    }

    markedNodes->push_back(node);


    if (caller != node) {
        ParallelRenderArgsPtr inputFrameArgs = node->getEffectInstance()->getParallelRenderArgsTLS();
        const FrameViewRequest* request = 0;
        bool isIdentity = false;
        if (inputFrameArgs && inputFrameArgs->request) {
            request = inputFrameArgs->request->getFrameViewRequest(time, view);
            if (request) {
                isIdentity = request->globalData.identityInputNb != -1;
            }
        }

        if (!request) {
            // An output this render does not reach. Its identity is asked under its own hash, and
            // recomputing a dirty one here would rehash every branch below the rendered nodes that
            // no viewer reads. Taken as not identity, it counts as one consumer of the image.
            U64 renderHash = 0;
            bool hashKnown = true;
            if (inputFrameArgs) {
                renderHash = node->getEffectInstance()->getRenderHash();
            } else {
                hashKnown = node->getHashValueIfUpToDate(&renderHash);
            }
            if (hashKnown) {
                double inputTimeId;
                ViewIdx identityView;
                int inputNbId;
                RectI format = node->getEffectInstance()->getMetadata(time, view)->getOutputFormat();

                // Uncached: the output format is not the node's image, and the identity cache keeps
                // whole-image answers keyed on the hash alone, which the node's own render would then
                // take for its region of definition.
                isIdentity = node->getEffectInstance()->isIdentity_public(false, renderHash, time, RenderScale::identity, format, view, &inputTimeId, &identityView, &inputNbId);
            }
        }


        if (!isIdentity) {
            outputs->push_back(node);

            return;
        }
    }

    ///Append outputs of this node instead
    NodesWList nodeOutputs;
    node->getOutputs_mt_safe(nodeOutputs);
    NodesWList outputsToAdd;
    for (NodesWList::iterator it = nodeOutputs.begin(); it != nodeOutputs.end(); ++it) {
        NodePtr output = it->lock();
        if (!output) {
            continue;
        }
        GroupOutput* isOutputNode = dynamic_cast<GroupOutput*>( output->getEffectInstance().get() );
        //If the node is an output node, add all the outputs of the group node instead
        if (isOutputNode) {
            NodeCollectionPtr collection = output->getGroup();
            assert(collection);
            NodeGroup* isGrp = dynamic_cast<NodeGroup*>( collection.get() );
            if (isGrp) {
                NodesWList groupOutputs;
                isGrp->getNode()->getOutputs_mt_safe(groupOutputs);
                for (NodesWList::iterator it2 = groupOutputs.begin(); it2 != groupOutputs.end(); ++it2) {
                    outputsToAdd.push_back(*it2);
                }
            }
        }

        //If the node is a group, add all its inputs
        NodeGroup* isGrp = dynamic_cast<NodeGroup*>(output->getEffectInstance().get());
        if (isGrp) {
            NodesList inputOutputs;
            isGrp->getInputsOutputs(&inputOutputs, false);
            for (NodesList::iterator it2 = inputOutputs.begin(); it2 != inputOutputs.end(); ++it2) {
                outputsToAdd.push_back(*it2);
            }

        }

    }
    nodeOutputs.insert( nodeOutputs.end(), outputsToAdd.begin(), outputsToAdd.end() );
    for (NodesWList::iterator it = nodeOutputs.begin(); it != nodeOutputs.end(); ++it) {
        NodePtr node = it->lock();
        if (node) {
            addIdentityNodesRecursively(caller, node.get(), time, view, outputs, markedNodes);
        }
    }
} // addIdentityNodesRecursively

bool
Node::shouldCacheOutput(bool isFrameVaryingOrAnimated,
                        double time,
                        ViewIdx view,
                        int /*visitsCount*/) const
{
    /*
     * Here is a list of reasons when caching is enabled for a node:
     * - It is references multiple times below in the graph
     * - Its single output has its settings panel opened,  meaning the user is actively editing the output
     * - The force caching parameter in the "Node" tab is checked
     * - The aggressive caching preference of Natron is checked
     * - We are in a recursive action (such as an analysis)
     * - The plug-in does temporal clip access
     * - Preview image is enabled (and Natron is not running in background)
     * - The node is a direct input of a viewer, this is to overcome linear graphs where all nodes would not be cached
     * - The node is not frame varying, meaning it will always produce the same image at any time
     * - The node is a roto node and it is being edited
     * - The node does not support tiles
     */

    std::list<const Node*> outputs;
    {
        std::list<const Node*> markedNodes;
        addIdentityNodesRecursively(this, this, time, view, &outputs, &markedNodes);
    }
    std::size_t sz = outputs.size();

    if (sz > 1) {
        ///The node is referenced multiple times below, cache it
        return true;
    } else {
        if (sz == 1) {
            const Node* output = outputs.front();
            ViewerInstance* isViewer = output->isEffectViewer();
            if (isViewer) {
                int activeInputs[2];
                isViewer->getActiveInputs(activeInputs[0], activeInputs[1]);
                if ( (output->getInput(activeInputs[0]).get() == this) ||
                     ( output->getInput(activeInputs[1]).get() == this) ) {
                    ///The node is a direct input of the viewer. Cache it because it is likely the user will make

                    ///changes to the viewer that will need this image.
                    return true;
                }
            }

            RotoPaint* isRoto = dynamic_cast<RotoPaint*>(output->getEffectInstance().get());
            if (isRoto) {
                // THe roto internally makes multiple references to the input so cache it
                return true;
            }

            if (!isFrameVaryingOrAnimated) {
                //This image never changes, cache it once.
                return true;
            }
            if ( output->isSettingsPanelVisible() ) {
                //Output node has panel opened, meaning the user is likely to be heavily editing

                //that output node, hence requesting this node a lot. Cache it.
                return true;
            }
            if ( _imp->effect->doesTemporalClipAccess() ) {
                //Very heavy to compute since many frames are fetched upstream. Cache it.
                return true;
            }
            if ( !_imp->effect->supportsTiles() ) {
                //No tiles, image is going to be produced fully, cache it to prevent multiple access

                //with different RoIs
                return true;
            }
            if (_imp->effect->getRecursionLevel() > 0) {
                //We are in a call from getImage() and the image needs to be computed, so likely in an

                //analysis pass. Cache it because the image is likely to get asked for severla times.
                return true;
            }
            if ( isForceCachingEnabled() ) {
                //Users wants it cached
                return true;
            }
            NodeGroup* parentIsGroup = dynamic_cast<NodeGroup*>( getGroup().get() );
            if ( parentIsGroup && parentIsGroup->getNode()->isForceCachingEnabled() && (parentIsGroup->getOutputNodeInput(false).get() == this) ) {
                //if the parent node is a group and it has its force caching enabled, cache the output of the Group Output's node input.
                return true;
            }

            if ( appPTR->isAggressiveCachingEnabled() ) {
                ///Users wants all nodes cached
                return true;
            }

            if ( isPreviewEnabled() && !appPTR->isBackground() ) {
                ///The node has a preview, meaning the image will be computed several times between previews & actual renders. Cache it.
                return true;
            }

            if ( isRotoPaintingNode() && isSettingsPanelVisible() ) {
                ///The Roto node is being edited, cache its output (special case because Roto has an internal node tree)
                return true;
            }

            RotoDrawableItemPtr attachedStroke = _imp->paintStroke.lock();
            if ( attachedStroke && attachedStroke->getContext()->getNode()->isSettingsPanelVisible() ) {
                ///Internal RotoPaint tree and the Roto node has its settings panel opened, cache it.
                return true;
            }
        } else {
            // outputs == 0, never cache, unless explicitly set or rotopaint internal node
            RotoDrawableItemPtr attachedStroke = _imp->paintStroke.lock();

            return isForceCachingEnabled() || appPTR->isAggressiveCachingEnabled() ||
                   ( attachedStroke && attachedStroke->getContext()->getNode()->isSettingsPanelVisible() );
        }
    }

    return false;
} // Node::shouldCacheOutput

bool
Node::refreshMaskEnabledNess(int inputNb)
{
    std::map<int, MaskSelector>::iterator found = _imp->maskSelectors.find(inputNb);
    NodePtr inp = getInput(inputNb);
    bool changed = false;

    if ( found != _imp->maskSelectors.end() ) {
        KnobBoolPtr enabled = found->second.enabled.lock();
        assert(enabled);
        enabled->blockValueChanges();
        bool curValue = enabled->getValue();
        bool newValue = inp ? true : false;
        changed = curValue != newValue;
        if (changed) {
            enabled->setValue(newValue);
        }
        enabled->unblockValueChanges();
    }

    return changed;
}

bool
Node::refreshDraftFlagInternal(const std::vector<NodeWPtr>& inputs)
{
    bool hasDraftInput = false;

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        NodePtr input = inputs[i].lock();
        if (input) {
            hasDraftInput |= input->isDraftModeUsed();
        }
    }
    hasDraftInput |= _imp->effect->supportsRenderQuality();
    bool changed;
    {
        QMutexLocker k(&_imp->pluginsPropMutex);
        changed = _imp->draftModeUsed != hasDraftInput;
        _imp->draftModeUsed = hasDraftInput;
    }

    return changed;
}

void
Node::refreshAllInputRelatedData(bool canChangeValues)
{
    refreshAllInputRelatedData( canChangeValues, getInputs_copy() );
}

bool
Node::refreshAllInputRelatedData(bool /*canChangeValues*/,
                                 const std::vector<NodeWPtr>& inputs)
{
    assert( QThread::currentThread() == qApp->thread() );
    RefreshingInputData_RAII _refreshingflag( _imp.get() );
    InputRelatedDataRefreshScope refreshing;
    {
        QMutexLocker k(&_imp->pluginsPropMutex);
        _imp->mustComputeInputRelatedData = false;
    }
    bool hasChanged = false;
    hasChanged |= refreshDraftFlagInternal(inputs);

    bool loadingProject = getApp()->getProject()->isLoadingProject();
    ///if all non optional clips are connected, call getClipPrefs
    ///The clip preferences action is never called until all non optional clips have been attached to the plugin.
    /// EDIT: we allow calling getClipPreferences even if some non optional clip is not connected so that layer menus get properly created
    const bool canCallRefreshMetadata = true; // = !hasMandatoryInputDisconnected();

    if (canCallRefreshMetadata) {
        if (loadingProject) {
            //Nb: we clear the action cache because when creating the node many calls to getRoD and stuff might have returned
            //empty rectangles, but since we force the hash to remain what was in the project file, we might then get wrong RoDs returned
            _imp->effect->clearActionsCache();
        }

        double time = (double)getApp()->getTimeLine()->currentFrame();
        ///Render scale support might not have been set already because getRegionOfDefinition could have failed until all non optional inputs were connected
        if (_imp->effect->supportsRenderScaleMaybe() == EffectInstance::eSupportsMaybe) {
            RectD rod;
            StatusEnum stat = _imp->effect->getRegionOfDefinition(getHashValue(), time, RenderScale::identity, ViewIdx(0), &rod);
            if (stat != eStatusFailed) {
                stat = _imp->effect->getRegionOfDefinition(getHashValue(), time, RenderScale::fromMipmapLevel(1), ViewIdx(0), &rod);
                if (stat != eStatusFailed) {
                    _imp->effect->setSupportsRenderScaleMaybe(EffectInstance::eSupportsYes);
                } else {
                    _imp->effect->setSupportsRenderScaleMaybe(EffectInstance::eSupportsNo);
                }
            }
        }
        // Renders compute their metadata per frame and need nothing from here. An OpenFX node
        // derives (and pushes to its clips) its time-invariant answer for its new state, and the
        // UI hook below follows the answer at the current frame.
        (void)_imp->effect->getTimeInvariantMetadata();
    }

    refreshChannelSelectors();

    registerProducedLayers();
    _imp->notifyLayerReferencesChanged();

    refreshIdentityState();

    _imp->effect->refreshCurrentFrameMetadata(getApp()->getTimeLine()->currentFrame(), false);

    if (loadingProject) {
        hasChanged |= refreshHashNow();
    }

    return hasChanged;
} // Node::refreshAllInputRelatedData

bool
Node::refreshInputRelatedDataInternal(bool domarking, std::set<Node*>& markedNodes)
{
    {
        QMutexLocker k(&_imp->pluginsPropMutex);
        if (!_imp->mustComputeInputRelatedData) {
            //We didn't change
            return false;
        }
    }

    if (domarking) {
        std::set<Node*>::iterator found = markedNodes.find(this);

        if ( found != markedNodes.end() ) {
            return false;
        }
    }

    ///Check if inputs must be refreshed first

    int maxInputs = getNInputs();
    std::vector<NodeWPtr> inputsCopy(maxInputs);
    for (int i = 0; i < maxInputs; ++i) {
        NodePtr input = getInput(i);
        inputsCopy[i] = input;
        if ( input && input->isInputRelatedDataDirty() ) {
            input->refreshInputRelatedDataInternal(true, markedNodes);
        }
    }

    if (domarking) {
        markedNodes.insert(this);
    }

    bool hasChanged = refreshAllInputRelatedData(false, inputsCopy);

    if ( isRotoPaintingNode() ) {
        RotoContextPtr roto = getRotoContext();
        assert(roto);
        NodePtr bottomMerge = roto->getRotoPaintBottomMergeNode();
        if (bottomMerge) {
            bottomMerge->refreshInputRelatedDataRecursiveInternal(markedNodes);
        }
    }

    return hasChanged;
}

bool
Node::isInputRelatedDataDirty() const
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    return _imp->mustComputeInputRelatedData;
}

void
Node::forceRefreshAllInputRelatedData()
{
    markInputRelatedDataDirtyRecursive();

    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( _imp->effect.get() );
    if (isGroup) {
        NodesList inputs;
        isGroup->getInputsOutputs(&inputs, false);
        for (NodesList::iterator it = inputs.begin(); it != inputs.end(); ++it) {
            if ( (*it) ) {
                (*it)->refreshInputRelatedDataRecursive();
            }
        }
        // The traversal above visits the nodes inside the group and never the container, whose
        // metadata is its output node's; a container with a host layer knob (Write) lists its
        // own inputs' layers on it, so its selectors need refreshing here like a plain node's.
        if (_imp->layerKnob.lock()) {
            refreshChannelSelectors();
        }
    } else {
        refreshInputRelatedDataRecursive();
    }
}

void
Node::markAllInputRelatedDataDirty()
{
    {
        QMutexLocker k(&_imp->pluginsPropMutex);
        _imp->mustComputeInputRelatedData = true;
    }
    if ( isRotoPaintingNode() ) {
        RotoContextPtr roto = getRotoContext();
        assert(roto);
        NodesList rotoNodes;
        roto->getRotoPaintTreeNodes(&rotoNodes);
        for (NodesList::iterator it = rotoNodes.begin(); it != rotoNodes.end(); ++it) {
            (*it)->markAllInputRelatedDataDirty();
        }
    }
}

void
Node::markInputRelatedDataDirtyRecursiveInternal(std::list<Node*>& markedNodes,
                                                 bool recurse)
{
    std::list<Node*>::iterator found = std::find(markedNodes.begin(), markedNodes.end(), this);

    if ( found != markedNodes.end() ) {
        return;
    }
    markAllInputRelatedDataDirty();
    markedNodes.push_back(this);
    if (recurse) {
        NodesList outputs;
        getOutputsWithGroupRedirection(outputs);
        for (NodesList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
            (*it)->markInputRelatedDataDirtyRecursiveInternal( markedNodes, true );
        }
    }
}

void
Node::markInputRelatedDataDirtyRecursive()
{
    std::list<Node*> marked;

    markInputRelatedDataDirtyRecursiveInternal(marked, true);
}

void
Node::refreshInputRelatedDataRecursiveInternal(std::set<Node*>& markedNodes)
{
    if ( getApp()->isCreatingNodeTree() ) {
        return;
    }
    std::set<Node*>::iterator found = markedNodes.find(this);

    if ( found != markedNodes.end() ) {
        return;
    }

    markedNodes.insert(this);
    refreshInputRelatedDataInternal(false, markedNodes);

    ///Now notify outputs we have changed
    NodesList outputs;
    getOutputsWithGroupRedirection(outputs);
    for (NodesList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
        (*it)->refreshInputRelatedDataRecursiveInternal( markedNodes );
    }
}

void
Node::refreshInputRelatedDataRecursive()
{
    std::set<Node*> markedNodes;

    refreshInputRelatedDataRecursiveInternal(markedNodes);
}

bool
Node::refreshShownInputRelatedData(std::set<Node*>& visited)
{
    if (!visited.insert(this).second) {
        return false;
    }
    if (!isInputRelatedDataDirty()) {
        return false;
    }

    int shown[2] = { -1, -1 };
    ViewerInstance* isViewer = dynamic_cast<ViewerInstance*>(_imp->effect.get());
    if (isViewer) {
        isViewer->getActiveInputs(shown[0], shown[1]);
    }
    const int maxInputs = getNInputs();
    std::vector<NodeWPtr> inputsCopy(maxInputs);
    for (int i = 0; i < maxInputs; ++i) {
        NodePtr input = getInput(i);
        inputsCopy[i] = input;
        if (!input || (isViewer && (i != shown[0]) && (i != shown[1]))) {
            continue;
        }
        input->refreshShownInputRelatedData(visited);
    }

    bool hasChanged = refreshAllInputRelatedData(false, inputsCopy);

    if (isRotoPaintingNode()) {
        RotoContextPtr roto = getRotoContext();
        assert(roto);
        NodePtr bottomMerge = roto->getRotoPaintBottomMergeNode();
        if (bottomMerge) {
            bottomMerge->refreshInputRelatedDataRecursiveInternal(visited);
        }
    }

    return hasChanged;
} // Node::refreshShownInputRelatedData

void
Node::refreshInputRelatedDataAfterInputChange()
{
    assert(QThread::currentThread() == qApp->thread());
    markAllInputRelatedDataDirty();
    refreshShownDownstreamInputRelatedData();

    NodeGroup* isGroup = dynamic_cast<NodeGroup*>(_imp->effect.get());
    if (isGroup) {
        // A container's inputs feed the nodes inside it, which are refreshed as nodes downstream
        // of the edit. A container with a host layer knob (Write) lists its own inputs' layers on
        // it, so its selectors are refreshed here like those of the node the edit is on.
        if (_imp->layerKnob.lock()) {
            refreshChannelSelectors();
        }

        return;
    }

    std::set<Node*> visited;
    refreshShownInputRelatedData(visited);
}

void
Node::refreshShownDownstreamInputRelatedData()
{
    assert(QThread::currentThread() == qApp->thread());

    std::unordered_set<const Node*> visited;
    visited.insert(this);
    // Breadth first: the vector is the queue, read from the front without erasing.
    std::vector<NodePtr> toVisit;
    appendOutputsWithGroupRedirection(&toVisit);
    std::vector<NodePtr> reached;
    for (std::size_t next = 0; next < toVisit.size(); ++next) {
        NodePtr node = toVisit[next];
        if (!node || !visited.insert(node.get()).second) {
            continue;
        }
        node->markAllInputRelatedDataDirty();
        reached.push_back(node);
        node->appendOutputsWithGroupRedirection(&toVisit);
    }

    // The draft flag is the one piece of this data a render reads; it only reads the inputs'
    // flags, so it is settled here for every node reached, without reading a hash. Breadth-first
    // order is upstream first on a chain; a diamond takes another pass.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < reached.size(); ++i) {
            if (reached[i]->_imp->effect) {
                changed |= reached[i]->refreshDraftFlagInternal(reached[i]->getInputs_copy());
            }
        }
    }

    // The rest is what panels and the node graph show: the nodes whose settings panel is visible,
    // and those showing a missing-channel message the edit may have resolved, are refreshed now.
    // The others are refreshed when their panel is shown (refreshPanelInputRelatedData()).
    for (std::size_t i = 0; i < reached.size(); ++i) {
        bool hasChannelSelectorMessage;
        {
            QMutexLocker k(&reached[i]->_imp->persistentMessageMutex);
            hasChannelSelectorMessage = reached[i]->_imp->persistentMessageFromChannelSelector;
        }
        if (hasChannelSelectorMessage || reached[i]->isSettingsPanelVisible()) {
            reached[i]->refreshPanelInputRelatedData();
        }
    }
}

Node::InputRelatedDataRefreshDeferral::InputRelatedDataRefreshDeferral()
{
    ++inputRelatedDataRefreshDeferralDepth;
}

Node::InputRelatedDataRefreshDeferral::~InputRelatedDataRefreshDeferral()
{
    --inputRelatedDataRefreshDeferralDepth;
}

bool
Node::canRunMainThreadMetadataRefresh() const
{
    if (QThread::currentThread() != qApp->thread()) {
        return false;
    }
    if ((hashComputeDepth > 0) || (inputRelatedDataRefreshDeferralDepth > 0)) {
        return false;
    }
    if (!_imp->effect || !isNodeCreated() || !isActivated()) {
        return false;
    }
    {
        QMutexLocker k(&_imp->isBeingDestroyedMutex);
        if (_imp->isBeingDestroyed) {
            return false;
        }
    }
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project || project->isProjectClosing()) {
        return false;
    }
    // While the project loads or a node tree is created, only the refresh that follows covers it.
    if ((app->isCreatingNodeTree() || project->isLoadingProject()) && (inputRelatedDataRefreshDepth == 0)) {
        return false;
    }

    return true;
}

void
Node::refreshPanelInputRelatedData()
{
    // A refresh already running orders the nodes it reaches itself, inputs first.
    if ((inputRelatedDataRefreshDepth > 0) || !canRunMainThreadMetadataRefresh()) {
        return;
    }
    if (isInputRelatedDataDirty()) {
        std::set<Node*> visited;
        refreshShownInputRelatedData(visited);
    } else {
        _imp->effect->refreshCurrentFrameMetadata(getApp()->getTimeLine()->currentFrame(), false);
    }
}

bool
Node::isDraftModeUsed() const
{
    QMutexLocker k(&_imp->pluginsPropMutex);

    return _imp->draftModeUsed;
}

void
Node::setPosition(double x,
                  double y)
{
    setLayoutFromGuiPosition(x, y);
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (gui) {
        gui->setPosition(x, y);
    }
}

void
Node::getPosition(double *x,
                  double *y) const
{
    QMutexLocker k(&_imp->layoutMutex);

    *x = _imp->posX;
    *y = _imp->posY;
}

void
Node::setSize(double w,
              double h)
{
    setLayoutFromGuiSize(w, h);
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (gui) {
        gui->setSize(w, h);
    }
}

void
Node::getSize(double* w,
              double* h) const
{
    {
        QMutexLocker k(&_imp->layoutMutex);
        if (_imp->hasSizeSet) {
            *w = _imp->sizeW;
            *h = _imp->sizeH;

            return;
        }
    }
    // A node that was never sized is laid out by its GUI.
    NodeGuiIPtr gui = _imp->guiPointer.lock();
    if (gui) {
        gui->getSize(w, h);

        return;
    }
    *w = 0.;
    *h = 0.;
}

bool
Node::hasSize() const
{
    QMutexLocker k(&_imp->layoutMutex);

    return _imp->hasSizeSet;
}

bool
Node::getColor(double* r,
               double *g,
               double* b) const
{
    {
        QMutexLocker k(&_imp->layoutMutex);
        if (_imp->userColorSet) {
            *r = _imp->userColor[0];
            *g = _imp->userColor[1];
            *b = _imp->userColor[2];

            return true;
        }
    }
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (gui) {
        gui->getColor(r, g, b);

        return true;
    }
    *r = 0.;
    *g = 0.;
    *b = 0.;

    return false;
}

void
Node::setColor(double r,
               double g,
               double b)
{
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (!gui) {
        setLayoutFromGuiUserColor(true, r, g, b);

        return;
    }
    gui->setColor(r, g, b);
    // The GUI drops a colour close to the category colour, which only it knows. A backdrop's
    // colour is its body rather than a user colour, so the GUI never reports one for it.
    setLayoutFromGuiUserColor(isBackdropNode() || gui->hasUserColor(), r, g, b);
}

bool
Node::hasUserColor() const
{
    QMutexLocker k(&_imp->layoutMutex);

    return _imp->userColorSet;
}

void
Node::getUserColor(double* r,
                   double* g,
                   double* b) const
{
    QMutexLocker k(&_imp->layoutMutex);

    *r = _imp->userColor[0];
    *g = _imp->userColor[1];
    *b = _imp->userColor[2];
}

void
Node::resetColor()
{
    setLayoutFromGuiUserColor(false, 0., 0., 0.);
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (gui) {
        gui->clearUserColor();
    }
}

void
Node::setLayoutFromGuiPosition(double x,
                               double y)
{
    QMutexLocker k(&_imp->layoutMutex);

    _imp->posX = x;
    _imp->posY = y;
}

void
Node::setLayoutFromGuiSize(double w,
                           double h)
{
    QMutexLocker k(&_imp->layoutMutex);

    _imp->sizeW = w;
    _imp->sizeH = h;
    _imp->hasSizeSet = true;
}

void
Node::setLayoutFromGuiUserColor(bool hasColor,
                                double r,
                                double g,
                                double b)
{
    QMutexLocker k(&_imp->layoutMutex);

    _imp->userColorSet = hasColor;
    _imp->userColor[0] = hasColor ? r : 0.;
    _imp->userColor[1] = hasColor ? g : 0.;
    _imp->userColor[2] = hasColor ? b : 0.;
}

void
Node::setNodeGuiPointer(const NodeGuiIPtr& gui)
{
    assert( !_imp->guiPointer.lock() );
    assert( QThread::currentThread() == qApp->thread() );
    _imp->guiPointer = gui;
}

NodeGuiIPtr
Node::getNodeGui() const
{
    return _imp->guiPointer.lock();
}

bool
Node::isUserSelected() const
{
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (!gui) {
        return false;
    }

    return gui->isUserSelected();
}

bool
Node::isSettingsPanelMinimized() const
{
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (!gui) {
        return false;
    }

    return gui->isSettingsPanelMinimized();
}

bool
Node::isSettingsPanelVisibleInternal(std::set<const Node*>& recursionList) const
{
    NodeGuiIPtr gui = _imp->guiPointer.lock();

    if (!gui) {
        return false;
    }
    NodePtr parent = _imp->multiInstanceParent.lock();
    if (parent) {
        return parent->isSettingsPanelVisible();
    }

    if ( recursionList.find(this) != recursionList.end() ) {
        return false;
    }
    recursionList.insert(this);

    {
        for (KnobLinkList::iterator it = _imp->nodeLinks.begin(); it != _imp->nodeLinks.end(); ++it) {
            NodePtr masterNode = it->masterNode.lock();
            if ( masterNode && (masterNode.get() != this) && masterNode->isSettingsPanelVisibleInternal(recursionList) ) {
                return true;
            }
        }
    }

    return gui->isSettingsPanelVisible();
}

bool
Node::isSettingsPanelVisible() const
{
    std::set<const Node*> tmplist;

    return isSettingsPanelVisibleInternal(tmplist);
}

void
Node::attachRotoItem(const RotoDrawableItemPtr& stroke)
{
    assert( QThread::currentThread() == qApp->thread() );
    _imp->paintStroke = stroke;
    // The hash reads the attached item, so the stored one must not predate it.
    computeHash();
}

RotoDrawableItemPtr
Node::getAttachedRotoItem() const
{
    return _imp->paintStroke.lock();
}

void
Node::declareNodeVariableToPython(const std::string& nodeName)
{
#ifdef NATRON_RUN_WITHOUT_PYTHON

    return;
#endif
    if (getScriptName_mt_safe().empty()) {
        return;
    }
    if ( NATRON_PYTHON_NAMESPACE::isKeyword(nodeName) ) {
        throw std::runtime_error(nodeName + " is a Python keyword");
    }

    PythonGILLocker pgl;
    PyObject* mainModule = appPTR->getMainModule();
    assert(mainModule);

    std::string appID = getApp()->getAppIDString();
    std::string nodeFullName = appID + "." + nodeName;
    bool alreadyDefined = false;
    PyObject* nodeObj = NATRON_PYTHON_NAMESPACE::getAttrRecursive(nodeFullName, mainModule, &alreadyDefined);
    assert(nodeObj);
    Q_UNUSED(nodeObj);

    if (!alreadyDefined) {
        std::stringstream ss;
        ss << nodeFullName << " = " << appID << ".getNode(\"" << nodeName << "\")\n";
#ifdef DEBUG
        ss << "if not " << nodeFullName << ":\n";
        ss << "    print(\"[BUG]: " << nodeFullName << " does not exist!\")";
#endif
        std::string script = ss.str();
        std::string output;
        std::string err;
        if ( !appPTR->isBackground() ) {
            getApp()->printAutoDeclaredVariable(script);
        }
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output) ) {
            qDebug() << err.c_str();
        }
    }
}

void
Node::setNodeVariableToPython(const std::string& oldName,
                              const std::string& newName)
{
#ifdef NATRON_RUN_WITHOUT_PYTHON

    return;
#endif
    if (getScriptName_mt_safe().empty()) {
        return;
    }
    QString appID = QString::fromUtf8( getApp()->getAppIDString().c_str() );
    QString str = QString( appID + QString::fromUtf8(".%1 = ") + appID + QString::fromUtf8(".%2\ndel ") + appID + QString::fromUtf8(".%2\n") ).arg( QString::fromUtf8( newName.c_str() ) ).arg( QString::fromUtf8( oldName.c_str() ) );
    std::string script = str.toStdString();
    std::string err;
    if ( !appPTR->isBackground() ) {
        getApp()->printAutoDeclaredVariable(script);
    }
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0) ) {
        qDebug() << err.c_str();
    }
}

void
Node::deleteNodeVariableToPython(const std::string& nodeName)
{
#ifdef NATRON_RUN_WITHOUT_PYTHON

    return;
#endif
   if (getScriptName_mt_safe().empty()) {
        return;
    }
    if ( getParentMultiInstance() ) {
        return;
    }

    AppInstancePtr app = getApp();
    if (!app) {
        return;
    }
    QString appID = QString::fromUtf8( getApp()->getAppIDString().c_str() );
    std::string nodeFullName = appID.toStdString() + "." + nodeName;
    bool alreadyDefined = false;
    PyObject* nodeObj = NATRON_PYTHON_NAMESPACE::getAttrRecursive(nodeFullName, appPTR->getMainModule(), &alreadyDefined);
    assert(nodeObj);
    Q_UNUSED(nodeObj);
    if (alreadyDefined) {
        std::string script = "del " + nodeFullName;
        std::string err;
        if ( !appPTR->isBackground() ) {
            getApp()->printAutoDeclaredVariable(script);
        }
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0) ) {
            qDebug() << err.c_str();
        }
    }
}

void
Node::declarePythonFields()
{
#ifdef NATRON_RUN_WITHOUT_PYTHON

    return;
#endif
    if (getScriptName_mt_safe().empty()) {
        return;
    }
    PythonGILLocker pgl;

    if ( !getGroup() ) {
        return;
    }

    std::locale locale;
    std::string nodeName = getFullyQualifiedName();
    std::string appID = getApp()->getAppIDString();
    bool alreadyDefined = false;
    std::string nodeFullName = appID + "." + nodeName;
    PyObject* nodeObj = NATRON_PYTHON_NAMESPACE::getAttrRecursive(nodeFullName, NATRON_PYTHON_NAMESPACE::getMainModule(), &alreadyDefined);
    assert(nodeObj);
    Q_UNUSED(nodeObj);
    if (!alreadyDefined) {
        qDebug() << QString::fromUtf8("declarePythonFields(): attribute ") + QString::fromUtf8( nodeFullName.c_str() ) + QString::fromUtf8(" is not defined");
        throw std::logic_error(std::string("declarePythonFields(): attribute ") + nodeFullName + " is not defined");
    }


    std::stringstream ss;
#ifdef DEBUG
    ss << "if not " << nodeFullName << ":\n";
    ss << "    print(\"[BUG]: " << nodeFullName << " is not defined!\")\n";
#endif
    const KnobsVec& knobs = getKnobs();
    for (U32 i = 0; i < knobs.size(); ++i) {
        const std::string& knobName = knobs[i]->getName();
        if ( !knobName.empty() && (knobName.find(" ") == std::string::npos) && !std::isdigit(knobName[0], locale) ) {
            if ( PyObject_HasAttrString( nodeObj, knobName.c_str() ) ) {
                continue;
            }
            ss << nodeFullName <<  "." << knobName << " = ";
            ss << nodeFullName << ".getParam(\"" << knobName << "\")\n";
        }
    }

    std::string script = ss.str();
    if ( !script.empty() ) {
        if ( !appPTR->isBackground() ) {
            getApp()->printAutoDeclaredVariable(script);
        }
        std::string err;
        std::string output;
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output) ) {
            qDebug() << err.c_str();
        }
    }
} // Node::declarePythonFields

void
Node::removeParameterFromPython(const std::string& parameterName)
{
#ifdef NATRON_RUN_WITHOUT_PYTHON

    return;
#endif
    if (getScriptName_mt_safe().empty()) {
        return;
    }
    PythonGILLocker pgl;
    std::string appID = getApp()->getAppIDString();
    std::string nodeName = getFullyQualifiedName();
    std::string nodeFullName = appID + "." + nodeName;
    bool alreadyDefined = false;
    PyObject* nodeObj = NATRON_PYTHON_NAMESPACE::getAttrRecursive(nodeFullName, NATRON_PYTHON_NAMESPACE::getMainModule(), &alreadyDefined);
    assert(nodeObj);
    Q_UNUSED(nodeObj);
    if (!alreadyDefined) {
        qDebug() << QString::fromUtf8("removeParameterFromPython(): attribute ") + QString::fromUtf8( nodeFullName.c_str() ) + QString::fromUtf8(" is not defined");
        throw std::logic_error(std::string("removeParameterFromPython(): attribute ") + nodeFullName + " is not defined");
    }
    // declarePythonFields() skips knobs whose name is not a valid Python identifier, so those have
    // no attribute to delete.
    if (!PyObject_HasAttrString(nodeObj, parameterName.c_str())) {
        return;
    }
    std::string script = "del " + nodeFullName + "." + parameterName;
    if ( !appPTR->isBackground() ) {
        getApp()->printAutoDeclaredVariable(script);
    }
    std::string err;
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0) ) {
        qDebug() << err.c_str();
    }
}

void
Node::declareAllPythonAttributes()
{
#ifdef NATRON_RUN_WITHOUT_PYTHON

    return;
#endif
    try {
        declareNodeVariableToPython( getFullyQualifiedName() );
        declarePythonFields();
        if (_imp->curveContext) {
            declareRotoPythonField();
        }
        if (_imp->trackContext) {
            declareTrackerPythonField();
        }
    } catch (const std::exception& e) {
        qDebug() << e.what();
    }
}

std::string
Node::getKnobChangedCallback() const
{
    KnobStringPtr s = _imp->knobChangedCallback.lock();

    return s ? s->getValue() : std::string();
}

std::string
Node::getInputChangedCallback() const
{
    KnobStringPtr s = _imp->inputChangedCallback.lock();

    return s ? s->getValue() : std::string();
}

void
Node::Implementation::runOnNodeCreatedCBInternal(const std::string& cb,
                                                 bool userEdited)
{
    std::vector<std::string> args;
    std::string error;
    if (_publicInterface->getScriptName_mt_safe().empty()) {
        return;
    }
    try {
        NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
    } catch (const std::exception& e) {
        _publicInterface->getApp()->appendToScriptEditor( std::string("Failed to get signature onNodeCreated callback: ")
                                                          + e.what() );

        return;
    }

    if ( !error.empty() ) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to get signature onNodeCreated callback: " + error);

        return;
    }

    std::string signatureError;
    signatureError.append("The on node created callback supports the following signature(s):\n");
    signatureError.append("- callback(thisNode,app,userEdited)");
    if (args.size() != 3) {
        _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onNodeCreated callback: " + signatureError);

        return;
    }

    if ( (args[0] != "thisNode") || (args[1] != "app") || (args[2] != "userEdited") ) {
        _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onNodeCreated callback: " + signatureError);

        return;
    }

    std::string appID = _publicInterface->getApp()->getAppIDString();
    std::string scriptName = _publicInterface->getScriptName_mt_safe();
    if ( scriptName.empty() ) {
        return;
    }
    std::stringstream ss;
    ss << cb << "(" << appID << "." << _publicInterface->getFullyQualifiedName() << "," << appID << ",";
    if (userEdited) {
        ss << "True";
    } else {
        ss << "False";
    }
    ss << ")\n";
    std::string output;
    std::string script = ss.str();
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output) ) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to run onNodeCreated callback: " + error);
    } else if ( !output.empty() ) {
        _publicInterface->getApp()->appendToScriptEditor(output);
    }
} // Node::Implementation::runOnNodeCreatedCBInternal

void
Node::Implementation::runOnNodeDeleteCBInternal(const std::string& cb)
{
    std::vector<std::string> args;
    std::string error;

    try {
        NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
    } catch (const std::exception& e) {
        _publicInterface->getApp()->appendToScriptEditor( std::string("Failed to get signature of onNodeDeletion callback: ")
                                                          + e.what() );

        return;
    }

    if ( !error.empty() ) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to get signature of onNodeDeletion callback: " + error);

        return;
    }

    std::string signatureError;
    signatureError.append("The on node deletion callback supports the following signature(s):\n");
    signatureError.append("- callback(thisNode,app)");
    if (args.size() != 2) {
        _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onNodeDeletion callback: " + signatureError);

        return;
    }

    if ( (args[0] != "thisNode") || (args[1] != "app") ) {
        _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onNodeDeletion callback: " + signatureError);

        return;
    }

    std::string appID = _publicInterface->getApp()->getAppIDString();
    std::stringstream ss;
    ss << cb << "(" << appID << "." << _publicInterface->getFullyQualifiedName() << "," << appID << ")\n";

    std::string err;
    std::string output;
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(ss.str(), &err, &output) ) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to run onNodeDeletion callback: " + err);
    } else if ( !output.empty() ) {
        _publicInterface->getApp()->appendToScriptEditor(output);
    }
}

void
Node::Implementation::runOnNodeCreatedCB(bool userEdited)
{
    if (!isPartOfProject) {
        return;
    }
    std::string cb = _publicInterface->getApp()->getProject()->getOnNodeCreatedCB();
    NodeCollectionPtr group = _publicInterface->getGroup();

    if (!group) {
        return;
    }
    if ( !cb.empty() ) {
        runOnNodeCreatedCBInternal(cb, userEdited);
    }

    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( group.get() );
    KnobStringPtr nodeCreatedCbKnob = nodeCreatedCallback.lock();
    if (!nodeCreatedCbKnob && isGroup) {
        cb = isGroup->getNode()->getAfterNodeCreatedCallback();
    } else if (nodeCreatedCbKnob) {
        cb = nodeCreatedCbKnob->getValue();
    }
    if ( !cb.empty() ) {
        runOnNodeCreatedCBInternal(cb, userEdited);
    }
}

void
Node::Implementation::runOnNodeDeleteCB()
{
    if (!isPartOfProject) {
        return;
    }

    if (_publicInterface->getScriptName_mt_safe().empty()) {
        return;
    }
    std::string cb = _publicInterface->getApp()->getProject()->getOnNodeDeleteCB();
    NodeCollectionPtr group = _publicInterface->getGroup();

    if (!group) {
        return;
    }
    if ( !cb.empty() ) {
        runOnNodeDeleteCBInternal(cb);
    }


    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( group.get() );
    KnobStringPtr nodeDeletedKnob = nodeRemovalCallback.lock();
    if (!nodeDeletedKnob && isGroup) {
        NodePtr grpNode = isGroup->getNode();
        if (grpNode) {
            cb = grpNode->getBeforeNodeRemovalCallback();
        }
    } else if (nodeDeletedKnob) {
        cb = nodeDeletedKnob->getValue();
    }
    if ( !cb.empty() ) {
        runOnNodeDeleteCBInternal(cb);
    }
}

std::string
Node::getBeforeRenderCallback() const
{
    KnobStringPtr s = _imp->beforeRender.lock();

    return s ? s->getValue() : std::string();
}

std::string
Node::getBeforeFrameRenderCallback() const
{
    KnobStringPtr s = _imp->beforeFrameRender.lock();

    return s ? s->getValue() : std::string();
}

std::string
Node::getAfterRenderCallback() const
{
    KnobStringPtr s = _imp->afterRender.lock();

    return s ? s->getValue() : std::string();
}

std::string
Node::getAfterFrameRenderCallback() const
{
    KnobStringPtr s = _imp->afterFrameRender.lock();

    return s ? s->getValue() : std::string();
}

std::string
Node::getAfterNodeCreatedCallback() const
{
    KnobStringPtr s = _imp->nodeCreatedCallback.lock();

    return s ? s->getValue() : std::string();
}

std::string
Node::getBeforeNodeRemovalCallback() const
{
    KnobStringPtr s = _imp->nodeRemovalCallback.lock();

    return s ? s->getValue() : std::string();
}

void
Node::runInputChangedCallback(int index)
{
    std::string cb = getInputChangedCallback();

    if ( !cb.empty() ) {
        _imp->runInputChangedCallback(index, cb);
    }
}

void
Node::Implementation::runInputChangedCallback(int index,
                                              const std::string& cb)
{
    // Undoing or redoing recorded edits replays what the callback did, so it must not run again.
    AppInstancePtr app = _publicInterface->getApp();
    if (app && app->isReplayingUndo()) {
        return;
    }

    std::vector<std::string> args;
    std::string error;

    try {
        NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
    } catch (const std::exception& e) {
        _publicInterface->getApp()->appendToScriptEditor( std::string("Failed to get signature of onInputChanged callback: ")
                                                          + e.what() );

        return;
    }

    if ( !error.empty() ) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to get signature of onInputChanged callback: " + error);

        return;
    }

    std::string signatureError;
    signatureError.append("The on input changed callback supports the following signature(s):\n");
    signatureError.append("- callback(inputIndex,thisNode,thisGroup,app)");
    if (args.size() != 4) {
        _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onInputChanged callback: " + signatureError);

        return;
    }

    if ( (args[0] != "inputIndex") || (args[1] != "thisNode") || (args[2] != "thisGroup") || (args[3] != "app") ) {
        _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onInputChanged callback: " + signatureError);

        return;
    }

    std::string appID = _publicInterface->getApp()->getAppIDString();
    NodeCollectionPtr collection = _publicInterface->getGroup();
    assert(collection);
    if (!collection) {
        return;
    }

    std::string thisGroupVar;
    NodeGroup* isParentGrp = dynamic_cast<NodeGroup*>( collection.get() );
    if (isParentGrp) {
        std::string nodeName = isParentGrp->getNode()->getFullyQualifiedName();
        std::string nodeFullName = appID + "." + nodeName;
        thisGroupVar = nodeFullName;
    } else {
        thisGroupVar = appID;
    }

    std::stringstream ss;
    ss << cb << "(" << index << "," << appID << "." << _publicInterface->getFullyQualifiedName() << "," << thisGroupVar << "," << appID << ")\n";

    std::string script = ss.str();
    std::string output;
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output) ) {
        _publicInterface->getApp()->appendToScriptEditor( tr("Failed to execute callback: %1").arg( QString::fromUtf8( error.c_str() ) ).toStdString() );
    } else {
        if ( !output.empty() ) {
            _publicInterface->getApp()->appendToScriptEditor(output);
        }
    }
} // Node::Implementation::runInputChangedCallback

int
Node::getMaskChannel(int inputNb, const std::list<ImageLayerDesc>& availableLayers, ImageLayerDesc* comps) const
{
    *comps = ImageLayerDesc::getNoneComponents();

    std::map<int, MaskSelector >::const_iterator it = _imp->maskSelectors.find(inputNb);

    if ( it == _imp->maskSelectors.end() ) {
        return -1;
    }
    KnobChannelSelectPtr channel = it->second.channel.lock();
    if (!channel) {
        return -1;
    }
    int channelIndex = -1;
    ImageLayerDesc layer;
    if (!channel->resolve(availableLayers, &layer, &channelIndex)) {
        return -1;
    }
    *comps = layer;

    return channelIndex;
}

void
Node::refreshChannelSelectors()
{
    if ( !isNodeCreated() ) {
        return;
    }

    _imp->effect->onChannelsSelectorRefreshed();

    if (checkSelectedChannelsPresent(0)) {
        clearChannelSelectorMessage();
    }

    Q_EMIT layerListRefreshed();
    s_layerSelectionChanged();
}

void
Node::relistLayerKnobs()
{
    Q_EMIT layerListRefreshed();
}

double
Node::getHostMixingValue(double time,
                         ViewIdx view) const
{
    KnobDoublePtr mix = _imp->mixWithSource.lock();

    return mix ? mix->getValueAtTime(time, 0, view) : 1.;
}


NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_Node.cpp"
