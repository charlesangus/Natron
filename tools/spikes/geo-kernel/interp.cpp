// Batched interpreter: a typed register bytecode where every register holds one value per lane
// and every instruction runs across all lanes of a batch, so dispatch costs once per instruction
// per batch rather than once per element. Branches run both sides under lane masks.

#include "gk.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>

#if defined(__clang__)
#define GK_LANES _Pragma("clang loop vectorize(assume_safety)")
#else
// Registers either coincide or are disjoint, so no lane loop carries a dependence.
#define GK_LANES _Pragma("GCC ivdep")
#endif

namespace gk {
namespace {

    enum class Code : std::uint8_t {
        FConst,
        IConst,
        UTime,
        UNumPt,
        FMov,
        IMov,
        FAdd,
        FSub,
        FMul,
        FDiv,
        FMulAdd,
        FNeg,
        FMin,
        FMax,
        FSin,
        FCos,
        FSqrt,
        FAbs,
        FFloor,
        FLt,
        FLe,
        FGt,
        FGe,
        FEq,
        FNe,
        IAdd,
        ISub,
        IMul,
        IDiv,
        IMod,
        INeg,
        ILt,
        ILe,
        IGt,
        IGe,
        IEq,
        INe,
        INot,
        IAnd,
        IOr,
        IAndNot,
        I2F,
        F2I,
        FSel,
        ISel,
        Noise,
        Near,
        GatherP,
        SkipIfNone,
    };

    struct Ins {
        Code op;
        std::int32_t d = 0, a = 0, b = 0, c = 0;
        float f = 0.f;
        std::int32_t i = 0;
    };

    bool
    writesFloat(Code c)
    {
        switch (c) {
        case Code::FMov:
        case Code::FAdd:
        case Code::FSub:
        case Code::FMul:
        case Code::FDiv:
        case Code::FMulAdd:
        case Code::FNeg:
        case Code::FMin:
        case Code::FMax:
        case Code::FSin:
        case Code::FCos:
        case Code::FSqrt:
        case Code::FAbs:
        case Code::FFloor:
        case Code::I2F:
        case Code::FSel:
        case Code::Noise:
            return true;
        default:
            return false;
        }
    }

    struct RV {
        Type t;
        int r[3];
    };

    struct Compiled {
        std::vector<Ins> prologue;
        std::vector<Ins> body;
        int nF = 0;
        int nI = 0;
        RV attr[kAttrCount] {};
        bool bound[kAttrCount] = {};
        bool written[kAttrCount] = {};
        int ptnum = -1;
    };

    class Gen {
    public:
        explicit Gen(Compiled& c)
            : _c(c)
        {
        }

        void program(const Program& p)
        {
            for (int a = 0; a < kAttrCount; ++a) {
                if (p.attrRead[a] || p.attrWritten[a]) {
                    _c.bound[a] = true;
                    _c.written[a] = p.attrWritten[a];
                    _c.attr[a] = RV { Type::Vec, { newF(), newF(), newF() } };
                }
            }
            for (Type t : p.locals) {
                RV v { t, { -1, -1, -1 } };
                for (int k = 0; k < (t == Type::Vec ? 3 : 1); ++k) {
                    v.r[k] = t == Type::Int ? newI() : newF();
                }
                _locals.push_back(v);
            }
            statements(p.body);
        }

    private:
        Compiled& _c;
        std::vector<RV> _locals;
        std::map<std::uint32_t, int> _fconst;
        std::map<int, int> _iconst;
        std::vector<int> _freeF, _freeI;
        struct Scope {
            std::vector<int> f, i;
            std::size_t start;
        };
        std::vector<Scope> _scopes;
        int _numpt = -1, _time = -1;
        int _mask = -1;

        int newF() { return _c.nF++; }
        int newI() { return _c.nI++; }

        int tempF()
        {
            int r;
            if (!_freeF.empty()) {
                r = _freeF.back();
                _freeF.pop_back();
            } else {
                r = newF();
            }
            _scopes.back().f.push_back(r);
            return r;
        }

        int tempI()
        {
            int r;
            if (!_freeI.empty()) {
                r = _freeI.back();
                _freeI.pop_back();
            } else {
                r = newI();
            }
            _scopes.back().i.push_back(r);
            return r;
        }

