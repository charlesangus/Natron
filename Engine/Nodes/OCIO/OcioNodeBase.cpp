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

#include "OcioNodeBase.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <list>
#include <mutex>

#include "Global/FloatingPointExceptions.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/OcioVulkanShader.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

// The name OpenColorIO gives the function the wrapper calls, and so the prefix of its uniform
// block's name.
const char* const kOcioFunction = "natronOcioApply";

// Binding 0 is OpenColorIO's uniform block, its LUT samplers follow, then the wrapper's buffers.
const unsigned kOcioFirstTextureBinding = 1;

// A node keeps the shaders of this many processors, so that a transform animated back and forth
// between a few values, or a view switched back, needs no new extraction.
const std::size_t kGpuProgramsKept = 4;

// Mirrors the wrapper's display block (std140).
struct OcioDisplayBlock {
    float luma[4];
    float gain;
    float invGamma;
    std::int32_t channel;
    // 0 leaves RGB, 1 thresholds it at 1 and 2 raises it to invGamma.
    std::int32_t gammaMode;
};

const char* const kOcioDisplayBlock = "natronDisplay";

// The GPU shader of one processor.
struct OcioGpuProgram {
    // Held, so that the address the program is found by is not reused.
    OCIO_NAMESPACE::ConstProcessorRcPtr processor;
    bool display = false;
    // False when OpenColorIO cannot give a shader, so that it is not asked again.
    bool ok = false;
    GpuRuntimeShader shader;
    // OpenColorIO's uniform block, empty when it has none.
    std::string uniformBlock;
    std::vector<std::byte> uniforms;
    std::vector<GpuShaderTexture> textures;
};

} // namespace

/**
 * @brief The GPU shaders of the processors a node rendered last, most recent first, each built once
 * per processor object: a processor is immutable, and OpenColorIO gives two processors that differ
 * only in a dynamic property's value the same cache ID, so the object, not its ID, identifies the
 * uniform values. Equal processors give equal text, which the pipeline cache compiles once.
 **/
struct OcioNodeKnobs::GpuPrograms {
    std::mutex mutex;
    std::list<std::shared_ptr<const OcioGpuProgram>> recent;
};

