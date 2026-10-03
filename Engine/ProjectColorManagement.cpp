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

#include "ProjectColorManagement.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>
#include <utility>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <OpenColorIO/OpenColorAppHelpers.h>

#include "Engine/Hash64.h"

NATRON_NAMESPACE_ENTER

namespace OCIO = OCIO_NAMESPACE;

namespace {
const char* const kURIPrefix = "ocio://";
const char* const kCustomConfigName = "Custom config";
const char* const kColorPickingRole = "color_picking";

bool
isURI(const std::string& source)
{
    return source.compare(0, std::char_traits<char>::length(kURIPrefix), kURIPrefix) == 0;
}

std::string
fromCString(const char* s)
{
    return s ? std::string(s) : std::string();
}
}

class EightBitDisplayLut {
public:
    static std::shared_ptr<const EightBitDisplayLut> bake(const OCIO::ConstCPUProcessorRcPtr& cpu);

    void apply(const OCIO::CPUProcessor& cpu, float* rgba, int width) const;

private:
    static constexpr int kSize = 65;
    static constexpr int kCells = kSize - 1;

    EightBitDisplayLut();

    float shape(float x) const
    {
        return (x > _break) ? (std::log2(x) - _log2Min) * _invLog2Range : _shapedBreak + (x - _break) * _linearSlope;
    }

    float unshape(float s) const
    {
        return (s > _shapedBreak) ? std::exp2(s * _log2Range + _log2Min) : _break + (s - _shapedBreak) / _linearSlope;
    }

    void interpolate(const float shaped[3], float out[3], int* cell) const;

    bool evaluateGrid(const OCIO::CPUProcessor& cpu);
    void checkCells(const OCIO::CPUProcessor& cpu);

    float _log2Min;
    float _log2Range;
    float _invLog2Range;
    float _break;
    float _shapedBreak;
    float _linearSlope;
    float _domainMin;
    float _domainMax;
    std::vector<float> _lut;
    std::vector<unsigned char> _cellNeedsExact;
};

namespace {
// Pixels go through the LUT only where it stays this close to the processor, so that rounding to
// 8 bits moves them by at most one code value.
const float kEightBitTolerance = 0.35f / 255.f;

// The LUT path costs about 60 ns per pixel; a cheaper processor runs exact.
const double kSlowProcessorNanosecondsPerPixel = 150.;

template <typename F>
void
runInParallel(int count, const F& f)
{
    const int nThreads = std::max(1, std::min(count, (int)std::thread::hardware_concurrency()));
    std::atomic<int> next(0);
    std::vector<std::thread> threads;
    for (int t = 1; t < nThreads; ++t) {
        threads.push_back(std::thread([&]() {
            for (int i = next++; i < count; i = next++) {
                f(i);
            }
        }));
    }
    for (int i = next++; i < count; i = next++) {
        f(i);
    }
    for (std::size_t t = 0; t < threads.size(); ++t) {
        threads[t].join();
    }
}
}

EightBitDisplayLut::EightBitDisplayLut()
    : _log2Min(-10.f)
    , _log2Range(18.f)
    , _invLog2Range(1.f / 18.f)
    , _break(std::exp2(-8.f))
    , _shapedBreak(2.f / 18.f)
    , _linearSlope(1.f / (18.f * std::exp2(-8.f) * std::log(2.f)))
    , _domainMin(0.f)
    , _domainMax(0.f)
    , _lut(3 * kSize * kSize * kSize)
    , _cellNeedsExact(kCells * kCells * kCells, 0)
{
    _domainMin = unshape(0.f);
    _domainMax = unshape(1.f);
}

