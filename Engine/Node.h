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

#ifndef NATRON_ENGINE_NODE_H
#define NATRON_ENGINE_NODE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
#include <QMetaType>
#include <QObject>
CLANG_DIAG_ON(deprecated)
#include <QMutex>

#include "Engine/AppManager.h"
#include "Engine/CacheEntryHolder.h"
#include "Engine/EffectInstance.h" // for LayerKnobSpec::RoleEnum
#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Markdown.h"
#include "Engine/ViewIdx.h"
#include "Global/KeySymbols.h"

#include "Serialization/SerializationFwd.h"

#define NATRON_PARAMETER_PAGE_NAME_EXTRA "Node"
#define NATRON_PARAMETER_PAGE_NAME_INFO "Info"


#define kDisableNodeKnobName "disableNode"
#define kLifeTimeNodeKnobName "nodeLifeTime"
#define kEnableLifeTimeNodeKnobName "enableNodeLifeTime"
#define kUserLabelKnobName "userTextArea"
#define kEnableMaskKnobName "enableMask"
#define kEnableInputKnobName "enableInput"
#define kMaskChannelKnobName "maskChannel"

// The host-owned "(Un)premult by" channel selector, and the plug-in-declared bool and choice
// it stands in for (openfx-misc's ofxsPremultDescribeParams()), which it switches off.
#define kUnPremultByKnobName "hostUnPremultBy"
#define kUnPremultByKnobLabel "(Un)premult by"
#define kUnPremultByPluginKnobName "unPremultBy"
#define kUnPremultByChannelPluginKnobName "unPremultByChannel"
#define kInputChannelKnobName "inputChannel"
#define kEnablePreviewKnobName "enablePreview"
#define kNodeParamChannelSet "channels"
#define kNodeParamChannelSetLabel "Channels"
#define kNodeParamLayerSelect "layer"
#define kNodeParamLayerSelectLabel "Layer"
#define kNodeParamLayerSeparator "layerSeparator"

#define kOfxMaskInvertParamName "maskInvert"
#define kOfxMixParamName "mix"

#define kReadOIIOAvailableViewsKnobName "availableViews"
#define kWriteOIIOParamViewsSelector "viewsSelector"


NATRON_NAMESPACE_ENTER

class Node
    : public QObject
    , public std::enable_shared_from_this<Node>
    , public CacheEntryHolder
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    struct Implementation;

public:
    // TODO: enable_shared_from_this
    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>
    Node(const AppInstancePtr& app,
         const NodeCollectionPtr& group,
         Plugin* plugin);

