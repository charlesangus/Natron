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

#ifndef Tests_GeoRenderTestEffect_h
#define Tests_GeoRenderTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"

#define kTestPluginIDGeoRenderTriangle "test.natron.built-in.GeoRenderTriangle"
#define kTestPluginIDGeoRenderTranslate "test.natron.built-in.GeoRenderTranslate"
#define kTestPluginIDGeoRenderPreviousFrame "test.natron.built-in.GeoRenderPreviousFrame"

#define kGeoRenderTestOffsetX 2.

NATRON_NAMESPACE_ENTER

inline std::atomic<int>&
geoRenderTestTriangleRenderCount()
{
    static std::atomic<int> count(0);

    return count;
}

inline std::atomic<int>&
geoRenderTestTranslateRenderCount()
{
    static std::atomic<int> count(0);

    return count;
}

// Called from inside the triangle's renderGeometry(), so a test can abort the render mid-pull.
inline std::function<void()>&
geoRenderTestTriangleHook()
{
    static std::function<void()> hook;

    return hook;
}

// When not empty, the triangle fails with this message.
inline std::string&
geoRenderTestTriangleError()
{
    static std::string error;

    return error;
}

// The input times the last GeoRenderPreviousFrame render received a detail for.
inline std::set<double>&
geoRenderTestPreviousFrameInputTimes()
{
    static std::set<double> times;

    return times;
}

inline std::array<std::array<double, 3>, 3>
geoRenderTestTrianglePoints()
{
    return { { { 0., 0., 0. }, { 1., 0., 0. }, { 0., 1., 0. } } };
}

/**
 * @brief A geometry generator: one triangle, at the same place at every time.
 **/
class GeoRenderTestTriangle
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new GeoRenderTestTriangle(n);
    }

    explicit GeoRenderTestTriangle(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDGeoRenderTriangle,
        .label = "Test Geo Render Triangle",
        .outputKind = eDataKindGeometry,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& /*args*/,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++geoRenderTestTriangleRenderCount();
        if (geoRenderTestTriangleHook()) {
            geoRenderTestTriangleHook()();
        }

        return renderGeometryGenerator([](GeoDetailBuilder& detail, std::string* error) {
            if (!geoRenderTestTriangleError().empty()) {
                *error = geoRenderTestTriangleError();

                return eStatusFailed;
            }

            const std::array<std::array<double, 3>, 3> points = geoRenderTestTrianglePoints();
            const GeoOffset first = detail.appendPoints(3);
            std::vector<GeoOffset> polygon;
            for (GeoOffset i = 0; i < 3; ++i) {
                detail.setPointPosition(first + i, points[i][0], points[i][1], points[i][2]);
                polygon.push_back(first + i);
            }
            detail.appendPrimitive(eGeoPrimitivePolygon, polygon);

            return eStatusOK;
        },
                                       outputDetail, errorMessage);
    }
};

/**
 * @brief A geometry op: moves every point of its input by kGeoRenderTestOffsetX along x.
 **/
class GeoRenderTestTranslate
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new GeoRenderTestTranslate(n);
    }

    explicit GeoRenderTestTranslate(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    // The Disable knob is one of the default knobs only a settings panel creates, and the
    // disabled pass-through test needs it.
    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return true;
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDGeoRenderTranslate,
        .label = "Test Geo Render Translate",
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++geoRenderTestTranslateRenderCount();

        return renderGeometryFromInput(args, 0, [](GeoDetailBuilder& detail, std::string* /*error*/) {
            std::vector<GeoOffset> points;
            detail.detail().pointMap().forEachLive([&points](GeoIndex /*index*/, GeoOffset offset) {
                points.push_back(offset);
            });
            for (GeoOffset point : points) {
                const std::array<double, 3> p = detail.detail().pointPosition(point);
                detail.setPointPosition(point, p[0] + kGeoRenderTestOffsetX, p[1], p[2]);
            }

            return eStatusOK; }, outputDetail, errorMessage);
    }
};

/**
 * @brief A geometry op reading its input at the frame before and the frame being rendered, and
 * passing the current one through unchanged.
 **/
class GeoRenderTestPreviousFrame
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new GeoRenderTestPreviousFrame(n);
    }

    explicit GeoRenderTestPreviousFrame(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        FramesNeededMap frames;
        RangeD range;

        range.min = time - 1.;
        range.max = time;
        frames[0][view].push_back(range);

        return frames;
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false, .time = InputTime::Any },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDGeoRenderPreviousFrame,
        .label = "Test Geo Render Previous Frame",
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* /*errorMessage*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        geoRenderTestPreviousFrameInputTimes().clear();
        const GeoInputDetailsMap::const_iterator input = args.inputDetails.find(0);
        if (input != args.inputDetails.end()) {
            for (const GeoDetailsByTime::value_type& detail : input->second) {
                if (detail.second) {
                    geoRenderTestPreviousFrameInputTimes().insert(detail.first);
                }
            }
        }
        *outputDetail = args.getInputDetail(0);

        return *outputDetail ? eStatusOK : eStatusFailed;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_GeoRenderTestEffect_h