void
EightBitDisplayLut::interpolate(const float shaped[3],
                                float out[3],
                                int* cell) const
{
    int i[3];
    float f[3];
    for (int k = 0; k < 3; ++k) {
        const float pos = shaped[k] * kCells;
        i[k] = std::min((int)pos, kCells - 1);
        f[k] = pos - i[k];
    }
    *cell = (i[2] * kCells + i[1]) * kCells + i[0];

    const int dr = 3;
    const int dg = 3 * kSize;
    const int db = 3 * kSize * kSize;
    const float* c000 = &_lut[3 * ((i[2] * kSize + i[1]) * kSize + i[0])];
    const float* c111 = c000 + dr + dg + db;
    const float fr = f[0];
    const float fg = f[1];
    const float fb = f[2];
    const float* c1;
    const float* c2;
    float w0, w1, w2, w3;
    if (fr > fg) {
        if (fg > fb) {
            c1 = c000 + dr;
            c2 = c000 + dr + dg;
            w0 = 1.f - fr;
            w1 = fr - fg;
            w2 = fg - fb;
            w3 = fb;
        } else if (fr > fb) {
            c1 = c000 + dr;
            c2 = c000 + dr + db;
            w0 = 1.f - fr;
            w1 = fr - fb;
            w2 = fb - fg;
            w3 = fg;
        } else {
            c1 = c000 + db;
            c2 = c000 + dr + db;
            w0 = 1.f - fb;
            w1 = fb - fr;
            w2 = fr - fg;
            w3 = fg;
        }
    } else {
        if (fb > fg) {
            c1 = c000 + db;
            c2 = c000 + dg + db;
            w0 = 1.f - fb;
            w1 = fb - fg;
            w2 = fg - fr;
            w3 = fr;
        } else if (fb > fr) {
            c1 = c000 + dg;
            c2 = c000 + dg + db;
            w0 = 1.f - fg;
            w1 = fg - fb;
            w2 = fb - fr;
            w3 = fr;
        } else {
            c1 = c000 + dg;
            c2 = c000 + dr + dg;
            w0 = 1.f - fg;
            w1 = fg - fr;
            w2 = fr - fb;
            w3 = fb;
        }
    }
    for (int k = 0; k < 3; ++k) {
        out[k] = w0 * c000[k] + w1 * c1[k] + w2 * c2[k] + w3 * c111[k];
    }
}

bool
EightBitDisplayLut::evaluateGrid(const OCIO::CPUProcessor& cpu)
{
    const int slab = kSize * kSize;
    std::vector<std::vector<float>> buffers(kSize);
    const auto fillSlab = [&](int b) {
        std::vector<float>& buf = buffers[b];
        buf.resize(4 * slab);
        for (int g = 0; g < kSize; ++g) {
            for (int r = 0; r < kSize; ++r) {
                float* p = &buf[4 * (g * kSize + r)];
                p[0] = unshape(r / (float)kCells);
                p[1] = unshape(g / (float)kCells);
                p[2] = unshape(b / (float)kCells);
                p[3] = 1.f;
            }
        }
        OCIO::PackedImageDesc desc(&buf[0], slab, 1, 4);
        cpu.apply(desc);
        float* dst = &_lut[3 * slab * b];
        for (int i = 0; i < slab; ++i) {
            dst[3 * i] = buf[4 * i];
            dst[3 * i + 1] = buf[4 * i + 1];
            dst[3 * i + 2] = buf[4 * i + 2];
        }
        std::vector<float>().swap(buf);
    };

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    fillSlab(0);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    if (ns / slab < kSlowProcessorNanosecondsPerPixel) {
        return false;
    }
    runInParallel(kSize - 1, [&](int i) { fillSlab(i + 1); });

    return true;
}

void
EightBitDisplayLut::checkCells(const OCIO::CPUProcessor& cpu)
{
    // Sample points inside a cell, as fractions of its size: the centre, then one point inside
    // each of the six tetrahedra that the interpolation splits the cell into.
    static const float kSamples[7][3] = {
        { 0.5f, 0.5f, 0.5f },
        { 0.75f, 0.25f, 0.25f },
        { 0.25f, 0.75f, 0.25f },
        { 0.25f, 0.25f, 0.75f },
        { 0.75f, 0.75f, 0.25f },
        { 0.75f, 0.25f, 0.75f },
        { 0.25f, 0.75f, 0.75f }
    };
    const int nSamples = 7;
    const int slab = kCells * kCells;

    runInParallel(kCells, [&](int b) {
        std::vector<float> shapedIn(3 * slab * nSamples);
        std::vector<float> values(4 * slab * nSamples);
        for (int g = 0; g < kCells; ++g) {
            for (int r = 0; r < kCells; ++r) {
                for (int s = 0; s < nSamples; ++s) {
                    const int idx = (g * kCells + r) * nSamples + s;
                    float* sh = &shapedIn[3 * idx];
                    sh[0] = (r + kSamples[s][0]) / kCells;
                    sh[1] = (g + kSamples[s][1]) / kCells;
                    sh[2] = (b + kSamples[s][2]) / kCells;
                    float* p = &values[4 * idx];
                    p[0] = unshape(sh[0]);
                    p[1] = unshape(sh[1]);
                    p[2] = unshape(sh[2]);
                    p[3] = 1.f;
                }
            }
        }
        OCIO::PackedImageDesc desc(&values[0], slab * nSamples, 1, 4);
        cpu.apply(desc);
        for (int c = 0; c < slab; ++c) {
            bool close = true;
            for (int s = 0; s < nSamples && close; ++s) {
                const int idx = c * nSamples + s;
                float approx[3];
                int cell;
                interpolate(&shapedIn[3 * idx], approx, &cell);
                for (int k = 0; k < 3; ++k) {
                    const float e = std::min(std::max(values[4 * idx + k], 0.f), 1.f);
                    const float a = std::min(std::max(approx[k], 0.f), 1.f);
                    if (!(std::fabs(e - a) <= kEightBitTolerance)) {
                        close = false;
                    }
                }
            }
            _cellNeedsExact[b * slab + c] = close ? 0 : 1;
        }
    });
}