public:
    virtual ~Node();

    NodeCollectionPtr getGroup() const;

    /**
     * @brief Returns true if this node is a "user" node. For internal invisible node, this would return false.
     * If this function returns false, the node will not be serialized.
     **/
    bool isPartOfProject() const;

    const Plugin* getPlugin() const;

    /**
     * @brief Used internally when instantiating a Python template, we first make a group and then pass a pointer
     * to the real plugin.
     **/
    void switchInternalPlugin(Plugin* plugin);

    void setPrecompNode(const PrecompNodePtr& precomp);
    PrecompNodePtr isPartOfPrecomp() const;

    /**
     * @brief Creates the EffectInstance that will be embedded into this node and set it up.
     * This function also loads all parameters. Node connections will not be setup in this method.
     **/
    void load(const CreateNodeArgs& args);

    void initNodeScriptName(const Serialization::NodeSerialization* serialization, const QString& fixedName);

    void loadKnob(const KnobIPtr& knob,
                  const Serialization::NodeSerialization& serialization,
                  bool updateKnobGui = false);

    ///Set values for Knobs given their serialization
    void setValuesFromSerialization(const CreateNodeArgs& args);

    ///to be called once all nodes have been loaded from the project or right away after the load() function.
    ///this is so the child of a multi-instance can retrieve the pointer to it's main instance
    void fetchParentMultiInstancePointer();


    ///If the node can have a roto context, create it
    void createRotoContextConditionnally();

    void createTrackerContextConditionnally();

    ///function called by EffectInstance to create a knob
    template <class K>
    std::shared_ptr<K> createKnob(const std::string &description,
                                    int dimension = 1,
                                    bool declaredByPlugin = true)
    {
        return appPTR->getKnobFactory().createKnob<K>(getEffectInstance(), description, dimension, declaredByPlugin);
    }

    /// This cannot be done in fromSerialization as to call this all the nodes in the project must have
    /// been loaded first.
    void storeKnobsLinks(const Serialization::NodeSerialization& serialization,
                         const std::map<std::string, std::string>& oldNewScriptNamesMapping);

    // Once all nodes are created, we can restore the links that were previously saved by storeKnobsLinks()
    void restoreKnobsLinks(const NodesList & allNodes,
                           const std::map<std::string, std::string>& oldNewScriptNamesMapping,
                           bool throwOnFailure);

    void restoreUserKnobs(const Serialization::NodeSerialization& serialization);

    /**
     * @brief Fills serialization with everything a project file saves for this node and, for a
     * group or a multi-instance node, the nodes it holds. The node's own input connections are
     * left out unless serializeInputs is set.
     **/
    void toSerialization(Serialization::NodeSerialization* serialization,
                         bool serializeInputs = true) const;

    /**
     * @brief Restores the knobs, roto and tracker data and user knobs saved in serialization
     * onto this already created node.
     **/
    void fromSerialization(const Serialization::NodeSerialization& serialization,
                           bool updateKnobGui = false);

    enum PresetApplyModeEnum {
        // A preset file. Knobs it leaves out are reset. The label, the sublabel, the disable
        // switch, a reader's or writer's file paths, the roto shapes and tracks and the node's
        // own user knobs are not the preset's: they keep their value, animation, expressions
        // and links whatever the preset holds for them.
        ePresetApplyModePreset,

        // A toSerialization() snapshot of this node. A knob it leaves out was at its default
        // when it was taken, which makes every one of them a reset, roto shapes and tracks included.
        ePresetApplyModeSnapshot
    };

    /**
     * @brief Makes this node match a sparse serialization, where fromSerialization() only
     * writes what serialization holds over the node's current state. Every persistent plug-in
     * knob loses its expression, links and animation, and the ones absent from serialization
     * return to their defaults (with the exceptions of mode). Expressions and links in
     * serialization are restored against the project's nodes. The node's own user knobs are
     * kept; the ones serialization creates are appended to createdUserKnobs, for undo to remove
     * with deleteUserKnobs().
     **/
    void applyPresetSerialization(const Serialization::NodeSerialization& serialization,
                                  PresetApplyModeEnum mode,
                                  std::list<KnobIPtr>* createdUserKnobs);

    /**
     * @brief Deletes those of knobs that are still user knobs of this node and rebuilds the
     * user pages of its settings panel.
     **/
    void deleteUserKnobs(const std::list<KnobIWPtr>& knobs);

    void setPagesOrder(const std::list<std::string>& pages);

    std::list<std::string> getPagesOrder() const;

    bool isNodeCreated() const;

    bool isGLFinishRequiredBeforeRender() const;

    void refreshAcceptedBitDepths();

    /**
     * @brief Quits any processing on going on this node, this call is non blocking
     **/
    void quitAnyProcessing_non_blocking();
    void quitAnyProcessing_blocking(bool allowThreadsToRestart);

    /**
     * @brief Returns true if all processing threads controlled by this node have quit
     **/
    bool areAllProcessingThreadsQuit() const;

    /* @brief Similar to quitAnyProcessing except that the threads aren't destroyed
     * This is called when a node is deleted by the user
     */
    void abortAnyProcessing_non_blocking();
    void abortAnyProcessing_blocking();

    /*Never call this yourself. This is needed by OfxEffectInstance so the pointer to the live instance
     * is set earlier.
     */
    void setEffect(const EffectInstancePtr& liveInstance);

    EffectInstancePtr getEffectInstance() const;

    /**
     * @brief Returns true if the node is a multi-instance node, that is, holding several other nodes.
     * e.g: the Tracker node.
     **/
    bool isMultiInstance() const;

    NodePtr getParentMultiInstance() const;

    ///Accessed by the serialization thread, but mt safe since never changed
    std::string getParentMultiInstanceName() const;

    void getChildrenMultiInstance(NodesList* children) const;

    /**
     * @brief The key the node's cached results are stored under: its content hash, so nodes in
     * the same state share their cache entries whatever their name, age or project. A hash
     * marked dirty is recomputed first, after the dirty hashes it folds in. MT-safe.
     **/
    U64 getHashValue() const;

    /**
     * @brief Hash of what the node renders: plug-in identity, render-affecting knob values,
     * inputs' content hashes and the project's render settings. Equal state gives an equal
     * hash whatever the node's name or age. Recomputed first if it is dirty. MT-safe.
     **/
    U64 getContentHash() const;

    /**
     * @brief The content hash as last computed, dirty or not: reading it recomputes nothing.
     **/
    U64 getStoredContentHash() const;

    /**
     * @brief Sets hash to the content hash and returns true if it is up to date, and returns false
     * if it is dirty: unlike getHashValue(), it never recomputes, so a caller that only looks at a
     * node does not make an edit rehash it.
     **/
    bool getHashValueIfUpToDate(U64* hash) const;

    /**
     * @brief A count that moves whenever the hash is marked dirty, by an edit of this node or of
     * anything it folds in, after applying the marks queued so far. A result derived from what
     * the hash covers may be stored under the hash only if this count, read before the hash,
     * is the same afterwards: a hash read once cannot tell that an input changed and changed back
     * during the derivation. MT-safe.
     **/
    U64 getHashMarkCount() const;

    /**
     * @brief Marks the hash for a recompute on its next read, and with it the hashes of every
     * node that folds this one in. Callable from any thread, under any lock: the nodes downstream
     * are marked before the next hash read on any thread.
     **/
    void markHashDirty();

    /**
     * @brief Marks every node's hash dirty, for changes to state all of them fold in, such as
     * the project's render settings. Lock-free.
     **/
    static void invalidateAllHashes(const char* reason = "invalidateAllHashes",
                                    const KnobI* knob = 0);

    /**
     * @brief Called with every move of a hash mark count: a node's own mark (`node` the node
     * marked), a mark reaching it from upstream, or a move of the count all nodes share (`node`
     * null). `knob` is the knob whose change marked, when known, and is only valid during the
     * call. Calls are serialized, from whichever thread marks; the trace must not mark. An empty
     * function removes it. For tests.
     **/
    typedef std::function<void(const Node* node, const char* reason, const KnobI* knob)> HashMarkTrace;
    static void setHashMarkTraceForTesting(const HashMarkTrace& trace);

    /**
     * @brief Marks the hash dirty and recomputes it now. Returns true if it changed.
     **/
    bool refreshHashNow();

    /**
     * @brief Called by the effect when the content of knob may have changed; see
     * KnobHolder::onKnobContentChanged().
     **/
    void onKnobContentChanged(const KnobI* knob);

    /**
     * @brief True unless the knobs were last seen without any expression. Cheap.
     **/
    bool mayHaveKnobExpressions() const;

    /**
     * @brief Registers knob's expression at dimension as reading this node's output metadata:
     * this node's hash dependents then include the knob's node, and a change of this node's
     * metadata refreshes the expression. Registered by KnobI::addMetadataDependency().
     **/
    void addMetadataReader(const KnobIPtr& knob, int dimension);
    void removeMetadataReader(const KnobI* knob, int dimension);

    /**
     * @brief The live expression knobs, with their dimension, that read this node's metadata.
     **/
    void getMetadataReaders(std::list<std::pair<KnobIPtr, int>>* readers) const;

    /**
     * @brief True if knob's metadata reads fold into this node's hash: a viewer's knobs all, any
     * other node's render-hashed ones.
     **/
    bool knobMetadataReadsFoldIntoHash(const KnobI& knob) const;

    /**
     * @brief True if this node's hash folds, directly or through other nodes' hashes, node's hash
     * or the content hash of knob's dimension. Either target may be null.
     **/
    bool hashDependsOn(const Node* node, const KnobI* knob, int dimension) const;

    /**
     * @brief Called when this node's hash moved, which its output metadata may follow. Drops the
     * readers' cached expression results and moves the content hashes of those on other nodes
     * now, and has the main thread evaluate them as changed. Callable from any thread.
     **/
    void onMetadataMayHaveChanged();

    /**
     * @brief Called when this node's output metadata was invalidated or refreshed under the same
     * hash. As onMetadataMayHaveChanged(), but moves the metadata revision first, so that the
     * content hashes of all the readers, this node's knobs included, move with it. Every node
     * downstream, whose hash does not move either, has its cached metadata dropped and its
     * revision and readers moved the same way.
     **/
    void onMetadataChangedUnderSameHash();

    /**
     * @brief Moved by onMetadataChangedUnderSameHash() on this node or on one upstream of it; the
     * content hash of an expression reading
     * this node's metadata folds it. Zero until then.
     **/
    U64 getMetadataRevision() const;

    /**
     * @brief True when the project frame range is part of this node's content hash: the effect
     * reads it, or an expression on one of its knobs can.
     **/
    bool dependsOnProjectFrameRange() const;

    virtual std::string getCacheID() const OVERRIDE FINAL;

    /**
     * @brief Forwarded to the live effect instance
     **/
    void beginEditKnobs();

    /**
     * @brief Forwarded to the live effect instance
     **/
    const std::vector<KnobIPtr> & getKnobs() const;

    /**
     * @brief When frozen is true all the knobs of this effect read-only so the user can't interact with it.
     * @brief This function will be called on all input nodes as well
     **/
    void setKnobsFrozen(bool frozen);


    /*Returns in viewers the list of all the viewers connected to this node*/
    void hasViewersConnected(std::list<ViewerInstance* >* viewers) const;

    void hasOutputNodesConnected(std::list<OutputEffectInstance* >* writers) const;

    /**
     * @brief Forwarded to the live effect instance
     **/
    int getMajorVersion() const;

    /**
     * @brief Forwarded to the live effect instance
     **/
    int getMinorVersion() const;


    /**
     * @brief Forwarded to the live effect instance
     **/
    bool isInputNode() const;

    /**
     * @brief Forwarded to the live effect instance
     **/
    bool isOutputNode() const;

    /**
     * @brief Forwarded to the live effect instance
     **/
    bool isOpenFXNode() const;

    /**
     * @brief Returns true if the node is either a roto  node
     **/
    bool isRotoNode() const;

    /**
     * @brief Returns true if this node is a tracker
     **/
    bool isTrackerNodePlugin() const;

    bool isPointTrackerNode() const;

    /**
     * @brief Returns true if this node is a backdrop
     **/
    bool isBackdropNode() const;

    /**
     * @brief Returns true if the node is a rotopaint node
     **/
    bool isRotoPaintingNode() const;

    ViewerInstance* isEffectViewer() const;
    NodeGroup* isEffectGroup() const;

    /**
     * @brief Returns a pointer to the rotoscoping context if the node is in the paint context, otherwise NULL.
     **/
    RotoContextPtr getRotoContext() const;
    TrackerContextPtr getTrackerContext() const;

    U64 getRotoAge() const;

    /**
     * @brief Forwarded to the live effect instance
     **/
    int getNInputs() const;

    /**
     * @brief Returns the data kind (image, deep, scene) effectively produced by this node's output.
     * A node with a concrete (non-polymorphic) declared kind always returns that kind. A polymorphic
     * node's kind comes from its own resolution policy: NativeEffectBase subclasses may supply one,
     * and every other node uses the engine default, resolveStructuralOutputDataKind() below.
     * A node the graph constrains to two different concrete kinds at once is ambiguous: it returns
     * eDataKindPolymorphic and sets *isAmbiguous, so a caller that only wants a value to display
     * reads "no single kind" and has no error to handle, while the connection checks can still
     * refuse to feed an ambiguous producer into an input declaring a concrete kind.
     * The result is cached per node and invalidated automatically on connection changes.
     **/
    DataKindEnum getEffectiveOutputDataKind(bool* isAmbiguous = 0) const WARN_UNUSED_RETURN;

    /**
     * @brief The engine's default data-kind resolution policy, exposed so that
     * NativeEffectBase::resolveOutputDataKind() can delegate to it. Resolves bidirectionally from
     * the current topology alone: the kinds reaching the node's polymorphic-declared inputs, and
     * the kinds its consumers require of it. Nothing about it depends on the order the edges were
     * made, so it survives a save/load round trip unchanged.
     **/
    DataKindEnum resolveStructuralOutputDataKind(bool* isAmbiguous) const WARN_UNUSED_RETURN;

    /**
     * @brief Invalidates the cached result of getEffectiveOutputDataKind() for this node and for
     * every node whose own resolution could depend on it. Resolution is bidirectional, so that is
     * both directions: consumers and inputs alike. Everything derived from the kinds of the nodes
     * it reaches is then brought back up to date: their data-kind diagnostic, and the
     * dataKindChanged() signal the GUI redraws its edges from.
     **/
    void invalidateEffectiveOutputDataKindCache();

    /**
     * @brief Recomputes this node's data-kind diagnostic: an eMessageTypeError persistent message
     * naming every connected input carrying a kind the node cannot handle, or no message at all
     * when every current input is acceptable. An edge that becomes invalid after it was made is
     * never disconnected, so this error state is how the user is told, and clearing it when they
     * fix the graph is the other half of that. Unrelated persistent messages are left alone.
     **/
    void refreshDataKindConflictMessage();

    /**
     * @brief Returns true if the given input supports the given components. If inputNb equals -1
     * then this function will check whether the effect can produce the given components.
     **/
    bool isSupportedComponent(int inputNb, const ImageLayerDesc& comp) const;

    /**
     * @brief Returns the most appropriate components that can be supported by the inputNb.
     * If inputNb equals -1 then this function will check the output components.
     **/
    ImageLayerDesc findClosestSupportedComponents(int inputNb, const ImageLayerDesc& comp) const;
    static ImageLayerDesc findClosestInList(const ImageLayerDesc& comp,
                                            const std::list<ImageLayerDesc>& components,
                                            bool multiPlanar);

    ImageBitDepthEnum getBestSupportedBitDepth() const;
    bool isSupportedBitDepth(ImageBitDepthEnum depth) const;
    ImageBitDepthEnum getClosestSupportedBitDepth(ImageBitDepthEnum depth);

    /**
     * @brief Returns the components and index of the channel to use to produce the mask.
     * None = -1
     * R = 0
     * G = 1
     * B = 2
     * A = 3
     **/
    int getMaskChannel(int inputNb, const std::list<ImageLayerDesc>& availableLayers, ImageLayerDesc* comps) const;

    int isMaskChannelKnob(const KnobI* knob) const;

    /**
     * @brief The channel the colour family's node-level "(Un)premult by" divides the plug-in's
     * source by and multiplies its result back by, as an index into *comps, or -1 for none.
     * The layer is one of availableLayers; a selection the input no longer carries resolves to
     * none, i.e. no (un)premult, rather than to a different layer.
     **/
    int getUnPremultChannel(const std::list<ImageLayerDesc>& availableLayers, ImageLayerDesc* comps) const;

    KnobChannelSelectPtr getUnPremultBySelector() const;

    /**
     * @brief The index, within plane, of the "(Un)premult by" divisor channel when the divisor
     * is plane's own (the classic "unpremult by its own alpha"), or -1 when the divisor belongs
     * to another layer and so is not one of plane's channels. That channel is the divisor, not
     * something to divide, so it is left alone. Colour layouts are matched by channel, so an
     * rgba.R divisor fetched as RGBA skips nothing in an alpha-only plane.
     **/
    static int getUnPremultSkipChannel(const ImageLayerDesc& plane, const ImageLayerDesc& divisorLayer, int divisorChannel);

    /**
     * @brief Returns whether masking is enabled or not
     **/
    bool isMaskEnabled(int inputNb) const;

    /**
     * @brief For every mask input that is connected, enabled and not set to None, and for the
     * "(Un)premult by" selector over a connected source, checks that the KnobChannelSelect value
     * resolves against that input's present layers, then defers to the effect's own
     * EffectInstance::checkExtraChannelsPresent() for any channel wiring the effect owns
     * itself (e.g. Shuffle's mapping). Returns false on the first miss and fills *message,
     * leaving a disconnected input or a None selection silent. Layers are read at the given
     * time and view, those of the render being checked.
     **/
    bool checkSelectedChannelsPresent(double time, ViewIdx view, std::string* message) const;

    /**
     * @brief Same, at the timeline's current frame and view 0, for a check made outside any render.
     **/
    bool checkSelectedChannelsPresent(std::string* message) const;

    /**
     * @brief Returns a pointer to the input Node at index 'index'
     * or NULL if it couldn't find such node.
     * MT-safe
     * This function uses thread-local storage because several render thread can be rendering concurrently
     * and we want the rendering of a frame to have a "snap-shot" of the tree throughout the rendering of the
     * frame.
     *
     * DO NOT CALL THIS ON THE SERIALIZATION THREAD, INSTEAD PREFER USING getInputNames()
     **/
    NodePtr getInput(int index) const;

    /**
     * @brief Returns the input as seen on the gui. This is not necessarily the same as the value returned by getInput.
     **/
    NodePtr getGuiInput(int index) const;

    /**
     * @brief Same as getInput except that it doesn't do group redirections for Inputs/Outputs
     **/
    NodePtr getRealInput(int index) const;

    NodePtr getRealGuiInput(int index) const;

