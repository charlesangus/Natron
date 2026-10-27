#include "pyramid.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "BlurKernels.h"

namespace bloomspike {

Rect
intersect(const Rect& a, const Rect& b)
{
    Rect r { std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2) };
    return r.empty() ? Rect() : r;
}

Rect
unite(const Rect& a, const Rect& b)
{
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    return { std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2), std::max(a.y2, b.y2) };
}

Rect
grow(const Rect& r, int by)
{
    return r.empty() ? r : Rect { r.x1 - by, r.y1 - by, r.x2 + by, r.y2 + by };
}

int
floorDiv(int a, int b)
{
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int
ceilDiv(int a, int b)
{
    return -floorDiv(-a, b);
}

Image::Image(const Rect& r, int channels)
    : bounds(r)
    , nc(channels)
    , px(std::size_t(r.width()) * r.height() * channels, 0.f)
{
}

Pool::Pool(unsigned threads)
    : n_(std::max(1u, threads))
{
    for (unsigned i = 1; i < n_; ++i) {
        workers_.emplace_back([this] { loop(); });
    }
}

Pool::~Pool()
{
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
        ++gen_;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        t.join();
    }
}

void
Pool::work()
{
    const std::function<void(int)>& fn = *fn_;
    const int n = count_;
    for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) {
        fn(i);
    }
}

void
Pool::loop()
{
    uint64_t seen = 0;
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
        cv_.wait(l, [&] { return gen_ != seen; });
        seen = gen_;
        if (stop_) {
            return;
        }
        l.unlock();
        work();
        l.lock();
        if (--pending_ == 0) {
            done_.notify_one();
        }
    }
}

