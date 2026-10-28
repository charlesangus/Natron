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

#ifndef Engine_Nodes_Image_PassExecutor_h
#define Engine_Nodes_Image_PassExecutor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/HostImageMemory.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#endif

NATRON_NAMESPACE_ENTER

struct StagePlan;

/**
 * @brief The planar-buffer plumbing of the Passes executor, ported from openfx-misc's
 * CImgFilterPluginHelper: the source over a planar buffer, whole-line parallelism, and the write
 * back with premultiply, mask and mix.
 **/
namespace PassExec {
// How many lines a worker filters or writes between two abort checks.
const int kAbortCheckLines = 16;

/**
 * @brief The index in an nComps-channel pixel of the channel on colour bit `bit`, or -1.
 **/
int channelIndexForBit(int nComps, int bit) WARN_UNUSED_RETURN;

/**
 * @brief The bits processed in a dstNComps-channel image holding `plane`. A one-channel plane
 * rendered into a wider image sits where EffectInstance's copy back reads it: alpha for a colour
 * plane, channel 0 otherwise.
 **/
std::bitset<4> processedBitsForImage(const ImageLayerDesc& plane, int dstNComps, const std::bitset<4>& planeBits) WARN_UNUSED_RETURN;

bool isFloatImage(const ImagePtr& image) WARN_UNUSED_RETURN;

bool isEmptyRect(const RectI& r) WARN_UNUSED_RETURN;

bool isEmptyRect(const RectD& r) WARN_UNUSED_RETURN;

/**
 * @brief OFX::Coords::toPixelEnclosing().
 **/
RectI toPixelEnclosing(const RectD& r, const RenderScale& scale, double par) WARN_UNUSED_RETURN;

/**
 * @brief OFX::Coords::toCanonical().
 **/
RectD toCanonical(const RectI& r, const RenderScale& scale, double par) WARN_UNUSED_RETURN;

/**
 * @brief OFX::Coords::rectIntersection(): rectangles that merely touch still intersect, and a
 * failed intersection leaves an empty rectangle at the origin.
 **/
bool intersectRects(const RectI& a, const RectI& b, RectI* out);

/**
 * @brief Copies `width` pixels of row y starting at x0 into row (nComps floats per pixel),
 * writing zero wherever the image has no pixel. An image in another layout is mapped channel by
 * colour bit, a channel it lacks reading as zero.
 **/
void readSourceRow(const Image* image,
                   const Image::ReadAccess* access,
                   const RectI& bounds,
                   int srcNComps,
                   int x0,
                   int y,
                   int width,
                   int nComps,
                   float* row);

/**
 * @brief One value per pixel of `channel` of row y, `fill` wherever the image has no pixel.
 **/
void readChannelRow(const Image* image,
                    const Image::ReadAccess* access,
                    const RectI& bounds,
                    int imageNComps,
                    int channel,
                    int x0,
                    int y,
                    int width,
                    float fill,
                    float* row);

/**
 * @brief One output plane of a render with the images it reads. Image::getBounds() takes the
 * image's lock, which a pool thread must never ask for: behind a writer waiting on an image this
 * render holds for reading, it blocks forever. lock() reads the bounds and takes the accesses on
 * the calling thread, and the pool threads only compute pixel addresses through them.
 **/
struct PlaneJob {
    ImagePtr dst;
    std::bitset<4> channels;
    ImagePtr src;
    ImagePtr divisor;
    int divisorChannel;
    int skipChannel;
    int nComps;
    int srcNComps;
    int divisorNComps;
    RectI srcBounds;
    RectI divisorBounds;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::ReadAccess> divisorAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;

    PlaneJob();

    void lock();
};

/**
 * @brief The mask a render reads, locked on the calling thread like PlaneJob. With no image, an
 * applied mask reads as zero everywhere.
 **/
struct MaskInput {
    bool applied;
    bool invert;
    ImagePtr image;
    int channel;
    int nComps;
    RectI bounds;
    std::shared_ptr<Image::ReadAccess> access;