private:

    NodePtr getInputInternal(bool useGuiInput, bool useGroupRedirections, int index) const;

public:

    /**
     * @brief Returns the input index of the node if it is an input of this node, -1 otherwise.
     **/
    int getInputIndex(const Node* node) const;

    /**
     * @brief Returns true if the node is currently executing the onInputChanged handler.
     **/
    bool duringInputChangedAction() const;

    /**
     *@brief Returns the inputs of the node as the Gui just set them.
     * The vector might be different from what getInputs_other_thread() could return.
     * This can only be called by the main thread.
     **/
    const std::vector<NodeWPtr> & getInputs() const WARN_UNUSED_RETURN;
    const std::vector<NodeWPtr> & getGuiInputs() const WARN_UNUSED_RETURN;
    std::vector<NodeWPtr> getInputs_copy() const WARN_UNUSED_RETURN;

    /**
     * @brief Returns the input index of the node n if it exists,
     * -1 otherwise.
     **/
    int inputIndex(const NodePtr& n) const;

    const std::vector<std::string> & getInputLabels() const;
    std::string getInputLabel(int inputNb) const;

    std::string getInputHint(int inputNb) const;

    void setInputLabel(int inputNb, const std::string& label);

    void setInputHint(int inputNb, const std::string& hint);

    bool isInputVisible(int inputNb) const;

    void setInputVisible(int inputNb, bool visible);

    int getInputNumberFromLabel(const std::string& inputLabel) const;

    bool isInputOnlyAlpha(int inputNb) const;

    bool isInputConnected(int inputNb) const;

    bool hasOutputConnected() const;

    bool hasInputConnected() const;

    bool hasOverlay() const;

    bool hasMandatoryInputDisconnected() const;

    bool hasAllInputsConnected() const;

    /**
     * @brief This is used by the auto-connection algorithm.
     * When connecting nodes together this function helps determine
     * on which input it should connect a new node.
     * It returns the first non optional empty input or the first optional
     * empty input if they are all optionals, or -1 if nothing matches the 2 first conditions..
     * if all inputs are connected.
     **/
    virtual int getPreferredInputForConnection()  const;
    virtual int getPreferredInput() const;

    NodePtr getPreferredInputNode() const;

    void setRenderThreadSafety(RenderSafetyEnum safety);
    RenderSafetyEnum getCurrentRenderThreadSafety() const;
    void revertToPluginThreadSafety();

    void setCurrentOpenGLRenderSupport(PluginOpenGLRenderSupport support);
    PluginOpenGLRenderSupport getCurrentOpenGLRenderSupport() const;

    void setCurrentSequentialRenderSupport(SequentialPreferenceEnum support);
    SequentialPreferenceEnum getCurrentSequentialRenderSupport() const;

    void setCurrentCanTransform(bool support);
    bool getCurrentCanTransform() const;

    void setCurrentSupportTiles(bool support);
    bool getCurrentSupportTiles() const;

    void refreshDynamicProperties();

    bool isRenderScaleSupportEnabledForPlugin() const;

    bool isMultiThreadingSupportEnabledForPlugin() const;

    //////////////////////ROTO-PAINT related functionalities//////////////////////
    //////////////////////////////////////////////////////////////////////////////

    void prepareForNextPaintStrokeRender();

    //Used by nodes below the rotopaint tree to optimize the RoI
    void setLastPaintStrokeDataNoRotopaint();
    void invalidateLastPaintStrokeDataNoRotopaint();

    void getPaintStrokeRoD(double time, RectD* bbox) const;
    RectD getPaintStrokeRoD_duringPainting() const;

    bool isLastPaintStrokeBitmapCleared() const;
    void clearLastPaintStrokeRoD();
    void getLastPaintStrokePoints(double time,
                                  unsigned int mipmapLevel,
                                  std::list<std::list<std::pair<Point, double> > >* strokes,
                                  int* strokeIndex) const;
    ImagePtr getOrRenderLastStrokeImage(unsigned int mipmapLevel,
                                        double par,
                                        const ImageLayerDesc& components,
                                        ImageBitDepthEnum depth) const;

    void setWhileCreatingPaintStroke(bool creating);
    bool isDuringPaintStrokeCreation() const;
    ////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////

private:

    int getPreferredInputInternal(bool connected) const;

public:


    /**
     * @brief Returns in 'outputs' a map of all nodes connected to this node
     * where the value of the map is the input index from which these outputs
     * are connected to this node.
     **/
    void getOutputsConnectedToThisNode(std::map<NodePtr, int>* outputs);

    const NodesWList & getOutputs() const;
    const NodesWList & getGuiOutputs() const;
    void getOutputs_mt_safe(NodesWList& outputs) const;

    /**
     * @brief Same as above but enters into subgroups
     **/
    void getOutputsWithGroupRedirection(NodesList& outputs) const;

    /**
     * @brief Same as getOutputsWithGroupRedirection(), appended to outputs.
     **/
    void appendOutputsWithGroupRedirection(std::vector<NodePtr>* outputs) const;

    /**
     * @brief Each input name is appended to the vector, in the same order
     * as they are in the internal inputs vector. Disconnected inputs are
     * represented as empty strings.
     **/
    void getInputNames(std::map<std::string, std::string> & inputNames) const;

    enum CanConnectInputReturnValue {
        eCanConnectInput_ok = 0,
        eCanConnectInput_indexOutOfRange,
        eCanConnectInput_inputAlreadyConnected,
        eCanConnectInput_givenNodeNotConnectable,
        eCanConnectInput_groupHasNoOutput,
        eCanConnectInput_graphCycles,
        eCanConnectInput_differentPars,
        eCanConnectInput_differentFPS,
        eCanConnectInput_multiResNotSupported,
        eCanConnectInput_incompatibleDataKind,
    };

    /**
     * @brief Returns true if a connection is possible for the given input number of the current node
     * to the given input. If the return value is eCanConnectInput_incompatibleDataKind and
     * conflictingNode is non-NULL, it is set to the node whose declared data kind requirement
     * conflicts with the connection (which may be a node other than input, if the conflict is only
     * revealed further downstream through a chain of polymorphic pass-through nodes).
     **/
    Node::CanConnectInputReturnValue canConnectInput(const NodePtr& input, int inputNumber, NodePtr* conflictingNode = 0) const;

    /**
     * @brief The data-kind half of canConnectInput(), factored out so it can also be applied to an
     * edge that already exists (e.g. one just restored from a project) rather than only to a
     * prospective one. Returns eCanConnectInput_incompatibleDataKind (with the same conflictingNode
     * semantics as canConnectInput()) or eCanConnectInput_ok.
     **/
    Node::CanConnectInputReturnValue checkDataKindCompatibility(const NodePtr& input, int inputNumber, NodePtr* conflictingNode = 0) const;

    /**
     * @brief Returns true if what input produces cannot be accepted by this node's declared input
     * kind at inputNumber -- the half of checkDataKindCompatibility() that judges a single edge on
     * its own, without simulating what the connection would mean further downstream. This is what
     * an already-restored graph is judged by: it names the node that is holding an input it cannot
     * handle, which is the node that must show the error.
     **/
    bool isInputDataKindUnacceptable(const NodePtr& input, int inputNumber) const WARN_UNUSED_RETURN;

    /** @brief Adds the node parent to the input inputNumber of the
     * node. Returns true if it succeeded, false otherwise.
     * When returning false, this means an input was already
     * connected for this inputNumber. It should be removed
     * beforehand.
     */
    virtual bool connectInput(const NodePtr& input, int inputNumber);

    bool connectInputBase(const NodePtr& input,
                          int inputNumber)
    {
        return Node::connectInput(input, inputNumber);
    }

    /** @brief Removes the node connected to the input inputNumber of the
     * node. Returns the inputNumber if it could remove it, otherwise returns
       -1.
     */
    virtual int disconnectInput(int inputNumber);

    /** @brief Removes the node input of the
     * node inputs. Returns the inputNumber if it could remove it, otherwise returns
       -1.*/
    virtual int disconnectInput(Node* input);

    /**
     * @brief Same as:
       disconnectInput(inputNumber);
       connectInput(input,inputNumber);
     * Except that it is atomic
     **/
    bool replaceInput(const NodePtr& input, int inputNumber);

    void setNodeGuiPointer(const NodeGuiIPtr& gui);

    NodeGuiIPtr getNodeGui() const;

    bool isSettingsPanelVisible() const;

    bool isSettingsPanelMinimized() const;

    void onOpenGLEnabledKnobChangedOnProject(bool activated);

