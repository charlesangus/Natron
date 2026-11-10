// LLVM JIT backend: one IR function per snippet, a plain per-element loop over the batch that
// LLVM's optimiser is left to vectorise, compiled through ORC's LLJIT for the host CPU.

#include "gk.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#if GK_WITH_LLVM

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

namespace gk {
namespace {

    using KernFn = void (*)(const float*, float*, const float*, float*, const float*, float*, std::int64_t, std::int64_t,
                            const float* const*, std::int64_t, const NearGrid*, float, std::int32_t);

    struct V {
        Type t;
        llvm::Value* v[3];
    };

    class IrGen {
    public:
        IrGen(llvm::Module& m, llvm::Function* fn)
            : _m(m)
            , _ctx(m.getContext())
            , _b(_ctx)
            , _fn(fn)
            , _f32(llvm::Type::getFloatTy(_ctx))
            , _i32(llvm::Type::getInt32Ty(_ctx))
            , _i64(llvm::Type::getInt64Ty(_ctx))
            , _ptr(llvm::PointerType::getUnqual(_ctx))
        {
            for (unsigned k = 0; k < fn->arg_size(); ++k) {
                _args.push_back(fn->getArg(k));
            }
        }

        void program(const Program& p)
        {
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(_ctx, "entry", _fn);
            llvm::BasicBlock* loop = llvm::BasicBlock::Create(_ctx, "loop", _fn);
            llvm::BasicBlock* exit = llvm::BasicBlock::Create(_ctx, "exit", _fn);
            _b.SetInsertPoint(entry);
            for (int a = 0; a < kAttrCount; ++a) {
                _bound[a] = p.attrRead[a] || p.attrWritten[a];
                _written[a] = p.attrWritten[a];
                if (_bound[a]) {
                    for (int k = 0; k < 3; ++k) {
                        _attr[a][k] = _b.CreateAlloca(_f32);
                    }
                }
            }
            for (Type t : p.locals) {
                std::array<llvm::AllocaInst*, 3> slots {};
                for (int k = 0; k < (t == Type::Vec ? 3 : 1); ++k) {
                    slots[k] = _b.CreateAlloca(t == Type::Int ? _i32 : _f32);
                }
                _locals.push_back({ t, slots });
            }
            llvm::Value* count = _args[7];
            _b.CreateCondBr(_b.CreateICmpSGT(count, _b.getInt64(0)), loop, exit);

            _b.SetInsertPoint(loop);
            llvm::PHINode* i = _b.CreatePHI(_i64, 2);
            i->addIncoming(_b.getInt64(0), entry);
            _i = i;
            for (int a = 0; a < kAttrCount; ++a) {
                if (_bound[a]) {
                    for (int k = 0; k < 3; ++k) {
                        llvm::Value* idx = _b.CreateAdd(_b.CreateMul(i, _b.getInt64(3)), _b.getInt64(k));
                        llvm::Value* v = _b.CreateLoad(_f32, _b.CreateGEP(_f32, _args[2 * a], idx));
                        _b.CreateStore(v, _attr[a][k]);
                    }
                }
            }
            statements(p.body);
            for (int a = 0; a < kAttrCount; ++a) {
                if (_written[a]) {
                    for (int k = 0; k < 3; ++k) {
                        llvm::Value* idx = _b.CreateAdd(_b.CreateMul(i, _b.getInt64(3)), _b.getInt64(k));
                        _b.CreateStore(_b.CreateLoad(_f32, _attr[a][k]), _b.CreateGEP(_f32, _args[2 * a + 1], idx));
                    }
                }
            }
            llvm::Value* next = _b.CreateAdd(i, _b.getInt64(1));
            i->addIncoming(next, _b.GetInsertBlock());
            _b.CreateCondBr(_b.CreateICmpSLT(next, count), loop, exit);
            _b.SetInsertPoint(exit);
            _b.CreateRetVoid();
        }

    private:
        struct Local {
            Type t;
            std::array<llvm::AllocaInst*, 3> slots;
        };