void
Pool::run(int n, const std::function<void(int)>& fn)
{
    if (n <= 0) {
        return;
    }
    if (n_ == 1 || n == 1) {
        for (int i = 0; i < n; ++i) {
            fn(i);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> l(m_);
        fn_ = &fn;
        count_ = n;
        next_.store(0);
        pending_ = n_ - 1;
        ++gen_;
    }
    cv_.notify_all();
    work();
    std::unique_lock<std::mutex> l(m_);
    done_.wait(l, [&] { return pending_ == 0; });
    fn_ = nullptr;
}

std::string
variantName(const Variant& v)
{
    return std::string(v.down == Down::binomial ? "1331" : "box") + "+" + (v.up == Up::bicubic ? "bicubic" : "bilinear");
}

const std::vector<Variant>&
allVariants()
{
    static const std::vector<Variant> v {
        { Down::binomial, Up::bilinear },
        { Down::binomial, Up::bicubic },
        { Down::box, Up::bilinear },
        { Down::box, Up::bicubic },
    };
    return v;
}

float
blurSigma(const BloomParams& p, int i)
{
    double scale = 1.;
    for (int k = 0; k < i; ++k) {
        scale *= p.ratio;
    }
    return static_cast<float>(p.size * scale / 2.4);
}

double
downVariance(Down down, int level)
{
    const double perStep = down == Down::binomial ? 0.75 : 0.25;
    return perStep * (1. - std::pow(4., -level)) / 3.;
}

double
upVariance(Up up, int factor)
{
    // Catmull-Rom reproduces quadratics, so its second moment about the output position is zero.
    if (factor <= 1 || up == Up::bicubic) {
        return 0.;
    }
    double sum = 0.;
    for (int m = 0; m < factor; ++m) {
        const double f = (2. * m + 1.) / (2. * factor);
        sum += f * (1. - f);
    }
    return sum / factor;
}

PyramidPlan
makePlan(const BloomParams& p, const Variant& v)
{
    PyramidPlan plan;
    plan.params = p;
    plan.variant = v;
    for (int i = 0; i < p.count; ++i) {
        LevelBlur b;
        b.sigma = blurSigma(p, i);
        while (b.sigma / std::ldexp(1., b.level + 1) >= p.sigmaMin) {
            ++b.level;
        }
        const int factor = 1 << b.level;
        if (b.level == 0) {
            b.sigmaEff = b.sigma;
        } else {
            const double s = b.sigma / factor;
            b.sigmaEff = std::sqrt(std::max(0., s * s - downVariance(v.down, b.level) - upVariance(v.up, factor)));
        }
        b.weights = Natron::BlurKernels::LineFilter::firGaussianWeights(b.sigmaEff);
        plan.levels = std::max(plan.levels, b.level + 1);
        plan.blurs.push_back(std::move(b));
    }
    return plan;
}

void
downWeights(Down down, float w[4], int& taps, int& first)
{
    if (down == Down::binomial) {
        w[0] = 0.125f;
        w[1] = 0.375f;
        w[2] = 0.375f;
        w[3] = 0.125f;
        taps = 4;
        first = -1;
    } else {
        w[0] = 0.5f;
        w[1] = 0.5f;
        w[2] = 0.f;
        w[3] = 0.f;
        taps = 2;
        first = 0;
    }
}

void
upTaps(Up up, int factor, int x, int& i0, float w[4], int& taps)
{
    if (factor == 1) {
        i0 = x;
        w[0] = 1.f;
        taps = 1;
        return;
    }
    // Level sample i is centred on absolute position factor * i + factor / 2, so pixel x sits at
    // level position (2x + 1 - factor) / (2 factor); the fraction is exact in float.
    const int num = 2 * x + 1 - factor;
    const int den = 2 * factor;
    const int i = floorDiv(num, den);
    const float f = float(num - i * den) / float(den);
    if (up == Up::bilinear) {
        i0 = i;
        w[0] = 1.f - f;
        w[1] = f;
        taps = 2;
    } else {
        const float f2 = f * f;
        const float f3 = f2 * f;
        i0 = i - 1;
        w[0] = 0.5f * (-f3 + 2.f * f2 - f);
        w[1] = 0.5f * (3.f * f3 - 5.f * f2 + 2.f);
        w[2] = 0.5f * (-3.f * f3 + 4.f * f2 + f);
        w[3] = 0.5f * (f3 - f2);
        taps = 4;
    }
}

namespace {

    // The level-(j+1) samples whose downsample taps reach the level-j samples of r.
    Rect downSupport(const Rect& r, Down down)
    {
        if (r.empty()) {
            return r;
        }
        float w[4];
        int taps, first;
        downWeights(down, w, taps, first);
        const auto axis = [&](int a, int b, int& lo, int& hi) {
            lo = ceilDiv(a - first - taps + 1, 2);
            hi = floorDiv(b - 1 - first, 2) + 1;
        };
        Rect out;
        axis(r.x1, r.x2, out.x1, out.x2);
        axis(r.y1, r.y2, out.y1, out.y2);
        return out;
    }

    // The level-j samples the level-(j+1) samples of r read.
    Rect downNeed(const Rect& r, Down down)
    {
        if (r.empty()) {
            return r;
        }
        float w[4];
        int taps, first;
        downWeights(down, w, taps, first);
        return { 2 * r.x1 + first, 2 * r.y1 + first, 2 * (r.x2 - 1) + first + taps, 2 * (r.y2 - 1) + first + taps };
    }

    Rect upNeed(const Rect& output, Up up, int factor)
    {
        if (output.empty()) {
            return output;
        }
        float w[4];
        int taps = 0;
        Rect out;
        int last;
        upTaps(up, factor, output.x1, out.x1, w, taps);
        upTaps(up, factor, output.x2 - 1, last, w, taps);
        out.x2 = last + taps;
        upTaps(up, factor, output.y1, out.y1, w, taps);
        upTaps(up, factor, output.y2 - 1, last, w, taps);
        out.y2 = last + taps;
        return out;
    }

    struct AxisTaps {
        std::vector<int> first;
        std::vector<float> weights;
        int taps = 1;
    };

    AxisTaps axisTaps(Up up, int factor, int lo, int hi)
    {
        AxisTaps t;
        t.first.resize(std::max(0, hi - lo));
        t.weights.resize(t.first.size() * 4);
        for (int x = lo; x < hi; ++x) {
            upTaps(up, factor, x, t.first[x - lo], &t.weights[std::size_t(x - lo) * 4], t.taps);
        }
        return t;
    }

    void downsample(const View& prev, Image& dst, Down down, Pool& pool)
    {
        float w[4];
        int taps, first;
        downWeights(down, w, taps, first);
        const Rect r = dst.bounds;
        const int nc = dst.nc;
        pool.run(r.height(), [&](int row) {
            const int y = r.y1 + row;
            float* out = dst.row(y);
            for (int x = r.x1; x < r.x2; ++x) {
                const int sx = 2 * x + first;
                const int sy = 2 * y + first;
                const bool inside = sx >= prev.valid.x1 && sx + taps <= prev.valid.x2 && sy >= prev.valid.y1
                    && sy + taps <= prev.valid.y2;
                for (int c = 0; c < nc; ++c) {
                    float acc = 0.f;
                    for (int ty = 0; ty < taps; ++ty) {
                        float rowAcc = 0.f;
                        if (inside) {
                            const float* s = prev.image->at(sx, sy + ty) + c;
                            for (int tx = 0; tx < taps; ++tx) {
                                rowAcc += w[tx] * s[std::size_t(tx) * nc];
                            }
                        } else {
                            for (int tx = 0; tx < taps; ++tx) {
                                rowAcc += w[tx] * prev.get(sx + tx, sy + ty, c);
                            }
                        }
                        acc += w[ty] * rowAcc;
                    }
                    *out++ = acc;
                }
            }
        });
    }

    // The separable FIR of `src` (zero outside src.valid) over `out`, rows first.
    Image blurLevel(const View& src, const Rect& out, const std::vector<float>& w, int nc, Pool& pool)
    {
        Image dst(out, nc);
        if (out.empty()) {
            return dst;
        }
        const int r = int(w.size()) - 1;
        const Rect midRect { out.x1, out.y1 - r, out.x2, out.y2 + r };
        Image mid(midRect, nc);
        const std::size_t rowFloats = std::size_t(out.width()) * nc;
        pool.run(midRect.height(), [&](int row) {
            const int y = midRect.y1 + row;
            std::vector<float> line(std::size_t(out.width() + 2 * r) * nc);
            for (int x = out.x1 - r; x < out.x2 + r; ++x) {
                for (int c = 0; c < nc; ++c) {
                    line[std::size_t(x - out.x1 + r) * nc + c] = src.get(x, y, c);
                }
            }
            float* m = mid.row(y);
            const float* centre = line.data() + std::size_t(r) * nc;
            for (std::size_t i = 0; i < rowFloats; ++i) {
                m[i] = w[0] * centre[i];
            }
            for (int k = 1; k <= r; ++k) {
                const float wk = w[k];
                const float* left = centre - std::size_t(k) * nc;
                const float* right = centre + std::size_t(k) * nc;
                for (std::size_t i = 0; i < rowFloats; ++i) {
                    m[i] += wk * (left[i] + right[i]);
                }
            }
        });
        pool.run(out.height(), [&](int row) {
            const int y = out.y1 + row;
            float* d = dst.row(y);
            const float* centre = mid.row(y);
            for (std::size_t i = 0; i < rowFloats; ++i) {
                d[i] = w[0] * centre[i];
            }
            for (int k = 1; k <= r; ++k) {
                const float wk = w[k];
                const float* below = mid.row(y - k);
                const float* above = mid.row(y + k);
                for (std::size_t i = 0; i < rowFloats; ++i) {
                    d[i] += wk * (below[i] + above[i]);
                }
            }
        });
        return dst;
    }

    void upsampleAdd(const View& level, Up up, int factor, Image& acc, Image* contribution, Pool& pool)
    {
        const Rect o = acc.bounds;
        const int nc = acc.nc;
        const AxisTaps tx = axisTaps(up, factor, o.x1, o.x2);
        const AxisTaps ty = axisTaps(up, factor, o.y1, o.y2);
        pool.run(o.height(), [&](int row) {
            const int y = o.y1 + row;
            const int iy = ty.first[row];
            const float* wy = &ty.weights[std::size_t(row) * 4];
            float* a = acc.row(y);
            float* cRow = contribution ? contribution->row(y) : nullptr;
            for (int col = 0; col < o.width(); ++col) {
                const int ix = tx.first[col];
                const float* wx = &tx.weights[std::size_t(col) * 4];
                for (int c = 0; c < nc; ++c) {
                    float v = 0.f;
                    for (int j = 0; j < ty.taps; ++j) {
                        float rowAcc = 0.f;
                        for (int i = 0; i < tx.taps; ++i) {
                            rowAcc += wx[i] * level.get(ix + i, iy + j, c);
                        }
                        v += wy[j] * rowAcc;
                    }
                    const std::size_t idx = std::size_t(col) * nc + c;
                    a[idx] += v;
                    if (cRow) {
                        cRow[idx] = v;
                    }
                }
            }
        });
    }

} // namespace

PyramidRegions
planRegions(const PyramidPlan& plan, const Rect& output, const Rect& input)
{
    const bool clip = !input.empty();
    const int nLevels = plan.levels;
    const Down down = plan.variant.down;
    std::vector<Rect> support(nLevels);
    if (clip) {
        support[0] = input;
        for (int j = 1; j < nLevels; ++j) {
            support[j] = downSupport(support[j - 1], down);
        }
    }
    PyramidRegions g;
    const std::size_t nb = plan.blurs.size();
    g.level.assign(nLevels, Rect());
    g.read.resize(nb);
    g.out.resize(nb);
    g.data.resize(nb);
    for (std::size_t i = 0; i < nb; ++i) {
        g.read[i] = upNeed(output, plan.variant.up, 1 << plan.blurs[i].level);
    }
    for (int j = nLevels - 1; j >= 0; --j) {
        Rect need;
        for (std::size_t i = 0; i < nb; ++i) {
            if (plan.blurs[i].level == j) {
                need = unite(need, grow(g.read[i], plan.blurs[i].radius()));
            }
        }
        if (j + 1 < nLevels) {
            need = unite(need, downNeed(g.level[j + 1], down));
        }
        g.level[j] = clip ? intersect(need, support[j]) : need;
    }
    for (std::size_t i = 0; i < nb; ++i) {
        const int k = plan.blurs[i].level;
        const int r = plan.blurs[i].radius();
        g.data[i] = clip ? intersect(grow(g.read[i], r), support[k]) : grow(g.read[i], r);
        g.out[i] = clip ? intersect(g.read[i], grow(support[k], r)) : g.read[i];
    }
    return g;
}

Rect
sourceCone(const PyramidPlan& plan, const Rect& output)
{
    return planRegions(plan, output, Rect()).level[0];
}

Image
renderPyramid(const PyramidPlan& plan, const Image& src, const Rect& input, const Rect& output, Pool& pool,
              std::vector<Image>* contributions)
{
    const int nc = src.nc;
    const PyramidRegions g = planRegions(plan, output, input);
    std::vector<Image> levels(plan.levels);
    for (int j = 1; j < plan.levels; ++j) {
        levels[j] = Image(g.level[j], nc);
        const View prev = j == 1 ? View { &src, g.level[0] } : View { &levels[j - 1], g.level[j - 1] };
        downsample(prev, levels[j], plan.variant.down, pool);
    }
    Image acc(output, nc);
    if (contributions) {
        contributions->clear();
    }
    for (std::size_t i = 0; i < plan.blurs.size(); ++i) {
        const LevelBlur& b = plan.blurs[i];
        const View data { b.level == 0 ? &src : &levels[b.level], g.data[i] };
        const Image blurred = blurLevel(data, g.out[i], b.weights, nc, pool);
        Image* contribution = nullptr;
        if (contributions) {
            contributions->emplace_back(output, nc);
            contribution = &contributions->back();
        }
        upsampleAdd(View { &blurred, g.out[i] }, plan.variant.up, 1 << b.level, acc, contribution, pool);
    }
    const float count = float(plan.blurs.size());
    for (float& v : acc.px) {
        v = v / count;
    }
    return acc;
}

Image
combine(const std::vector<Image>& contributions, int count)
{
    Image out(contributions.at(0).bounds, contributions[0].nc);
    for (int i = 0; i < count; ++i) {
        const std::vector<float>& c = contributions.at(i).px;
        for (std::size_t k = 0; k < out.px.size(); ++k) {
            out.px[k] += c[k];
        }
    }
    for (float& v : out.px) {
        v = v / float(count);
    }
    return out;
}

double
nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace bloomspike