private:

    bool replaceInputInternal(const NodePtr& input, int inputNumber, bool useGuiValues);

    int disconnectInputInternal(Node* input, bool useGuiInputs);


    bool isSettingsPanelVisibleInternal(std::set<const Node*>& recursionList) const;

public:

    bool isUserSelected() const;

    bool shouldCacheOutput(bool isFrameVaryingOrAnimated, double time, ViewIdx view, int visitsCount) const;

    /**
     * @brief Set the position of the node on the nodegraph. It is stored on the node and, in a
     * GUI session, forwarded to the node's GUI. getPosition() returns the stored value.
     **/
    void setPosition(double x, double y);
    void getPosition(double *x, double *y) const;

    void setSize(double w, double h);
    void getSize(double* w, double* h) const;

    /**
     * @brief Whether the size was ever set, on the node or by its GUI. A node never sized has no
     * layout size to save.
     **/
    bool hasSize() const;

    /**
     * @brief Get the colour of the node as it appears on the nodegraph: the user colour
     * if one was set, otherwise the plug-in category colour. Ignored in background mode,
     * since there is then no nodegraph to display it on.
     **/
    bool getColor(double* r, double *g, double* b) const;
    void setColor(double r, double g, double b);
    bool hasUserColor() const;
    void getUserColor(double* r, double* g, double* b) const;
    void resetColor();

    /**
     * @brief Set the suggested overlay colour, stored on the node and forwarded to the GUI.
     **/
    void setOverlayColor(double r, double g, double b);
    bool hasOverlayColor() const;

    /**
     * @brief The overlay colour held by the node itself, without asking its GUI. Meaningful when
     * hasOverlayColor() is true.
     **/
    void getStoredOverlayColor(double* r, double* g, double* b) const;

    /**
     * @brief Store-only counterparts of the setters above, for the node's GUI to mirror its own
     * changes into. They never call back into the GUI.
     **/
    void setLayoutFromGuiPosition(double x, double y);
    void setLayoutFromGuiSize(double w, double h);
    void setLayoutFromGuiUserColor(bool hasColor, double r, double g, double b);
    void setLayoutFromGuiOverlayColor(bool hasColor, double r, double g, double b);

    std::string getKnobChangedCallback() const;
    std::string getInputChangedCallback() const;

    /**
     * @brief This is used exclusively by nodes in the underlying graph of the implementation of the RotoPaint.
     * Do not use that anywhere else.
     **/
    void attachRotoItem(const RotoDrawableItemPtr& stroke);
    RotoDrawableItemPtr getAttachedRotoItem() const;

protected:

    void runInputChangedCallback(int index);

private:

    /**
     * @brief Adds an output to this node.
     **/
    void connectOutput(bool useGuiValues, const NodePtr& output);

    /** @brief Removes the node output of the
     * node outputs. Returns the outputNumber if it could remove it,
       otherwise returns -1.*/
    int disconnectOutput(bool useGuiValues, const Node* output);

public:

    /**
     * @brief Switches the 2 first inputs that are not a mask, if and only if they have compatible components/bitdepths
     **/
    void switchInput0And1();
    /*============================*/

    /**
     * @brief Forwarded to the live effect instance
     **/
    std::string getPluginID() const;


    /**
     * @brief Forwarded to the live effect instance
     **/
    std::string getPluginLabel() const;

    /**
     * @brief Returns the path to where the resources are stored for this plug-in
     **/
    std::string getPluginResourcesPath() const;

    void getPluginGrouping(std::list<std::string>* grouping) const;

    /**
     * @brief Resolves the NodeCategoryEnum used to colour this node in the node graph.
     * Backdrop is not a category (it keeps its own Settings::getDefaultBackdropColor()):
     * callers must check isBackdropNode() before calling this.
     **/
    NodeCategoryEnum getNodeCategory() const WARN_UNUSED_RETURN;

    /**
     * @brief The domain half of getNodeCategory(): Deep, Native 3D or USD 3D, from the
     * plugin grouping already split into its components. Returns false for any other grouping.
     **/
    static bool domainCategoryFromGrouping(const std::list<std::string>& grouping,
                                           NodeCategoryEnum* category) WARN_UNUSED_RETURN;

    /**
     * @brief The table + keyword heuristic half of getNodeCategory(): maps a plugin's major
     * grouping and label to a category, for plugins that are not a Reader/Writer/Generator
     * and whose grouping is not a domain category. Keywords match whole words of the label.
     **/
    static NodeCategoryEnum categoryFromGroupingAndLabel(const std::string& majorGrouping,
                                                         const std::string& label) WARN_UNUSED_RETURN;

    /**
     * @brief Like getNodeCategory(), but from a Plugin alone, for a menu entry before any node
     * of it exists. Generators can't be detected without an instance, so they fall back to the
     * grouping heuristic instead of the Generator category.
     **/
    static NodeCategoryEnum categoryForPlugin(const Plugin& plugin) WARN_UNUSED_RETURN;

    /**
     * @brief Forwarded to the live effect instance
     **/
    std::string getPluginDescription() const;

    /**
     * @brief Returns the absolute file-path to the plug-in icon.
     **/
    std::string getPluginIconFilePath() const;

    // These functions are gone in Natron 2.2
    std::string getPyPlugID() const;
    std::string getPyPlugLabel() const;
    std::string getPyPlugDescription() const;
    void getPyPlugGrouping(std::list<std::string>* grouping) const;
    std::string getPyPlugIconFilePath() const;
    int getPyPlugMajorVersion() const;

    /*============================*/
    AppInstancePtr getApp() const;

    /* @brief Make this node inactive. It will appear
       as if it was removed from the graph editor
       but the object still lives to allow
       undo/redo operations.
       @param outputsToDisconnect A list of the outputs whose inputs must be disconnected
       @param disconnectAll If set to true the parameter outputsToDisconnect is ignored and all outputs' inputs are disconnected
       @param reconnect If set to true Natron will attempt to re-connect disconnected output to an input of this node
       @param hideGui When true, the node gui will be notified so it gets hidden
     */
    void deactivate(const std::list<NodePtr> & outputsToDisconnect = std::list<NodePtr>(),
                    bool disconnectAll = true,
                    bool reconnect = true,
                    bool hideGui = true,
                    bool triggerRender = true,
                    bool unslaveKnobs = true);


    /* @brief Make this node active. It will appear
       again on the node graph.
       WARNING: this function can only be called
       after a call to deactivate() has been made.
     *
     * @param outputsToRestore Only the outputs specified that were previously connected to the node prior to the call to
     * deactivate() will be reconnected as output to this node.
     * @param restoreAll If true, the parameter outputsToRestore will be ignored.
     */
    void activate(const std::list<NodePtr> & outputsToRestore = std::list<NodePtr>(),
                  bool restoreAll = true,
                  bool triggerRender = true);

    /**
     * @brief Calls deactivate() and then remove the node from the project. The object will be destroyed
     * when the caller releases the reference to this Node
     * @param autoReconnect If set to true, outputs connected to this node will try to connect to the input of this node automatically.
     **/
    void destroyNode(bool blockingDestroy, bool autoReconnect);

private:

    void doDestroyNodeInternalEnd(bool autoReconnect);

