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

#ifndef Engine_InverseWarpChain_h
#define Engine_InverseWarpChain_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/GpuWeight.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One node's inverse warp over a window: points in the node's output pixels at the render
 * scale, pixel centres at +0.5 as Resampler::resampleRowFromCoords() takes them, to the pixels of
 * the input it samples. Immutable once built, so the render threads of a consumer share it.
 **/
class InverseWarpMap {
public:
    InverseWarpMap(const RenderScale& scale,
                   double par);

    virtual ~InverseWarpMap();

    /**
     * @brief Maps the n points (sx[i], sy[i]) in place. When J is not null it holds four doubles
     * per point, (Jxx, Jxy, Jyx, Jyy) in resampleRowFromCoords()'s layout, the derivative of the
     * point in the consumer's output pixels; each becomes D·J, D this map's derivative at the
     * point. A point with no source becomes NaN, and a NaN point stays NaN with its J untouched.
     **/
    virtual void mapRow(std::size_t n,
                        double* sx,
                        double* sy,
                        double* J) const
        = 0;

    /**
     * @brief A canonical region of the input holding the source of every point of the canonical
     * region, grown by filterSupport, in pixels, times the largest kernel scale (the largest
     * singular value of D) the map has there. Null when no point has a source; infinite for an
     * infinite region. The default samples the region through mapRow().
     **/
    virtual RectD pullBack(const RectD& canonical,
                           double filterSupport) const;

    /**
     * @brief Appends the block warpchain.slang maps points through and returns its tag, one of
     * InverseWarpChain's kGpuTag values, or 0, appending nothing, for a map with no GPU form.
     * Offsets within the block are relative to its first word.
     **/
    virtual uint32_t packGpu(std::vector<uint32_t>* words) const;

    const RenderScale& getScale() const
    {
        return _scale;
    }

    double getPixelAspectRatio() const
    {
        return _par;
    }

protected:
    RectD canonicalToPixel(const RectD& canonical) const;
    RectD pixelToCanonical(const RectD& pixel) const;

private:
    RenderScale _scale;
    double _par;
};

/**
 * @brief The inverse of a 3x3 matrix in pixel coordinates as an InverseWarpMap.
 **/
class MatrixInverseWarpMap
    : public InverseWarpMap {
public:
    /**
     * @brief forward maps the input's pixels to the output's, as getTransform() answers it. A
     * singular forward gives no source anywhere.
     **/
    MatrixInverseWarpMap(const Transform::Matrix3x3& forward,
                         const RenderScale& scale,
                         double par);

    virtual void mapRow(std::size_t n,
                        double* sx,
                        double* sy,
                        double* J) const OVERRIDE FINAL;

    /**
     * @brief The bounding box of the region's corners through the inverse, as transformInputRois()
     * takes it. A matrix keeps the footprint the consumer's region of interest already holds, so
     * the support adds nothing.
     **/
    virtual RectD pullBack(const RectD& canonical,
                           double filterSupport) const OVERRIDE FINAL;

    /**
     * @brief The inverse as nine two-float pairs, row-major; all zero when singular, which the
     * kernel's projective divide reads as no source.
     **/
    virtual uint32_t packGpu(std::vector<uint32_t>* words) const OVERRIDE FINAL;

    const Transform::Matrix3x3& getForward() const
    {
        return _forward;
    }

private:
    Transform::Matrix3x3 _forward;
    Transform::Matrix3x3 _inverse;
    double _m[9];
    bool _invertible;
};

