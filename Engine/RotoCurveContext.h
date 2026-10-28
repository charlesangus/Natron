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

#ifndef Engine_RotoCurveContext_h
#define Engine_RotoCurveContext_h

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
#include <set>
#include <string>

// clang-format off
CLANG_DIAG_OFF(deprecated-declarations)
#include <QObject>
#include <QMutex>
#include <QMetaType>
CLANG_DIAG_ON(deprecated-declarations)
// clang-format on

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/RotoItem.h"
#include "Engine/ViewIdx.h"

// The mime type of curves on the system clipboard: the text of serializeItems().
#define kRotoCurvesMimeType "application/x-natron-curves"

// #define NATRON_ROTO_INVERTIBLE
// #define NATRON_ROTO_ENABLE_MOTION_BLUR

NATRON_NAMESPACE_ENTER

struct RotoContextPrivate;
class ContentHasher;

/**
 * @brief Host data a subclass keeps about its curves (links, roles), saved by undo commands that
 * edit curves so undoing the edit also restores what the hooks below changed.
 **/
class RotoCurveContextHostState {
public:
    virtual ~RotoCurveContextHostState() { }
};

typedef std::shared_ptr<RotoCurveContextHostState> RotoCurveContextHostStatePtr;

/**
 * @class The curves of a node that hosts Roto shapes: layers, Beziers, their animation, the
 * selection, the feather and the serialization. It builds no render node; RotoContext adds the
 * RotoPaint render tree, the strokes and the paint pages on top of it.
 **/