public:


    /**
     * @brief Forwarded to the live effect instance
     **/
    KnobIPtr getKnobByName(const std::string & name) const;

    /*@brief The derived class should query this to abort any long process
       in the engine function.*/
    bool aborted() const;

    bool makePreviewByDefault() const;

    void togglePreview();

    bool isPreviewEnabled() const;

    /**
     * @brief Makes a small 8bits preview image of size width x height of format ARGB32.
     * Pre-condition:
     *  - buf has been allocated for the correct amount of memory needed to fill the buffer.
     * Post-condition:
     *  - buf must not be freed or overflown.
     * It will serve as a preview on the node's graphical user interface.
     * This function is called directly by the GUI to display the preview.
     * In order to notify the GUI that you want to refresh the preview, just
     * call refreshPreviewImage(time).
     *
     * The width and height might be modified by the function, so their value can
     * be queried at the end of the function
     **/
    bool makePreviewImage(SequenceTime time, int *width, int *height, unsigned int* buf);

    /**
     * @brief Returns true if the node is currently rendering a preview image.
     **/
    bool isRenderingPreview() const;


    /**
     * @brief Returns true if the node is active for use in the graph editor. MT-safe
     **/
    bool isActivated() const;


    /**
     * @brief Use this function to post a transient message to the user. It will be displayed using
     * a dialog. The message can be of 4 types...
     * INFORMATION_MESSAGE : you just want to inform the user about something.
     * eMessageTypeWarning : you want to inform the user that something important happened.
     * eMessageTypeError : you want to inform the user an error occurred.
     * eMessageTypeQuestion : you want to ask the user about something.
     * The function will return true always except for a message of type eMessageTypeQuestion, in which
     * case the function may return false if the user pressed the 'No' button.
     * @param content The message you want to pass.
     **/
    bool message(MessageTypeEnum type, const std::string & content) const;

    /**
     * @brief Use this function to post a persistent message to the user. It will be displayed on the
     * node's graphical interface and on any connected viewer. The message can be of 3 types...
     * INFORMATION_MESSAGE : you just want to inform the user about something.
     * eMessageTypeWarning : you want to inform the user that something important happened.
     * eMessageTypeError : you want to inform the user an error occurred.
     * @param content The message you want to pass.
     **/
    void setPersistentMessage(MessageTypeEnum type, const std::string & content);

    /**
     * @brief Clears any message posted previously by setPersistentMessage.
     * This function will also be called on all inputs
     **/
    void clearPersistentMessage(bool recurse);

    /**
     * @brief Posts the error message of a failed checkSelectedChannelsPresent(), recording
     * with it that the channel-selector check owns it, so that clearChannelSelectorMessage()
     * can later take it back. Any other message posted since drops that ownership.
     * renderSequence is the AbortableRenderInfo::getRenderSequence() of the failing render, or
     * 0 outside a render. A render older than the latest clear is stale and posts nothing.
     **/
    void setChannelSelectorMessage(const std::string& content, U64 renderSequence = 0);

    /**
     * @brief What a render whose channel check failed does: setChannelSelectorMessage() with the
     * render's sequence number, unless the render was aborted or superseded by a newer request
     * (AbortableRenderInfo::isSuperseded()). That is decided atomically with the post, so a
     * render superseded before refreshChannelSelectorMessageAtTime() looks at the message
     * never posts after it.
     **/
    void setChannelSelectorMessageFromRender(const std::string& content, const AbortableRenderInfoPtr& render);

    /**
     * @brief Clears the persistent message only if it is still the one setChannelSelectorMessage()
     * posted; any other message is left alone. The check and the clear are atomic. Every render
     * created before this call counts as older than it.
     **/
    void clearChannelSelectorMessage();

    /**
     * @brief What a render does once it delivered its image, its channel check having passed:
     * clears the channel-selector message like clearChannelSelectorMessage(), but only if the
     * render was not aborted and is newer (a larger renderSequence) than the render that posted
     * the message. A render finishing late for a frame the user has left must not retire the
     * error of the frame now shown.
     **/
    void clearChannelSelectorMessageFromRender(U64 renderSequence, bool renderAborted);

    /**
     * @brief Clears the channel-selector message if the check now passes at the given time in
     * every one of the given views, the views being displayed. A viewer showing a cached frame
     * renders nothing, so moving the timeline back to a frame that renders must retire the
     * error without waiting for a render.
     **/
    void refreshChannelSelectorMessageAtTime(double time, const std::list<ViewIdx>& views);

    void refreshChannelSelectorMessageAtTime(double time, ViewIdx view);

    /**
     * @brief Appends every node, in any app, whose persistent message the channel-selector
     * check currently owns.
     **/
    static void getNodesOwningChannelSelectorMessage(NodesList* nodes);

    /**
     * @brief Clears the persistent message before a preview render, except a channel-selector
     * error, which stays until a render or check that passes retires it, and a project-load
     * warning, which no render can retire.
     **/
    void clearPersistentMessageForPreview();

private:
    void postPersistentMessage(MessageTypeEnum type, const std::string& content, bool fromChannelSelector, U64 renderSequence, const AbortableRenderInfo* render);

    // Returns whether the stored message changed.
    bool storePersistentMessage(MessageTypeEnum type, const std::string& content, bool fromChannelSelector, U64 renderSequence, const AbortableRenderInfo* render);

    // Sets persistentMessageFromChannelSelector and keeps getNodesOwningChannelSelectorMessage()
    // in step with it. The caller holds persistentMessageMutex.
    void setChannelSelectorOwnership(bool owned);

    void clearPersistentMessageRecursive(std::list<Node*>& markedNodes);

    void clearPersistentMessageInternal();

public:


    void purgeAllInstancesCaches();

    bool notifyInputNIsRendering(int inputNb);

    void notifyInputNIsFinishedRendering(int inputNb);

    bool notifyRenderingStarted();

    void notifyRenderingEnded();

    int getIsInputNRenderingCounter(int inputNb) const;

    int getIsNodeRenderingCounter() const;

    /**
     * @brief forwarded to the live instance
     **/
    void setOutputFilesForWriter(const std::string & pattern);


    ///called by EffectInstance
    void registerPluginMemory(size_t nBytes);

    ///called by EffectInstance
    void unregisterPluginMemory(size_t nBytes);

    //see eRenderSafetyInstanceSafe in EffectInstance::renderRoI
    //only 1 clone can render at any time
    QMutex & getRenderInstancesSharedMutex();

    void refreshPreviewsRecursivelyDownstream(double time);

    void refreshPreviewsRecursivelyUpstream(double time);

public:
    void onAllKnobsSlaved(bool isSlave, KnobHolder* master);

    void onKnobSlaved(const KnobIPtr& slave, const KnobIPtr& master, int dimension, bool isSlave);

    NodePtr getMasterNode() const;

    /**
     * @brief Attempts to lock an image for render. If it successfully obtained the lock,
     * the thread can continue and render normally. If another thread is currently
     * rendering that image, this function will wait until the image is available for render again.
     * This is used internally by EffectInstance::renderRoI
     **/
    void lock(const ImagePtr& entry);
    bool tryLock(const ImagePtr& entry);
    void unlock(const ImagePtr& entry);


    /**
     * @brief DO NOT EVER USE THIS FUNCTION. This is provided for compatibility with plug-ins that
     * do not respect the OpenFX specification.
     **/
    ImagePtr getImageBeingRendered(double time, unsigned int mipmapLevel, ViewIdx view);

    void beginInputEdition();

    void endInputEdition(bool triggerRender);

    void onInputChanged(int inputNb, bool isInputA = true);

    bool onEffectKnobValueChanged(KnobI* what, ValueChangedReasonEnum reason);

    /**
     * @brief Whether the node is disabled at the timeline's current frame. For the UI only:
     * anything that renders, or answers an action for a render, must use isNodeDisabled(time).
     **/
    bool isNodeDisabled() const;

    /**
     * @brief Whether the node is disabled at `time`, which makes it pass its preferred input through.
     **/
    bool isNodeDisabled(double time) const;

    /**
     * @brief Whether the node is disabled whatever the time: its Disable (or its container's) is
     * on and neither animated nor driven by an expression. A lifetime range never makes this
     * true on its own, since the node is enabled inside that range.
     **/
    bool isNodeDisabledAtAllTimes() const;

    void setNodeDisabled(bool disabled);

    KnobBoolPtr getDisabledKnob() const;

    bool isLifetimeActivated(int *firstFrame, int *lastFrame) const;

    std::string getNodeExtraLabel() const;

    /**
     * @brief Show keyframe markers on the timeline. The signal to refresh the timeline's gui
     * will be emitted only if emitSignal is set to true.
     * Calling this function without calling hideKeyframesFromTimeline() has no effect.
     **/
    void showKeyframesOnTimeline(bool emitSignal);

    /**
     * @brief Hide keyframe markers on the timeline. The signal to refresh the timeline's gui
     * will be emitted only if emitSignal is set to true.
     * Calling this function without calling showKeyframesOnTimeline() has no effect.
     **/
    void hideKeyframesFromTimeline(bool emitSignal);

    bool areKeyframesVisibleOnTimeline() const;

    /**
     * @brief The given label is appended in the node's label but will not be editable
     * by the user from the settings panel.
     * If a custom data tag is found, it will replace any custom data.
     **/
    void replaceCustomDataInlabel(const QString & data);

private:

    void restoreSublabel();

public:

    /**
     * @brief Returns whether this node or one of its inputs (recursively) is marked as
     * eSequentialPreferenceOnlySequential
     *
     * @param nodeName If the return value is true, this will be set to the name of the node
     * which is sequential.
     *
     **/
    bool hasSequentialOnlyNodeUpstream(std::string & nodeName) const;


    /**
     * @brief Updates the sub label knob: e.g for the Merge node it corresponds to the
     * operation name currently used and visible on the node
     **/
    void updateEffectLabelKnob(const QString & name);

    /**
     * @brief Returns true if an effect should be able to connect this node.
     **/
    bool canOthersConnectToThisNode() const;

    /**
     * @brief Clears any pointer referring to the last rendered image
     **/
    void clearLastRenderedImage();

    struct KnobLink
    {
        ///The knob being slaved
        KnobIWPtr slave;
        KnobIWPtr master;

        ///The master node to which the knob is slaved to
        NodeWPtr masterNode;

        ///The dimension being slaved, -1 if irrelevant
        int dimension;
    };

    void getKnobsLinks(std::list<KnobLink> & links) const;

    /*Initialises inputs*/
    void initializeInputs();

    /**
     * @brief Forwarded to the live effect instance
     **/
    void initializeKnobs(bool loadingSerialization);

    void findPluginFormatKnobs();

private:

    void initializeDefaultKnobs(bool loadingSerialization);

    void findPluginFormatKnobs(const KnobsVec & knobs, bool loadingSerialization);

    void findRightClickMenuKnob(const KnobsVec& knobs);

    void createNodePage(const KnobPagePtr& settingsPage);

    void createInfoPage();

    void createHostMixKnob(const KnobPagePtr& mainPage);

    void createUnPremultSelector(const KnobPagePtr& mainPage);

    void createStageFooterSeparator(const KnobPagePtr& mainPage);

    void createMaskSelectors(const std::vector<std::pair<bool, bool> >& hasMaskChannelSelector,
                             const std::vector<std::string>& inputLabels,
                             const KnobPagePtr& mainPage,
                             bool addNewLineOnLastMask,
                             KnobIPtr* lastKnobCreated);

    KnobPagePtr getOrCreateMainPage();

    void createLabelKnob(const KnobPagePtr& settingsPage, const std::string& label);

    void createLayerKnob(const LayerKnobSpec& spec, const KnobPagePtr& mainPage);

    void adoptChannelQuad();

    void refreshGeneratorOutputComponentsKnob();