#ifdef NATRON_HAVE_GPU
namespace {

std::string
wrapperSource(const std::string& ocioGlsl,
              unsigned firstFreeBinding,
              bool callsOcio,
              bool display)
{
    const std::string src = std::to_string(firstFreeBinding);
    const std::string dst = std::to_string(firstFreeBinding + 1);
    const std::string disp = std::to_string(firstFreeBinding + 2);
    std::string text = "#version 460\n"
                       "layout(local_size_x = 8, local_size_y = 8) in;\n";
    text += ocioGlsl;
    text += "\nlayout(std430, set = 0, binding = " + src + ") readonly buffer Src { float src[]; };\n"
                                                           "layout(std430, set = 0, binding = "
        + dst + ") writeonly buffer Dst { float dst[]; };\n";
    if (display) {
        text += "layout(std140, set = 0, binding = " + disp + ") uniform NatronOcioDisplay { vec4 luma; float gain; float invGamma; int channel; int gammaMode; } " + kOcioDisplayBlock + ";\n";
    }
    text += "layout(push_constant) uniform Region { int x1; int y1; uint width; uint height; uint nComps; uint channelMask; } region;\n"
            "void main() {\n"
            "    if (gl_GlobalInvocationID.x >= region.width || gl_GlobalInvocationID.y >= region.height) return;\n"
            "    uint base = (gl_GlobalInvocationID.y * region.width + gl_GlobalInvocationID.x) * region.nComps;\n"
            "    bool hasAlpha = region.nComps == 4u;\n"
            "    float alpha = hasAlpha ? src[base + 3u] : 1.0;\n"
            "    vec4 p = vec4(src[base], src[base + 1u], src[base + 2u], alpha);\n";
    if (display) {
        const std::string d = kOcioDisplayBlock;
        text += "    p.rgb *= " + d + ".gain;\n"
                                      "    if ("
            + d + ".channel == 1) { p.rgb = vec3(p.r); }\n"
                  "    else if ("
            + d + ".channel == 2) { p.rgb = vec3(p.g); }\n"
                  "    else if ("
            + d + ".channel == 3) { p.rgb = vec3(p.b); }\n"
                  "    else if ("
            + d + ".channel == 4) { p.rgb = vec3(alpha); }\n"
                  "    else if ("
            + d + ".channel == 5) { p.rgb = vec3(" + d + ".luma.r * p.r + " + d + ".luma.g * p.g + " + d + ".luma.b * p.b); }\n";
    }
    if (callsOcio) {
        text += std::string("    p = ") + kOcioFunction + "(p);\n";
    }
    if (display) {
        const std::string d = kOcioDisplayBlock;
        text += "    if (" + d + ".gammaMode == 1) { p.rgb = vec3(greaterThanEqual(p.rgb, vec3(1.0))); }\n"
                                 "    else if ("
            + d + ".gammaMode == 2) { p.rgb = pow(max(p.rgb, vec3(0.0)), vec3(" + d + ".invGamma)); }\n";
    }
    text += "    dst[base] = p.r;\n"
            "    dst[base + 1u] = p.g;\n"
            "    dst[base + 2u] = p.b;\n"
            "    if (hasAlpha) { dst[base + 3u] = alpha; }\n"
            "}\n";

    return text;
}

std::shared_ptr<const OcioGpuProgram>
buildProgram(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
             bool display)
{
    std::shared_ptr<OcioGpuProgram> program = std::make_shared<OcioGpuProgram>();
    program->processor = processor;
    program->display = display;
    if (!processor) {
        program->shader.source = wrapperSource(std::string(), kOcioFirstTextureBinding, false, display);
        program->ok = true;

        return program;
    }
    OcioVulkanShader shader;
    std::string error;
    try {
        const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
        if (!OcioVulkanShader::build(processor->getDefaultGPUProcessor(), kOcioFunction, kOcioFirstTextureBinding, &shader, &error)) {
            return program;
        }
    } catch (const std::exception&) {
        return program;
    }
    if (shader.uniformBytes() > 0) {
        program->uniformBlock = shader.uniformBlockName();
        if (program->uniformBlock.empty() || !shader.fillUniforms(program->uniforms)) {
            return program;
        }
    }
    unsigned firstFree = kOcioFirstTextureBinding;
    for (const OcioVulkanShader::Texture& t : shader.textures()) {
        GpuShaderTexture texture;
        texture.binding = t.sampler;
        texture.cacheKey = t.cacheKey;
        texture.dimensions = (int)t.desc.dimensions;
        texture.width = (int)t.desc.width;
        texture.height = (int)t.desc.height;
        texture.depth = (int)t.desc.depth;
        texture.channels = (int)t.desc.channels;
        texture.linear = t.desc.filter == gpu::GpuTextureFilter::Linear;
        texture.texels = t.texels;
        program->textures.push_back(texture);
        firstFree = std::max(firstFree, t.binding + 1);
    }
    program->shader.source = wrapperSource(shader.glsl(), firstFree, true, display);
    program->ok = true;

    return program;
}

} // namespace
#endif // NATRON_HAVE_GPU

GpuOpRef
OcioNodeKnobs::gpu(const OcioProcessor& processor,
                   const OcioDisplayAdjust* display) const
{
#ifdef NATRON_HAVE_GPU
    if (!processor.error.empty() || (!processor.processor && !display)) {
        return GpuOpRef::none();
    }
    std::shared_ptr<const OcioGpuProgram> program;
    {
        std::lock_guard<std::mutex> lock(_gpuPrograms->mutex);
        std::list<std::shared_ptr<const OcioGpuProgram>>& recent = _gpuPrograms->recent;
        for (std::list<std::shared_ptr<const OcioGpuProgram>>::iterator it = recent.begin(); it != recent.end(); ++it) {
            if (((*it)->processor == processor.processor) && ((*it)->display == (display != nullptr))) {
                program = *it;
                recent.splice(recent.begin(), recent, it);
                break;
            }
        }
        if (!program) {
            program = buildProgram(processor.processor, display != nullptr);
            recent.push_front(program);
            if (recent.size() > kGpuProgramsKept) {
                recent.pop_back();
            }
        }
    }
    if (!program->ok) {
        return GpuOpRef::none();
    }
    GpuShaderInputs inputs;
    if (!program->uniformBlock.empty()) {
        GpuShaderUniform uniform;
        uniform.binding = program->uniformBlock;
        uniform.data = program->uniforms;
        inputs.uniforms.push_back(uniform);
    }
    if (display) {
        OcioDisplayBlock block;
        for (int c = 0; c < 3; ++c) {
            block.luma[c] = display->luma[c];
        }
        block.luma[3] = 0.f;
        block.gain = display->gain;
        block.invGamma = (display->gamma > 0.) ? (float)(1. / display->gamma) : 0.f;
        block.channel = display->channel;
        block.gammaMode = (display->gamma <= 0.) ? 1 : ((display->gamma != 1.) ? 2 : 0);
        inputs.addUniform(kOcioDisplayBlock, block);
    }
    inputs.textures = program->textures;
    GpuOpRef ref = GpuOpRef::runtime(program->shader, inputs);
    ref.acceptedComponents = GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4);

    return ref;