        bool isScopeTemp(int r, bool isFloat) const
        {
            const auto& v = isFloat ? _scopes.back().f : _scopes.back().i;
            for (int x : v) {
                if (x == r) {
                    return true;
                }
            }
            return false;
        }

        void emit(Code op, int d, int a = 0, int b = 0, int c = 0)
        {
            Ins in;
            in.op = op;
            in.d = d;
            in.a = a;
            in.b = b;
            in.c = c;
            _c.body.push_back(in);
        }

        int constF(float v)
        {
            std::uint32_t bits;
            std::memcpy(&bits, &v, 4);
            auto it = _fconst.find(bits);
            if (it != _fconst.end()) {
                return it->second;
            }
            Ins in;
            in.op = Code::FConst;
            in.d = newF();
            in.f = v;
            _c.prologue.push_back(in);
            return _fconst[bits] = in.d;
        }

        int constI(int v)
        {
            auto it = _iconst.find(v);
            if (it != _iconst.end()) {
                return it->second;
            }
            Ins in;
            in.op = Code::IConst;
            in.d = newI();
            in.i = v;
            _c.prologue.push_back(in);
            return _iconst[v] = in.d;
        }

        RV f(int r) { return RV { Type::Float, { r, r, r } }; }
        RV i(int r) { return RV { Type::Int, { r, r, r } }; }

        RV unaryF(Code op, const RV& a)
        {
            int d = tempF();
            emit(op, d, a.r[0]);
            return f(d);
        }

        RV binary(Code fop, Code iop, Type t, const RV& a, const RV& b)
        {
            if (t == Type::Vec) {
                RV out { Type::Vec, {} };
                for (int k = 0; k < 3; ++k) {
                    out.r[k] = tempF();
                    emit(fop, out.r[k], a.r[k], b.r[k]);
                }
                return out;
            }
            if (t == Type::Float) {
                int d = tempF();
                emit(fop, d, a.r[0], b.r[0]);
                return f(d);
            }
            int d = tempI();
            emit(iop, d, a.r[0], b.r[0]);
            return i(d);
        }

        int dot(const RV& a, const RV& b)
        {
            int d = tempF();
            emit(Code::FMul, d, a.r[0], b.r[0]);
            emit(Code::FMulAdd, d, a.r[1], b.r[1], d);
            emit(Code::FMulAdd, d, a.r[2], b.r[2], d);
            return d;
        }