public:
    void onSetSupportRenderScaleMaybeSet(int support);

    bool useScaleOneImagesWhenRenderScaleSupportIsDisabled() const;

    /**
     * @brief Fills keyframes with all different keyframes time that all parameters of this
     * node have. Some keyframes might appear several times.
     **/
    void getAllKnobsKeyframes(std::list<SequenceTime>* keyframes);

    bool hasAnimatedKnob() const;


    void setNodeIsRendering(std::list<NodeWPtr>& nodes);
    void unsetNodeIsRendering();

    /**
     * @brief Returns true if the parallel render args thread-storage is set
     **/
    bool isNodeRendering() const;

    bool hasPersistentMessage() const;

    bool hasAnyPersistentMessage() const;

    /**
     * @brief The persistent message. A missing-channel message is first rechecked by
     * refreshInputRelatedDataIfStale(), which may recompute dirty hashes, so the caller must not
     * hold a lock it takes.
     **/
    void getPersistentMessage(QString* message, int* type, bool prefixLabelAndType = true) const;

    /**
     * @brief Appends to nodes, once each and nearest first, every node in roots or upstream of
     * them (through every input, into and out of groups) that has a persistent message.
     **/
    static void getNodesWithPersistentMessageUpstream(const NodesList& roots, NodesList* nodes);

    /**
     * @brief Attempts to detect cycles considering input being an input of this node.
     * Returns true if it couldn't detect any cycle, false otherwise.
     **/
    bool checkIfConnectingInputIsOk(Node* input) const;

    bool isForceCachingEnabled() const;


    /**
     * @brief Declares to Python all parameters as attribute of the variable representing this node.
     **/
    void declarePythonFields();

private:
    /**
     * @brief Declares to Python all parameters, roto, tracking attributes
     * This is called in activate() whenever the node was deleted
     **/
    void declareAllPythonAttributes();

public:
    /**
     * @brief Set the node name.
     * Throws a run-time error with the message in case of error
     **/
    void setScriptName(const std::string & name);

    void setScriptName_no_error_check(const std::string & name);


    /**
     * @brief The node unique name.
     **/
    const std::string & getScriptName() const;
    std::string getScriptName_mt_safe() const;

    /**
       @brief Returns the name of the node, prepended by the name of all the group containing it, e.g:
     * - a node in the "root" project would be: Blur1
     * - a node within the group 1 of the project would be : <g>group1</g>Blur1
     * - a node within the group 1 of the group 1 of the project would be : <g>group1</g><g>group1</g>Blur1
     **/
    std::string getFullyQualifiedName() const;

    void setLabel(const std::string& label);

    const std::string& getLabel() const;
    std::string getLabel_mt_safe() const;
    std::string getBeforeRenderCallback() const;
    std::string getBeforeFrameRenderCallback() const;
    std::string getAfterRenderCallback() const;
    std::string getAfterFrameRenderCallback() const;
    std::string getAfterNodeCreatedCallback() const;
    std::string getBeforeNodeRemovalCallback() const;

    void onFileNameParameterChanged(KnobI* fileKnob);

    static void getOriginalFrameRangeForReader(const std::string& pluginID, const std::string& canonicalFileName, int* firstFrame, int* lastFrame);

    void computeFrameRangeForReader(KnobI* fileKnob);

    /**
     * @brief The overlay colour the GUI shows if there is one, otherwise the stored one.
     * Returns false if there is none.
     **/
    bool getOverlayColor(double* r, double* g, double* b) const;

    bool canHandleRenderScaleForOverlays() const;

    /**
     * @brief Push a new undo command to the undo/redo stack associated to this node.
     * The stack takes ownership of the shared pointer, so you should not hold a strong reference to the passed pointer.
     * If no undo/redo stack is present, the command will just be redone once then destroyed.
     **/
    void pushUndoCommand(const UndoCommandPtr& command);


    /**
     * @brief Set the cursor to be one of the default cursor.
     * @returns True if it successfully set the cursor, false otherwise.
     * Note: this can only be called during an overlay interact action.
     **/
    void setCurrentCursor(CursorEnum defaultCursor);
    bool setCurrentCursor(const QString& customCursorFilePath);

    bool shouldDrawOverlay() const;


    void drawHostOverlay(double time,
                         const RenderScale& renderScale,
                         ViewIdx view);

    bool onOverlayPenDownDefault(double time,
                                 const RenderScale& renderScale,
                                 ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure) WARN_UNUSED_RETURN;

    bool onOverlayPenDoubleClickedDefault(double time,
                                          const RenderScale& renderScale,
                                          ViewIdx view, const QPointF & viewportPos, const QPointF & pos) WARN_UNUSED_RETURN;


    bool onOverlayPenMotionDefault(double time,
                                   const RenderScale& renderScale,
                                   ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure) WARN_UNUSED_RETURN;

    bool onOverlayPenUpDefault(double time,
                               const RenderScale& renderScale,
                               ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure) WARN_UNUSED_RETURN;

    bool onOverlayKeyDownDefault(double time,
                                 const RenderScale& renderScale,
                                 ViewIdx view, Key key, KeyboardModifiers modifiers) WARN_UNUSED_RETURN;

    bool onOverlayKeyUpDefault(double time,
                               const RenderScale& renderScale,
                               ViewIdx view, Key key, KeyboardModifiers modifiers) WARN_UNUSED_RETURN;

    bool onOverlayKeyRepeatDefault(double time,
                                   const RenderScale& renderScale,
                                   ViewIdx view, Key key, KeyboardModifiers modifiers) WARN_UNUSED_RETURN;

    bool onOverlayFocusGainedDefault(double time,
                                     const RenderScale& renderScale,
                                     ViewIdx view) WARN_UNUSED_RETURN;

    bool onOverlayFocusLostDefault(double time,
                                   const RenderScale& renderScale,
                                   ViewIdx view) WARN_UNUSED_RETURN;

    void addPositionInteract(const KnobDoublePtr& position,
                             const KnobBoolPtr& interactive);

    void addRectangleInteract(const KnobDoublePtr& bottomLeft,
                              const KnobDoublePtr& size,
                              const KnobBoolPtr& interactive,
                              const KnobBoolPtr& enable);

    /**
     * @brief Host overlays declared before the node has a GUI; they are handed to the GUI by
     * initializeHostOverlays().
     **/
    std::list<HostOverlayKnobsPtr> getPendingHostOverlays() const;

    void addTransformInteract(const KnobDoublePtr& translate,
                              const KnobDoublePtr& scale,
                              const KnobBoolPtr& scaleUniform,
                              const KnobDoublePtr& rotate,
                              const KnobDoublePtr& skewX,
                              const KnobDoublePtr& skewY,
                              const KnobChoicePtr& skewOrder,
                              const KnobDoublePtr& center,
                              const KnobBoolPtr& invert,
                              const KnobBoolPtr& interactive);

    void addCornerPinInteract(const KnobDoublePtr& from1,
                              const KnobDoublePtr& from2,
                              const KnobDoublePtr& from3,
                              const KnobDoublePtr& from4,
                              const KnobDoublePtr& to1,
                              const KnobDoublePtr& to2,
                              const KnobDoublePtr& to3,
                              const KnobDoublePtr& to4,
                              const KnobBoolPtr& enable1,
                              const KnobBoolPtr& enable2,
                              const KnobBoolPtr& enable3,
                              const KnobBoolPtr& enable4,
                              const KnobChoicePtr& overlayPoints,
                              const KnobBoolPtr& invert,
                              const KnobBoolPtr& interactive);

    void removePositionHostOverlay(KnobI* knob);

    void initializeHostOverlays();

    bool hasHostOverlay() const;

    void setCurrentViewportForHostOverlays(OverlaySupport* viewPort);

    bool hasHostOverlayForParam(const KnobI* knob) const;

    void setPluginIDAndVersionForGui(const std::list<std::string>& grouping,
                                     const std::string& pluginLabel,
                                     const std::string& pluginID,
                                     const std::string& pluginDesc,
                                     const std::string& pluginIconFilePath,
                                     unsigned int version);

    void setPluginPythonModule(const std::string& pythonModule);

    bool hasPyPlugBeenEdited() const;
    void setPyPlugEdited(bool edited);

    bool isPyPlug() const;

    std::string getPluginPythonModule() const;

    /**
     * @brief Tells the effect and the layer/channel knob GUIs that the layers present on the
     * inputs may have changed; the knobs list their layers themselves at display time.
     **/
    void refreshChannelSelectors();

    /**
     * @brief Emits layerListRefreshed() and nothing else, so the layer/channel knob GUIs relist
     * at the timeline's current frame. An input's layers can differ from one frame to the next,
     * so a list built at another frame can lack layers the input carries now.
     **/
    void relistLayerKnobs();

    // True for the handful of plug-ins (see adoptChannelQuad()) whose R/G/B/A quad the host
    // does not adopt as a per-channel mask: their quad stays visible and the layer knob's
    // row-0 channel buttons are ignored (the host treats every plane as fully processed).
    bool pluginOwnsChannelMask() const;

    /**
     * @brief False when the node's layer knob resolves to no channel at the given time and
     * view, which makes the node an identity of its preferred input. A node without a layer
     * knob always has something to process.
     **/
    bool hasAtLeastOneChannelToProcess(double time, ViewIdx view) const;

    void removeParameterFromPython(const std::string& parameterName);

    double getHostMixingValue(double time, ViewIdx view) const;

    void removeAllImagesFromCache(bool blocking);

    bool isDraftModeUsed() const;
    bool isInputRelatedDataDirty() const;

    /**
     * @brief Refreshes the input-related data of this node and of every node downstream of it
     * now, which reads all their hashes. For explicit refreshes of whole trees; an edit calls
     * refreshInputRelatedDataAfterInputChange() instead.
     **/
    void forceRefreshAllInputRelatedData();

    /**
     * @brief What an edit of this node's inputs refreshes: this node's input-related data now,
     * after that of its dirty inputs, while the nodes downstream are only marked stale (see
     * markDownstreamInputRelatedDataStale()). Main thread only.
     **/
    void refreshInputRelatedDataAfterInputChange();

    /**
     * @brief Marks the input-related data (metadata, produced layers, channel selectors) of every
     * node downstream of this one stale, reading no hash, so that a branch nothing reads is never
     * rehashed for it. Each node is refreshed by refreshInputRelatedDataIfStale() once it is read;
     * the nodes whose settings panel is visible are shown, and are refreshed now. Main thread only.
     **/
    void markDownstreamInputRelatedDataStale();

    /**
     * @brief Refreshes this node's input-related data, after that of its dirty inputs, if an edit
     * upstream left it stale. A viewer refreshes only the inputs it shows. Reads the hashes of the
     * nodes it refreshes. Callable from any thread, but does nothing off the main thread, inside a
     * hash recompute or another refresh, while the project loads or closes, or while a node tree
     * is being created: those read the data as it stands, and the refresh that follows them
     * covers it.
     **/
    void refreshInputRelatedDataIfStale();

    /**
     * @brief While one lives on the calling thread, refreshInputRelatedDataIfStale() does nothing
     * there. Held by a caller that may sit under a lock a refresh takes in the other order: an
     * OpenFX clip fetching its input's metadata runs under the host's metadata mutex, while a
     * refresh takes an effect's clip preferences lock for writing, which a render holds for
     * reading while it waits on that same metadata mutex.
     **/
    class InputRelatedDataRefreshDeferral {
    public:
        InputRelatedDataRefreshDeferral();
        ~InputRelatedDataRefreshDeferral();

    private:
        InputRelatedDataRefreshDeferral(const InputRelatedDataRefreshDeferral&);
        InputRelatedDataRefreshDeferral& operator=(const InputRelatedDataRefreshDeferral&);
    };

    /**
     * @brief What a render of this node refreshes before it reads metadata and layers:
     * refreshInputRelatedDataIfStale() on every node upstream of it, the paint trees of RotoPaint
     * nodes and the nodes inside groups included, and through a viewer only the inputs it shows. Each node is looked at, not
     * only the stale ones' inputs: a viewer refreshed while it showed other inputs, and a RotoPaint
     * node whose paint tree was edited, are clean above stale nodes. Does nothing off the main
     * thread.
     **/
    void refreshInputRelatedDataForRender();

    bool isInputRelatedDataStale() const;

    /**
     * @brief Has the main thread run refreshInputRelatedDataIfStale() on this node soon, for a
     * reader off the main thread that found it stale. Callable from any thread.
     **/
    void requestInputRelatedDataRefresh();

    void markAllInputRelatedDataDirty();

    /**
     * @brief Fills ids with the layer ID of every layer/channel knob row on this node that
     * currently targets a non-empty selection. This is the single source of truth
     * Project::getLayerUsers() relies on to refuse removing a referenced layer.
     **/
    virtual void getReferencedLayerIDs(std::set<std::string>* ids) const;

    /**
     * @brief The channel set or layer select the host created on this node's main page,
     * or null when getLayerKnobSpec() asked for none.
     **/
    KnobIPtr getLayerKnob() const;

    /**
     * @brief The layers a layer/channel knob of this node may choose from: an input-bound
     * knob lists the present layers of its input (always including Color), a target knob
     * lists the project registry. A knob bound to the preferred input lists the input the
     * effect passes through at that time and view (EffectInstance::getLayersPassThroughInput()),
     * at the time and view it is read at there: a Switch lists the input it routes, a
     * FrameHold its held frame. A knob not declared on this node, such as a group's user
     * param aliased onto an inner node's layer knob, delegates to the node holding that inner
     * knob; the inner knob itself always resolves against its own node.
     **/
    void listLayersForKnob(const KnobIPtr& knob, std::list<ImageLayerDesc>* layers) const;

    /**
     * @brief Same, with an input-bound knob's present layers taken at the given time and view
     * instead of the timeline's current frame.
     **/
    void listLayersForKnob(const KnobIPtr& knob, double time, ViewIdx view, std::list<ImageLayerDesc>* layers) const;

    /**
     * @brief listLayersForKnob() as a user sees it: the colour storage entry is replaced, in
     * place, by the colour views it presents (ImageLayerDesc::expandColorViews()). This is the
     * list for the GUI and Python; engine code keeps to the storage-level listLayersForKnob().
     **/
    void listLayerViewsForKnob(const KnobIPtr& knob, std::list<ImageLayerDesc>* layers) const;

    void listLayerViewsForKnob(const KnobIPtr& knob, double time, ViewIdx view, std::list<ImageLayerDesc>* layers) const;

    /**
     * @brief listLayersForKnob() for a single-channel picker that lists the colour storage
     * entry as the one colour view matching its channel layout
     * (ImageLayerDesc::collapseColorToLayoutView()), because rgba.R, rgb.R alias one channel
     * and must not be offered twice.
     **/
    void listChannelViewsForKnob(const KnobIPtr& knob, std::list<ImageLayerDesc>* layers) const;

    /**
     * @brief Whether listLayersForKnob() lists the project registry for this knob (a target
     * knob) rather than an input's present layers. A group param aliased onto an inner node's
     * layer knob reports the inner knob's role.
     **/
    bool isTargetLayerKnob(const KnobIPtr& knob) const;

    /**
     * @brief Resolves the layer knob (channel set or layer select) against the layers it is
     * bound to at the given time and view. Returns false when the node has no layer knob,
     * in which case selected is left empty.
     **/
    bool resolveLayerKnob(double time, ViewIdx view, std::vector<ResolvedLayer>* selected) const;

    /**
     * @brief Same, for a caller that already has the effect's layers pass-through input and the
     * time and view it is read at (EffectInstance::getLayersPassThroughInput()): a knob bound to
     * the preferred input resolves against that input's present layers there, without asking
     * the effect's isIdentity() again.
     **/
    bool resolveLayerKnob(double time,
                          ViewIdx view,
                          int passThroughInputNb,
                          double passThroughTime,
                          ViewIdx passThroughView,
                          std::vector<ResolvedLayer>* selected) const;

    /**
     * @brief Points the layer knob at one registry layer, every channel, and makes it resolve
     * against the registry from now on whatever its inputs carry. The nodes of a RotoPaint
     * tree take the plane their RotoPaint targets this way: that plane need not exist
     * upstream yet. No-op on a node without a layer knob.
     **/
    void retargetLayerKnob(const std::string& layerID);

    /**
     * @brief Gives a channel set, layer select or channel select knob a listing role: an
     * input-bound knob (kPreferredInput's caller-facing spelling is Node::getPreferredInput(),
     * any other value a literal input number) lists that input's present layers, a target knob
     * lists the project registry. Works the same for a plugin- or node-owned knob as for the
     * host's own layerKnob, and makes listLayersForKnob() and getReferencedLayerIDs() see it.
     **/
    void declareLayerKnob(const KnobIPtr& knob, int inputNb, LayerKnobSpec::RoleEnum role);

    /**
     * @brief Repoints an already-declared input-bound layer/channel knob at another input and
     * emits layerListRefreshed() so its GUI relists. No-op on a knob never declared.
     **/
    void setLayerKnobInput(const KnobIPtr& knob, int inputNb);

    /**
     * @brief Registers every non-Color layer this node produces (per
     * getComponentsNeededAndProduced_public()'s output entry) with the project-level
     * LayerRegistry. Main thread only, called at the end of refreshAllInputRelatedData().
     **/
    void registerProducedLayers();

    const std::vector<std::string>& getCreatedViews() const;

    void refreshCreatedViews(bool silent);

    void refreshIdentityState();

    bool getHideInputsKnobValue() const;
    void setHideInputsKnobValue(bool hidden);

    int getFrameStepKnobValue() const;

    void refreshFormatParamChoice(const std::vector<ChoiceOption>& entries, int defValue, bool loadingProject);

    bool handleFormatKnob(KnobI* knob);

    QString makeDocumentation(bool genHTML) const;

    void refreshPreviewsAfterProjectLoad();

    enum StreamWarningEnum
    {
        //A bitdepth conversion occurs and converts to a lower bitdepth the stream,
        //inducing a quality loss
        eStreamWarningBitdepth,

        //The node has inputs with different aspect ratios but does not handle it
        eStreamWarningPixelAspectRatio,

        //The node has inputs with different frame rates which may produce unwanted results
        eStreamWarningFrameRate,
    };

    void setStreamWarning(StreamWarningEnum warning, const QString& message);
    void setStreamWarnings(const std::map<StreamWarningEnum, QString>& warnings);
    void clearStreamWarning(StreamWarningEnum warning);
    void getStreamWarnings(std::map<StreamWarningEnum, QString>* warnings) const;

