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

#ifndef Engine_RotoItem_h
#define Engine_RotoItem_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
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
#include "Engine/FitCurve.h"
#include "Engine/EngineFwd.h"

#include "Serialization/SerializationFwd.h"

#define kRotoLayerBaseName "Layer"
#define kRotoBezierBaseName "Bezier"
#define kRotoOpenBezierBaseName "Pencil"
#define kRotoEllipseBaseName "Ellipse"
#define kRotoRectangleBaseName "Rectangle"
#define kRotoObjectBaseName "Object"
#define kRotoPaintBrushBaseName "Brush"
#define kRotoPaintEraserBaseName "Eraser"
#define kRotoPaintBlurBaseName "Blur"
#define kRotoPaintSmearBaseName "Smear"
#define kRotoPaintSharpenBaseName "Sharpen"
#define kRotoPaintCloneBaseName "Clone"
#define kRotoPaintRevealBaseName "Reveal"
#define kRotoPaintDodgeBaseName "Dodge"
#define kRotoPaintBurnBaseName "Burn"

NATRON_NAMESPACE_ENTER

class ContentHasher;

struct RotoItemPrivate;
class RotoItem
    : public QObject
    , public std::enable_shared_from_this<RotoItem>
{
public:

    enum SelectionReasonEnum
    {
        eSelectionReasonOverlayInteract = 0, ///when the user presses an interact
        eSelectionReasonSettingsPanel, ///when the user interacts with the settings panel
        eSelectionReasonOther ///when the project loader restores the selection
    };


public:
    // TODO: enable_shared_from_this
    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>

    RotoItem(const RotoCurveContextPtr& context,
             const std::string& name,
             RotoLayerPtr parent = RotoLayerPtr());

public:
    static RotoItemPtr create(const RotoCurveContextPtr& context,
                              const std::string& name,
                              RotoLayerPtr parent = RotoLayerPtr())
    {
        return std::make_shared<RotoItem>(context, name, parent);
    }

    virtual ~RotoItem();

    virtual void clone(const RotoItem*  other);

    ///only callable on the main-thread
    bool setScriptName(const std::string & name);

    std::string getScriptName() const;
    std::string getFullyQualifiedName() const;
    std::string getLabel() const;

    void setLabel(const std::string& label);

    ///only callable on the main-thread
    void setParentLayer(RotoLayerPtr layer);

    ///MT-safe
    RotoLayerPtr getParentLayer() const;

    ///only callable from the main-thread
    void setGloballyActivated(bool a, bool setChildren);

    ///MT-safe
    bool isGloballyActivated() const;

    bool isDeactivatedRecursive() const;

    void setLocked(bool l, bool lockChildren, RotoItem::SelectionReasonEnum reason);
    bool getLocked() const;

    bool isLockedRecursive() const;

    /**
     * @brief Returns at which hierarchy level the item is.
     * The base layer is 0.
     * All items into that base layer are on level 1.
     * etc...
     **/
    int getHierarchyLevel() const;

    /**
     * @brief Fills the parts of the serialization every roto item saves. The derived classes'
     * own toSerialization/fromSerialization call these.
     **/
    void toSerialization(Serialization::RotoItemSerialization* serialization) const;
    void fromSerialization(const Serialization::RotoItemSerialization& serialization);

    /**
     * @brief Returns the name of the node holding this item
     **/
    std::string getRotoNodeName() const;
    RotoCurveContextPtr getContext() const;

    /**
     * @brief The context, when it is a paint context (Roto, RotoPaint). Null on a host whose
     * curves build no render node.
     **/
    RotoContextPtr getPaintContext() const;
    RotoItemPtr getPreviousItemInLayer() const;

    /**
     * @brief Hash of everything this item contributes to a render: its activation, its
     * render-hashed knobs, its geometry and, for a layer, its children in order. Equal states
     * hash equal whatever the item's identity or name. Never 0. MT-safe.
     **/
    U64 getContentHash() const;

    /**
     * @brief Must follow every change to the state appendGeometryContentHash() covers.
     * Lock-free, so it may be called with itemMutex held.
     **/
    void invalidateContentHash();

    /**
     * @brief hook is called with the item each time an item recomputes a memoized part of its
     * content hash. An empty hook removes it.
     **/
    static void setContentRehashedHookForTests(const std::function<void(const RotoItem*)>& hook);

protected:
    void notifyContentRehashedForTests() const;

    /**
     * @brief Appends the state only the roto API mutates (control points, stroke curves...).
     * Memoized by getContentHash() until the next invalidateContentHash().
     **/
    virtual void appendGeometryContentHash(ContentHasher& h) const;

    /**
     * @brief Appends the state that can change without passing through
     * invalidateContentHash(): knobs (edited, animated or slaved through the knob API) and
     * children. Called by every getContentHash(); what it reads is memoized at its own level.
     **/
    virtual void appendLiveContentHash(ContentHasher& h) const;

    ///This mutex protects every-member this class and the derived class might have.
    ///That is for the RotoItem class:
    ///  - name
    ///  - globallyActivated
    ///  - locked
    ///  - parentLayer
    ///
    ///For the RotoDrawableItem:
    ///  - overlayColor
    ///
    ///For the RotoLayer class:
    ///  - items
    ///
    ///For the Bezier class:
    ///  - points
    ///  - featherPoints
    ///  - finished
    ///  - pointsAtDistance
    ///  - featherPointsAtDistance
    ///  - featherPointsAtDistanceVal
    mutable QMutex itemMutex;

private:

    void setGloballyActivated_recursive(bool a);
    void setLocked_recursive(bool locked, RotoItem::SelectionReasonEnum reason);

    void restoreSavedState(const std::string& name,
                           const std::string& label,
                           bool activated,
                           const std::string& parentLayerName,
                           bool locked);

    std::unique_ptr<RotoItemPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

Q_DECLARE_METATYPE(NATRON_NAMESPACE::RotoItem*);

#endif // Engine_RotoItem_h