        llvm::Module& _m;
        llvm::LLVMContext& _ctx;
        llvm::IRBuilder<> _b;
        llvm::Function* _fn;
        llvm::Type* _f32;
        llvm::Type* _i32;
        llvm::Type* _i64;
        llvm::Type* _ptr;
        std::vector<llvm::Value*> _args;
        llvm::Value* _i = nullptr;
        bool _bound[kAttrCount] = {};
        bool _written[kAttrCount] = {};
        llvm::AllocaInst* _attr[kAttrCount][3] = {};
        std::vector<Local> _locals;

        V f(llvm::Value* v) { return V { Type::Float, { v, v, v } }; }
        V i(llvm::Value* v) { return V { Type::Int, { v, v, v } }; }

        llvm::Value* boolToInt(llvm::Value* b) { return _b.CreateZExt(b, _i32); }

        llvm::Value* intrinsic(llvm::Intrinsic::ID id, llvm::Value* a)
        {
            return _b.CreateUnaryIntrinsic(id, a);
        }

        llvm::Value* dot(const V& a, const V& b)
        {
            llvm::Value* d = _b.CreateFMul(a.v[0], b.v[0]);
            d = _b.CreateFAdd(_b.CreateFMul(a.v[1], b.v[1]), d);
            return _b.CreateFAdd(_b.CreateFMul(a.v[2], b.v[2]), d);
        }

        V binary(Op op, Type t, const V& a, const V& b)
        {
            auto one = [&](llvm::Value* x, llvm::Value* y) -> llvm::Value* {
                if (t != Type::Int) {
                    switch (op) {
                    case Op::Add:
                        return _b.CreateFAdd(x, y);
                    case Op::Sub:
                        return _b.CreateFSub(x, y);
                    case Op::Mul:
                        return _b.CreateFMul(x, y);
                    default:
                        return _b.CreateFDiv(x, y);
                    }
                }
                switch (op) {
                case Op::Add:
                    return _b.CreateAdd(x, y);
                case Op::Sub:
                    return _b.CreateSub(x, y);
                case Op::Mul:
                    return _b.CreateMul(x, y);
                case Op::Div: {
                    llvm::Value* bad = _b.CreateOr(_b.CreateICmpEQ(y, _b.getInt32(0)),
                                                   _b.CreateAnd(_b.CreateICmpEQ(y, _b.getInt32(-1)),
                                                                _b.CreateICmpEQ(x, _b.getInt32(INT32_MIN))));
                    llvm::Value* safe = _b.CreateSelect(bad, _b.getInt32(1), y);
                    return _b.CreateSelect(bad, _b.getInt32(0), _b.CreateSDiv(x, safe));
                }
                default: {
                    llvm::Value* bad = _b.CreateOr(_b.CreateICmpEQ(y, _b.getInt32(0)), _b.CreateICmpEQ(y, _b.getInt32(-1)));
                    llvm::Value* safe = _b.CreateSelect(bad, _b.getInt32(1), y);
                    return _b.CreateSelect(bad, _b.getInt32(0), _b.CreateSRem(x, safe));
                }
                }
            };
            if (t == Type::Vec) {
                return V { Type::Vec, { one(a.v[0], b.v[0]), one(a.v[1], b.v[1]), one(a.v[2], b.v[2]) } };
            }
            llvm::Value* r = one(a.v[0], b.v[0]);
            return t == Type::Int ? i(r) : f(r);
        }

        llvm::Function* external(const char* name, llvm::Type* ret, std::initializer_list<llvm::Type*> params)
        {
            llvm::FunctionType* ft = llvm::FunctionType::get(ret, params, false);
            return llvm::cast<llvm::Function>(_m.getOrInsertFunction(name, ft).getCallee());
        }