private:

    bool setStreamWarningInternal(StreamWarningEnum warning, const QString& message);

    /**
     * @brief One accumulated data-kind constraint: no constraint yet (eDataKindPolymorphic), one
     * concrete kind, or ambiguous -- constrained to more than one concrete kind at once, which is
     * a different thing from being unconstrained.
     **/
    struct DataKindConstraint {
        DataKindEnum kind;
        bool ambiguous;

        explicit DataKindConstraint(DataKindEnum kind_ = eDataKindPolymorphic)
            : kind(kind_)
            , ambiguous(false)
        {
        }

        void merge(const DataKindConstraint& other);
    };

    static DataKindConstraint effectiveDataKindConstraintOf(const NodePtr& node);

    DataKindConstraint resolveDataKindConstraint(int overrideInputNb, const DataKindConstraint& overrideConstraint) const;

    void collectUpstreamDataKindConstraint(int overrideInputNb, const DataKindConstraint& overrideConstraint, DataKindConstraint* constraint) const;

    void collectDownstreamDataKindRequirement(DataKindConstraint* constraint) const;

    bool findDataKindConflictDownstream(const DataKindConstraint& constraint, NodePtr* conflictingNode) const;

    /**
     * @brief For a group node: whether constraint arriving at its input inputNb conflicts with
     * what the GroupInput standing for that input feeds, inside the group and beyond its output.
     **/
    bool findDataKindConflictInsideGroup(int inputNb, const DataKindConstraint& constraint, NodePtr* conflictingNode) const;

    /**
     * @brief The nodes whose hash folds in this one's: outputs through group redirections, and
     * the internal nodes of a roto paint tree.
     **/
    void getHashDependents(std::vector<NodePtr>* dependents) const;

    struct HashInputs {
        bool isViewer = false;
        // By input slot: a viewer's two shown inputs, null when unconnected; for any other node
        // the connected inputs it folds in.
        std::vector<std::pair<int, NodePtr>> bySlot;
        // Other nodes whose hashes the knobs' content hashes fold: those whose metadata the
        // knobs' expressions read, directly or through the knobs they read.
        std::vector<NodePtr> metadataSources;
    };

    /**
     * @brief The nodes whose stored hashes this node's hashes fold in.
     **/
    void getHashInputs(HashInputs* inputs) const;

    /**
     * @brief Appends the other nodes whose hashes this node's hash folds through the metadata
     * read by the expressions of its knobs, skipping any already in sources. The (knob, dimension)
     * pairs walked are added to reached, if not null, and not walked again.
     **/
    void appendMetadataSources(std::vector<NodePtr>* sources, std::set<std::pair<const KnobI*, int>>* reached) const;

    void refreshMetadataReaders(bool sameHash);

    U32 getInputSlotCount() const;

    /**
     * @brief Marks this node, and if includeGroupContents every node inside it when it is a
     * group. Its dependents are marked by the next flushHashDirtyMarks().
     **/
    void markHashDirtyInternal(bool includeGroupContents,
                               const char* reason,
                               const KnobI* knob);

    /**
     * @brief Marks the dependents of every node marked since the last flush. Any hash read
     * calls it first.
     **/
    static void flushHashDirtyMarks();

    /**
     * @brief flushHashDirtyMarks(), except when only reader is marked: none of the marks then
     * reaches a hash reader folds in.
     **/
    static void flushHashDirtyMarksBeforeRead(const Node* reader);

    bool isHashDirty() const;

    /**
     * @brief Recomputes this node's dirty hash and, inputs first, every dirty hash it folds in.
     **/
    void refreshDirtyHashes();

    struct HashWalk;

    /**
     * @brief Recomputes the hashes if they are dirty. With requireCleanInputs, gives up and
     * returns false if a hash it folds in is dirty, which only a concurrent mark or a cycle causes.
     * Without it, folds the dirty hash in but leaves this node dirty.
     **/
    bool recomputeHashIfDirty(bool requireCleanInputs, HashWalk* walk);

    /**
     * @brief Refreshes the node's content hash from its knobs, state and inputs,
     * reading the inputs' stored hashes. Leaves the dirty state and the change hooks to the caller.
     * @param projectDigest The project's content digest; 0 takes a fresh one.
     * @return True if the content hash has changed, false otherwise
     **/
    bool computeHashInternal(U64 projectDigest, const HashInputs& inputs, U64* contentHash) WARN_UNUSED_RETURN;

    U64 computeContentHash(U64 projectDigest, const HashInputs& inputs) const;

    /**
     * @brief Digest of the render-hashed knobs by name, memoized until the effect's knobs content
     * generation moves. hasExpression is set if any knob, hashed or not, has an expression.
     **/
    U64 getKnobsContentDigest(bool* hasExpression) const;

    void refreshCreatedViews(KnobI* knob, bool silent);

    void refreshInputRelatedDataRecursiveInternal(std::set<Node*>& markedNodes);

    void refreshInputRelatedDataRecursive();

    void refreshAllInputRelatedData(bool canChangeValues);

    bool refreshMaskEnabledNess(int inpubNb);

    void markInputRelatedDataDirtyRecursive();

    void markInputRelatedDataDirtyRecursiveInternal(std::list<Node*>& markedNodes, bool recurse);

    bool refreshAllInputRelatedData(bool hasSerializationData, const std::vector<NodeWPtr>& inputs);

    bool refreshInputRelatedDataInternal(bool domarking, std::set<Node*>& markedNodes);

    // Like refreshInputRelatedDataInternal(), except that a viewer only refreshes the inputs it
    // shows: its hash folds in only those, so the others are not read through it.
    bool refreshShownInputRelatedData(std::set<Node*>& visited);

    bool refreshDraftFlagInternal(const std::vector<NodeWPtr>& inputs);

    void setNameInternal(const std::string& name, bool throwErrors);

    std::string getFullyQualifiedNameInternal(const std::string& scriptName) const;

    void s_layerSelectionChanged() { Q_EMIT layerSelectionChanged(); }

