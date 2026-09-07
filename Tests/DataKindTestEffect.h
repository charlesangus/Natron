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

#ifndef Tests_DataKindTestEffect_h
#define Tests_DataKindTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/NoOpBase.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define kTestPluginIDDataKindDeepSource "test.natron.built-in.DataKindDeepSource"
#define kTestPluginIDDataKindImageSink "test.natron.built-in.DataKindImageSink"
#define kTestPluginIDDataKindDeepSink "test.natron.built-in.DataKindDeepSink"
#define kTestPluginIDDataKindPolyTwoInputs "test.natron.built-in.DataKindPolyTwoInputs"
#define kTestPluginIDDataKindPolyAndImageInput "test.natron.built-in.DataKindPolyAndImageInput"
#define kTestPluginIDDataKindScenePolicy "test.natron.built-in.DataKindScenePolicy"

NATRON_NAMESPACE_ENTER

class DataKindTestDeepSource
    : public NoOpBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DataKindTestDeepSource(n);
    }

    DataKindTestDeepSource(NodePtr n)
        : NoOpBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return kTestPluginIDDataKindDeepSource;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Test Data Kind Deep Source";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN
    {
        return 0;
    }

    virtual DataKindEnum getOutputDataKind() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eDataKindDeep;
    }
};

class DataKindTestImageSink
    : public NoOpBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DataKindTestImageSink(n);
    }

    DataKindTestImageSink(NodePtr n)
        : NoOpBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return kTestPluginIDDataKindImageSink;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Test Data Kind Image Sink";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual DataKindEnum getOutputDataKind() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eDataKindImage;
    }

    virtual DataKindEnum getInputDataKind(int /*inputNb*/) const OVERRIDE WARN_UNUSED_RETURN
    {
        return eDataKindImage;
    }
};

// No shipped node is a polymorphic pass-through with more than one input, so nothing else can
// exercise what a node touching two different concrete kinds at once resolves to.
class DataKindTestPolyTwoInputs
    : public NoOpBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DataKindTestPolyTwoInputs(n);
    }

    DataKindTestPolyTwoInputs(NodePtr n)
        : NoOpBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return kTestPluginIDDataKindPolyTwoInputs;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Test Data Kind Poly Two Inputs";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    // Distinct per input: a node's connections are serialized keyed by input label, so two
    // inputs sharing a label cannot both survive a project round-trip.
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return inputNb == 0 ? "A" : "B";
    }

    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN
    {
        return 2;
    }
};

// Same shape, but the second input declares a concrete kind, the way a mask input would: it must
// not hand its kind to the polymorphic output.
class DataKindTestPolyAndImageInput
    : public NoOpBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DataKindTestPolyAndImageInput(n);
    }

    DataKindTestPolyAndImageInput(NodePtr n)
        : NoOpBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return kTestPluginIDDataKindPolyAndImageInput;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Test Data Kind Poly And Image Input";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return inputNb == 0 ? "A" : "B";
    }

    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN
    {
        return 2;
    }

    virtual DataKindEnum getInputDataKind(int inputNb) const OVERRIDE WARN_UNUSED_RETURN
    {
        return inputNb == 0 ? eDataKindPolymorphic : eDataKindImage;
    }
};

class DataKindTestDeepSink
    : public NoOpBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DataKindTestDeepSink(n);
    }

    DataKindTestDeepSink(NodePtr n)
        : NoOpBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return kTestPluginIDDataKindDeepSink;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Test Data Kind Deep Sink";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual DataKindEnum getOutputDataKind() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eDataKindDeep;
    }

    virtual DataKindEnum getInputDataKind(int /*inputNb*/) const OVERRIDE WARN_UNUSED_RETURN
    {
        return eDataKindDeep;
    }
};

// Stands in for a native node that owns its resolution policy (a Switch following its selected
// input, say): it declares a polymorphic output but answers with a fixed kind regardless of what
// the graph around it structurally says, which is the only thing the hook has to guarantee.
class DataKindTestScenePolicy
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DataKindTestScenePolicy(n);
    }

    explicit DataKindTestScenePolicy(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

    virtual DataKindEnum resolveOutputDataKind(bool* isAmbiguous) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (isAmbiguous) {
            *isAmbiguous = false;
        }

        return eDataKindScene;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDDataKindScenePolicy;
        desc.label = "Test Data Kind Scene Policy";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindPolymorphic));
        desc.outputKind = eDataKindPolymorphic;

        return desc;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_DataKindTestEffect_h