        V expr(const Expr& e)
        {
            auto arg = [&](int k) { return expr(*e.args[k]); };
            switch (e.op) {
            case Op::ConstF:
                return f(llvm::ConstantFP::get(_f32, e.f));
            case Op::ConstI:
                return i(_b.getInt32(e.i));
            case Op::Local: {
                const Local& l = _locals[e.index];
                if (l.t == Type::Vec) {
                    return V { Type::Vec, { _b.CreateLoad(_f32, l.slots[0]), _b.CreateLoad(_f32, l.slots[1]), _b.CreateLoad(_f32, l.slots[2]) } };
                }
                llvm::Value* v = _b.CreateLoad(l.t == Type::Int ? _i32 : _f32, l.slots[0]);
                return l.t == Type::Int ? i(v) : f(v);
            }
            case Op::AttrRead:
                return V { Type::Vec, { _b.CreateLoad(_f32, _attr[e.index][0]), _b.CreateLoad(_f32, _attr[e.index][1]), _b.CreateLoad(_f32, _attr[e.index][2]) } };
            case Op::PtNum:
                return i(_b.CreateTrunc(_b.CreateAdd(_args[6], _i), _i32));
            case Op::NumPt:
                return i(_args[12]);
            case Op::Time:
                return f(_args[11]);
            case Op::I2F:
                return f(_b.CreateSIToFP(arg(0).v[0], _f32));
            case Op::F2I:
                return i(_b.CreateFPToSI(arg(0).v[0], _i32));
            case Op::Comp:
                return f(arg(0).v[e.index]);
            case Op::MakeVec: {
                if (e.args.size() == 1) {
                    llvm::Value* x = arg(0).v[0];
                    return V { Type::Vec, { x, x, x } };
                }
                return V { Type::Vec, { arg(0).v[0], arg(1).v[0], arg(2).v[0] } };
            }
            case Op::Neg: {
                V a = arg(0);
                if (e.type == Type::Int) {
                    return i(_b.CreateNeg(a.v[0]));
                }
                if (e.type == Type::Vec) {
                    return V { Type::Vec, { _b.CreateFNeg(a.v[0]), _b.CreateFNeg(a.v[1]), _b.CreateFNeg(a.v[2]) } };
                }
                return f(_b.CreateFNeg(a.v[0]));
            }
            case Op::Not:
                return i(boolToInt(_b.CreateICmpEQ(arg(0).v[0], _b.getInt32(0))));
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod: {
                V a = arg(0), b = arg(1);
                return binary(e.op, e.type, a, b);
            }
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq:
            case Op::Ne: {
                V a = arg(0), b = arg(1);
                const int k = int(e.op) - int(Op::Lt);
                if (e.args[0]->type == Type::Float) {
                    static const llvm::CmpInst::Predicate p[] = { llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_OGT,
                                                                  llvm::CmpInst::FCMP_OGE, llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_UNE };
                    return i(boolToInt(_b.CreateFCmp(p[k], a.v[0], b.v[0])));
                }
                static const llvm::CmpInst::Predicate p[] = { llvm::CmpInst::ICMP_SLT, llvm::CmpInst::ICMP_SLE, llvm::CmpInst::ICMP_SGT,
                                                              llvm::CmpInst::ICMP_SGE, llvm::CmpInst::ICMP_EQ, llvm::CmpInst::ICMP_NE };
                return i(boolToInt(_b.CreateICmp(p[k], a.v[0], b.v[0])));
            }
            case Op::And:
            case Op::Or: {
                V a = arg(0), b = arg(1);
                llvm::Value* x = _b.CreateICmpNE(a.v[0], _b.getInt32(0));
                llvm::Value* y = _b.CreateICmpNE(b.v[0], _b.getInt32(0));
                return i(boolToInt(e.op == Op::And ? _b.CreateAnd(x, y) : _b.CreateOr(x, y)));
            }
            case Op::Sin:
                return f(intrinsic(llvm::Intrinsic::sin, arg(0).v[0]));
            case Op::Cos:
                return f(intrinsic(llvm::Intrinsic::cos, arg(0).v[0]));
            case Op::Sqrt:
                return f(intrinsic(llvm::Intrinsic::sqrt, arg(0).v[0]));
            case Op::Abs:
                return f(intrinsic(llvm::Intrinsic::fabs, arg(0).v[0]));
            case Op::Floor:
                return f(intrinsic(llvm::Intrinsic::floor, arg(0).v[0]));
            case Op::Min:
            case Op::Max: {
                llvm::Value* a = arg(0).v[0];
                llvm::Value* b = arg(1).v[0];
                if (e.op == Op::Min) {
                    return f(_b.CreateSelect(_b.CreateFCmpOLT(b, a), b, a));
                }
                return f(_b.CreateSelect(_b.CreateFCmpOLT(a, b), b, a));
            }
            case Op::Dot: {
                V a = arg(0), b = arg(1);
                return f(dot(a, b));
            }
            case Op::Length: {
                V a = arg(0);
                return f(intrinsic(llvm::Intrinsic::sqrt, dot(a, a)));
            }
            case Op::Noise: {
                V a = arg(0);
                llvm::Function* fn = external("nk_noise3", _f32, { _f32, _f32, _f32 });
                return f(_b.CreateCall(fn, { a.v[0], a.v[1], a.v[2] }));
            }
            case Op::NearPoint: {
                V a = arg(0);
                llvm::Function* fn = external("nk_nearpoint", _i32, { _ptr, _f32, _f32, _f32 });
                return i(_b.CreateCall(fn, { _args[10], a.v[0], a.v[1], a.v[2] }));
            }
            case Op::PointP: {
                llvm::Value* j = _b.CreateSExt(arg(0).v[0], _i64);
                llvm::Value* in = _b.CreateICmpULT(j, _args[9]);
                llvm::Value* safe = _b.CreateSelect(in, j, _b.getInt64(0));
                llvm::Value* page = _b.CreateLoad(_ptr, _b.CreateGEP(_ptr, _args[8], _b.CreateLShr(safe, kPageBits)));
                llvm::Value* base = _b.CreateMul(_b.CreateAnd(safe, _b.getInt64(kPageSize - 1)), _b.getInt64(3));
                V out { Type::Vec, {} };
                for (int k = 0; k < 3; ++k) {
                    llvm::Value* v = _b.CreateLoad(_f32, _b.CreateGEP(_f32, page, _b.CreateAdd(base, _b.getInt64(k))));
                    out.v[k] = _b.CreateSelect(in, v, llvm::ConstantFP::get(_f32, 0.0));
                }
                return out;
            }
            }
            return f(nullptr);
        }

