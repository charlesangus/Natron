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

#include "RotoLayer.h"

#include <algorithm> // min, max
#include <sstream>
#include <locale>
#include <limits>
#include <cassert>
#include <stdexcept>

#include <QLineF>
#include <QDebug>

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/ContentHasher.h"
#include "Engine/CoonsRegularization.h"
#include "Engine/FeatherPoint.h"
#include "Engine/Format.h"
#include "Engine/Hash64.h"
#include "Engine/Image.h"
#include "Engine/ImageParams.h"
#include "Engine/Interpolation.h"
#include "Engine/RenderStats.h"
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewerInstance.h"

#define kBlurCImgParamSize "size"
#define kTimeOffsetParamOffset "timeOffset"
#define kFrameHoldParamFirstFrame "firstFrame"

#define kTransformParamTranslate "translate"
#define kTransformParamRotate "rotate"
#define kTransformParamScale "scale"
#define kTransformParamUniform "uniform"
#define kTransformParamSkewX "skewX"
#define kTransformParamSkewY "skewY"
#define kTransformParamSkewOrder "skewOrder"
#define kTransformParamCenter "center"
#define kTransformParamFilter "filter"
#define kTransformParamResetCenter "resetCenter"
#define kTransformParamBlackOutside "black_outside"

//This will enable correct evaluation of beziers
//#define ROTO_USE_MESH_PATTERN_ONLY

// The number of pressure levels is 256 on an old Wacom Graphire 4, and 512 on an entry-level Wacom Bamboo
// 512 should be OK, see:
// http://www.davidrevoy.com/article182/calibrating-wacom-stylus-pressure-on-krita
#define ROTO_PRESSURE_LEVELS 512

#ifndef M_PI
#define M_PI        3.14159265358979323846264338327950288   /* pi             */
#endif

NATRON_NAMESPACE_ENTER

////////////////////////////////////Layer////////////////////////////////////

RotoLayer::RotoLayer(const RotoCurveContextPtr& context,
                     const std::string& n,
                     const RotoLayerPtr& parent)
    : RotoItem(context, n, parent)
    , _imp(new RotoLayerPrivate())
{
}

RotoLayer::~RotoLayer()
{
}

void
RotoLayer::clone(const RotoItem* other)
{
    RotoItem::clone(other);
    const RotoLayer* isOtherLayer = dynamic_cast<const RotoLayer*>(other);

    if (!isOtherLayer) {
        return;
    }
    RotoLayerPtr this_shared = std::dynamic_pointer_cast<RotoLayer>( shared_from_this() );
    assert(this_shared);

    // Built without itemMutex held: creating and attaching a copy's nodes hashes them, and that
    // reads whether this layer is activated, which takes itemMutex.
    RotoItems copies;
    std::list<RotoLayerPtr> layers;
    const RotoItems otherItems = isOtherLayer->getItems_mt_safe();
    for (RotoItems::const_iterator it = otherItems.begin(); it != otherItems.end(); ++it) {
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        RotoStrokeItemPtr isStroke = std::dynamic_pointer_cast<RotoStrokeItem>(*it);
        if (isBezier) {
            // clone() needs the copy owned by a shared_ptr, so it runs after construction.
            BezierPtr copy(new Bezier(getContext(), isBezier->getScriptName(), RotoLayerPtr(), false));
            getContext()->onItemCreated(copy, true);
            copy->clone(isBezier.get());
            copy->setParentLayer(this_shared);
            copies.push_back(copy);
        } else if (isStroke) {
            RotoStrokeItemPtr copy(new RotoStrokeItem(isStroke->getBrushType(), getContext(), isStroke->getScriptName(), RotoLayerPtr()));
            getContext()->onItemCreated(copy, true);
            copy->clone(isStroke.get());
            copy->setParentLayer(this_shared);
            copies.push_back(copy);
        } else {
            assert(isLayer);
            if (isLayer) {
                // clone() needs the copy owned by a shared_ptr, so it runs after construction.
                RotoLayerPtr copy(new RotoLayer(getContext(), isLayer->getScriptName(), RotoLayerPtr()));
                copy->clone(isLayer.get());
                copy->setParentLayer(this_shared);
                copies.push_back(copy);
                layers.push_back(copy);
            }
        }
    }

    {
        QMutexLocker l(&itemMutex);
        _imp->items.swap(copies);
    }
    for (std::list<RotoLayerPtr>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        getContext()->addLayer(*it);
    }
    // The children are hashed in order.
    invalidateContentHash();
}

void
RotoLayer::addItem(const RotoItemPtr & item,
                   bool declareToPython )
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    RotoLayerPtr parentLayer = item->getParentLayer();
    if (parentLayer) {
        parentLayer->removeItem(item);
    }

    item->setParentLayer( std::dynamic_pointer_cast<RotoLayer>( shared_from_this() ) );
    {
        QMutexLocker l(&itemMutex);
        _imp->items.push_back(item);
    }
    if (declareToPython) {
        getContext()->declareItemAsPythonField(item);
    }
    // The children are hashed in order.
    invalidateContentHash();
    getContext()->onStructureChanged();
}

void
RotoLayer::insertItem(const RotoItemPtr & item,
                      int index)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(index >= 0);

    RotoLayerPtr parentLayer = item->getParentLayer();
    if ( parentLayer && (parentLayer.get() != this) ) {
        parentLayer->removeItem(item);
    }


    {
        QMutexLocker l(&itemMutex);
        if (parentLayer.get() != this) {
            item->setParentLayer( std::dynamic_pointer_cast<RotoLayer>( shared_from_this() ) );
        } else {
            RotoItems::iterator found = std::find(_imp->items.begin(), _imp->items.end(), item);
            if ( found != _imp->items.end() ) {
                _imp->items.erase(found);
            }
        }
        RotoItems::iterator it = _imp->items.begin();
        if ( index >= (int)_imp->items.size() ) {
            it = _imp->items.end();
        } else {
            std::advance(it, index);
        }
        ///insert before the iterator
        _imp->items.insert(it, item);
    }
    invalidateContentHash();
    getContext()->declareItemAsPythonField(item);
    getContext()->onStructureChanged();
}

void
RotoLayer::removeItem(const RotoItemPtr& item)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        QMutexLocker l(&itemMutex);
        for (RotoItems::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it) {
            if (*it == item) {
                l.unlock();
                getContext()->removeItemAsPythonField(item);
                l.relock();
                _imp->items.erase(it);
                break;
            }
        }
    }
    item->setParentLayer( RotoLayerPtr() );
    invalidateContentHash();
    RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>( item.get() );
    if (isStroke) {
        isStroke->disconnectNodes();
    }
    getContext()->onStructureChanged();
}

int
RotoLayer::getChildIndex(const RotoItemPtr & item) const
{
    QMutexLocker l(&itemMutex);
    int i = 0;

    for (RotoItems::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it, ++i) {
        if (*it == item) {
            return i;
        }
    }

    return -1;
}

const RotoItems &
RotoLayer::getItems() const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->items;
}

RotoItems
RotoLayer::getItems_mt_safe() const
{
    QMutexLocker l(&itemMutex);

    return _imp->items;
}

void
RotoLayer::appendLiveContentHash(ContentHasher& h) const
{
    RotoItem::appendLiveContentHash(h);

    // Children render in reverse list order, so the order is part of the content.
    RotoItems items = getItems_mt_safe();
    h.appendU64(items.size());
    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        h.appendU64((*it)->getContentHash());
    }
}

NATRON_NAMESPACE_EXIT