#else
    (void)processor;
    (void)display;

    return GpuOpRef::none();
#endif
}

OcioNodeKnobs::OcioNodeKnobs()
    : _holder(nullptr)
    , _spaces()
    , _menuRefreshers()
    , _watched()
    , _watchId(-1)
    , _gpuPrograms(new GpuPrograms())
{
}

OcioNodeKnobs::~OcioNodeKnobs()
{
    unwatchConfig();
}

void
OcioNodeKnobs::attach(KnobHolder* holder)
{
    _holder = holder;
}

void
OcioNodeKnobs::addSpace(KnobHolder* holder,
                        const KnobPagePtr& page,
                        const OcioSpaceKnobs::Description& description,
                        const OcioSpaceKnobs::AutomaticSpace& automaticSpace,
                        const OcioSpaceKnobs::ChoiceMade& choiceMade)
{
    _holder = holder;
    std::unique_ptr<OcioSpaceKnobs> knobs(new OcioSpaceKnobs());
    if (_spaces.empty()) {
        knobs->create(holder, page, description, automaticSpace, choiceMade);
    } else {
        knobs->createSharing(*_spaces.front(), holder, page, description, automaticSpace, choiceMade);
    }
    _spaces.push_back(std::move(knobs));
}

std::size_t
OcioNodeKnobs::spaceCount() const
{
    return _spaces.size();
}

std::string
OcioNodeKnobs::space(std::size_t index) const
{
    return index < _spaces.size() ? _spaces[index]->space() : std::string();
}

std::string
OcioNodeKnobs::spaceOrWorking(std::size_t index) const
{
    const std::string name = space(index);

    return name.empty() ? workingSpace() : name;
}

void
OcioNodeKnobs::addKnob(const KnobPagePtr& page,
                       const KnobIPtr& knob)
{
    KnobGroupPtr context = _spaces.empty() ? KnobGroupPtr() : _spaces.front()->contextGroup();
    if (context) {
        const std::vector<KnobIPtr> children = page->getChildren();
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (children[i] == context) {
                page->insertKnob((int)i, knob);

                return;
            }
        }
    }
    page->addKnob(knob);
}

std::string
OcioNodeKnobs::workingSpace() const
{
    if (!_spaces.empty()) {
        return _spaces.front()->workingSpace();
    }
    ProjectPtr p = project();

    return p ? p->getWorkingColorSpace() : std::string();
}

std::vector<std::pair<std::string, std::string>>
OcioNodeKnobs::contextVariables(double time) const
{
    return _spaces.empty() ? std::vector<std::pair<std::string, std::string>>() : _spaces.front()->contextVariables(time);
}

bool
OcioNodeKnobs::knobChanged(KnobI* k,
                           ValueChangedReasonEnum reason)
{
    bool handled = false;

    // Every space refreshes on the shared config knobs, so none may stop the others.
    for (std::size_t i = 0; i < _spaces.size(); ++i) {
        if (_spaces[i]->knobChanged(k, reason)) {
            handled = true;
        }
    }

    return handled;
}

void
OcioNodeKnobs::refreshMenus()
{
    for (std::size_t i = 0; i < _spaces.size(); ++i) {
        _spaces[i]->refreshMenu();
    }
    for (std::size_t i = 0; i < _menuRefreshers.size(); ++i) {
        _menuRefreshers[i]();
    }
}

void
OcioNodeKnobs::sourceChanged()
{
    for (std::size_t i = 0; i < _spaces.size(); ++i) {
        _spaces[i]->sourceChanged();
    }
}

void
OcioNodeKnobs::addMenuRefresher(const std::function<void()>& refresher)
{
    _menuRefreshers.push_back(refresher);
}

ProjectColorManagementPtr
OcioNodeKnobs::colorManagement() const
{
    ProjectPtr p = project();

    return p ? p->getColorManagement() : ProjectColorManagementPtr();
}

KnobChoicePtr
OcioNodeKnobs::createDirectionKnob(KnobHolder* holder)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, tr("Direction").toStdString());
    knob->setName("direction");
    knob->setHintToolTip(tr("Forward applies the transform, Inverse undoes it.").toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("forward", tr("Forward").toStdString(), std::string()));
    options.push_back(ChoiceOption("inverse", tr("Inverse").toStdString(), std::string()));
    knob->populateChoices(options);
    knob->setDefaultValue(0);

    return knob;
}