namespace InverseWarp {
/**
 * @brief Maps n points through inverse, a row-major 3x3 matrix in pixel coordinates, in place,
 * chaining jacobian (four values per point, when not null) with the projective map's derivative.
 * A point at or behind infinity, or a NaN one, becomes NaN.
 **/
template <typename T>
void mapRowThroughMatrix(const double* inverse,
                         std::size_t n,
                         T* sx,
                         T* sy,
                         T* jacobian);

/**
 * @brief The largest singular value of the 2x2 matrix (a, b; c, d).
 **/
double largestSingularValue(double a,
                            double b,
                            double c,
                            double d);

/**
 * @brief Appends v as the two floats, hi then lo, whose sum the GPU kernels compute with.
 **/
void pushGpuDouble(std::vector<uint32_t>* words,
                   double v);
} // namespace InverseWarp

/**
 * @brief The inverse warps between a consumer and the source it samples, from its nearest input
 * upstream. Each entry is a matrix or a node's map; maps depend on the window they are built
 * over, so an entry may hold a factory that prepare() calls with that window.
 **/
class InverseWarpChain {
public:
    enum StepKind {
        eStepMatrix,
        eStepMap,
    };

    /**
     * @brief What a GPU consumer must sample to take a step, as bits of a mask: a step not
     * known to have a GPU form is eGpuStepUnsupported.
     **/
    enum GpuStep : unsigned {
        eGpuStepMatrix = 1u,
        eGpuStepSplineWarp = 2u,
        eGpuStepLens = 4u,
        eGpuStepUnsupported = 8u,
    };

    // The tags of warpchain.slang's step blocks.
    static constexpr uint32_t kGpuTagMatrix = 1;
    static constexpr uint32_t kGpuTagSplineWarp = 2;
    static constexpr uint32_t kGpuTagLens = 3;

    // The flags of warpchain.slang's format crop: a crop rect is set, and a point outside it has
    // no source rather than being clamped inside it.
    static constexpr uint32_t kGpuCropFlag = 2;
    static constexpr uint32_t kGpuEdgeBlackFlag = 4;

    /**
     * @brief Appends, as packGpu() writes a chain, the one-step chain of the inverse of forward.
     **/
    static void packGpuMatrix(const Transform::Matrix3x3& forward,
                              std::vector<uint32_t>* words);

    typedef std::function<InverseWarpMapConstPtr(const RectD& window)> MapFactory;

    /**
     * @brief scale and par are those matrix entries convert their regions with.
     **/
    InverseWarpChain(const RenderScale& scale,
                     double par);

    /**
     * @brief Appends a matrix upstream of the entries so far, multiplied into the last entry when
     * that is a matrix too. forward is in pixel coordinates at the chain's scale.
     **/
    void appendMatrix(const Transform::Matrix3x3& forward);

    /**
     * @brief Appends a map that prepare() builds over its window. gpuStep is the GpuStep bit of
     * the maps the factory builds; gpuWeight is what the node providing them weighs in GPU
     * placement, which the consumer taking the chain takes on, as that node does not render.
     **/
    void appendMap(const MapFactory& factory,
                   unsigned gpuStep = eGpuStepUnsupported,
                   const std::optional<GpuWeight>& gpuWeight = std::nullopt);

    /**
     * @brief Appends a map built already, which prepare() keeps whatever the window.
     **/
    void appendMap(const InverseWarpMapConstPtr& map,
                   unsigned gpuStep = eGpuStepUnsupported);

    /**
     * @brief Marks the source the chain reads as having an infinite region of definition: a
     * consumer's region of it is clipped before prepare() sees it, so prepare() fails on the mark
     * instead of on the window, building no map.
     **/
    void setSourceInfinite(bool infinite)
    {
        _sourceInfinite = infinite;
    }

    const RenderScale& getScale() const
    {
        return _scale;
    }

    double getPixelAspectRatio() const
    {
        return _par;
    }

    /**
     * @brief The GpuStep bits of every entry.
     **/
    unsigned getGpuSteps() const;

    /**
     * @brief weight with the GPU weights of the map entries added, Heavy when one of them is.
     **/
    GpuWeight addMapGpuWeights(GpuWeight weight) const WARN_UNUSED_RETURN;

    std::size_t getStepCount() const
    {
        return _steps.size();
    }

