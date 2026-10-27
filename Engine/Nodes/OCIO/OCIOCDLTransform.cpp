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

#include "OCIOCDLTransform.h"

#include <cstring>
#include <exception>
#include <fstream>

#include <QFileInfo>

#include "Global/FloatingPointExceptions.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

namespace {
KnobColorPtr
addCDLColorKnob(KnobHolder* holder,
                OcioNodeKnobs& ocio,
                const KnobPagePtr& page,
                const char* name,
                const QString& label,
                const QString& hint,
                double defaultValue,
                double displayMin,
                double displayMax)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label.toStdString(), 3);

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    for (int i = 0; i < 3; ++i) {
        knob->setDefaultValue(defaultValue, i);
        knob->setDisplayMinimum(displayMin, i);
        knob->setDisplayMaximum(displayMax, i);
    }
    ocio.addKnob(page, knob);

    return knob;
}
} // anon namespace

void
OCIOCDLTransformOp::createKnobs(KnobHolder* holder,
                                const KnobPagePtr& page)
{
    ocio.attach(holder);

    slope = addCDLColorKnob(holder, ocio, page, kOCIOCDLTransformParamSlope, EffectInstance::tr("Slope"),
                            EffectInstance::tr("ASC CDL slope, the gain applied first."), 1., 0., 4.);
    offset = addCDLColorKnob(holder, ocio, page, kOCIOCDLTransformParamOffset, EffectInstance::tr("Offset"),
                             EffectInstance::tr("ASC CDL offset, added after the slope."), 0., -0.2, 0.2);
    power = addCDLColorKnob(holder, ocio, page, kOCIOCDLTransformParamPower, EffectInstance::tr("Power"),
                            EffectInstance::tr("ASC CDL power, applied after the offset."), 1., 0., 4.);

    KnobDoublePtr saturationKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Saturation").toStdString());
    saturationKnob->setName(kOCIOCDLTransformParamSaturation);
    saturationKnob->setHintToolTip(EffectInstance::tr("ASC CDL saturation, applied last.").toStdString());
    saturationKnob->setDefaultValue(1.);
    saturationKnob->setDisplayMinimum(0.);
    saturationKnob->setDisplayMaximum(4.);
    ocio.addKnob(page, saturationKnob);
    saturation = saturationKnob;

    KnobChoicePtr directionKnob = OcioNodeKnobs::createDirectionKnob(holder);
    directionKnob->setName(kOCIOCDLTransformParamDirection);
    ocio.addKnob(page, directionKnob);
    direction = directionKnob;

    KnobBoolPtr readFromFileKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Read from File").toStdString());
    readFromFileKnob->setName(kOCIOCDLTransformParamReadFromFile);
    readFromFileKnob->setHintToolTip(EffectInstance::tr("Applies the CDL of the file instead of the knobs, which then show its "
                                                        "values.")
                                         .toStdString());
    readFromFileKnob->setAnimationEnabled(false);
    readFromFileKnob->setDefaultValue(false);
    ocio.addKnob(page, readFromFileKnob);
    readFromFile = readFromFileKnob;

    files.create(holder, page, ocio, EffectInstance::tr("The ASC CDL file to apply: a .cc, or a .ccc or .cdl with the CCC Id "
                                                        "of the correction to apply."));

    KnobOutputFilePtr exportKnob = AppManager::createKnob<KnobOutputFile>(holder, EffectInstance::tr("Export .cc").toStdString());
    exportKnob->setName(kOCIOCDLTransformParamExport);
    exportKnob->setHintToolTip(EffectInstance::tr("Writes the CDL of the knobs, at the current time, to a new ColorCorrection "
                                                  "file (.cc), which OCIOFileTransform or an OpenColorIO config can load. The "
                                                  "file must not exist yet.")
                                   .toStdString());
    exportKnob->setAnimationEnabled(false);
    exportKnob->setIsPersistent(false);
    exportKnob->setEvaluateOnChange(false);
    ocio.addKnob(page, exportKnob);
    exportFile = exportKnob;
}