        void statements(const std::vector<Stmt>& list)
        {
            for (const Stmt& s : list) {
                if (s.kind == Stmt::Assign) {
                    V v = expr(*s.value);
                    if (s.toAttr) {
                        if (s.comp >= 0) {
                            _b.CreateStore(v.v[0], _attr[s.target][s.comp]);
                        } else {
                            for (int k = 0; k < 3; ++k) {
                                _b.CreateStore(v.v[k], _attr[s.target][k]);
                            }
                        }
                    } else {
                        const Local& l = _locals[s.target];
                        if (s.comp >= 0) {
                            _b.CreateStore(v.v[0], l.slots[s.comp]);
                        } else {
                            for (int k = 0; k < (l.t == Type::Vec ? 3 : 1); ++k) {
                                _b.CreateStore(v.v[k], l.slots[k]);
                            }
                        }
                    }
                    continue;
                }
                llvm::Value* c = _b.CreateICmpNE(expr(*s.cond).v[0], _b.getInt32(0));
                llvm::BasicBlock* thenB = llvm::BasicBlock::Create(_ctx, "then", _fn);
                llvm::BasicBlock* elseB = llvm::BasicBlock::Create(_ctx, "else", _fn);
                llvm::BasicBlock* merge = llvm::BasicBlock::Create(_ctx, "merge", _fn);
                _b.CreateCondBr(c, thenB, elseB);
                _b.SetInsertPoint(thenB);
                statements(s.thenBody);
                _b.CreateBr(merge);
                _b.SetInsertPoint(elseB);
                statements(s.elseBody);
                _b.CreateBr(merge);
                _b.SetInsertPoint(merge);
            }
        }
    };

