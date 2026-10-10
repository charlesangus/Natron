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

#ifndef Engine_Nodes_IO_NativeRead_h
#define Engine_Nodes_IO_NativeRead_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "Engine/EffectInstance.h" // for PLUGINID_NATRON_READ
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/ReadTimeDomain.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGIN_MAJOR_NATRON_READ 2
#define kNatronReadNodeOCIOParamInputSpace "ocioInputSpace"

NATRON_NAMESPACE_ENTER

/**
 * @brief The native image reader, the only plug-in registered under PLUGINID_NATRON_READ,
 * at major 2.
 *
 * The planned knobs keep GenericReader's script names, so the host hooks and the DopeSheet
 * that look knobs up by name apply unchanged:
 *   - file and proxy: filename (kOfxImageEffectFileParamName, a metadata slave), proxy,
 *     proxyThreshold, originalProxyScale, customProxyScale
 *   - time: originalFrameRange, firstFrame, lastFrame, before, after, onMissingFrame,
 *     frameMode, startingTime, timeOffset, timeDomainUserEdited, frameRate, customFps
 *   - colour: ocioInputSpace, ocioInputSpaceIndex, ocioInputSpaceSet, and the hidden
 *     ocioConfigFile and ocioWorkingSpace
 *   - views: the hidden availableViews
 * The file, proxy, time and colour knobs exist so far. A sequence pattern is resolved to one file per output
 * frame through ReadTimeDomain. The colour plane and every other layer of the file are produced,
 * the layers grouped as OiioReadSupport::fileLayers groups them. A file of several views, as
 * parts with a `view` attribute or channels prefixed per its `multiView` list, fills the hidden
 * availableViews so the project gets those views; each project view then reads its own part or
 * channels, and a view the file lacks reads the file's default view.
 **/
class NativeRead
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NativeRead(node);
    }

    explicit NativeRead(NodePtr node);

    virtual bool isReader() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool isGenerator() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool isViewAware() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual void getFrameRange(double* first, double* last) OVERRIDE FINAL;

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    bool rejectUnsupportedFormat(const std::string& path);

    // The frames a filename pattern matches on disk, listed once per pattern.
    struct FrameListing {
        std::string pattern;
        bool singleImage;
        std::string singlePath;
        std::set<int> frames;

        FrameListing()
            : pattern()
            , singleImage(true)
            , singlePath()
            , frames()
        {
        }
    };

    // What an output frame loads: a file, black, or an error to report.
    struct Target {
        ReadTimeDomain::Result::Kind kind;
        std::string path;
        std::string message;
        int frame; // the file frame path was resolved from, valid for eFile

        Target()
            : kind(ReadTimeDomain::Result::eBlack)
            , path()
            , message()
            , frame(0)
        {
        }
    };

    void refreshAvailableViews(bool silent);
    std::string projectViewName(ViewIdx view) const;

    std::shared_ptr<const FrameListing> frameListing(bool proxy = false) const;
    void invalidateFrameListing();
    ReadTimeDomain::Settings settingsAt(double time) const;
    Target targetAtTime(double time, bool proxy = false) const;
    std::string representativePath(bool proxy = false) const;
    double fileFrameRate() const;
    void refreshFrameRateFromFile();
    void refreshTimeKnobState();
    void refreshProxyScale();
    void refreshProxyKnobState();
    bool proxyKnobChanged(KnobI* k,
                          ValueChangedReasonEnum reason);
    bool proxySourceAt(double time,
                       unsigned int level,
                       Target* source,
                       unsigned int* fileLevel) const;
    bool colourKnobChanged(KnobI* k,
                           ValueChangedReasonEnum reason);
    void guessInputSpace();
    void refreshInputSpaceMenu();
    std::string workingSpaceName() const;

    KnobFileWPtr _filename;
    KnobFileWPtr _proxy;
    KnobDoubleWPtr _proxyThreshold;
    KnobDoubleWPtr _originalProxyScale;
    KnobBoolWPtr _customProxyScale;
    KnobIntWPtr _originalFrameRange;
    KnobIntWPtr _firstFrame;
    KnobIntWPtr _lastFrame;
    KnobChoiceWPtr _before;
    KnobChoiceWPtr _after;
    KnobChoiceWPtr _onMissingFrame;
    KnobChoiceWPtr _frameMode;
    KnobIntWPtr _startingTime;
    KnobIntWPtr _timeOffset;
    KnobBoolWPtr _timeDomainUserEdited;
    KnobDoubleWPtr _frameRate;
    KnobBoolWPtr _customFps;
    KnobStringWPtr _ocioConfigFile;
    KnobStringWPtr _ocioWorkingSpace;
    KnobStringWPtr _inputSpace;
    KnobChoiceWPtr _inputSpaceMenu;
    KnobBoolWPtr _inputSpaceSet;

    KnobStringWPtr _availableViews;

    mutable std::mutex _listingMutex;
    mutable std::shared_ptr<const FrameListing> _listing;
    mutable std::shared_ptr<const FrameListing> _proxyListing;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_NativeRead_h