std::shared_ptr<const EightBitDisplayLut>
EightBitDisplayLut::bake(const OCIO::ConstCPUProcessorRcPtr& cpu)
{
    if (!cpu || cpu->isNoOp()) {
        return std::shared_ptr<const EightBitDisplayLut>();
    }
    try {
        std::shared_ptr<EightBitDisplayLut> lut(new EightBitDisplayLut);
        if (!lut->evaluateGrid(*cpu)) {
            return std::shared_ptr<const EightBitDisplayLut>();
        }
        lut->checkCells(*cpu);

        return lut;
    } catch (const std::exception& e) {
        qDebug() << "EightBitDisplayLut::bake:" << e.what();

        return std::shared_ptr<const EightBitDisplayLut>();
    }
}

void
EightBitDisplayLut::apply(const OCIO::CPUProcessor& cpu,
                          float* rgba,
                          int width) const
{
    std::vector<int> exactPixels;
    std::vector<float> exactValues;
    for (int x = 0; x < width; ++x) {
        float* p = rgba + 4 * x;
        bool inDomain = true;
        float shaped[3];
        for (int k = 0; k < 3; ++k) {
            if (!(p[k] >= _domainMin && p[k] <= _domainMax)) {
                inDomain = false;
                break;
            }
            shaped[k] = std::min(std::max(shape(p[k]), 0.f), 1.f);
        }
        if (inDomain) {
            float out[3];
            int cell;
            interpolate(shaped, out, &cell);
            if (!_cellNeedsExact[cell]) {
                p[0] = out[0];
                p[1] = out[1];
                p[2] = out[2];
                continue;
            }
        }
        exactPixels.push_back(x);
        exactValues.insert(exactValues.end(), p, p + 4);
    }
    if (exactPixels.empty()) {
        return;
    }
    OCIO::PackedImageDesc desc(&exactValues[0], (long)exactPixels.size(), 1, 4);
    cpu.apply(desc);
    for (std::size_t i = 0; i < exactPixels.size(); ++i) {
        float* p = rgba + 4 * exactPixels[i];
        p[0] = exactValues[4 * i];
        p[1] = exactValues[4 * i + 1];
        p[2] = exactValues[4 * i + 2];
    }
}

ProjectColorManagement::ProjectColorManagement()
    : _mutex()
    , _config()
    , _source()
    , _directory()
    , _workingSpace()
    , _workingToPicking()
    , _pickingToWorking()
    , _displayCache()
    , _conversionCache()
    , _callbacksMutex()
    , _callbacks()
    , _nextCallbackId(1)
{
}

ProjectColorManagement::~ProjectColorManagement()
{
}

ProjectColorManagement::LoadErrorEnum
ProjectColorManagement::load(const std::string& source,
                             const std::string& baseDir,
                             std::string* error)
{
    std::string resolved = source;
    std::string directory;
    if (!isURI(source)) {
        QFileInfo info(QString::fromStdString(source));
        if (info.isRelative() && !baseDir.empty()) {
            info.setFile(QDir(QString::fromStdString(baseDir)), QString::fromStdString(source));
        }
        if (!info.isFile()) {
            if (error) {
                *error = "OpenColorIO config file not found: " + info.absoluteFilePath().toStdString();
            }

            return eLoadErrorNoSuchFile;
        }
        resolved = info.absoluteFilePath().toStdString();
        directory = info.absolutePath().toStdString();
    }

    OCIO::ConstConfigRcPtr config;
    try {
        config = OCIO::Config::CreateFromFile(resolved.c_str());
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }

        return eLoadErrorOCIO;
    }
    if (!config) {
        if (error) {
            *error = "OpenColorIO could not load the config " + resolved;
        }

        return eLoadErrorOCIO;
    }

    {
        QMutexLocker k(&_mutex);
        _config = config;
        _source = resolved;
        _directory = directory;
        _displayCache.clear();
        _conversionCache.clear();
    }
    rebuildPickingProcessors();

    return eLoadErrorNone;
}