    class JitKernel : public Kernel {
    public:
        JitKernel(KernFn fn, llvm::orc::ResourceTrackerSP rt)
            : _fn(fn)
            , _rt(std::move(rt))
        {
        }

        ~JitKernel() override
        {
            if (llvm::Error e = _rt->remove()) {
                llvm::consumeError(std::move(e));
            }
        }

        std::unique_ptr<void, void (*)(void*)> makeThreadState() const override
        {
            return { nullptr, [](void*) { } };
        }

        void run(void*, const Batch& b, const Input1& in1, const Uniforms& u) const override
        {
            _fn(b.in[0], b.out[0], b.in[1], b.out[1], b.in[2], b.out[2], b.base, b.count, in1.pageTable.data(), in1.count, in1.grid,
                u.time, u.numpt);
        }

    private:
        KernFn _fn;
        llvm::orc::ResourceTrackerSP _rt;
    };

    bool
    usesNoise(const Expr& e)
    {
        if (e.op == Op::Noise) {
            return true;
        }
        for (const auto& a : e.args) {
            if (usesNoise(*a)) {
                return true;
            }
        }
        return false;
    }

    bool
    usesNoise(const std::vector<Stmt>& list)
    {
        for (const Stmt& s : list) {
            if ((s.value && usesNoise(*s.value)) || (s.cond && usesNoise(*s.cond)) || usesNoise(s.thenBody) || usesNoise(s.elseBody)) {
                return true;
            }
        }
        return false;
    }