    MaskInput();

    void lock();
};

/**
 * @brief The processed channels of job, one buffer plane each in channel order, the way
 * CImgFilterPluginHelper extracts them. planeOfChannel maps a channel to its plane or -1.
 **/
struct ProcessedPlanes {
    std::vector<int> channel;
    int planeOfChannel[4];

    explicit ProcessedPlanes(const PlaneJob& job);

    int count() const WARN_UNUSED_RETURN
    {
        return (int)channel.size();
    }
};

/**
 * @brief How nLines lines of pixelsPerLine pixels each are split for nThreads threads:
 * consecutive bands of linesPerBand lines, the last possibly shorter, count in all. One band when
 * nThreads is 1 or the work is too small to be worth sharing; no band when nLines is not positive.
 **/
struct LineBands {
    int linesPerBand = 0;
    int count = 0;
};

LineBands splitLines(int nLines, std::size_t pixelsPerLine, int nThreads) WARN_UNUSED_RETURN;

/**
 * @brief Runs body(first, end) over [0, nLines) split by splitLines() on the global pool,
 * stopping once cancel is set. Each line is processed whole by one call, so the result does not
 * depend on the split.
 **/
void forEachLineChunk(int nLines,
                      std::size_t pixelsPerLine,
                      int nThreads,
                      RenderCancellation& cancel,
                      const std::function<void(int, int)>& body);

/**
 * @brief How the source reads outside its image when filling the buffer.
 **/
enum class Boundary {
    // Zero (Dirichlet).
    Zero,
    // The nearest pixel of the image (Neumann).
    Nearest
};

/**
 * @brief Fills buffer, one bufferRect-sized plane per processed channel, with the source of job,
 * outside its image by boundary, divided by the "(Un)premult by" channel where that is usable.
 **/
void fillBuffer(const PlaneJob& job,
                const ProcessedPlanes& planes,
                const RectI& bufferRect,
                Boundary boundary,
                int nThreads,
                RenderCancellation& cancel,
                float* buffer);

/**
 * @brief What writeWindow() writes: roi, of which processWindow (when processing) is taken from
 * the planar buffer over bufferRect and the rest passed through from the source.
 **/
struct WindowLayout {
    RectI roi;
    RectI processWindow;
    bool processing;
    RectI bufferRect;
    const float* buffer;
    float mix;
};

/**
 * @brief Writes job's window in row bands: each processed channel of the processed window is
 * the buffer value multiplied back by the divisor, then blended with the undivided
 * source by mask x mix; unprocessed channels and the pixels outside the processed window pass
 * through. A buffer value outside bufferRect reads as zero.
 **/
void writeWindow(const PlaneJob& job,
                 const ProcessedPlanes& planes,
                 const MaskInput& mask,
                 const WindowLayout& layout,
                 int nThreads,
                 RenderCancellation& cancel);

/**
 * @brief Whether view holds nComps floats per pixel at a float-aligned offset, rows at least as
 * long as its bounds.
 **/
bool isUsableGpuView(const GpuImageView& view, int nComps) WARN_UNUSED_RETURN;

/**
 * @brief The part of view over r, which lies within view's bounds; an empty view when r is empty.
 **/
GpuImageView cropGpuView(const GpuImageView& view, const RectI& r) WARN_UNUSED_RETURN;

#ifdef NATRON_HAVE_GPU
gpu::GpuBlurRect toGpuBlurRect(const RectI& r) WARN_UNUSED_RETURN;

gpu::GpuBlurImage toGpuBlurImage(const GpuImageView& view) WARN_UNUSED_RETURN;
#endif
} // namespace PassExec

/**
 * @brief What one pass of a Passes op computes over the planar scratch buffers.
 *
 * LineX and LineY filter every row, or every column, of one scratch slot in place, each line
 * whole. Gather computes each output pixel from a neighbourhood of the buffers, Point from the same
 * pixel of them. Resample and Downsample change the sampling grid; they are declared for the
 * resampling ops and neither executor runs them yet.
 **/
