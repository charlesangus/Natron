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

#ifndef Engine_Nodes_Geometry_GeoSdfData_h
#define Engine_Nodes_Geometry_GeoSdfData_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "Engine/Nodes/Geometry/GeoUsdConvert.h"

#include <pxr/usd/sdf/abstractData.h>

NATRON_NAMESPACE_ENTER

/**
 * @brief The scene description of a natrongeo layer: GeoUsdConvert's field-level query, plus
 * the edits made through the layer since.
 *
 * Reads go to the converter, so a partition's arrays are built only when one of its default
 * values is read. Writes are kept in an overlay rather than refused, because SdfLayer edits
 * its current data in place to apply a reload's or a refresh's differences. rebase() swaps in
 * a converter equal to the edited content and drops the overlay. Like SdfData, reads may run
 * concurrently with each other but not with writes or rebase().
 **/
class GeoSdfData
    : public PXR_NS::SdfAbstractData {
public:
    GeoSdfData(std::shared_ptr<const GeoUsdConvert> convert,
               GeoDetailConstPtr source);
    virtual ~GeoSdfData() OVERRIDE;

    /** @brief The converter reads fall through to. */
    const std::shared_ptr<const GeoUsdConvert>& convert() const
    {
        return _convert;
    }

    /** @brief The provider's detail the converter was built from, before any unpacking. */
    const GeoDetailConstPtr& source() const
    {
        return _source;
    }

    bool hasEdits() const;

    /** @brief Reads through @p convert from now on; the caller vouches it equals the current content. */
    void rebase(std::shared_ptr<const GeoUsdConvert> convert,
                GeoDetailConstPtr source);

    virtual bool StreamsData() const OVERRIDE;

    virtual void CreateSpec(const PXR_NS::SdfPath& path,
                            PXR_NS::SdfSpecType specType) OVERRIDE;
    virtual bool HasSpec(const PXR_NS::SdfPath& path) const OVERRIDE;
    virtual void EraseSpec(const PXR_NS::SdfPath& path) OVERRIDE;
    virtual void MoveSpec(const PXR_NS::SdfPath& oldPath,
                          const PXR_NS::SdfPath& newPath) OVERRIDE;
    virtual PXR_NS::SdfSpecType GetSpecType(const PXR_NS::SdfPath& path) const OVERRIDE;

    virtual bool Has(const PXR_NS::SdfPath& path,
                     const PXR_NS::TfToken& fieldName,
                     PXR_NS::SdfAbstractDataValue* value) const OVERRIDE;
    virtual bool Has(const PXR_NS::SdfPath& path,
                     const PXR_NS::TfToken& fieldName,
                     PXR_NS::VtValue* value = NULL) const OVERRIDE;
    virtual PXR_NS::VtValue Get(const PXR_NS::SdfPath& path,
                                const PXR_NS::TfToken& fieldName) const OVERRIDE;
    virtual void Set(const PXR_NS::SdfPath& path,
                     const PXR_NS::TfToken& fieldName,
                     const PXR_NS::VtValue& value) OVERRIDE;
    virtual void Set(const PXR_NS::SdfPath& path,
                     const PXR_NS::TfToken& fieldName,
                     const PXR_NS::SdfAbstractDataConstValue& value) OVERRIDE;
    virtual void Erase(const PXR_NS::SdfPath& path,
                       const PXR_NS::TfToken& fieldName) OVERRIDE;
    virtual std::vector<PXR_NS::TfToken> List(const PXR_NS::SdfPath& path) const OVERRIDE;

    virtual std::set<double> ListAllTimeSamples() const OVERRIDE;
    virtual std::set<double> ListTimeSamplesForPath(const PXR_NS::SdfPath& path) const OVERRIDE;
    virtual bool GetBracketingTimeSamples(double time,
                                          double* tLower,
                                          double* tUpper) const OVERRIDE;
    virtual size_t GetNumTimeSamplesForPath(const PXR_NS::SdfPath& path) const OVERRIDE;
    virtual bool GetBracketingTimeSamplesForPath(const PXR_NS::SdfPath& path,
                                                 double time,
                                                 double* tLower,
                                                 double* tUpper) const OVERRIDE;
    virtual bool QueryTimeSample(const PXR_NS::SdfPath& path,
                                 double time,
                                 PXR_NS::VtValue* optionalValue = NULL) const OVERRIDE;
    virtual bool QueryTimeSample(const PXR_NS::SdfPath& path,
                                 double time,
                                 PXR_NS::SdfAbstractDataValue* optionalValue) const OVERRIDE;
    virtual void SetTimeSample(const PXR_NS::SdfPath& path,
                               double time,
                               const PXR_NS::VtValue& value) OVERRIDE;
    virtual void EraseTimeSample(const PXR_NS::SdfPath& path,
                                 double time) OVERRIDE;

protected:
    virtual void _VisitSpecs(PXR_NS::SdfAbstractDataSpecVisitor* visitor) const OVERRIDE;

private:
    struct Edit {
        bool erased = false;
        // A spec created over nothing, or recreated after an erase: the converter is not read.
        bool replaces = false;
        PXR_NS::SdfSpecType type = PXR_NS::SdfSpecTypeUnknown;
        std::map<PXR_NS::TfToken, PXR_NS::VtValue> fields;
        std::set<PXR_NS::TfToken> erasedFields;
    };

    const Edit* findEdit(const PXR_NS::SdfPath& path) const;
    // Whether an erase or a recreation of a strict ancestor hides the converter's spec at @p path.
    bool ancestorHidesBase(const PXR_NS::SdfPath& path) const;
    Edit& editFor(const PXR_NS::SdfPath& path);
    bool getField(const PXR_NS::SdfPath& path,
                  const PXR_NS::TfToken& fieldName,
                  PXR_NS::VtValue* value) const;
    PXR_NS::SdfTimeSampleMap timeSamples(const PXR_NS::SdfPath& path) const;

    std::shared_ptr<const GeoUsdConvert> _convert;
    GeoDetailConstPtr _source;
    std::unordered_map<PXR_NS::SdfPath, Edit, PXR_NS::SdfPath::Hash> _edits;
};

/** @brief The converter behind an open natrongeo layer, null for another layer. */
std::shared_ptr<const GeoUsdConvert> geoLayerConvert(const PXR_NS::SdfLayerHandle& layer);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoSdfData_h
