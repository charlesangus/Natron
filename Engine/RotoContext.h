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

#ifndef Engine_RotoContext_h
#define Engine_RotoContext_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
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
#include "Engine/FitCurve.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoItem.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

//Small RAII class that properly destroys the cairo image upon destruction
class CairoImageWrapper
{
public:

    CairoImageWrapper()
        : cairoImg(0)
        , ctx(0)
    {
    }

    CairoImageWrapper(cairo_surface_t* img,
                      cairo_t* context)
        : cairoImg(img)
        , ctx(context)
    {
    }

    ~CairoImageWrapper();

    cairo_surface_t* cairoImg;
    cairo_t* ctx;
};

/**
 * @class The curve context of Roto and RotoPaint: on top of the curves, it holds the strokes, the
 * Stroke, Clone and Transform pages, and the internal tree of nodes that renders the items.
 **/
class RotoContext
    : public RotoCurveContext {
    Q_OBJECT

    struct MakeSharedEnabler;

    RotoContext(const NodePtr& node);

public:
    static RotoContextPtr create(const NodePtr& node);

    virtual ~RotoContext();

    RotoStrokeItemPtr makeStroke(RotoStrokeType type,
                                 const std::string& baseName,
                                 bool clearSel);

    virtual void onItemCreated(const RotoDrawableItemPtr& item, bool connectNodes) OVERRIDE;

    virtual void onStructureChanged() OVERRIDE;

    /**
     * @brief Returns true if  all items have the same compositing operator and there are only strokes or bezier (because they
     * are not masked)
     **/
    bool isRotoPaintTreeConcatenatable() const;

    static bool isRotoPaintTreeConcatenatableInternal(const std::list<RotoDrawableItemPtr>& items, int* blendingMode);

    void clearViewersLastRenderedStrokes();

    void resetTransformCenter();

    void resetCloneTransformCenter();

    void resetTransformsCenter(bool doClone, bool doTransform);

    void resetTransform();

    void resetCloneTransform();

private:

    void resetTransformInternal(const KnobDoublePtr& translate,
                                const KnobDoublePtr& scale,
                                const KnobDoublePtr& center,
                                const KnobDoublePtr& rotate,
                                const KnobDoublePtr& skewX,
                                const KnobDoublePtr& skewY,
                                const KnobBoolPtr& scaleUniform,
                                const KnobChoicePtr& skewOrder,
                                const KnobDoublePtr& extraMatrix);

public:
    /**
     * @brief Rebuilds the connection between nodes used internally by the rotopaint tree
     * To be called whenever changes position in the hierarchy or when one gets removed/inserted
     **/
    void refreshRotoPaintTree();

    /**
     * @brief Points the layer knob of every node of the rotopaint tree at the layer the
     * RotoPaint node targets, so the tree renders that plane in place.
     **/
    void retargetRotoPaintTree();

    void onRotoPaintInputChanged(const NodePtr& node);

    void getRotoPaintTreeNodes(NodesList* nodes) const;

    NodePtr getRotoPaintBottomMergeNode() const;

    void setWhileCreatingPaintStrokeOnMergeNodes(bool b);

    /**
     * @brief When finishing a stroke with the paint brush, we need to re-render it because the interpolation of the curve
     * will be much smoother with more points than what it was during painting.
     * We explicitly freeze the UI while waiting for the image to be drawn, otherwise the user might attempt to do a
     * multiple stroke on top of it which could make some artifacts.
     **/
    void evaluateNeatStrokeRender();


    bool mustDoNeatRender() const;

    void setIsDoingNeatRender(bool doing);

    bool isDoingNeatRender() const;

    void s_breakMultiStroke() { Q_EMIT breakMultiStroke(); }

    bool knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     ViewSpec view,
                     double time,
                     bool originatedFromMainThread);

    static bool allocateAndRenderSingleDotStroke(int brushSizePixel, double brushHardness, double alpha, CairoImageWrapper& wrapper);

Q_SIGNALS:

    /**
     * @brief Signalled when the gui should force a new stroke to be added
     **/
    void breakMultiStroke();

private:
    NodePtr getOrCreateGlobalMergeNode(int* availableInputIndex);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoContext_h
