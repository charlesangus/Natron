// FastNoise2 node setup behind the C interface in fn2shim.h. Compiled once per floating-point
// mode with FN2SPIKE_MODE set to strict or relaxed.

#include "fn2shim.h"

#include <FastNoise/FastNoise.h>

#include <cstring>

#define FN2SPIKE_CAT2(a, b, c) a##b##c
#define FN2SPIKE_CAT(a, b, c) FN2SPIKE_CAT2(a, b, c)
#define FN2SPIKE_FN(name) FN2SPIKE_CAT(fn2spike_, FN2SPIKE_MODE, name)

namespace {

struct Handle {
    FastNoise::SmartNode<> root;
};

FastSIMD::FeatureSet
parseSimd(const char* s)
{
    if (!s || !std::strcmp(s, "max"))
        return FastSIMD::FeatureSet::Max;
    if (!std::strcmp(s, "scalar"))
        return FastSIMD::FeatureSet::SCALAR;
    if (!std::strcmp(s, "sse2"))
        return FastSIMD::FeatureSet::SSE2;
    if (!std::strcmp(s, "sse41"))
        return FastSIMD::FeatureSet::SSE41;
    if (!std::strcmp(s, "avx2"))
        return FastSIMD::FeatureSet::AVX2;
    if (!std::strcmp(s, "avx512"))
        return FastSIMD::FeatureSet::AVX512;
    return FastSIMD::FeatureSet::Invalid;
}

template <typename T>
FastNoise::SmartNode<T>
makeScaled(FastSIMD::FeatureSet fs, float featureScale)
{
    auto node = FastNoise::New<T>(fs);
    if (node)
        node->SetScale(featureScale);
    return node;
}

} // namespace

extern "C" {

void*
FN2SPIKE_FN(_create)(int type, float featureScale, int octaves, float lacunarity, float gain, const char* maxSimd)
{
    const FastSIMD::FeatureSet fs = parseSimd(maxSimd);
    if (fs == FastSIMD::FeatureSet::Invalid)
        return nullptr;

    FastNoise::SmartNode<> source;
    switch (type) {
    case 0:
        source = makeScaled<FastNoise::Perlin>(fs, featureScale);
        break;
    case 1:
        source = makeScaled<FastNoise::SuperSimplex>(fs, featureScale);
        break;
    case 2: {
        auto cellular = makeScaled<FastNoise::CellularDistance>(fs, featureScale);
        if (cellular) {
            cellular->SetDistanceFunction(FastNoise::DistanceFunction::Euclidean);
            cellular->SetReturnType(FastNoise::CellularDistance::ReturnType::Index0);
        }
        source = cellular;
        break;
    }
    default:
        return nullptr;
    }
    if (!source)
        return nullptr;

    auto* h = new Handle;
    if (octaves <= 1) {
        h->root = source;
    } else {
        auto fbm = FastNoise::New<FastNoise::FractalFBm>(fs);
        if (!fbm) {
            delete h;
            return nullptr;
        }
        fbm->SetSource(source);
        fbm->SetOctaveCount(octaves);
        fbm->SetLacunarity(lacunarity);
        fbm->SetGain(gain);
        h->root = fbm;
    }
    return h;
}

void
FN2SPIKE_FN(_destroy)(void* handle)
{
    delete static_cast<Handle*>(handle);
}

void
FN2SPIKE_FN(_row)(void* handle, float* out, int count, const float* xs, const float* ys, const float* zs,
                  const float* ws, int seed)
{
    static_cast<Handle*>(handle)->root->GenPositionArray4D(out, count, xs, ys, zs, ws, 0.0f, 0.0f, 0.0f, 0.0f, seed);
}

const char*
FN2SPIKE_FN(_simd)(void* handle)
{
    return FastSIMD::GetFeatureSetString(static_cast<Handle*>(handle)->root->GetActiveFeatureSet());
}
}