    StepKind getStepKind(std::size_t index) const
    {
        return _steps[index].kind;
    }

    /**
     * @brief The entry's map, null for a map not built yet or whose provider gave none.
     **/
    const InverseWarpMapConstPtr& getStepMap(std::size_t index) const
    {
        return _steps[index].map;
    }

    bool hasMapStep() const;

    /**
     * @brief A copy with every map built: the nearest entry over window, the consumer's region
     * of its input, and each further one over the region the one before it pulls back with
     * filterSupport. An entry whose window is null has no map. A map entry whose window is
     * infinite or not finite is never built or pulled back over: the copy has an error, no map
     * from that entry on and a null source region.
     **/
    InverseWarpChain prepare(const RectD& window,
                             double filterSupport) const;

    /**
     * @brief prepare() shared by every caller with the same window and filterSupport, whatever
     * thread it is on: the first one prepares while the others wait for it. The chain holds what
     * it prepared, so the prepared copies go with it.
     **/
    InverseWarpChainConstPtr prepareShared(const RectD& window,
                                           double filterSupport) const WARN_UNUSED_RETURN;

    bool isPrepared() const
    {
        return _prepared;
    }

    /**
     * @brief The canonical output region of each entry, as prepare() gave it.
     **/
    const std::vector<RectD>& getStepWindows() const
    {
        return _windows;
    }

    /**
     * @brief What the last entry pulls back from the window prepare() was given: the region of
     * the source the chain reads.
     **/
    const RectD& getSourceRegion() const
    {
        return _sourceRegion;
    }

    /**
     * @brief getSourceRegion() in the source's pixels at the chain's scale, par being the source's
     * pixel aspect ratio. A consumer reads the source fetched upstream of the chain only there,
     * or at the nearest edge of the source where the region misses it (Resampler::cropToNearest()),
     * as the GPU does, since the image fetched can be larger, holding what other renders
     * requested.
     **/
    RectI getSourcePixels(double par) const;

    /**
     * @brief Why prepare() could not build the chain, empty when it could; a render through a
     * chain with an error fails with this message.
     **/
    const std::string& getError() const
    {
        return _error;
    }

    bool hasError() const
    {
        return !_error.empty();
    }

    /**
     * @brief The error prepare() gives for a map over an infinite region.
     **/
    static const std::string& getInfiniteWindowError();

    /**
     * @brief Every entry's mapRow(), downstream to upstream. A map entry with no map gives no
     * source.
     **/
    void mapRow(std::size_t n,
                double* sx,
                double* sy,
                double* J) const;

    /**
     * @brief The prepared chain as warpchain.slang reads it: the entry count, a tag and a word
     * offset per entry, then each entry's block. False, with words cleared, when an entry has no
     * map or no GPU form.
     **/
    bool packGpu(std::vector<uint32_t>* words) const;

private:
    struct Step {
        StepKind kind = eStepMatrix;
        unsigned gpuStep = eGpuStepMatrix;
        std::optional<GpuWeight> gpuWeight;
        Transform::Matrix3x3 forward;
        InverseWarpMapConstPtr map;
        MapFactory factory;
    };

    struct PreparedCache;

    // A copy starts with an empty cache: one sharing its source's would make the copies
    // prepareShared() stores hold the cache that holds them.
    struct PreparedCacheSlot {
        PreparedCacheSlot();
        PreparedCacheSlot(const PreparedCacheSlot&);
        PreparedCacheSlot& operator=(const PreparedCacheSlot&);
        ~PreparedCacheSlot();

        std::unique_ptr<PreparedCache> cache;
    };

    RenderScale _scale;
    double _par;
    std::vector<Step> _steps;
    std::vector<RectD> _windows;
    RectD _sourceRegion;
    std::string _error;
    bool _sourceInfinite = false;
    bool _prepared = false;
    PreparedCacheSlot _preparedCache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_InverseWarpChain_h
