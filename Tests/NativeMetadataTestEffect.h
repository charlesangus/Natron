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

#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDMetadataSource "test.natron.built-in.MetadataSource"

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
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
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

    // Read without a lock and invisible to the node hash, so it must be set before the node is
    // connected or asked for metadata.
    void setExtraMetadata(const ImageMetadata& extra)
    {
        _extra = extra;
    }

    int derivationCount() const
    {
        return _derivations.load();
    }

    // Unlike a knob change, leaves the caches of the nodes downstream in place, so a test can
    // tell whether one of them answered from its own cache.
    void dropOwnMetadataCache()
    {
        invalidateOutputMetadata();
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDMetadataSource;
        desc.label = "Test Metadata Source";
        desc.description = "";
        desc.outputKind = eDataKindImage;

        return desc;
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
    }

    KnobIntWPtr _tag;
    std::atomic<int> _derivations;
    ImageMetadata _extra;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_NativeMetadataTestEffect_h
