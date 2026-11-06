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

#ifndef Serialization_SerializationFwd_h
#define Serialization_SerializationFwd_h

#include "Global/Macros.h"
// Global/Macros.h only defines the NATRON_NAMESPACE_* macros and compiler
// portability macros; it pulls in no Engine, Gui, Qt, Python or boost
// headers, so including it here does not break the plain-data boundary.

#include <memory>

NATRON_NAMESPACE_ENTER
namespace Serialization {
struct ApplicationWindowSerialization;
struct BezierCPSerialization;
struct BezierSerialization;
struct CurveSerialization;
struct GroupKnobChildSerialization;
struct GroupKnobSerialization;
struct GuiLayoutSerialization;
struct KeyFrameSerialization;
struct KnobDimensionSerialization;
struct KnobDriverSerialization;
struct KnobMasterSerialization;
struct KnobSerialization;
struct LayerRegistryEntrySerialization;
struct LayerSerialization;
struct NodeCollectionSerialization;
struct NodeSerialization;
struct PaneLayoutSerialization;
struct ProjectSavedByVersionSerialization;
struct ProjectSerialization;
struct RotoContextSerialization;
struct RotoDrawableItemSerialization;
struct RotoItemSerialization;
struct RotoLayerChildSerialization;
struct RotoLayerSerialization;
struct RotoStrokeCurvesSerialization;
struct RotoStrokeItemSerialization;
struct SplitterChildSerialization;
struct SplitterSerialization;
struct TrackSerialization;
struct TrackerContextSerialization;
class ZipOutput;

typedef std::shared_ptr<NodeSerialization> NodeSerializationPtr;
}
NATRON_NAMESPACE_EXIT

#endif // Serialization_SerializationFwd_h