OCIO_NAMESPACE::CDLTransformRcPtr
OCIOCDLTransformOp::knobCDL(double time) const
{
    KnobColorPtr slopeKnob = slope.lock();
    KnobColorPtr offsetKnob = offset.lock();
    KnobColorPtr powerKnob = power.lock();
    KnobDoublePtr saturationKnob = saturation.lock();
    double sop[9] = { 1., 1., 1., 0., 0., 0., 1., 1., 1. };

    for (int i = 0; i < 3; ++i) {
        if (slopeKnob) {
            sop[i] = slopeKnob->getValueAtTime(time, i);
        }
        if (offsetKnob) {
            sop[3 + i] = offsetKnob->getValueAtTime(time, i);
        }
        if (powerKnob) {
            sop[6 + i] = powerKnob->getValueAtTime(time, i);
        }
    }
    OCIO_NAMESPACE::CDLTransformRcPtr cdl = OCIO_NAMESPACE::CDLTransform::Create();
    cdl->setSOP(sop);
    cdl->setSat(saturationKnob ? saturationKnob->getValueAtTime(time) : 1.);

    return cdl;
}

OCIO_NAMESPACE::ConstTransformRcPtr
OCIOCDLTransformOp::transform(const KnobReader& reader) const
{
    const OCIO_NAMESPACE::TransformDirection dir = OcioNodeKnobs::direction(reader.value(direction));

    if (reader.value(readFromFile)) {
        // The file goes through a FileTransform, so the project's processor cache and Reload
        // treat it as every other file.
        const std::string src = files.path();
        if (src.empty()) {
            return OCIO_NAMESPACE::ConstTransformRcPtr();
        }
        OCIO_NAMESPACE::FileTransformRcPtr transform = OCIO_NAMESPACE::FileTransform::Create();
        transform->setSrc(src.c_str());
        transform->setCCCId(files.cccid().c_str());
        transform->setDirection(dir);

        return transform;
    }
    double sop[9];
    for (int i = 0; i < 3; ++i) {
        sop[i] = reader.value(slope, i);
        sop[3 + i] = reader.value(offset, i);
        sop[6 + i] = reader.value(power, i);
    }
    OCIO_NAMESPACE::CDLTransformRcPtr transform = OCIO_NAMESPACE::CDLTransform::Create();
    transform->setSOP(sop);
    transform->setSat(reader.value(saturation));
    transform->setDirection(dir);

    return transform;
}

void
OCIOCDLTransformOp::refreshFromFile()
{
    KnobBoolPtr readKnob = readFromFile.lock();
    const bool fromFile = readKnob && readKnob->getValue();
    KnobColorPtr knobs[3] = { slope.lock(), offset.lock(), power.lock() };
    KnobDoublePtr saturationKnob = saturation.lock();

    for (int k = 0; k < 3; ++k) {
        if (knobs[k]) {
            knobs[k]->setAllDimensionsEnabled(!fromFile);
        }
    }
    if (saturationKnob) {
        saturationKnob->setAllDimensionsEnabled(!fromFile);
    }
    if (KnobStringPtr cccid = files.cccidKnob()) {
        cccid->setAllDimensionsEnabled(fromFile);
    }
    if (KnobFilePtr file = files.fileKnob()) {
        file->setAllDimensionsEnabled(fromFile);
    }
    const std::string src = files.path();
    if (!fromFile || src.empty()) {
        return;
    }
    double sop[9];
    double sat = 1.;
    try {
        const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
        OCIO_NAMESPACE::CDLTransformRcPtr cdl = OCIO_NAMESPACE::CDLTransform::CreateFromFile(src.c_str(), files.cccid().c_str());
        cdl->getSOP(sop);
        sat = cdl->getSat();
    } catch (const std::exception&) {
        // The render reports a file it cannot read.
        return;
    }
    for (int k = 0; k < 3; ++k) {
        if (!knobs[k]) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            knobs[k]->setValue(sop[3 * k + i], ViewSpec::all(), i, true);
        }
    }
    if (saturationKnob) {
        saturationKnob->setValue(sat, ViewSpec::all(), 0, true);
    }
}