        RV expr(const Expr& e)
        {
            auto arg = [&](int k) { return expr(*e.args[k]); };
            switch (e.op) {
            case Op::ConstF:
                return f(constF(e.f));
            case Op::ConstI:
                return i(constI(e.i));
            case Op::Local:
                return _locals[e.index];
            case Op::AttrRead:
                return _c.attr[e.index];
            case Op::PtNum:
                if (_c.ptnum < 0) {
                    _c.ptnum = newI();
                }
                return i(_c.ptnum);
            case Op::NumPt:
                if (_numpt < 0) {
                    _numpt = newI();
                    Ins in;
                    in.op = Code::UNumPt;
                    in.d = _numpt;
                    _c.prologue.push_back(in);
                }
                return i(_numpt);
            case Op::Time:
                if (_time < 0) {
                    _time = newF();
                    Ins in;
                    in.op = Code::UTime;
                    in.d = _time;
                    _c.prologue.push_back(in);
                }
                return f(_time);
            case Op::I2F: {
                RV a = arg(0);
                int d = tempF();
                emit(Code::I2F, d, a.r[0]);
                return f(d);
            }
            case Op::F2I: {
                RV a = arg(0);
                int d = tempI();
                emit(Code::F2I, d, a.r[0]);
                return i(d);
            }
            case Op::Comp:
                return f(arg(0).r[e.index]);
            case Op::MakeVec:
                if (e.args.size() == 1) {
                    const int r = arg(0).r[0];
                    return RV { Type::Vec, { r, r, r } };
                }
                return RV { Type::Vec, { arg(0).r[0], arg(1).r[0], arg(2).r[0] } };
            case Op::Neg: {
                RV a = arg(0);
                if (e.type == Type::Int) {
                    int d = tempI();
                    emit(Code::INeg, d, a.r[0]);
                    return i(d);
                }
                RV out = a;
                for (int k = 0; k < (e.type == Type::Vec ? 3 : 1); ++k) {
                    out.r[k] = tempF();
                    emit(Code::FNeg, out.r[k], a.r[k]);
                }
                return e.type == Type::Vec ? out : f(out.r[0]);
            }
            case Op::Not: {
                RV a = arg(0);
                int d = tempI();
                emit(Code::INot, d, a.r[0]);
                return i(d);
            }
            case Op::Add:
                return binary(Code::FAdd, Code::IAdd, e.type, arg(0), arg(1));
            case Op::Sub:
                return binary(Code::FSub, Code::ISub, e.type, arg(0), arg(1));
            case Op::Mul:
                return binary(Code::FMul, Code::IMul, e.type, arg(0), arg(1));
            case Op::Div:
                return binary(Code::FDiv, Code::IDiv, e.type, arg(0), arg(1));
            case Op::Mod:
                return binary(Code::FAdd, Code::IMod, Type::Int, arg(0), arg(1));
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq:
            case Op::Ne: {
                static const Code fc[] = { Code::FLt, Code::FLe, Code::FGt, Code::FGe, Code::FEq, Code::FNe };
                static const Code ic[] = { Code::ILt, Code::ILe, Code::IGt, Code::IGe, Code::IEq, Code::INe };
                const int k = int(e.op) - int(Op::Lt);
                RV a = arg(0), b = arg(1);
                int d = tempI();
                emit(e.args[0]->type == Type::Float ? fc[k] : ic[k], d, a.r[0], b.r[0]);
                return i(d);
            }
            case Op::And:
                return binary(Code::FAdd, Code::IAnd, Type::Int, arg(0), arg(1));
            case Op::Or:
                return binary(Code::FAdd, Code::IOr, Type::Int, arg(0), arg(1));
            case Op::Sin:
                return unaryF(Code::FSin, arg(0));
            case Op::Cos:
                return unaryF(Code::FCos, arg(0));
            case Op::Sqrt:
                return unaryF(Code::FSqrt, arg(0));
            case Op::Abs:
                return unaryF(Code::FAbs, arg(0));
            case Op::Floor:
                return unaryF(Code::FFloor, arg(0));
            case Op::Min:
                return binary(Code::FMin, Code::IAdd, Type::Float, arg(0), arg(1));
            case Op::Max:
                return binary(Code::FMax, Code::IAdd, Type::Float, arg(0), arg(1));
            case Op::Dot: {
                RV a = arg(0), b = arg(1);
                return f(dot(a, b));
            }
            case Op::Length: {
                RV a = arg(0);
                int d = dot(a, a);
                emit(Code::FSqrt, d, d);
                return f(d);
            }
            case Op::Noise: {
                RV a = arg(0);
                int d = tempF();
                emit(Code::Noise, d, a.r[0], a.r[1], a.r[2]);
                return f(d);
            }
            case Op::NearPoint: {
                RV a = arg(0);
                int d = tempI();
                emit(Code::Near, d, a.r[0], a.r[1], a.r[2]);
                return i(d);
            }
            case Op::PointP: {
                RV idx = arg(0);
                RV out { Type::Vec, { tempF(), tempF(), tempF() } };
                emit(Code::GatherP, out.r[0], out.r[1], out.r[2], idx.r[0]);
                return out;
            }
            }
            throw std::logic_error("unhandled op");
        }

        bool reads(const Ins& in, int r, bool isFloat) const
        {
            // Conservative: any operand slot equal to r counts as a read when the bank can match.
            (void)isFloat;
            return in.a == r || in.b == r || in.c == r || ((in.op == Code::FSel || in.op == Code::ISel) && in.d == r);
        }

        // Retargets the instruction that produced temp src so it writes dst directly, when nothing
        // after it in this statement touches src or dst.
        bool retarget(int src, int dst, bool isFloat)
        {
            if (!isScopeTemp(src, isFloat)) {
                return false;
            }
            const std::size_t start = _scopes.back().start;
            for (std::size_t w = _c.body.size(); w-- > start;) {
                Ins& in = _c.body[w];
                if (in.d != src || in.op == Code::GatherP || writesFloat(in.op) != isFloat || in.op == Code::SkipIfNone) {
                    if (in.d == src || in.a == src || in.b == src || in.c == src || in.d == dst || in.a == dst || in.b == dst
                        || in.c == dst) {
                        return false;
                    }
                    continue;
                }
                in.d = dst;
                return true;
            }
            return false;
        }