OCIO_NAMESPACE::TransformDirection
OcioNodeKnobs::direction(int index)
{
    return (index == 1) ? OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE : OCIO_NAMESPACE::TRANSFORM_DIR_FORWARD;
}

void
OcioNodeKnobs::populateMenu(const KnobChoicePtr& menu,
                            const std::vector<std::string>& names)
{
    if (!menu) {
        return;
    }
    std::vector<ChoiceOption> options;
    options.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        options.push_back(ChoiceOption(names[i]));
    }
    menu->populateChoices(options);
}

void
OcioNodeKnobs::reloadFile(const std::string& path) const
{
    if (ProjectColorManagementPtr cm = colorManagement()) {
        cm->reloadFile(path);
    }
}

ProjectPtr
OcioNodeKnobs::project() const
{
    AppInstancePtr app = _holder ? _holder->getApp() : AppInstancePtr();

    return app ? app->getProject() : ProjectPtr();
}

OCIO_NAMESPACE::ConstTransformRcPtr
OcioNodeKnobs::spaceTransform(const std::string& from,
                              const std::string& to) const
{
    if (from.empty() || to.empty() || (from == to)) {
        return OCIO_NAMESPACE::ConstTransformRcPtr();
    }
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();
    const OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement ? colorManagement->getConfig() : OCIO_NAMESPACE::ConstConfigRcPtr();
    if (config) {
        // A role or alias names the same space as the space itself.
        const std::string resolvedFrom = IoColorSpace::nameInConfig(config, from);
        if (!resolvedFrom.empty() && (resolvedFrom == IoColorSpace::nameInConfig(config, to))) {
            return OCIO_NAMESPACE::ConstTransformRcPtr();
        }
    }
    OCIO_NAMESPACE::ColorSpaceTransformRcPtr transform = OCIO_NAMESPACE::ColorSpaceTransform::Create();
    transform->setSrc(from.c_str());
    transform->setDst(to.c_str());

    return transform;
}

OcioProcessor
OcioNodeKnobs::processor(const OCIO_NAMESPACE::ConstTransformRcPtr& transform,
                         double time) const
{
    OcioProcessor result;

    if (!transform) {
        return result;
    }
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();
    if (!colorManagement) {
        result.error = tr("No project to take the OpenColorIO config from.").toStdString();

        return result;
    }
    std::string error;
    if (p->getOCIOConfigError(&error)) {
        result.error = error;

        return result;
    }
    const ProjectColorManagement::TransformProcessorPtr built = colorManagement->getTransformProcessor(transform, contextVariables(time), &error);
    if (!built || !built->cpu) {
        result.error = tr("Cannot build the OpenColorIO transform: %1").arg(QString::fromStdString(error)).toStdString();

        return result;
    }
    if (!built->cpu->isNoOp()) {
        result.processor = built->processor;
        result.cpu = built->cpu;
    }

    return result;
}

void
OcioNodeKnobs::watchConfig(const std::function<void()>& callback)
{
    unwatchConfig();
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();
    if (!colorManagement) {
        return;
    }
    _watchId = colorManagement->addConfigChangedCallback(callback);
    _watched = colorManagement;
}

void
OcioNodeKnobs::unwatchConfig()
{
    ProjectColorManagementPtr colorManagement = _watched.lock();

    if (colorManagement && (_watchId >= 0)) {
        colorManagement->removeConfigChangedCallback(_watchId);
    }
    _watched.reset();
    _watchId = -1;
}

void
OcioNodeKnobs::applyRow(const OcioProcessor& processor,
                        const RowIO& io)
{
    if (io.dst != io.src[0]) {
        std::memcpy(io.dst, io.src[0], sizeof(float) * io.width * io.nComps);
    }
    if (!processor.cpu || (io.width <= 0) || ((io.nComps != 3) && (io.nComps != 4))) {
        return;
    }
    // Pixel by pixel in place: CPUProcessor::apply() builds image helpers per call, which a row
    // must not allocate. RGB only, so alpha is left as it is.
    try {
        const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
        const OCIO_NAMESPACE::CPUProcessor& cpu = *processor.cpu;
        float* pixel = io.dst;
        for (int x = 0; x < io.width; ++x, pixel += io.nComps) {
            cpu.applyRGB(pixel);
        }
    } catch (const std::exception&) {
        // A row that fails keeps the source, as the node does when no processor can be built.
        if (io.dst != io.src[0]) {
            std::memcpy(io.dst, io.src[0], sizeof(float) * io.width * io.nComps);
        }
    }
}

NATRON_NAMESPACE_EXIT