bool
OCIOCDLTransformOp::exportCC(const std::string& path,
                             double time,
                             std::string* error) const
{
    if (path.empty()) {
        *error = EffectInstance::tr("Export: no file name.").toStdString();

        return false;
    }
    if (QFileInfo(QString::fromStdString(path)).exists()) {
        *error = EffectInstance::tr("Export: %1 already exists.").arg(QString::fromStdString(path)).toStdString();

        return false;
    }
    try {
        const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
        // The writer is looked up by its name, "ColorCorrection", whose extension is cc.
        const char* format = nullptr;
        for (int i = 0; i < OCIO_NAMESPACE::GroupTransform::GetNumWriteFormats(); ++i) {
            const char* extension = OCIO_NAMESPACE::GroupTransform::GetFormatExtensionByIndex(i);
            if (extension && (std::strcmp(extension, "cc") == 0)) {
                format = OCIO_NAMESPACE::GroupTransform::GetFormatNameByIndex(i);
                break;
            }
        }
        if (!format) {
            *error = EffectInstance::tr("Export: this OpenColorIO cannot write .cc files.").toStdString();

            return false;
        }
        OCIO_NAMESPACE::GroupTransformRcPtr group = OCIO_NAMESPACE::GroupTransform::Create();
        group->appendTransform(knobCDL(time));
        ProjectColorManagementPtr colorManagement = ocio.colorManagement();
        OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement ? colorManagement->getConfig() : OCIO_NAMESPACE::ConstConfigRcPtr();
        if (!config) {
            config = OCIO_NAMESPACE::Config::CreateRaw();
        }
        std::ofstream out(path.c_str());
        if (!out) {
            *error = EffectInstance::tr("Export: cannot write %1.").arg(QString::fromStdString(path)).toStdString();

            return false;
        }
        group->write(config, format, out);
        out.close();
        if (!out) {
            *error = EffectInstance::tr("Export: cannot write %1.").arg(QString::fromStdString(path)).toStdString();

            return false;
        }
    } catch (const std::exception& e) {
        *error = EffectInstance::tr("Export: %1").arg(QString::fromUtf8(e.what())).toStdString();

        return false;
    }

    return true;
}

OCIOCDLTransform::OCIOCDLTransform(NodePtr node)
    : OcioNode<OCIOCDLTransformOp>(node)
{
}

OCIOCDLTransform::~OCIOCDLTransform()
{
}

void
OCIOCDLTransform::onKnobsLoaded()
{
    op().files.refreshCCCIdMenu();
    op().refreshFromFile();
    OcioNode<OCIOCDLTransformOp>::onKnobsLoaded();
}

bool
OCIOCDLTransform::knobChanged(KnobI* k,
                              ValueChangedReasonEnum reason,
                              ViewSpec view,
                              double time,
                              bool originatedFromMainThread)
{
    bool handled = op().files.knobChanged(k, reason, op().ocio);
    KnobBoolPtr readKnob = op().readFromFile.lock();
    KnobOutputFilePtr exportKnob = op().exportFile.lock();

    if (reason != eValueChangedReasonTimeChanged) {
        if (handled || (readKnob && (k == readKnob.get()))) {
            op().refreshFromFile();
            handled = true;
        } else if (exportKnob && (k == exportKnob.get())) {
            // Only a path the user enters exports, and the knob is emptied after, so the same
            // name can be entered again once the file is moved away.
            const std::string path = exportKnob->getValue();
            if ((reason == eValueChangedReasonUserEdited) && !path.empty()) {
                std::string error;
                if (!op().exportCC(path, time, &error)) {
                    message(eMessageTypeError, error);
                }
                exportKnob->setValue(std::string());
            }
            handled = true;
        }
    }

    return OcioNode<OCIOCDLTransformOp>::knobChanged(k, reason, view, time, originatedFromMainThread) || handled;
}

NATRON_NAMESPACE_EXIT