        void assign(const Stmt& s)
        {
            RV v = expr(*s.value);
            RV target = s.toAttr ? _c.attr[s.target] : _locals[s.target];
            const bool isFloat = v.t != Type::Int;
            const int n = v.t == Type::Vec ? 3 : 1;
            for (int k = 0; k < n; ++k) {
                const int dst = s.comp >= 0 ? target.r[s.comp] : target.r[k];
                const int src = v.r[k];
                if (dst == src) {
                    continue;
                }
                if (_mask >= 0) {
                    emit(isFloat ? Code::FSel : Code::ISel, dst, _mask, src, dst);
                    continue;
                }
                bool shared = false;
                for (int j = 0; j < n; ++j) {
                    shared |= j != k && v.r[j] == src;
                }
                if (!shared && retarget(src, dst, isFloat)) {
                    continue;
                }
                emit(isFloat ? Code::FMov : Code::IMov, dst, src);
            }
        }

        void statements(const std::vector<Stmt>& list)
        {
            for (const Stmt& s : list) {
                _scopes.push_back(Scope { {}, {}, _c.body.size() });
                if (s.kind == Stmt::Assign) {
                    assign(s);
                } else {
                    RV c = expr(*s.cond);
                    const int mThen = tempI();
                    const int mElse = tempI();
                    if (_mask < 0) {
                        emit(Code::IAnd, mThen, c.r[0], c.r[0]);
                        emit(Code::INot, mElse, c.r[0]);
                    } else {
                        emit(Code::IAnd, mThen, _mask, c.r[0]);
                        emit(Code::IAndNot, mElse, _mask, c.r[0]);
                    }
                    const int outer = _mask;
                    auto branch = [&](int m, const std::vector<Stmt>& body) {
                        if (body.empty()) {
                            return;
                        }
                        const std::size_t skip = _c.body.size();
                        emit(Code::SkipIfNone, 0, m);
                        _mask = m;
                        statements(body);
                        _c.body[skip].i = std::int32_t(_c.body.size());
                    };
                    branch(mThen, s.thenBody);
                    branch(mElse, s.elseBody);
                    _mask = outer;
                }
                Scope done = std::move(_scopes.back());
                _scopes.pop_back();
                _freeF.insert(_freeF.end(), done.f.begin(), done.f.end());
                _freeI.insert(_freeI.end(), done.i.begin(), done.i.end());
            }
        }
    };

    struct ThreadState {
        float* F = nullptr;
        std::int32_t* I = nullptr;
        bool primed = false;
        float time = 0.f;
        std::int32_t numpt = 0;
        ~ThreadState()
        {
            std::free(F);
            std::free(I);
        }
    };