public Q_SLOTS:


    void onProcessingQuitInDestroyNodeInternal(int taskID, const GenericWatcherCallerArgsPtr& args);

    void onRefreshIdentityStateRequestReceived();

    void doRefreshEdgesGUI()
    {
        Q_EMIT refreshEdgesGUI();
    }

    /*will force a preview re-computation not matter of the project's preview mode*/
    void computePreviewImage(double time)
    {
        Q_EMIT previewRefreshRequested(time);
    }

    /*will refresh the preview only if the project is in auto-preview mode*/
    void refreshPreviewImage(double time)
    {
        Q_EMIT previewImageChanged(time);
    }

    void onMasterNodeDeactivated();

    void onInputLabelChanged(const QString & name);

    void notifySettingsPanelClosed(bool closed )
    {
        Q_EMIT settingsPanelClosed(closed);
    }

    void dequeueActions();

    void onParentMultiInstanceInputChanged(int input);

Q_SIGNALS:

    void rightClickMenuKnobPopulated();

    void s_refreshPreviewsAfterProjectLoadRequested();

    void hideInputsKnobChanged(bool hidden);

    void refreshIdentityStateRequested();

    void availableViewsChanged();

    void layerSelectionChanged();

    /**
     * @brief Emitted by refreshChannelSelectors() and relistLayerKnobs(): the layer/channel knob
     * GUIs list their layers themselves and only need to know that the input's present layers
     * may have changed.
     **/
    void layerListRefreshed();

    void settingsPanelClosed(bool);

    void persistentMessageChanged();

    /**
     * @brief Emitted for every node whose effective data kind may have changed, which is the whole
     * connected component the change reaches and not only the node whose input was edited: kinds
     * resolve bidirectionally, so an edit downstream can retype nodes upstream of it.
     **/
    void dataKindChanged();

    void inputsInitialized();

    void inputLabelChanged(int, QString);

    void knobsInitialized();

    /*
     * @brief Emitted whenever an input changed on the GUI. Note that at the time this signal is emitted, the value returned by
     * getInput() is not necessarily the same as the value returned by getGuiInput() since the node might still be rendering.
     */
    void inputChanged(int);

    void outputsChanged();

    void activated(bool triggerRender);

    void deactivated(bool triggerRender);

    void canUndoChanged(bool);

    void canRedoChanged(bool);

    void labelChanged(QString);
    void scriptNameChanged(QString);
    void inputEdgeLabelChanged(int, QString);

    void inputVisibilityChanged(int);

    void refreshEdgesGUI();

    void previewImageChanged(double);

    void previewRefreshRequested(double);

    void inputNIsRendering(int inputNb);

    void inputNIsFinishedRendering(int inputNb);

    void renderingStarted();

    void renderingEnded();

    ///how much has just changed, this not the new value but the difference between the new value
    ///and the old value
    void pluginMemoryUsageChanged(qint64 mem);

    void allKnobsSlaved(bool b);

    ///Called when a knob is either slaved or unslaved
    void knobsLinksChanged();

    void knobSlaved();

    void previewKnobToggled();

    void disabledKnobToggled(bool disabled);

    void streamWarningsChanged();
    void nodeExtraLabelChanged(QString);


    void mustDequeueActions();

protected:
    /**
     * @brief Marks the hash of this node, and of every node downstream of it, for a recompute
     * when each is next read. Callable from any thread.
     **/
    void computeHash();

private:


    void declareRotoPythonField();
    void declareTrackerPythonField();

    std::string makeCacheInfo() const;
    std::string makeInfoForInput(int inputNumber) const;

    void setNodeIsRenderingInternal(std::list<NodeWPtr>& markedNodes);


    /**
     * @brief If the node is an input of this node, set ok to true, otherwise
     * calls this function recursively on all inputs.
     **/
    void isNodeUpstream(const Node* input, bool* ok) const;

    void declareNodeVariableToPython(const std::string& nodeName);
    void setNodeVariableToPython(const std::string& oldName, const std::string& newName);
    void deleteNodeVariableToPython(const std::string& nodeName);

    std::unique_ptr<Implementation> _imp;
};

/**
 * @brief An InspectorNode is a type of node that is able to have a dynamic number of inputs.
 * Only 1 input is considered to be the "active" input of the InspectorNode, but several inputs
 * can be connected. This Node is suitable for effects that take only 1 input in parameter.
 * This is used for example by the Viewer, to be able to switch quickly from several inputs
 * while still having 1 input active.
 **/
class InspectorNode
    : public Node
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    InspectorNode(const AppInstancePtr& app,
                  const NodeCollectionPtr& group,
                  Plugin* plugin);

    virtual ~InspectorNode();


    /**
     * @brief Same as connectInputBase but if another input is already connected to 'input' then
     * it will disconnect it prior to connecting the input of the given number.
     **/
    virtual bool connectInput(const NodePtr& input, int inputNumber) OVERRIDE;
    virtual int getPreferredInputForConnection() const OVERRIDE FINAL;
    virtual int getPreferredInput() const OVERRIDE FINAL;

    void refreshActiveInputs(int inputNbChanged, bool isASide);

    void setInputA(int inputNb);

    void setInputB(int inputNb);

    void getActiveInputs(int & a, int &b) const;

    void setActiveInputAndRefresh(int inputNb, bool isASide);

Q_SIGNALS:

    void refreshOptionalState();

    void activeInputsChanged();

private:

    int getPreferredInputInternal(bool connected) const;


    mutable QMutex _activeInputsMutex;
    int _activeInputs[2]; //< indexes of the inputs used for the wipe
};


class RenderingFlagSetter
{
    NodeWPtr node;
    std::list<NodeWPtr> nodes;

public:

    RenderingFlagSetter(const NodePtr& n);

    ~RenderingFlagSetter();
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_NODE_H