    class LlvmJit : public JitEngine {
    public:
        LlvmJit(int opt, std::unique_ptr<llvm::MemoryBuffer> builtins)
            : _opt(opt)
            , _builtins(std::move(builtins))
        {
            auto jtmb = llvm::cantFail(llvm::orc::JITTargetMachineBuilder::detectHost());
            jtmb.setCodeGenOptLevel(opt >= 3       ? llvm::CodeGenOptLevel::Aggressive
                                        : opt == 0 ? llvm::CodeGenOptLevel::None
                                                   : llvm::CodeGenOptLevel::Default);
            _cpu = jtmb.getCPU();
            _features = jtmb.getFeatures().getString();
            _tm = llvm::cantFail(jtmb.createTargetMachine());
            _jit = llvm::cantFail(llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(jtmb)).create());
            llvm::orc::SymbolMap syms;
            syms[_jit->mangleAndIntern("nk_nearpoint")] = { llvm::orc::ExecutorAddr::fromPtr(&nk_nearpoint),
                                                            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable };
            llvm::cantFail(_jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(syms))));
        }

        std::string describe() const override
        {
            return "LLVM " LLVM_VERSION_STRING ", cpu " + _cpu + ", O" + std::to_string(_opt);
        }

        std::unique_ptr<Kernel> compile(const Program& p) override
        {
            auto ctx = std::make_unique<llvm::LLVMContext>();
            auto m = std::make_unique<llvm::Module>("snippet", *ctx);
            m->setDataLayout(_tm->createDataLayout());
            m->setTargetTriple(_tm->getTargetTriple());
            const std::string name = "gk_kernel_" + std::to_string(_counter++);

            llvm::Type* ptr = llvm::PointerType::getUnqual(*ctx);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*ctx);
            llvm::FunctionType* ft = llvm::FunctionType::get(
                llvm::Type::getVoidTy(*ctx),
                { ptr, ptr, ptr, ptr, ptr, ptr, i64, i64, ptr, i64, ptr, llvm::Type::getFloatTy(*ctx), llvm::Type::getInt32Ty(*ctx) }, false);
            llvm::Function* fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, *m);
            for (unsigned k = 0; k < 6; ++k) {
                fn->addParamAttr(k, llvm::Attribute::NoAlias);
            }
            fn->addFnAttr(llvm::Attribute::NoUnwind);

            IrGen g(*m, fn);
            g.program(p);

            if (usesNoise(p.body)) {
                auto b = llvm::parseBitcodeFile(_builtins->getMemBufferRef(), *ctx);
                if (!b) {
                    llvm::errs() << "builtins bitcode: " << llvm::toString(b.takeError()) << "\n";
                    return nullptr;
                }
                (*b)->setDataLayout(m->getDataLayout());
                (*b)->setTargetTriple(m->getTargetTriple());
                if (llvm::Linker::linkModules(*m, std::move(*b), llvm::Linker::Flags::LinkOnlyNeeded)) {
                    return nullptr;
                }
            }
            for (llvm::Function& f : *m) {
                if (f.isDeclaration()) {
                    continue;
                }
                if (&f != fn) {
                    f.setLinkage(llvm::GlobalValue::InternalLinkage);
                }
                // The builtins carry clang's generic x86-64 attributes; host-wide ones let them inline.
                f.removeFnAttr("target-cpu");
                f.removeFnAttr("target-features");
                f.removeFnAttr("tune-cpu");
                f.addFnAttr("target-cpu", _cpu);
                f.addFnAttr("target-features", _features);
            }
            for (llvm::GlobalVariable& gv : m->globals()) {
                if (!gv.isDeclaration()) {
                    gv.setLinkage(llvm::GlobalValue::InternalLinkage);
                }
            }
            if (llvm::verifyModule(*m, &llvm::errs())) {
                return nullptr;
            }

            if (_opt > 0) {
                llvm::LoopAnalysisManager lam;
                llvm::FunctionAnalysisManager fam;
                llvm::CGSCCAnalysisManager cgam;
                llvm::ModuleAnalysisManager mam;
                llvm::PassBuilder pb(_tm.get());
                pb.registerModuleAnalyses(mam);
                pb.registerCGSCCAnalyses(cgam);
                pb.registerFunctionAnalyses(fam);
                pb.registerLoopAnalyses(lam);
                pb.crossRegisterProxies(lam, fam, cgam, mam);
                const llvm::OptimizationLevel lvl = _opt == 1 ? llvm::OptimizationLevel::O1
                    : _opt == 2                               ? llvm::OptimizationLevel::O2
                                                              : llvm::OptimizationLevel::O3;
                llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(lvl);
                mpm.run(*m, mam);
            }
            if (const char* dump = std::getenv("GK_DUMP_IR"); dump && *dump) {
                m->print(llvm::errs(), nullptr);
            }

            auto rt = _jit->getMainJITDylib().createResourceTracker();
            if (llvm::Error e = _jit->addIRModule(rt, llvm::orc::ThreadSafeModule(std::move(m), llvm::orc::ThreadSafeContext(std::move(ctx))))) {
                llvm::errs() << llvm::toString(std::move(e)) << "\n";
                return nullptr;
            }
            auto sym = _jit->lookup(name);
            if (!sym) {
                llvm::errs() << llvm::toString(sym.takeError()) << "\n";
                return nullptr;
            }
            return std::make_unique<JitKernel>(sym->toPtr<KernFn>(), std::move(rt));
        }

    private:
        int _opt;
        int _counter = 0;
        std::unique_ptr<llvm::MemoryBuffer> _builtins;
        std::unique_ptr<llvm::TargetMachine> _tm;
        std::unique_ptr<llvm::orc::LLJIT> _jit;
        std::string _cpu;
        std::string _features;
    };

} // namespace

std::unique_ptr<JitEngine>
makeJitEngine(int optLevel, const std::string& builtinsBitcodePath)
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto buf = llvm::MemoryBuffer::getFile(builtinsBitcodePath);
    if (!buf) {
        std::fprintf(stderr, "cannot read %s\n", builtinsBitcodePath.c_str());
        return nullptr;
    }
    return std::make_unique<LlvmJit>(optLevel, std::move(*buf));
}

} // namespace gk

#else

namespace gk {

std::unique_ptr<JitEngine>
makeJitEngine(int, const std::string&)
{
    std::fprintf(stderr, "built without LLVM: JIT backend unavailable\n");
    return nullptr;
}

} // namespace gk

#endif