    template <int L>
    void
    execute(const std::vector<Ins>& code, float* F, std::int32_t* I, const Input1& in1, const Uniforms& u)
    {
        const Ins* ins = code.data();
        const std::size_t n = code.size();
        for (std::size_t pc = 0; pc < n; ++pc) {
            const Ins& in = ins[pc];
            float* fd = F + std::size_t(in.d) * L;
            const float* fa = F + std::size_t(in.a) * L;
            const float* fb = F + std::size_t(in.b) * L;
            const float* fc = F + std::size_t(in.c) * L;
            std::int32_t* id = I + std::size_t(in.d) * L;
            const std::int32_t* ia = I + std::size_t(in.a) * L;
            const std::int32_t* ib = I + std::size_t(in.b) * L;
            const std::int32_t* ic = I + std::size_t(in.c) * L;
#define GK_F(expr)                                               \
    do {                                                         \
        GK_LANES for (int l = 0; l < L; ++l) { fd[l] = (expr); } \
    } while (0)
#define GK_I(expr)                                               \
    do {                                                         \
        GK_LANES for (int l = 0; l < L; ++l) { id[l] = (expr); } \
    } while (0)
            switch (in.op) {
            case Code::FConst:
                GK_F(in.f);
                break;
            case Code::IConst:
                GK_I(in.i);
                break;
            case Code::UTime:
                GK_F(u.time);
                break;
            case Code::UNumPt:
                GK_I(u.numpt);
                break;
            case Code::FMov:
                GK_F(fa[l]);
                break;
            case Code::IMov:
                GK_I(ia[l]);
                break;
            case Code::FAdd:
                GK_F(fa[l] + fb[l]);
                break;
            case Code::FSub:
                GK_F(fa[l] - fb[l]);
                break;
            case Code::FMul:
                GK_F(fa[l] * fb[l]);
                break;
            case Code::FDiv:
                GK_F(fa[l] / fb[l]);
                break;
            case Code::FMulAdd:
                GK_F(fa[l] * fb[l] + fc[l]);
                break;
            case Code::FNeg:
                GK_F(-fa[l]);
                break;
            case Code::FMin:
                GK_F(fb[l] < fa[l] ? fb[l] : fa[l]);
                break;
            case Code::FMax:
                GK_F(fa[l] < fb[l] ? fb[l] : fa[l]);
                break;
            case Code::FSin:
                GK_F(std::sin(fa[l]));
                break;
            case Code::FCos:
                GK_F(std::cos(fa[l]));
                break;
            case Code::FSqrt:
                GK_F(std::sqrt(fa[l]));
                break;
            case Code::FAbs:
                GK_F(std::fabs(fa[l]));
                break;
            case Code::FFloor:
                GK_F(std::floor(fa[l]));
                break;
            case Code::FLt:
                GK_I(fa[l] < fb[l]);
                break;
            case Code::FLe:
                GK_I(fa[l] <= fb[l]);
                break;
            case Code::FGt:
                GK_I(fa[l] > fb[l]);
                break;
            case Code::FGe:
                GK_I(fa[l] >= fb[l]);
                break;
            case Code::FEq:
                GK_I(fa[l] == fb[l]);
                break;
            case Code::FNe:
                GK_I(fa[l] != fb[l]);
                break;
            case Code::IAdd:
                GK_I(std::int32_t(std::uint32_t(ia[l]) + std::uint32_t(ib[l])));
                break;
            case Code::ISub:
                GK_I(std::int32_t(std::uint32_t(ia[l]) - std::uint32_t(ib[l])));
                break;
            case Code::IMul:
                GK_I(std::int32_t(std::uint32_t(ia[l]) * std::uint32_t(ib[l])));
                break;
            case Code::IDiv:
                GK_I(ib[l] == 0 || (ib[l] == -1 && ia[l] == INT32_MIN) ? 0 : ia[l] / ib[l]);
                break;
            case Code::IMod:
                GK_I(ib[l] == 0 || ib[l] == -1 ? 0 : ia[l] % ib[l]);
                break;
            case Code::INeg:
                GK_I(std::int32_t(0u - std::uint32_t(ia[l])));
                break;
            case Code::ILt:
                GK_I(ia[l] < ib[l]);
                break;
            case Code::ILe:
                GK_I(ia[l] <= ib[l]);
                break;
            case Code::IGt:
                GK_I(ia[l] > ib[l]);
                break;
            case Code::IGe:
                GK_I(ia[l] >= ib[l]);
                break;
            case Code::IEq:
                GK_I(ia[l] == ib[l]);
                break;
            case Code::INe:
                GK_I(ia[l] != ib[l]);
                break;
            case Code::INot:
                GK_I(ia[l] == 0);
                break;
            case Code::IAnd:
                GK_I((ia[l] != 0) & (ib[l] != 0));
                break;
            case Code::IOr:
                GK_I((ia[l] != 0) | (ib[l] != 0));
                break;
            case Code::IAndNot:
                GK_I((ia[l] != 0) & (ib[l] == 0));
                break;
            case Code::I2F:
                GK_F(float(ia[l]));
                break;
            case Code::F2I:
                GK_I(std::int32_t(fa[l]));
                break;
            case Code::FSel:
                GK_F(ia[l] ? fb[l] : fc[l]);
                break;
            case Code::ISel:
                GK_I(ia[l] ? ib[l] : ic[l]);
                break;
            case Code::Noise:
                GK_F(gkNoise3(fa[l], fb[l], fc[l]));
                break;
            case Code::Near:
                for (int l = 0; l < L; ++l) {
                    id[l] = nk_nearpoint(in1.grid, fa[l], fb[l], fc[l]);
                }
                break;
            case Code::GatherP: {
                float* x = F + std::size_t(in.d) * L;
                float* y = F + std::size_t(in.a) * L;
                float* z = F + std::size_t(in.b) * L;
                for (int l = 0; l < L; ++l) {
                    const std::int64_t j = ic[l];
                    if (std::uint64_t(j) < std::uint64_t(in1.count)) {
                        const float* p = in1.pageTable[j >> kPageBits] + (j & (kPageSize - 1)) * 3;
                        x[l] = p[0];
                        y[l] = p[1];
                        z[l] = p[2];
                    } else {
                        x[l] = y[l] = z[l] = 0.f;
                    }
                }
                break;
            }
            case Code::SkipIfNone: {
                int any = 0;
                for (int l = 0; l < L; ++l) {
                    any |= ia[l];
                }
                if (!any) {
                    pc = std::size_t(in.i) - 1;
                }
                break;
            }
            }
#undef GK_F
#undef GK_I
        }
    }

