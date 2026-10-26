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

#ifndef Tests_NativeMetadataTestEffect_h
#define Tests_NativeMetadataTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <functional>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDMetadataSource "test.natron.built-in.MetadataSource"
#define kTestPluginIDMetadataFilter "test.natron.built-in.MetadataFilter"

NATRON_NAMESPACE_ENTER

// A source whose metadata is a function of its "tag" knob and of the frame, and which counts how
// often it is asked, so a test can tell a cached answer from a recomputed one.
class MetadataSourceTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new MetadataSourceTestEffect(n);
    }

    explicit MetadataSourceTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , _tag()
        , _derivations(0)
        , _extra()
        , _nComps()
        , _bitDepth()
        , _par()
        , _format()
        , _nodeMetadataDerivations(0)
        , _identityEvaluations(0)
        , _deriveHook()
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    // Called inside each getPreferredMetadataAt(), so a test can edit or purge while a derivation
    // runs. Set from the thread that asks for metadata.
    void setDeriveHook(const std::function<void()>& hook)
    {
        _deriveHook = hook;
    }

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++_derivations;

        KnobIntPtr tagKnob = _tag.lock();
        ImageMetadata m;

        m.setInt("exr/tag", tagKnob ? tagKnob->getValue() : 0);
        m.setString("exr/origin", "source");
        m.setInt("ofx/frame", static_cast<int>(time));
        m.merge(_extra, ImageMetadata::eMergePreferOther);

        return m;
    }

    // Read without a lock and invisible to the node hash, so it must be set from the thread that
    // asks for metadata, and a change after the first ask needs changeMetadataUnderSameHash() too.
    void setExtraMetadata(const ImageMetadata& extra)
    {
        _extra = extra;
    }

    int derivationCount() const
    {
        return _derivations.load();
    }

    // How often the node metadata were derived at a frame, through getPreferredMetadataAt().
    int nodeMetadataDerivationCount() const
    {
        return _nodeMetadataDerivations.load();
    }

    int identityEvaluationCount() const
    {
        return _identityEvaluations.load();
    }

    KnobIntPtr getNCompsKnob() const
    {
        return _nComps.lock();
    }

    KnobIntPtr getBitDepthKnob() const
    {
        return _bitDepth.lock();
    }

    KnobDoublePtr getParKnob() const
    {
        return _par.lock();
    }

    KnobIntPtr getFormatKnob() const
    {
        return _format.lock();
    }

    // Unlike a knob change, leaves the caches of the nodes downstream in place, so a test can
    // tell whether one of them answered from its own cache.
    void dropOwnMetadataCache()
    {
        dropCachedOutputMetadata();
    }

    // As a file reloaded under an unchanged hash.
    void changeMetadataUnderSameHash()
    {
        invalidateOutputMetadata();
    }

protected:
    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++_identityEvaluations;

        return NativeEffectBase::isIdentity(time, scale, roi, view, inputTime, inputView, inputNb);
    }

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL
    {
        ++_nodeMetadataDerivations;
        applyNodeMetadata(time, metadata);
        if (_deriveHook) {
            _deriveHook();
        }

        return eStatusOK;
    }

private:
    void applyNodeMetadata(double time,
                           NodeMetadata& metadata)
    {
        KnobIntPtr nComps = _nComps.lock();
        KnobIntPtr bitDepth = _bitDepth.lock();
        KnobDoublePtr par = _par.lock();
        KnobIntPtr format = _format.lock();
        if (!nComps || !bitDepth || !par || !format) {
            return;
        }
        metadata.setNComps(-1, nComps->getValueAtTime(time));
        metadata.setComponentsType(-1, kNatronColorLayerID);
        metadata.setBitDepth(-1, static_cast<ImageBitDepthEnum>(bitDepth->getValueAtTime(time)));
        metadata.setPixelAspectRatio(-1, par->getValueAtTime(time));
        const int width = format->getValueAtTime(time, 0);
        const int height = format->getValueAtTime(time, 1);
        if ((width > 0) && (height > 0)) {
            metadata.setOutputFormat(RectI(0, 0, width, height));
        }
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDMetadataSource,
        .label = "Test Metadata Source",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobIntPtr tag = createKnob<KnobInt>(std::string("Tag"));

        tag->setName("tag");
        tag->setAnimationEnabled(false);
        tag->setDefaultValue(1);
        page->addKnob(tag);
        _tag = tag;

        KnobIntPtr nComps = createKnob<KnobInt>(std::string("Components"));
        nComps->setName("nComps");
        nComps->setDefaultValue(4);
        page->addKnob(nComps);
        _nComps = nComps;

        KnobIntPtr bitDepth = createKnob<KnobInt>(std::string("Bit Depth"));
        bitDepth->setName("bitDepth");
        bitDepth->setDefaultValue(static_cast<int>(eImageBitDepthFloat));
        page->addKnob(bitDepth);
        _bitDepth = bitDepth;

        KnobDoublePtr par = createKnob<KnobDouble>(std::string("Pixel Aspect Ratio"));
        par->setName("par");
        par->setDefaultValue(1.);
        page->addKnob(par);
        _par = par;

        // Width and height; 0 keeps the default format.
        KnobIntPtr format = createKnob<KnobInt>(std::string("Format"), 2);
        format->setName("format");
        format->setDefaultValue(0, 0);
        format->setDefaultValue(0, 1);
        page->addKnob(format);
        _format = format;
    }

    KnobIntWPtr _tag;
    std::atomic<int> _derivations;
    ImageMetadata _extra;
    KnobIntWPtr _nComps;
    KnobIntWPtr _bitDepth;
    KnobDoubleWPtr _par;
    KnobIntWPtr _format;
    std::atomic<int> _nodeMetadataDerivations;
    std::atomic<int> _identityEvaluations;
    std::function<void()> _deriveHook;
};

// A one-input filter that counts its identity evaluations, so a test can tell a cached
// pass-through choice from a recomputed one.
class MetadataFilterTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new MetadataFilterTestEffect(n);
    }

    explicit MetadataFilterTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , _identityEvaluations(0)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    int identityEvaluationCount() const
    {
        return _identityEvaluations.load();
    }

protected:
    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++_identityEvaluations;

        return NativeEffectBase::isIdentity(time, scale, roi, view, inputTime, inputView, inputNb);
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDMetadataFilter,
        .label = "Test Metadata Filter",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    std::atomic<int> _identityEvaluations;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_NativeMetadataTestEffect_h