std::string
ProjectColorManagement::getConfigSource() const
{
    QMutexLocker k(&_mutex);

    return _source;
}

std::string
ProjectColorManagement::getConfigDirectory() const
{
    QMutexLocker k(&_mutex);

    return _directory;
}

OCIO::ConstConfigRcPtr
ProjectColorManagement::getConfig() const
{
    QMutexLocker k(&_mutex);

    return _config;
}

std::vector<std::string>
ProjectColorManagement::getColorSpaces() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE);
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE, i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getRoles() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumRoles();
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getRoleName(i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getDisplays() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumDisplays();
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getDisplay(i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getViews(const std::string& display) const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumViews(display.c_str());
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getView(display.c_str(), i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getLooks() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumLooks();
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getLookNameByIndex(i)));
    }

    return ret;
}

std::string
ProjectColorManagement::getDefaultDisplay() const
{
    OCIO::ConstConfigRcPtr config = getConfig();

    return config ? fromCString(config->getDefaultDisplay()) : std::string();
}

std::string
ProjectColorManagement::getDefaultView(const std::string& display) const
{
    OCIO::ConstConfigRcPtr config = getConfig();

    return config ? fromCString(config->getDefaultView(display.c_str())) : std::string();
}

std::string
ProjectColorManagement::resolveRoleOrName(const std::string& roleOrName) const
{
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config || roleOrName.empty()) {
        return std::string();
    }
    OCIO::ConstColorSpaceRcPtr cs = config->getColorSpace(roleOrName.c_str());

    return cs ? fromCString(cs->getName()) : std::string();
}

ProjectColorManagement::DisplayProcessorPtr
ProjectColorManagement::getDisplayProcessor(const std::string& src,
                                            const std::string& display,
                                            const std::string& view,
                                            const std::string& look,
                                            std::string* error) const
{
    const std::string key = src + "|" + display + "|" + view + "|" + look;
    OCIO::ConstConfigRcPtr config;
    {
        QMutexLocker k(&_mutex);
        std::map<std::string, DisplayProcessorPtr>::const_iterator it = _displayCache.find(key);
        if (it != _displayCache.end()) {
            return it->second;
        }
        config = _config;
    }
    if (!config) {
        if (error) {
            *error = "No OpenColorIO config is loaded";
        }

        return DisplayProcessorPtr();
    }

    std::shared_ptr<DisplayProcessor> result = std::make_shared<DisplayProcessor>();
    try {
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(src.c_str());
        transform->setDisplay(display.c_str());
        transform->setView(view.c_str());

        OCIO::LegacyViewingPipelineRcPtr pipeline = OCIO::LegacyViewingPipeline::Create();
        pipeline->setDisplayViewTransform(transform);
        if (!look.empty()) {
            pipeline->setLooksOverrideEnabled(true);
            pipeline->setLooksOverride(look.c_str());
        }

        result->processor = pipeline->getProcessor(config);
        result->cpu = result->processor->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32, OCIO::OPTIMIZATION_DEFAULT);
        result->cacheID = result->processor->getCacheID();
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }

        return DisplayProcessorPtr();
    }

    Hash64 hash;
    Hash64_appendQString(&hash, QString::fromStdString(result->cacheID));
    hash.computeHash();
    result->cacheHash = hash.value();

    DisplayProcessorPtr shared = result;
    QMutexLocker k(&_mutex);
    if (_config != config) {
        return shared;
    }
    std::pair<std::map<std::string, DisplayProcessorPtr>::iterator, bool> inserted = _displayCache.insert(std::make_pair(key, shared));

    return inserted.first->second;
}

void
ProjectColorManagement::DisplayProcessor::applyForEightBitOutput(float* rgba,
                                                                 int width) const
{
    if (!cpu || (width <= 0)) {
        return;
    }
    std::call_once(_eightBitLutOnce, [this]() {
        _eightBitLut = EightBitDisplayLut::bake(cpu);
    });
    if (_eightBitLut) {
        _eightBitLut->apply(*cpu, rgba, width);

        return;
    }
    OCIO::PackedImageDesc desc(rgba, width, 1, 4);
    cpu->apply(desc);
}

