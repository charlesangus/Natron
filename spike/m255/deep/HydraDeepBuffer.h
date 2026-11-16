// Stand-in for the deep buffer a Hydra render delegate hands out through
// HdRenderBuffer::GetResource(), held in the VtValue as HydraDeepBufferPtr.
//
// Pixel p (row-major, rows in the order of the delegate's other AOVs) owns the samples
// [offsets[p], offsets[p] + counts[p]) of every channel, sorted front to back. Colour channels
// are premultiplied by the sample's alpha; Z and ZBack are camera-space depths along the view
// axis in scene units, ZBack == Z for surfaces.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct HydraDeepBuffer {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> counts;
    std::vector<uint64_t> offsets;
    std::vector<std::string> channelNames;
    std::vector<std::vector<float>> channels;

    uint64_t totalSamples() const
    {
        return offsets.empty() ? 0 : offsets.back() + counts.back();
    }

    int channelIndex(const std::string& name) const
    {
        for (size_t i = 0; i < channelNames.size(); ++i) {
            if (channelNames[i] == name) {
                return int(i);
            }
        }
        return -1;
    }
};

using HydraDeepBufferPtr = std::shared_ptr<const HydraDeepBuffer>;
