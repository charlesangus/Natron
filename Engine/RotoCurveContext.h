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
#include <memory>
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

public:
    /**
     * @brief The knobs of the Transform page, which edit the selected items' transforms like the
     * other context knobs. Members are null until createTransformKnobs() is called.
     **/
    struct TransformKnobs {
        KnobDoublePtr translate;
        KnobDoublePtr rotate;
        KnobDoublePtr scale;
        KnobBoolPtr scaleUniform;
        KnobDoublePtr skewX;
        KnobDoublePtr skewY;
        KnobChoicePtr skewOrder;
        KnobDoublePtr center;
        KnobDoublePtr extraMatrix;
        KnobBoolPtr interactive;
        KnobButtonPtr resetCenter;
        KnobButtonPtr resetTransform;
    };

    /**
     * @brief Creates the Transform page's knobs on page. The paint context makes them itself; any
     * other host calls this once, from its initializeKnobs().
     **/
    void createTransformKnobs(const KnobPagePtr& page);
    TransformKnobs getTransformKnobs() const;
    bool isTransformKnob(const KnobI* knob) const;

    /**
     * @brief Sets center to getDefaultTransformCenter(), or else to the centre of the selected
     * items' bounding box, at the current time.
     **/
    void resetTransformCenter();

    /**
     * @brief Resets the transform knobs, except Interactive, to their defaults.
     **/
    void resetTransform();

    /**
     * @brief Runs the reset buttons of the Transform page. Returns false when k is not one of them.
     **/
    bool onTransformButtonPressed(const KnobI* k);

    /**
     * @brief The center an item's transform starts at, when the host has one; otherwise the item
     * takes the centre of its bounding box. MT-safe.
     **/
    virtual bool getDefaultTransformCenter(double /*time*/, double* /*x*/, double* /*y*/) const
    {
        return false;
    }

    /**
     * @brief The transform item is drawn and rendered with, from local, the one its own knobs
     * make. The host may replace local or compose it with transforms it holds. MT-safe.
     **/
    virtual void composeItemTransform(const RotoDrawableItem& item, double time, const Transform::Matrix3x3& local, Transform::Matrix3x3* matrix) const;

protected:
    /**
     * @brief False when the Transform page must not edit item's own transform knobs, so selecting
     * the item leaves them untied. See refreshTransformKnobTies().
     **/
    virtual bool tiesTransformKnobs(const RotoDrawableItem& /*item*/) const
    {
        return true;
    }

    /**
     * @brief Ties or unties a selected item's transform knobs after tiesTransformKnobs() changed
     * for it. An item inside a selected layer ends up untied, so edits reach only the layer.
     **/
    void refreshTransformKnobTies(const RotoDrawableItemPtr& item);

    void resetTransformInternal(const KnobDoublePtr& translate,
                                const KnobDoublePtr& scale,
                                const KnobDoublePtr& center,
                                const KnobDoublePtr& rotate,
                                const KnobDoublePtr& skewX,
                                const KnobDoublePtr& skewY,
                                const KnobBoolPtr& scaleUniform,
                                const KnobChoicePtr& skewOrder,
                                const KnobDoublePtr& extraMatrix);

    static void setCenterKnob(const KnobDoublePtr& center, double x, double y);

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

protected:
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
    void selectInternal(const RotoItemPtr& b, bool slaveKnobs = true);
    void deselectInternal(RotoItemPtr b);
    void tieItemKnob(const KnobIPtr& itemKnob, const KnobIPtr& contextKnob, bool slave);
    void untieItemKnob(const KnobIPtr& itemKnob, bool copyState);

    void restoreSavedSelection(const std::list<std::string>& selectedItemNames);

    void removeItemRecursively(const RotoItemPtr& item, RotoItem::SelectionReasonEnum reason);

protected:
    std::unique_ptr<RotoContextPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoCurveContext_h