class RotoCurveContext
    : public QObject,
      public std::enable_shared_from_this<RotoCurveContext> {
    Q_OBJECT

    struct MakeSharedEnabler;

protected:
    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>

    /**
     * @param isPaintNode True when items default to paint behaviour (RotoPaint, not Roto): see
     * isRotoPaint().
     **/
    RotoCurveContext(const NodePtr& node, bool isPaintNode);

public:
    /**
     * @brief Makes a geometry-only context: only the General and Shape pages are created on the node.
     **/
    static RotoCurveContextPtr create(const NodePtr& node);

    virtual ~RotoCurveContext();

    /**
     * @brief We have chosen to disable rotopainting and roto shapes from the same RotoContext because the rendering techniques are
     * very much different. The rotopainting systems requires an entire compositing tree held inside whereas the rotoshapes
     * are rendered and optimized by Cairo internally.
     **/
    bool isRotoPaint() const;

    void createBaseLayer();

    RotoLayerPtr getOrCreateBaseLayer();

    /**
     * @brief Returns true when the context is empty (it has no shapes)
     **/
    bool isEmpty() const;

    /**
     * @brief Turn on/off auto-keying
     **/
    void setAutoKeyingEnabled(bool enabled);
    bool isAutoKeyingEnabled() const;

    /**
     * @brief Turn on/off feather link. When enabled the feather point will move by the same delta as the corresponding spline point
     * is moved by.
     **/
    void setFeatherLinkEnabled(bool enabled);
    bool isFeatherLinkEnabled() const;

    /**
     * @brief Turn on/off ripple edit. When enabled moving a point at a given keyframe will move it to the same position
     * at all keyframes.
     **/
    void setRippleEditEnabled(bool enabled);
    bool isRippleEditEnabled() const;

    int getTimelineCurrentTime() const;

    /**
     * @brief Create a new layer to the currently selected layer.
     **/
    RotoLayerPtr addLayer();

private:
    RotoLayerPtr addLayerInternal(bool declarePython);

public:
    /**
     * @brief Add an existing layer to the layers
     **/
    void addLayer(const RotoLayerPtr& layer);

    /**
     * @brief Make a new bezier curve and append it into the currently selected layer.
     * @param baseName A hint to name the item. It can be something like "Bezier", "Ellipse", "Rectangle" , etc...
     **/
    BezierPtr makeBezier(double x, double y, const std::string& baseName, double time, bool isOpenBezier);
    BezierPtr makeEllipse(double x, double y, double diameter, bool fromCenter, double time);
    BezierPtr makeSquare(double x, double y, double initialSize, double time);
    std::string generateUniqueName(const std::string& baseName);

    /**
     * @brief Called on every drawable item made for this context, by creation, copy or load,
     * before it is used. The paint context builds the item's render nodes here.
     * @param connectNodes False while loading: the render tree is connected once loading ends.
     **/
    virtual void onItemCreated(const RotoDrawableItemPtr& item, bool connectNodes);

    /**
     * @brief Copies a Bezier or stroke of this context under the given name, through onItemCreated(),
     * for the curve panel's paste and duplicate. A Bezier copy is not inserted in any layer; a stroke
     * copy is inserted at the top of the source's layer.
     **/
    RotoDrawableItemPtr makeDrawableCopy(const RotoDrawableItemPtr& source, const std::string& name);

    /**
     * @brief Copies a layer of this context, with all its children, for the curve panel's paste
     * and duplicate. The children's drawables are made through onItemCreated(). The copy keeps the
     * source's names, which the caller makes unique, and is not inserted in any layer.
     **/
    RotoLayerPtr makeLayerCopy(const RotoLayerPtr& source);

    /**
     * @brief Called whenever an item is inserted, removed or moved in the hierarchy, or when
     * its compositing changes. The paint context rebuilds its render tree here.
     **/
    virtual void onStructureChanged();

    /**
     * @brief Called after addControlPointAfterIndex(afterIndex, t) split a segment of curve, on the
     * main thread, outside the curve's lock.
     **/
    virtual void onControlPointInserted(const BezierPtr& /*curve*/,
                                        int /*afterIndex*/,
                                        double /*t*/)
    {
    }

    /**
     * @brief Called by removeControlPointByIndex() before it removes the point at index, on the
     * main thread, outside the curve's lock. index may be out of range, in which case nothing is
     * removed.
     **/
    virtual void onControlPointAboutToBeRemoved(const BezierPtr& /*curve*/,
                                                int /*index*/)
    {
    }

    /**
     * @brief Called after setCurveFinished() opened or closed curve.
     **/
    virtual void onCurveFinishedChanged(const BezierPtr& /*curve*/)
    {
    }

    /**
     * @brief Called for the item and each of its descendants when removeItem() deletes it.
     **/
    virtual void onItemRemoved(const RotoItemPtr& /*item*/)
    {
    }

    /**
     * @brief Returns null when the context keeps no host data.
     **/
    virtual RotoCurveContextHostStatePtr saveHostState() const
    {
        return RotoCurveContextHostStatePtr();
    }

    virtual void restoreHostState(const RotoCurveContextHostStatePtr& /*state*/)
    {
    }

    /**
     * @brief Removes the given item from the context. This also removes the item from the selection
     * if it was selected. If the item has children, this will also remove all the children.
     **/
    void removeItem(const RotoItemPtr& item, RotoItem::SelectionReasonEnum reason = RotoItem::eSelectionReasonOther);

    /// This is here for undo/redo purpose. Do not call this
    void addItem(const RotoLayerPtr& layer, int indexInLayer, const RotoItemPtr& item, RotoItem::SelectionReasonEnum reason);
    /**
     * @brief Returns a const ref to the layers list. This can only be called from
     * the main thread.
     **/
    const std::list<RotoLayerPtr>& getLayers() const;

    /**
     * @brief Returns a bezier curves nearby the point (x,y) and the parametric value
     * which would be used to find the exact Bezier point lying on the curve. When accept is
     * given, curves it refuses are skipped.
     **/
    BezierPtr isNearbyBezier(double x, double y, double acceptance, int* index, double* t, bool* feather,
                             const std::function<bool(const BezierPtr&)>& accept = std::function<bool(const BezierPtr&)>()) const;

    /**
     * @brief Returns the region of definition of the shape unioned to the region of definition of the node
     * or the project format.
     **/
    void getMaskRegionOfDefinition(double time,
                                   ViewIdx view,
                                   RectD* rod) const; //!< rod in canonical coordinates

    void getGlobalMotionBlurSettings(const double time,
                                     double* startTime,
                                     double* endTime,
                                     double* timeStep) const;

protected:
    void getItemsRegionOfDefinition(const std::list<RotoItemPtr>& items, double time, ViewIdx view, RectD* rod) const;

public:
    /**
     * @brief To be called when a change was made to trigger a new render.
     **/
    void evaluateChange();
    void evaluateChange_noIncrement();

    void incrementAge();

    /**
     *@brief Returns the age of the roto context
     **/
    U64 getAge();

    /**
     * @brief Hash of every item of the context, as rendered: the base layer, which folds its
     * children in order (see RotoItem::getContentHash()). Equal trees hash equal. Never 0.
     * MT-safe.
     **/
    U64 getContentHash() const;

    virtual void toSerialization(Serialization::RotoContextSerialization* serialization) const;

    /**
     * @brief Must be called on a context that holds nothing but its base layer.
     **/
    virtual void fromSerialization(const Serialization::RotoContextSerialization& serialization);

    /**
     * @brief A self-contained text holding items, for pasting into any curve host. Each item is
     * written once: an item inside a layer that is also given comes with that layer, and the base
     * layer stands for its children. Throws std::runtime_error when encoding fails.
     **/
    virtual std::string serializeItems(const std::list<RotoItemPtr>& items) const;

    /**
     * @brief Inserts the items of a serializeItems() text at the top of layer, the base layer when
     * null, in their order, on the main thread. A name already used here gets a "_copy" suffix.
     * Strokes are dropped unless this is a paint host, and counted in droppedStrokes. Track links
     * are resolved by tracker and track names, and stay where they were when those are not found.
     * Returns the inserted items. Throws std::invalid_argument when blob cannot be read.
     **/
    virtual std::list<RotoItemPtr> pasteItems(const std::string& blob,
                                              const RotoLayerPtr& layer,
                                              int* droppedStrokes = nullptr);

    /**
     * @brief How many items pasteItems(blob) would insert at the top level, and in droppedStrokes
     * how many strokes it would drop. Returns 0 when blob cannot be read.
     **/
    int countPastableItems(const std::string& blob, int* droppedStrokes = nullptr) const;

protected:
    void itemsToSerialization(const std::list<RotoItemPtr>& items,
                              Serialization::RotoContextSerialization* serialization) const;

    /**
     * @brief The text of serialization, with its blobs inlined.
     **/
    std::string encodeItems(const Serialization::RotoContextSerialization& serialization) const;

    /**
     * @brief Throws std::invalid_argument when blob cannot be read.
     **/
    static Serialization::RotoContextSerialization decodeItems(const std::string& blob);

    /**
     * @brief The work of pasteItems() on a decoded text, without the render. names maps each
     * pasted item's name in the text to its name here.
     **/
    std::list<RotoItemPtr> pasteSerialization(const Serialization::RotoContextSerialization& serialization,
                                              const RotoLayerPtr& layer,
                                              std::map<std::string, std::string>* names,
                                              int* droppedStrokes);

    /**
     * @brief Appends what a subclass holds beside the items to getContentHash(). MT-safe.
     **/
    virtual void appendHostContentHash(ContentHasher& /*h*/) const
    {
    }

public:
    /**
     * @brief This must be called by the GUI whenever an item is selected. This is recursive for layers.
     **/
    void select(const RotoItemPtr& b, RotoItem::SelectionReasonEnum reason);
    void selectFromLayer(const RotoItemPtr& b, RotoItem::SelectionReasonEnum reason);

    /// for convenience
    void select(const std::list<RotoDrawableItemPtr>& beziers, RotoItem::SelectionReasonEnum reason);
    void select(const std::list<RotoItemPtr>& items, RotoItem::SelectionReasonEnum reason);

    /**
     * @brief This must be called by the GUI whenever an item is deselected. This is recursive for layers.
     **/
    void deselect(const RotoItemPtr& b, RotoItem::SelectionReasonEnum reason);

    /// for convenience
    void deselect(const std::list<BezierPtr>& beziers, RotoItem::SelectionReasonEnum reason);
    void deselect(const std::list<RotoItemPtr>& items, RotoItem::SelectionReasonEnum reason);

    void clearAndSelectPreviousItem(const RotoItemPtr& item, RotoItem::SelectionReasonEnum reason);

    void clearSelection(RotoItem::SelectionReasonEnum reason);

    /// only callable on main-thread
    void setKeyframeOnSelectedCurves();

    /// only callable on main-thread
    void removeKeyframeOnSelectedCurves();

    void removeAnimationOnSelectedCurves();

    /// only callable on main-thread
    void goToPreviousKeyframe();

    /// only callable on main-thread
    void goToNextKeyframe();

    /**
     * @brief Returns a list of the currently selected curves. Can only be called on the main-thread.
     **/
    std::list<RotoDrawableItemPtr> getSelectedCurves() const;

    /**
     * @brief Returns a const ref to the selected items. This can only be called on the main thread.
     **/
    const std::list<RotoItemPtr>& getSelectedItems() const;

    /**
     * @brief Returns a list of all the curves in the order in which they should be rendered.
     * Non-active curves will not be inserted into the list.
     * MT-safe
     **/
    std::list<RotoDrawableItemPtr> getCurvesByRenderOrder(bool onlyActivated = true) const;

    int getNCurves() const;

    NodePtr getNode() const;

    RotoLayerPtr getLayerByName(const std::string& n) const;
    RotoItemPtr getItemByName(const std::string& n) const;
    RotoItemPtr getLastInsertedItem() const;

#ifdef NATRON_ROTO_INVERTIBLE
    KnobBoolPtr getInvertedKnob() const;
#endif

    KnobColorPtr getColorKnob() const;

    KnobChoicePtr getMotionBlurTypeKnob() const;
    RotoItemPtr getLastItemLocked() const;
    RotoLayerPtr getDeepestSelectedLayer() const;

    void onItemLockedChanged(const RotoItemPtr& item, RotoItem::SelectionReasonEnum reason);

    void emitRefreshViewerOverlays();

    /**
     * @brief Tells the panel to re-read the columns the host's RotoCurvePanelHook adds for item,
     * or for every item if item is null.
     **/
    void emitItemPanelColumnsChanged(const RotoItemPtr& item);

    void getBeziersKeyframeTimes(std::list<double>* times) const;

    /**
     * @brief Returns the name of the node holding this item
     **/
    std::string getRotoNodeName() const;

    void onItemGloballyActivatedChanged(const RotoItemPtr& item);
    void onItemScriptNameChanged(const RotoItemPtr& item);
    void onItemLabelChanged(const RotoItemPtr& item);

    void onItemKnobChanged();

    void declarePythonFields();

    void changeItemScriptName(const std::string& oldFullyQualifiedName, const std::string& newFullyQUalifiedName);

    void declareItemAsPythonField(const RotoItemPtr& item);
    void removeItemAsPythonField(const RotoItemPtr& item);

    /**
     * @brief First searches through the selected layer which one is the deepest in the hierarchy.
     * If nothing is found, it searches through the selected items and find the deepest selected item's layer
     **/
    RotoLayerPtr findDeepestSelectedLayer() const;

    void dequeueGuiActions();

    /**
     * @brief Watches the tracks the linked points follow, their trackers and tracker nodes, so
     * edits re-render the node; refreshes the links' state and the warning naming missing
     * tracks. Called whenever links or items change, on the main thread.
     **/
    void refreshTrackLinkListeners();

    /**
     * @brief Points loaded links whose track object is unknown at their tracks, found by the
     * tracker node's fully qualified name and the track's script name. Called once every node
     * of a load or paste exists. Links that do not resolve keep their last centre.
     **/
    void resolveTrackLinks();

Q_SIGNALS:

    /**
     * Emitted when the selection is changed. The integer corresponds to the
     * RotoItem::SelectionReasonEnum enum.
     **/
    void selectionChanged(int);

    void restorationComplete();

    void itemInserted(int, int);

    void itemRemoved(const RotoItemPtr&, int);

    void refreshViewerOverlays();

    void itemLockedChanged(int reason);

    void itemGloballyActivatedChanged(const RotoItemPtr&);
    void itemScriptNameChanged(const RotoItemPtr&);
    void itemLabelChanged(const RotoItemPtr&);
    void itemPanelColumnsChanged(const RotoItemPtr&);

public Q_SLOTS:

    void onAutoKeyingChanged(bool enabled);

    void onFeatherLinkChanged(bool enabled);

    void onRippleEditChanged(bool enabled);

    void onSelectedKnobCurveChanged();

    void onSelectedKnobValueChanged();

    void onLifeTimeKnobValueChanged(ViewSpec, int, int);

private:
    std::list<BezierPtr> getTrackLinkedCurves() const;

    // Refreshes every link's state and the warning; with redraw, also invalidates the linked
    // curves and re-renders.
    void updateTrackLinks(bool redraw);

    void selectInternal(const RotoItemPtr& b, bool slaveKnobs = true);
    void deselectInternal(RotoItemPtr b);

    void restoreSavedSelection(const std::list<std::string>& selectedItemNames);

    void removeItemRecursively(const RotoItemPtr& item, RotoItem::SelectionReasonEnum reason);

protected:
    std::unique_ptr<RotoContextPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoCurveContext_h
