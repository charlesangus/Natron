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

#include "ApertureKnobs.h"

#include <cfloat>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

namespace Optics {

namespace {
    struct Range {
        double minimum;
        double maximum;
        double displayMinimum;
        double displayMaximum;
    };

    constexpr double kUnbounded = DBL_MAX;
    const Range kFraction = { 0., 1., 0., 1. };
    const Range kPositive = { 0., kUnbounded, 0., 1. };

    class Builder {
    public:
        Builder(KnobHolder* holder,
                const std::string& prefix,
                const std::string& labelPrefix)
            : _holder(holder)
            , _prefix(prefix)
            , _labelPrefix(labelPrefix)
        {
        }

        KnobGroupPtr group(const KnobPagePtr& page,
                           const char* name,
                           const char* label,
                           const QString& hint,
                           bool open) const
        {
            KnobGroupPtr group = AppManager::createKnob<KnobGroup>(_holder, labelOf(label));

            group->setName(_prefix + name);
            group->setHintToolTip(hint.toStdString());
            group->setDefaultValue(open);
            page->addKnob(group);

            return group;
        }

        KnobDoublePtr number(const KnobGroupPtr& group,
                             const char* name,
                             const char* label,
                             const QString& hint,
                             const Range& range,
                             double defaultValue,
                             int dimensions = 1) const
        {
            KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(_holder, labelOf(label), dimensions);

            knob->setName(_prefix + name);
            knob->setHintToolTip(hint.toStdString());
            for (int d = 0; d < dimensions; ++d) {
                knob->setMinimum(range.minimum, d);
                knob->setMaximum(range.maximum, d);
                knob->setDisplayMinimum(range.displayMinimum, d);
                knob->setDisplayMaximum(range.displayMaximum, d);
                knob->setDefaultValue(defaultValue, d);
            }
            if (dimensions > 1) {
                knob->setCanAutoFoldDimensions(true);
            }
            group->addKnob(knob);

            return knob;
        }

        KnobIntPtr integer(const KnobGroupPtr& group,
                           const char* name,
                           const char* label,
                           const QString& hint,
                           int minimum,
                           int displayMaximum,
                           int defaultValue) const
        {
            KnobIntPtr knob = AppManager::createKnob<KnobInt>(_holder, labelOf(label));

            knob->setName(_prefix + name);
            knob->setHintToolTip(hint.toStdString());
            knob->setMinimum(minimum);
            knob->setDisplayMinimum(minimum);
            knob->setDisplayMaximum(displayMaximum);
            knob->setDefaultValue(defaultValue);
            group->addKnob(knob);

            return knob;
        }

    private:
        std::string labelOf(const char* label) const
        {
            const std::string translated = EffectInstance::tr(label).toStdString();

            return _labelPrefix.empty() ? translated : _labelPrefix + " " + translated;
        }

        KnobHolder* _holder;
        std::string _prefix;
        std::string _labelPrefix;
    };

    QString
    tr(const char* text)
    {
        return EffectInstance::tr(text);
    }