    template <int L>
    class InterpKernel : public Kernel {
    public:
        explicit InterpKernel(Compiled c)
            : _c(std::move(c))
        {
        }

        std::unique_ptr<void, void (*)(void*)> makeThreadState() const override
        {
            auto* s = new ThreadState;
            s->F = static_cast<float*>(std::aligned_alloc(64, sizeof(float) * L * std::max(_c.nF, 1)));
            s->I = static_cast<std::int32_t*>(std::aligned_alloc(64, sizeof(std::int32_t) * L * std::max(_c.nI, 1)));
            return { s, [](void* p) { delete static_cast<ThreadState*>(p); } };
        }

        void run(void* state, const Batch& batch, const Input1& in1, const Uniforms& u) const override
        {
            ThreadState& s = *static_cast<ThreadState*>(state);
            if (!s.primed || s.time != u.time || s.numpt != u.numpt) {
                execute<L>(_c.prologue, s.F, s.I, in1, u);
                s.primed = true;
                s.time = u.time;
                s.numpt = u.numpt;
            }
            for (std::int64_t off = 0; off < batch.count; off += L) {
                const int n = int(std::min<std::int64_t>(L, batch.count - off));
                for (int a = 0; a < kAttrCount; ++a) {
                    if (!_c.bound[a]) {
                        continue;
                    }
                    const float* src = batch.in[a] + off * 3;
                    float* x = s.F + std::size_t(_c.attr[a].r[0]) * L;
                    float* y = s.F + std::size_t(_c.attr[a].r[1]) * L;
                    float* z = s.F + std::size_t(_c.attr[a].r[2]) * L;
                    for (int l = 0; l < n; ++l) {
                        x[l] = src[3 * l];
                        y[l] = src[3 * l + 1];
                        z[l] = src[3 * l + 2];
                    }
                    for (int l = n; l < L; ++l) {
                        x[l] = y[l] = z[l] = 0.f;
                    }
                }
                if (_c.ptnum >= 0) {
                    std::int32_t* p = s.I + std::size_t(_c.ptnum) * L;
                    for (int l = 0; l < L; ++l) {
                        p[l] = std::int32_t(batch.base + off) + l;
                    }
                }
                execute<L>(_c.body, s.F, s.I, in1, u);
                for (int a = 0; a < kAttrCount; ++a) {
                    if (!_c.written[a]) {
                        continue;
                    }
                    float* dst = batch.out[a] + off * 3;
                    const float* x = s.F + std::size_t(_c.attr[a].r[0]) * L;
                    const float* y = s.F + std::size_t(_c.attr[a].r[1]) * L;
                    const float* z = s.F + std::size_t(_c.attr[a].r[2]) * L;
                    for (int l = 0; l < n; ++l) {
                        dst[3 * l] = x[l];
                        dst[3 * l + 1] = y[l];
                        dst[3 * l + 2] = z[l];
                    }
                }
            }
        }

        const Compiled& compiled() const { return _c; }

    private:
        Compiled _c;
    };

} // namespace

std::unique_ptr<Kernel>
compileInterpreter(const Program& program, int lanes)
{
    Compiled c;
    Gen g(c);
    g.program(program);
    switch (lanes) {
    case 64:
        return std::make_unique<InterpKernel<64>>(std::move(c));
    case 256:
        return std::make_unique<InterpKernel<256>>(std::move(c));
    case 1024:
        return std::make_unique<InterpKernel<1024>>(std::move(c));
    }
    throw std::invalid_argument("lanes must be 64, 256 or 1024");
}

std::size_t
interpreterInstructionCount(const Program& program)
{
    Compiled c;
    Gen g(c);
    g.program(program);
    return c.body.size();
}

} // namespace gk