enum class PassKind : unsigned char {
    LineX,
    LineY,
    Gather,
    Point,
    Resample,
    Downsample
};

/**
 * @brief The scratch buffers a pass reads and writes: nPlanes planes of width x height floats
 * over rect per slot, slot 0 holding the divided source when the passes start. channelBits holds
 * the channel of each plane, in the bit space of pixelKernelChannelBit() for an nComps-channel
 * image, for a pass that combines channels. mask is set for a list with opMask whose mask is
 * applied: one plane of width x height floats over rect, the mask's channel there and 0 outside
 * the mask's image; null when no mask is applied, which reads as 1 everywhere.
 **/
struct PassScratch {
    RectI rect;
    int width = 0;
    int height = 0;
    int nPlanes = 0;
    std::size_t planeSize = 0;
    std::vector<float*> buffers;
    int nComps = 0;
    std::vector<int> channelBits;
    const float* mask = nullptr;

    float* plane(int slot,
                 int p) const
    {
        return buffers[slot] + (std::size_t)p * planeSize;
    }
};

/**
 * @brief Filters count adjacent lines of length samples, consecutive samples stride floats
 * apart and adjacent lines one float apart. count is 1 except for a LineY pass with a column
 * block.
 **/
using PassLineFn = std::function<void(float* line, int length, std::ptrdiff_t stride, int count)>;

/**
 * @brief Computes one line of a Gather or Point pass: a row of one plane (line = plane x height
 * + row) for a per-plane pass, a row of every plane otherwise. For a pass over columns, a column
 * instead: line = plane x width + column, or the column of every plane.
 **/
using PassRowFn = std::function<void(int line)>;

/**
 * @brief One pass of a pass list.
 *
 * haloX and haloY are how far, in pixels, the pass reads beyond what it writes, and growX and
 * growY how far it spreads the source's region of definition; both add up over the list. A pass
 * with no work only contributes them.
 *
 * The executor calls lineWorker (LineX, LineY) or rowWorker (Gather, Point) once per chunk of
 * lines a thread takes, so the function it returns may own scratch memory it reuses across the
 * chunk's lines; a chunk is only ever run by one thread.
 **/
struct Pass {
    PassKind kind = PassKind::Point;
    int slot = 0;
    int haloX = 0;
    int haloY = 0;
    int growX = 0;
    int growY = 0;
    // Samples read per output sample, which weighs the split of the lines between threads.
    std::size_t tapsPerSample = 1;
    // For LineY, the number of adjacent columns one call filters, so that it reads the plane row
    // by row; 1 filters each column alone.
    int columnBlock = 1;
    bool perPlane = true;
    // For Gather and Point, whether the lines are columns rather than rows, for a pass that runs
    // down the columns, such as the column sums of a summed-area table.
    bool columns = false;
    // What gpuCostHint() reports for a list holding this pass: sigma as GpuCostHint defines it.
    double sigma = 0.;
    bool recursive = false;
    std::function<PassLineFn()> lineWorker;
    std::function<PassRowFn(const PassScratch&)> rowWorker;

    bool hasWork() const WARN_UNUSED_RETURN
    {
        return lineWorker || rowWorker;
    }

    static Pass lineX(int halo, int grow, std::function<PassLineFn()> worker) WARN_UNUSED_RETURN;
    static Pass lineY(int halo, int grow, std::function<PassLineFn()> worker) WARN_UNUSED_RETURN;
    static Pass gather(int haloX, int haloY, bool perPlane, std::function<PassRowFn(const PassScratch&)> worker) WARN_UNUSED_RETURN;
    static Pass point(bool perPlane, std::function<PassRowFn(const PassScratch&)> worker) WARN_UNUSED_RETURN;
};