    template <typename KNOB, typename T>
    T
    read(const KnobReader& reader,
         const std::weak_ptr<KNOB>& knob,
         T fallback,
         int dimension = 0)
    {
        return (T)reader.valueOr(knob, fallback, dimension);
    }
} // anonymous namespace

ApertureKnobs
ApertureKnobs::create(KnobHolder* holder,
                      const KnobPagePtr& page,
                      const std::string& prefix,
                      const std::string& labelPrefix,
                      const ApertureKnobOptions& options)
{
    ApertureKnobs k;
    const Builder b(holder, prefix, labelPrefix);
    const ApertureParams defaults;

    const KnobGroupPtr shape = b.group(page, "Shape", "Shape", tr("The aperture's polygon: its blades, their roundness and the edge's softness."), true);
    k._size = b.number(shape, "Size", "Size", tr("The aperture's width and height as fractions of the mask."), kPositive, defaults.sizeX, 2);
    if (options.blades) {
        k._blades = b.integer(shape, "Blades", "Blades", tr("How many blades the diaphragm has, the polygon's number of sides."), 2, 12, defaults.blades);
    }
    {
        const Range range = { -kUnbounded, kUnbounded, -0.1, 1. };
        k._roundness = b.number(shape, "Roundness", "Roundness", tr("Bends each blade between its corners: negative values push the blade's middle outward, rounding the polygon, and positive values pull it inward."),
                                range, defaults.roundness);
    }
    if (options.rotation) {
        const Range range = { -kUnbounded, kUnbounded, 0., 360. };
        k._rotation = b.number(shape, "Rotation", "Rotation", tr("The polygon's rotation, in degrees."), range, defaults.rotation);
    }
    k._softness = b.number(shape, "Softness", "Softness", tr("How soft the polygon's edge is."), kFraction, defaults.softness);

    const KnobGroupPtr grating = b.group(page, "Grating", "Grating",
                                         tr("Thin radial bars from the aperture's rim towards its centre, as diffraction from the blade edges makes."), false);
    k._gratingStrength = b.number(grating, "GratingStrength", "Grating Strength", tr("How much the bars darken the aperture. 0 turns the grating off."), kFraction, defaults.gratingStrength);
    k._gratingDensity = b.number(grating, "GratingDensity", "Grating Density", tr("How many bars there are: 1 gives 360."), kFraction, defaults.gratingDensity);
    k._gratingLength = b.number(grating, "GratingLength", "Grating Length", tr("How far the bars reach in from the rim."), kFraction, defaults.gratingLength);
    k._gratingWidth = b.number(grating, "GratingWidth", "Grating Width", tr("How wide the bars are."), kFraction, defaults.gratingWidth);
    k._gratingSoftness = b.number(grating, "GratingSoftness", "Grating Softness", tr("How soft the bars' edges are."), kFraction, defaults.gratingSoftness);

    const KnobGroupPtr scratches = b.group(page, "Scratches", "Scratches", tr("Thin lines scattered over the aperture, as scratches on the front element."), false);
    k._scratchesStrength = b.number(scratches, "ScratchesStrength", "Scratches Strength", tr("How much the scratches darken the aperture. 0 turns them off."), kFraction,
                                    defaults.scratchesStrength);
    k._scratchesDensity = b.number(scratches, "ScratchesDensity", "Scratches Density", tr("How many scratches there are: 1 gives 1000."), kFraction, defaults.scratchesDensity);
    k._scratchesLength = b.number(scratches, "ScratchesLength", "Scratches Length", tr("How long the scratches are."), kFraction, defaults.scratchesLength);
    k._scratchesWidth = b.number(scratches, "ScratchesWidth", "Scratches Width", tr("How wide the scratches are."), kFraction, defaults.scratchesWidth);
    {
        const Range range = { -kUnbounded, kUnbounded, 0., 360. };
        k._scratchesRotation = b.number(scratches, "ScratchesRotation", "Scratches Rotation", tr("The scratches' common direction, in degrees."), range,
                                        defaults.scratchesRotation);
    }
    k._scratchesRotationVariation = b.number(scratches, "ScratchesRotationVariation", "Scratches Rotation Variation",
                                             tr("How far each scratch turns from the common direction at random: 1 allows any direction."), kFraction,
                                             defaults.scratchesRotationVariation);
    k._scratchesSoftness = b.number(scratches, "ScratchesSoftness", "Scratches Softness", tr("How soft the scratches' edges are."), kFraction, defaults.scratchesSoftness);
    {
        const Range range = { 0., kUnbounded, 0., 2. };
        k._scratchesParallax = b.number(scratches, "ScratchesParallax", "Scratches Parallax",
                                        tr("How far the scratches shift with the light's position, per axis."), range, defaults.scratchesParallaxX, 2);
    }

    const KnobGroupPtr dust = b.group(page, "Dust", "Dust", tr("Small discs scattered over the aperture, as dust on the front element."), false);
    k._dustStrength = b.number(dust, "DustStrength", "Dust Strength", tr("How much the dust darkens the aperture. 0 turns it off."), kFraction, defaults.dustStrength);
    k._dustDensity = b.number(dust, "DustDensity", "Dust Density", tr("How many particles there are: 1 gives 1000."), kFraction, defaults.dustDensity);
    k._dustRadius = b.number(dust, "DustRadius", "Dust Radius", tr("How large the particles are."), kFraction, defaults.dustRadius);
    k._dustSoftness = b.number(dust, "DustSoftness", "Dust Softness", tr("How soft the particles' edges are."), kFraction, defaults.dustSoftness);
    {
        const Range range = { 0., kUnbounded, 0., 2. };
        k._dustParallax = b.number(dust, "DustParallax", "Dust Parallax", tr("How far the dust shifts with the light's position, per axis."), range,
                                   defaults.dustParallaxX, 2);
    }

    const KnobGroupPtr image = b.group(page, "Image", "Image", tr("An image multiplied into the aperture, read from its first channel."), false);
    k._imageStrength = b.number(image, "ImageStrength", "Image Strength", tr("How much the image darkens the aperture where it is dark. 0 turns it off."), kFraction,
                                defaults.imageStrength);
    k._imageSize = b.number(image, "ImageSize", "Image Size", tr("The image's width and height as fractions of the mask."), kPositive, defaults.imageSizeX, 2);
    k._imageBlack = b.number(image, "ImageBlack", "Image Black", tr("Image values below this count as 0."), kFraction, defaults.imageBlack);
    k._imageWhite = b.number(image, "ImageWhite", "Image White", tr("Image values above this count as 1."), kFraction, defaults.imageWhite);

    return k;
} // ApertureKnobs::create

ApertureParams
ApertureKnobs::params(double time,
                      ViewIdx view) const
{
    const KnobReader reader(time, view);
    ApertureParams p;

    p.sizeX = read(reader, _size, p.sizeX, 0);
    p.sizeY = read(reader, _size, p.sizeY, 1);
    p.blades = read(reader, _blades, p.blades);
    p.roundness = read(reader, _roundness, p.roundness);
    p.rotation = read(reader, _rotation, p.rotation);
    p.softness = read(reader, _softness, p.softness);

    p.gratingStrength = read(reader, _gratingStrength, p.gratingStrength);
    p.gratingDensity = read(reader, _gratingDensity, p.gratingDensity);
    p.gratingLength = read(reader, _gratingLength, p.gratingLength);
    p.gratingWidth = read(reader, _gratingWidth, p.gratingWidth);
    p.gratingSoftness = read(reader, _gratingSoftness, p.gratingSoftness);

    p.scratchesStrength = read(reader, _scratchesStrength, p.scratchesStrength);
    p.scratchesDensity = read(reader, _scratchesDensity, p.scratchesDensity);
    p.scratchesLength = read(reader, _scratchesLength, p.scratchesLength);
    p.scratchesWidth = read(reader, _scratchesWidth, p.scratchesWidth);
    p.scratchesRotation = read(reader, _scratchesRotation, p.scratchesRotation);
    p.scratchesRotationVariation = read(reader, _scratchesRotationVariation, p.scratchesRotationVariation);
    p.scratchesSoftness = read(reader, _scratchesSoftness, p.scratchesSoftness);
    p.scratchesParallaxX = read(reader, _scratchesParallax, p.scratchesParallaxX, 0);
    p.scratchesParallaxY = read(reader, _scratchesParallax, p.scratchesParallaxY, 1);

    p.dustStrength = read(reader, _dustStrength, p.dustStrength);
    p.dustDensity = read(reader, _dustDensity, p.dustDensity);
    p.dustRadius = read(reader, _dustRadius, p.dustRadius);
    p.dustSoftness = read(reader, _dustSoftness, p.dustSoftness);
    p.dustParallaxX = read(reader, _dustParallax, p.dustParallaxX, 0);
    p.dustParallaxY = read(reader, _dustParallax, p.dustParallaxY, 1);

    p.imageStrength = read(reader, _imageStrength, p.imageStrength);
    p.imageSizeX = read(reader, _imageSize, p.imageSizeX, 0);
    p.imageSizeY = read(reader, _imageSize, p.imageSizeY, 1);
    p.imageBlack = read(reader, _imageBlack, p.imageBlack);
    p.imageWhite = read(reader, _imageWhite, p.imageWhite);

    return p;
} // ApertureKnobs::params

} // namespace Optics

NATRON_NAMESPACE_EXIT
