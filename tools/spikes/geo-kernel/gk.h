// Shared front end, data layout and backend interfaces for the geometry kernel execution spike.
// Throwaway: nothing here is linked into Natron.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "builtins.h"

namespace gk {

// ---------------------------------------------------------------------------------------------
// Front end
// ---------------------------------------------------------------------------------------------

enum class Type : std::uint8_t { Int,
                                 Float,
                                 Vec };

enum class Attr : std::uint8_t { P,
                                 N,
                                 Cd };
inline constexpr int kAttrCount = 3;

enum class Op : std::uint8_t {
    ConstI,
    ConstF,
    Local,
    AttrRead,
    PtNum,
    NumPt,
    Time,
    I2F,
    F2I,
    Comp,
    MakeVec,
    Neg,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    And,
    Or,
    Sin,
    Cos,
    Sqrt,
    Abs,
    Floor,
    Min,
    Max,
    Length,
    Dot,
    Noise,
    NearPoint,
    PointP,
};

struct Expr {
    Op op;
    Type type;
    int index = 0; // local slot, attribute, or vector component
    float f = 0.f;
    int i = 0;
    std::vector<std::unique_ptr<Expr>> args;
};

struct Stmt {
    enum Kind { Assign,
                If } kind
        = Assign;
    bool toAttr = false;
    int target = 0; // local slot or Attr
    int comp = -1; // -1: whole value, otherwise the vector component written
    std::unique_ptr<Expr> value;
    std::unique_ptr<Expr> cond;
    std::vector<Stmt> thenBody;
    std::vector<Stmt> elseBody;
};

struct Program {
    std::vector<Stmt> body;
    std::vector<Type> locals;
    bool attrRead[kAttrCount] = {};
    bool attrWritten[kAttrCount] = {};
    bool usesInput1 = false;
};

struct CompileError {
    int line = 0;
    int col = 0;
    std::string message;
};

// Returns false and fills err on failure.
bool compileSource(const std::string& source, Program& out, CompileError& err);

// ---------------------------------------------------------------------------------------------
// Paged storage: a stand-in for GeoPagedColumn with the same page size, AoS tuples, constant
// pages and copy-on-write page sharing.
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kPageBits = 10;
inline constexpr std::size_t kPageSize = std::size_t(1) << kPageBits;

struct Page {
    bool constant = false;
    std::unique_ptr<float[]> data; // 3 floats if constant, otherwise kPageSize * 3
};
using PagePtr = std::shared_ptr<const Page>;

struct Column {
    std::size_t size = 0;
    std::vector<PagePtr> pages;
};

struct Detail {
    std::size_t count = 0;
    std::shared_ptr<Column> attr[kAttrCount]; // null when absent
};

// What a kernel sees of the second input, and the uniforms.
struct Input1 {
    std::vector<const float*> pageTable;
    std::int64_t count = 0;
    const NearGrid* grid = nullptr;
};

struct Uniforms {
    float time = 0.f;
    std::int32_t numpt = 0;
};

// One page-sized batch. in[a] is the source page data (dense, AoS, tuple 3) or null when the
// attribute is unbound; out[a] is a fresh page the kernel fills for every element in [0, count).
struct Batch {
    const float* in[kAttrCount] = {};
    float* out[kAttrCount] = {};
    std::int64_t base = 0;
    std::int64_t count = 0;
};

class Kernel {
public:
    virtual ~Kernel() = default;
    // Called once per worker thread; the returned state is passed to run().
    virtual std::unique_ptr<void, void (*)(void*)> makeThreadState() const = 0;
    virtual void run(void* threadState, const Batch& batch, const Input1& in1, const Uniforms& u) const = 0;
};

// Interpreter: lanes per instruction is 64, 256 or 1024.
std::unique_ptr<Kernel> compileInterpreter(const Program& program, int lanes);
std::size_t interpreterInstructionCount(const Program& program);

class JitEngine {
public:
    virtual ~JitEngine() = default;
    virtual std::unique_ptr<Kernel> compile(const Program& program) = 0;
    virtual std::string describe() const = 0;
};

// Null with a message on stderr when the spike was built without LLVM.
std::unique_ptr<JitEngine> makeJitEngine(int optLevel, const std::string& builtinsBitcodePath);

} // namespace gk