/**
 * @brief Everything a pass list's GPU recorder reads and writes. source holds the source over
 * at least sourceValid, outside which it reads zero, divided by the divisor when the Premult
 * stage has one. The recorder writes filtered over output, the passes running over domain: output
 * grown by the halo and clipped to rod, this effect's region of definition. Dispatches the
 * recorder makes in sequence must be separated by compute barriers; the executor orders its own.
 * inputs holds the effect's other inputs by input number, each cropped to that input's region of
 * definition, outside which it reads zero; an input that is disconnected, not read on the GPU or
 * empty there, and the source's own entry, are views without a buffer.
 *
 * For a list with opMask, maskApplied is whether the mask is connected and enabled; mask is then
 * its view, maskComponent the component of it that holds the mask's channel, and the mask reads 0
 * outside mask.bounds, everywhere when mask has no buffer or maskComponent is -1. Without an
 * applied mask, the mask reads 1 everywhere.
 **/
struct PassGpuIO {
    gpu::GpuRecordContext& context;
    GpuImageView source;
    RectI sourceValid;
    GpuImageView filtered;
    RectI output;
    RectI domain;
    RectI rod;
    int haloX = 0;
    int haloY = 0;
    int nComps = 0;
    // The processed channels, in the bit space of pixelKernelChannelBit(); the finish restores the
    // others from the source, but a recorder that combines channels must leave them out.
    std::bitset<4> channels;
    std::vector<GpuImageView> inputs;
    // The layers getGpuExtraInputLayers() adds, each cropped to its input's region of definition.
    std::vector<GpuLayerInput> layerInputs;
    bool maskApplied = false;
    GpuImageView mask;
    int maskComponent = -1;

    explicit PassGpuIO(gpu::GpuRecordContext& ctx)
        : context(ctx)
    {
    }
};

using PassGpuRecorder = std::function<StatusEnum(const PassGpuIO&)>;

/**
 * @brief What a Passes op computes for one render: passes in order over scratchSlots scratch
 * buffers, the result read from resultSlot. boundary is how slot 0 reads the source outside its
 * image. gpu records the whole list on the GPU; without it the op stays on the CPU.
 *
 * opMask is for an op that applies its mask itself, inside the passes, rather than the MaskMix
 * stage blending its output with the source: the executors hand the mask over in PassScratch::mask
 * and PassGpuIO::mask, over the source's region of interest. The op's pipeline must not have the
 * MaskMix stage.
 **/
struct PassList {
    std::vector<Pass> passes;
    PassExec::Boundary boundary = PassExec::Boundary::Zero;
    int scratchSlots = 1;
    int resultSlot = 0;
    bool opMask = false;
    PassGpuRecorder gpu;

    bool empty() const WARN_UNUSED_RETURN
    {
        return passes.empty();
    }
};

/**
 * @brief What a Passes op's passes() is built for: the scale the render runs at, and the pixel
 * aspect ratio of the op's source, which turns a horizontal size in canonical units into pixels.
 **/
struct PassContext {
    RenderScale scale;
    double pixelAspectRatio = 1.;
};

/**
 * @brief How far a pass list reads beyond its output (halo) and spreads the source's region of
 * definition (grow), the sums over its passes.
 **/
struct PassExtent {
    int haloX = 0;
    int haloY = 0;
    int growX = 0;
    int growY = 0;
};

PassExtent passListExtent(const PassList& list) WARN_UNUSED_RETURN;

/**
 * @brief The pixels list reads to produce rect: rect grown by the halo.
 **/
RectI passListSourceRoI(const RectI& rect, const PassList& list) WARN_UNUSED_RETURN;

/**
 * @brief The region of definition list gives a source whose region is srcRoD: srcRoD grown by
 * the grow sums, unchanged when it is empty.
 **/
RectI passListRoD(const RectI& srcRoD, const PassList& list) WARN_UNUSED_RETURN;

