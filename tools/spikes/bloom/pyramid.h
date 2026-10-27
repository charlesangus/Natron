#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bloomspike {

// Half-open pixel rect in absolute image coordinates, or in the index space of a pyramid level.
struct Rect {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;

    int width() const { return x2 > x1 ? x2 - x1 : 0; }
    int height() const { return y2 > y1 ? y2 - y1 : 0; }
    bool empty() const { return x1 >= x2 || y1 >= y2; }
    bool operator==(const Rect&) const = default;
};

Rect intersect(const Rect& a, const Rect& b);
Rect unite(const Rect& a, const Rect& b);
Rect grow(const Rect& r, int by);
int floorDiv(int a, int b);
int ceilDiv(int a, int b);

// Interleaved float pixels of `bounds`, row by row from y1.
struct Image {
    Rect bounds;
    int nc = 1;
    std::vector<float> px;

    Image() = default;
    Image(const Rect& r, int channels);
    std::size_t stride() const { return std::size_t(bounds.width()) * nc; }
    float* row(int y) { return px.data() + std::size_t(y - bounds.y1) * stride(); }
    const float* row(int y) const { return px.data() + std::size_t(y - bounds.y1) * stride(); }
    float* at(int x, int y) { return row(y) + std::size_t(x - bounds.x1) * nc; }
    const float* at(int x, int y) const { return row(y) + std::size_t(x - bounds.x1) * nc; }
};

// Reads an image as zero outside `valid`, which lies within the image's bounds.
struct View {
    const Image* image = nullptr;
    Rect valid;

    float get(int x, int y, int c) const
    {
        if (x < valid.x1 || x >= valid.x2 || y < valid.y1 || y >= valid.y2) {
            return 0.f;
        }
        return image->at(x, y)[c];
    }
};

// A fixed set of worker threads; run() hands out indices 0..n-1 to them and to the caller.
class Pool {
public:
    explicit Pool(unsigned threads);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    unsigned size() const { return n_; }
    void run(int n, const std::function<void(int)>& fn);

private:
    void work();
    void loop();

    unsigned n_;
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_;
    std::condition_variable done_;
    const std::function<void(int)>* fn_ = nullptr;
    int count_ = 0;
    std::atomic<int> next_ { 0 };
    unsigned pending_ = 0;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

enum class Down { box,
                  binomial };
enum class Up { bilinear,
                bicubic };

struct Variant {
    Down down;
    Up up;
};

std::string variantName(const Variant& v);
const std::vector<Variant>& allVariants();

struct BloomParams {
    double size = 50.;
    double ratio = 2.;
    int count = 5;
    double sigmaMin = 2.5;
};

// CImgBlur's sigma for blur i: (float)(size * ratio^i / 2.4), the power taken by repeated multiplication.
float blurSigma(const BloomParams& p, int i);

struct LevelBlur {
    int level = 0;
    float sigma = 0.f;
    double sigmaEff = 0.;
    std::vector<float> weights;

    int radius() const { return int(weights.size()) - 1; }
};

struct PyramidPlan {
    BloomParams params;
    Variant variant;
    std::vector<LevelBlur> blurs;
    int levels = 1;
};

// Variance, in level-k pixels squared, that the downsample chain down to level k adds.
double downVariance(Down down, int level);
// Mean variance, in level pixels squared, that upsampling by `factor` adds over the output phases.
double upVariance(Up up, int factor);

PyramidPlan makePlan(const BloomParams& p, const Variant& v);

// What renderPyramid() computes for one output rect from source data limited to `input`.
struct PyramidRegions {
    // Per level: the samples computed, zero elsewhere.
    std::vector<Rect> level;
    // Per blur, at its level: the samples the upsampler reads, the blurred samples stored (zero
    // outside), and the pre-blur samples read (zero outside).
    std::vector<Rect> read;
    std::vector<Rect> out;
    std::vector<Rect> data;
};

// With an empty `input`, nothing is clipped and level[0] is the source cone of `output`.
PyramidRegions planRegions(const PyramidPlan& plan, const Rect& output, const Rect& input);
Rect sourceCone(const PyramidPlan& plan, const Rect& output);

// The source pixels of `src` within `input` (zero outside), bloomed over `output`. When
// `contributions` is set it receives each blur's upsampled, unscaled image over `output`.
Image renderPyramid(const PyramidPlan& plan, const Image& src, const Rect& input, const Rect& output, Pool& pool,
                    std::vector<Image>* contributions = nullptr);

// The sum of contributions[0..count-1], added in order, divided by count.
Image combine(const std::vector<Image>& contributions, int count);

// The CPU sample formulas shared with kernels/down.slang and kernels/up.slang.
void downWeights(Down down, float w[4], int& taps, int& first);
void upTaps(Up up, int factor, int x, int& i0, float w[4], int& taps);

double nowMs();

} // namespace bloomspike