OCIO::ConstCPUProcessorRcPtr
ProjectColorManagement::getConversionProcessor(const std::string& src,
                                               const std::string& dst) const
{
    const std::string key = src + "|" + dst;
    OCIO::ConstConfigRcPtr config;
    {
        QMutexLocker k(&_mutex);
        std::map<std::string, OCIO::ConstCPUProcessorRcPtr>::const_iterator it = _conversionCache.find(key);
        if (it != _conversionCache.end()) {
            return it->second;
        }
        config = _config;
    }
    if (!config) {
        return OCIO::ConstCPUProcessorRcPtr();
    }

    OCIO::ConstCPUProcessorRcPtr cpu;
    try {
        OCIO::ConstProcessorRcPtr processor = config->getProcessor(src.c_str(), dst.c_str());
        cpu = processor->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32, OCIO::OPTIMIZATION_DEFAULT);
    } catch (const std::exception&) {
        return OCIO::ConstCPUProcessorRcPtr();
    }

    QMutexLocker k(&_mutex);
    if (_config == config) {
        _conversionCache[key] = cpu;
    }

    return cpu;
}

void
ProjectColorManagement::setWorkingSpace(const std::string& name)
{
    {
        QMutexLocker k(&_mutex);
        _workingSpace = name;
    }
    rebuildPickingProcessors();
}

void
ProjectColorManagement::rebuildPickingProcessors()
{
    OCIO::ConstConfigRcPtr config;
    std::string working;
    {
        QMutexLocker k(&_mutex);
        config = _config;
        working = _workingSpace;
    }

    OCIO::ConstCPUProcessorRcPtr forward, backward;
    if (config && !working.empty()) {
        try {
            forward = config->getProcessor(working.c_str(), kColorPickingRole)->getDefaultCPUProcessor();
            backward = config->getProcessor(kColorPickingRole, working.c_str())->getDefaultCPUProcessor();
        } catch (const std::exception&) {
            forward.reset();
            backward.reset();
        }
    }

    QMutexLocker k(&_mutex);
    if ((_config == config) && (_workingSpace == working)) {
        _workingToPicking = forward;
        _pickingToWorking = backward;
    }
}

void
ProjectColorManagement::workingToColorPicking(float* r,
                                              float* g,
                                              float* b) const
{
    OCIO::ConstCPUProcessorRcPtr cpu;
    {
        QMutexLocker k(&_mutex);
        cpu = _workingToPicking;
    }
    if (!cpu) {
        return;
    }
    float rgb[3] = { *r, *g, *b };
    cpu->applyRGB(rgb);
    *r = rgb[0];
    *g = rgb[1];
    *b = rgb[2];
}

void
ProjectColorManagement::colorPickingToWorking(float* r,
                                              float* g,
                                              float* b) const
{
    OCIO::ConstCPUProcessorRcPtr cpu;
    {
        QMutexLocker k(&_mutex);
        cpu = _pickingToWorking;
    }
    if (!cpu) {
        return;
    }
    float rgb[3] = { *r, *g, *b };
    cpu->applyRGB(rgb);
    *r = rgb[0];
    *g = rgb[1];
    *b = rgb[2];
}

std::vector<ChoiceOption>
ProjectColorManagement::builtinConfigOptions()
{
    std::vector<ChoiceOption> ret;
    const OCIO::BuiltinConfigRegistry& registry = OCIO::BuiltinConfigRegistry::Get();
    const size_t n = registry.getNumBuiltinConfigs();
    for (size_t i = 0; i < n; ++i) {
        std::string label = fromCString(registry.getBuiltinConfigUIName(i));
        if (registry.isBuiltinConfigRecommended(i)) {
            label += " (recommended)";
        }
        ret.push_back(ChoiceOption(std::string(kURIPrefix) + fromCString(registry.getBuiltinConfigName(i)), label, std::string()));
    }
    ret.push_back(ChoiceOption(kCustomConfigName));

    return ret;
}

int
ProjectColorManagement::addConfigChangedCallback(const ConfigChangedCallback& cb)
{
    QMutexLocker k(&_callbacksMutex);
    const int id = _nextCallbackId++;
    _callbacks[id] = cb;

    return id;
}

void
ProjectColorManagement::removeConfigChangedCallback(int id)
{
    QMutexLocker k(&_callbacksMutex);
    _callbacks.erase(id);
}

void
ProjectColorManagement::notifyConfigChanged()
{
    std::vector<ConfigChangedCallback> callbacks;
    {
        QMutexLocker k(&_callbacksMutex);
        for (std::map<int, ConfigChangedCallback>::const_iterator it = _callbacks.begin(); it != _callbacks.end(); ++it) {
            callbacks.push_back(it->second);
        }
    }
    for (std::size_t i = 0; i < callbacks.size(); ++i) {
        if (callbacks[i]) {
            callbacks[i]();
        }
    }
}

NATRON_NAMESPACE_EXIT