/**
 * @brief The largest sigma of the list's passes and whether any is recursive; empty for a list
 * with no pass reporting a sigma.
 **/
std::optional<GpuCostHint> passListCostHint(const PassList& list) WARN_UNUSED_RETURN;

bool isRunnablePassList(const PassList& list) WARN_UNUSED_RETURN;

/**
 * @brief The scratch sets live at once, counted by ScratchSetHolder. A set is counted from its
 * holder's construction until its buffers are back in the pool or freed.
 **/
struct ScratchSetRegistry {
    std::mutex mutex;
    int active = 0;
    /// Run between deciding a set's fate and releasing it, so that a test can start another
    /// holder in that window.
    std::function<void()> beforeRelease;

    static ScratchSetRegistry& global();
};

/**
 * @brief Marks one runPassesCpu() call's scratch as live for its lifetime, and releases it when
 * it ends. When it ends while another call's scratch is still live, as when two frames render at
 * once, the scratch goes back to the OS instead of the pool, so that overlapping renders do not
 * leave two sets resident. A lone call pools its scratch for the next render; no holder can
 * register until that is done, so the next one finds the pooled set rather than allocating
 * another. Declare it after the storages it covers.
 **/
class ScratchSetHolder {
public:
    ScratchSetHolder(HostScratchFloats& source,
                     std::vector<HostScratchFloats>& extra,
                     ScratchSetRegistry& registry);
    ~ScratchSetHolder();

    ScratchSetHolder(const ScratchSetHolder&) = delete;
    ScratchSetHolder& operator=(const ScratchSetHolder&) = delete;

private:
    HostScratchFloats& _source;
    std::vector<HostScratchFloats>& _extra;
    ScratchSetRegistry& _registry;
};

/**
 * @brief Renders args through plan with list on the CPU.
 *
 * The images are fetched and locked as runStagesCpu() does. The window, empty with a mix of 0,
 * grown by the halo and clipped to effect's region of definition, is the scratch rect: slot 0 is
 * filled with the source there (fillBuffer()), the passes run in order, each over whole lines
 * split between threads, and the window is written from resultSlot (writeWindow()). Fails on a
 * list isRunnablePassList() refuses.
 **/
StatusEnum runPassesCpu(EffectInstance& effect,
                        const StagePlan& plan,
                        const EffectInstance::RenderActionArgs& args,
                        const PassList& list) WARN_UNUSED_RETURN;

/**
 * @brief Runs the passes of list in order over scratch, each over whole lines split between
 * threads, as runPassesCpu() runs them after filling slot 0: for an op that runs another node's
 * passes on a buffer of its own. The caller sizes every slot and fills slot 0; the list must be
 * runnable (isRunnablePassList()). Stops between passes once cancel is set.
 **/
void runPassListOnScratch(const PassList& list,
                          const PassScratch& scratch,
                          int nThreads,
                          RenderCancellation& cancel);

/**
 * @brief Whether recordPassesGpu() can run list for query through plan: canRunStagesGpu(), at
 * most 4 channels, a runnable list and, unless it is empty, a GPU recorder.
 **/
bool canRecordPassesGpu(const StagePlan& plan,
                        const GpuQuery& query,
                        const PassList& list) WARN_UNUSED_RETURN;

/**
 * @brief Records list over args.outputRect of args.output through plan, as runPassesCpu()
 * renders it. The source is divided into a scratch buffer over the domain only when the Premult
 * stage resolves a divisor; the list's recorder writes the filtered window into a scratch buffer,
 * or straight into the output when no stage changes it, and recordStagesGpuFinish() multiplies
 * back, masks and mixes. An empty list, or a mix of 0, finishes from the divided source over the
 * window.
 **/
StatusEnum recordPassesGpu(EffectInstance& effect,
                           const StagePlan& plan,
                           const GpuRenderArgs& args,
                           const PassList& list) WARN_UNUSED_RETURN;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_PassExecutor_h
