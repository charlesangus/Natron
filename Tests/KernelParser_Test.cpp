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

#include "Global/Macros.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Kernel/KernelBinding.h"
#include "Engine/Kernel/KernelParser.h"

NATRON_NAMESPACE_USING

namespace {

class FakeBinding : public KernelBinding {
public:
    mutable std::vector<KernelAttributeRequest> requests;

    std::optional<KernelVariable> builtinVariable(std::string_view name) const override
    {
        KernelVariable v;
        v.kind = KernelVariableKind::eBuiltin;
        v.readOnly = true;
        if (name == "ptnum" || name == "primnum" || name == "vtxnum" || name == "numpt") {
            v.type = KernelType(KernelBaseType::eInt);
            return v;
        }
        if (name == "Frame" || name == "Time") {
            v.type = KernelType(KernelBaseType::eFloat);
            return v;
        }
        return std::nullopt;
    }

    KernelBindResult bindAttribute(const KernelAttributeRequest& request) const override
    {
        requests.push_back(request);
        KernelBindResult result;
        if (request.name == "missing") {
            result.error = "no attribute 'missing' here";
            return result;
        }
        KernelVariable v;
        v.id = static_cast<std::int32_t>(requests.size());
        v.type = request.type;
        v.readOnly = request.input > 0;
        result.variable = v;
        return result;
    }

    void functions(std::string_view name, std::vector<KernelFunctionSignature>& overloads) const override
    {
        const KernelType i(KernelBaseType::eInt);
        const KernelType s(KernelBaseType::eString);
        if (name == "point") {
            for (KernelBaseType result : { KernelBaseType::eFloat, KernelBaseType::eVector }) {
                KernelFunctionSignature sig;
                sig.name = "point";
                sig.result = KernelType(result);
                sig.params = { { i, false }, { s, true }, { i, false } };
                sig.id = result == KernelBaseType::eFloat ? 0 : 1;
                overloads.push_back(sig);
            }
        } else if (name == "addpoint") {
            KernelFunctionSignature sig;
            sig.name = "addpoint";
            sig.result = i;
            sig.params = { { i, false }, { KernelType(KernelBaseType::eVector), false } };
            sig.sideEffects = true;
            sig.id = 2;
            overloads.push_back(sig);
        }
    }
};

struct Compiled {
    bool ok = false;
    KernelProgram program;
    KernelCompileError error;
    FakeBinding binding;
};

void
compile(std::string_view source, Compiled& out)
{
    out.ok = kernelCompile(source, out.binding, out.program, out.error);
}

std::vector<KernelOp>
bodyOps(const KernelProgram& program)
{
    std::vector<KernelOp> ops;
    for (std::size_t pc = program.prologueSize; pc < program.instructions.size(); ++pc) {
        ops.push_back(program.instructions[pc].op);
    }
    return ops;
}

std::size_t
countOps(const KernelProgram& program, KernelOp op)
{
    std::size_t n = 0;
    for (const KernelInstruction& in : program.instructions) {
        n += in.op == op ? 1 : 0;
    }
    return n;
}

// Expects source to fail at the 1-based line, and at the column where marker starts on that line.
void
expectError(const std::string& source, int line, std::string_view marker, std::string_view message)
{
    Compiled c;
    compile(source, c);
    ASSERT_FALSE(c.ok) << source;
    std::size_t lineStart = 0;
    for (int l = 1; l < line; ++l) {
        lineStart = source.find('\n', lineStart) + 1;
    }
    const std::size_t at = source.find(marker, lineStart);
    ASSERT_NE(std::string::npos, at) << marker;
    EXPECT_EQ(line, c.error.line) << source << "\n"
                                  << c.error.message;
    EXPECT_EQ(static_cast<int>(at - lineStart) + 1, c.error.column) << source << "\n"
                                                                    << c.error.message;
    EXPECT_NE(std::string::npos, c.error.message.find(message)) << c.error.message;
}

std::vector<std::string>
readIncludeLines(const std::filesystem::path& path)
{
    std::vector<std::string> includes;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("#include") != std::string::npos) {
            includes.push_back(line);
        }
    }
    return includes;
}

} // anonymous namespace

TEST(KernelParserTest, ComponentCompoundAssignIsOneVectorWrite)
{
    Compiled c;
    compile("@P.y += 1;", c);
    ASSERT_TRUE(c.ok) << c.error.message;

    const KernelProgramBinding* p = c.program.findBinding("P");
    ASSERT_NE(nullptr, p);
    EXPECT_EQ(KernelType(KernelBaseType::eVector), p->type);
    EXPECT_EQ(KernelVariableKind::eAttribute, p->variable.kind);
    ASSERT_EQ(3u, p->registers.size());
    EXPECT_TRUE(p->read);
    EXPECT_TRUE(p->written);
    EXPECT_TRUE(p->needsGather);

    ASSERT_EQ(1u, c.binding.requests.size());
    EXPECT_EQ("P", c.binding.requests[0].name);
    EXPECT_FALSE(c.binding.requests[0].typeExplicit);
    EXPECT_TRUE(c.binding.requests[0].write);

    ASSERT_EQ(1u, c.program.prologueSize);
    const KernelInstruction& one = c.program.instructions[0];
    EXPECT_EQ(KernelOp::eFConst, one.op);
    EXPECT_EQ(1.f, one.fimm);

    ASSERT_EQ(std::vector<KernelOp> { KernelOp::eFAdd }, bodyOps(c.program));
    const KernelInstruction& add = c.program.instructions[1];
    EXPECT_EQ(p->registers[1], add.d);
    EXPECT_EQ(p->registers[1], add.a);
    EXPECT_EQ(one.d, add.b);
}

TEST(KernelParserTest, WholeWriteNeedsNoGather)
{
    Compiled c;
    compile("@Cd = {1, 0, 0};", c);
    ASSERT_TRUE(c.ok) << c.error.message;
    const KernelProgramBinding* cd = c.program.findBinding("Cd");
    ASSERT_NE(nullptr, cd);
    EXPECT_EQ(KernelType(KernelBaseType::eVector), cd->type);
    EXPECT_TRUE(cd->written);
    EXPECT_FALSE(cd->read);
    EXPECT_FALSE(cd->needsGather);
    EXPECT_EQ(2u, c.program.prologueSize);
    EXPECT_EQ((std::vector<KernelOp> { KernelOp::eFMov, KernelOp::eFMov, KernelOp::eFMov }), bodyOps(c.program));
}

TEST(KernelParserTest, BuiltinsAndPrefixedBindings)
{
    Compiled c;
    compile("f@x = @ptnum * 2;", c);
    ASSERT_TRUE(c.ok) << c.error.message;
    const KernelProgramBinding* ptnum = c.program.findBinding("ptnum");
    ASSERT_NE(nullptr, ptnum);
    EXPECT_EQ(KernelVariableKind::eBuiltin, ptnum->variable.kind);
    EXPECT_EQ(KernelType(KernelBaseType::eInt), ptnum->type);
    EXPECT_TRUE(ptnum->read);
    EXPECT_FALSE(ptnum->written);

    const KernelProgramBinding* x = c.program.findBinding("x");
    ASSERT_NE(nullptr, x);
    EXPECT_EQ(KernelType(KernelBaseType::eFloat), x->type);
    EXPECT_TRUE(x->written);
    EXPECT_FALSE(x->needsGather);
    ASSERT_EQ(1u, c.binding.requests.size());
    EXPECT_TRUE(c.binding.requests[0].typeExplicit);

    EXPECT_EQ((std::vector<KernelOp> { KernelOp::eIMul, KernelOp::eI2F }), bodyOps(c.program));
    EXPECT_EQ(x->registers[0], c.program.instructions.back().d);
}

TEST(KernelParserTest, KnownNamesAndOtherInputs)
{
    Compiled c;
    compile("@P.x = @opinput1_P.x; @id = 3; @orient = {0, 0, 0, 1}; s@name = \"a\"; @mass = 2;", c);
    ASSERT_TRUE(c.ok) << c.error.message;
    const KernelProgramBinding* other = c.program.findBinding("P", 1);
    ASSERT_NE(nullptr, other);
    EXPECT_EQ(KernelType(KernelBaseType::eVector), other->type);
    EXPECT_TRUE(other->read);
    EXPECT_FALSE(other->written);
    EXPECT_EQ(KernelType(KernelBaseType::eInt), c.program.findBinding("id")->type);
    EXPECT_EQ(KernelType(KernelBaseType::eVector4), c.program.findBinding("orient")->type);
    EXPECT_EQ(KernelType(KernelBaseType::eString), c.program.findBinding("name")->type);
    EXPECT_EQ(KernelType(KernelBaseType::eFloat), c.program.findBinding("mass")->type);
}

TEST(KernelParserTest, IfElseRunsBothBranchesUnderMasks)
{
    Compiled c;
    compile("if (@P.y > 0) @P.y = 0; else @P.y = 1;", c);
    ASSERT_TRUE(c.ok) << c.error.message;
    const std::vector<KernelOp> expected { KernelOp::eFGt, KernelOp::eIAnd, KernelOp::eIAndNot,
                                           KernelOp::eJumpIfNone, KernelOp::eFSel, KernelOp::eJumpIfNone,
                                           KernelOp::eFSel };
    ASSERT_EQ(expected, bodyOps(c.program));
    const std::size_t first = c.program.prologueSize + 3;
    const std::size_t second = c.program.prologueSize + 5;
    EXPECT_EQ(static_cast<std::int32_t>(second), c.program.instructions[first].imm);
    EXPECT_EQ(static_cast<std::int32_t>(c.program.instructions.size()), c.program.instructions[second].imm);
    EXPECT_TRUE(c.program.findBinding("P")->needsGather);
}

TEST(KernelParserTest, LoopsJumpBackwards)
{
    const char* const sources[] = {
        "int n = 0; for (int i = 0; i < 4; i++) n += i; i@count = n;",
        "int i = 0; while (i < 3) { i++; if (i == 2) break; } i@n = i;",
        "do { @a += 1; } while (@a < 10);",
        "float vals[] = {1, 2, 3}; float sum = 0; foreach (float v; vals) sum += v; @sum = sum;",
        "foreach (int i, float v; array(1.0, 2.0)) { @x += v * i; }",
    };
    for (const char* source : sources) {
        Compiled c;
        compile(source, c);
        ASSERT_TRUE(c.ok) << source << ": " << c.error.message;
        bool backwards = false;
        for (std::size_t pc = 0; pc < c.program.instructions.size(); ++pc) {
            const KernelInstruction& in = c.program.instructions[pc];
            if (in.op == KernelOp::eJump) {
                backwards |= in.imm <= static_cast<std::int32_t>(pc);
            }
            if (in.op == KernelOp::eJump || in.op == KernelOp::eJumpIfNone) {
                EXPECT_GE(in.imm, static_cast<std::int32_t>(c.program.prologueSize)) << source;
                EXPECT_LE(in.imm, static_cast<std::int32_t>(c.program.instructions.size())) << source;
            }
        }
        EXPECT_TRUE(backwards) << source;
    }
}

TEST(KernelParserTest, MainBodyReturnMasksLaterWrites)
{
    Compiled c;
    compile("if (@ptnum == 0) return; @P.y = 1;", c);
    ASSERT_TRUE(c.ok) << c.error.message;
    EXPECT_EQ(KernelOp::eFSel, c.program.instructions.back().op);
    EXPECT_TRUE(c.program.findBinding("P")->needsGather);
}

TEST(KernelParserTest, UserFunctionsAreInlined)
{
    Compiled c;
    compile("float twice(float x) { return x * 2; }\n"
            "float sq(float x) { return x * x; }\n"
            "vector sq(vector v) { return v * v; }\n"
            "void lift(vector p; float amount) { p.y += amount; }\n"
            "float absf(float x) { if (x > 0) return x; return -x; }\n"
            "@pscale = twice(@pscale);\n"
            "@P = sq(@P);\n"
            "@a = sq(2) + absf(@a);\n"
            "lift(@N, 0.5);\n",
            c);
    ASSERT_TRUE(c.ok) << c.error.message;
    EXPECT_EQ(0u, countOps(c.program, KernelOp::eCall));
    EXPECT_TRUE(c.program.findBinding("N")->written);
    for (const KernelAttributeRequest& r : c.binding.requests) {
        EXPECT_TRUE(r.write) << r.name;
    }
}

TEST(KernelParserTest, BindingFunctionsResolveByExpectedType)
{
    Compiled c;
    compile("vector q = point(1, \"P\", @ptnum); float h = point(0, \"pscale\", @ptnum); int pt = addpoint(0, q + h);", c);
    ASSERT_TRUE(c.ok) << c.error.message;
    ASSERT_EQ(3u, c.program.functions.size());
    EXPECT_EQ(KernelFunctionProvider::eBinding, c.program.functions[0].provider);
    EXPECT_EQ(1, c.program.functions[0].signature.id);
    EXPECT_EQ(0, c.program.functions[1].signature.id);
    EXPECT_EQ(2, c.program.functions[2].signature.id);
    EXPECT_EQ(3u, countOps(c.program, KernelOp::eCall));
}

TEST(KernelParserTest, ValidCorpusCompiles)
{
    const char* const corpus[] = {
        "vector n = normalize(@N); @Cd = abs(n);",
        "@P += @N * 0.1 * sin(@Frame);",
        "float d = length(@P); if (d > 1.0) { @P /= d; } else if (d < 0.5) @P *= 2;",
        "int count = 0; for (int i = 0; i < 10; ++i) { if (i % 2 == 0) continue; count += i; } i@odd = count;",
        "string s = sprintf(\"%d_%s\", @ptnum, \"pt\"); s@label = s + \"x\";",
        "matrix3 m = ident(); vector v = @P * m; @P = v;",
        "matrix m = ident(); @P = @P * m;",
        "vector4 q = quaternion(radians(45), {0, 1, 0}); @orient = q; @N = qrotate(q, @N);",
        "int arr[]; append(arr, 3); resize(arr, 5); arr[1] = 7; i@len = len(arr);",
        "@Cd = @P.x > 0 ? {1, 0, 0} : {0, 0, 1};",
        "@P.xz = @P.zx;",
        "v@vel = set(1, 2, 3); @P += @v * @Time;",
        "float n = noise(@P * 4); vector r = rand(@ptnum); @Cd = r * n;",
        "int bits = (@ptnum & 3) << 2 | 1; i@flags = ~bits ^ 5;",
        "@N = @P ^ @N;",
        "@a = fit(@P.y, 0, 1, -1, 1) + clamp(@P.x, 0, 1) + lerp(0, 1, 0.5) + smooth(0, 1, @P.z);",
        "int a = 5 / 2; float b = 5.0 / 2; s@t = itoa(a) + \" \" + sprintf(\"%g\", b);",
        "matrix m = ident(); m = transpose(m) * invert(m); f@det = determinant(m);",
        "/* block */ float x = 1e-3; // line\n x += 0x10; @w2 = x;",
        "int i = @ptnum; @Cd[i % 3] = 1; @a = @P[i % 3] + getcomp(@P, 1);",
        "if (@ptnum < @numpt / 2 && @Cd.r > 0.5 || !(@id == 3)) @P.y -= 1;",
        "float x = @a; x = x > 1 ? 1 : x; @a = max(x, 0) + min(@ptnum, 3) + pow(2, 3) + atan(1, 2);",
        "vector2 st = @uv.xy; @uv = set(st.x, st.y, 0);",
        "function float half(float x) { return x / 2; } @a = half(@a);",
    };
    for (const char* source : corpus) {
        Compiled c;
        compile(source, c);
        EXPECT_TRUE(c.ok) << source << "\n"
                          << c.error.line << ":" << c.error.column << " " << c.error.message;
        if (c.ok) {
            EXPECT_FALSE(kernelDisassemble(c.program).empty()) << source;
        }
    }
}

TEST(KernelParserTest, ErrorsCarryLineAndColumn)
{
    expectError("float x = 1;\nvector v = \"a\";", 2, "\"a\"", "cannot convert a string to a vector");
    expectError("@P.x = foo + 1;", 1, "foo", "unknown variable 'foo'");
    expectError("@P.x = 1;\n  @P = bar(1);", 2, "bar", "unknown function 'bar'");
    expectError("int h = pcopen(0, \"P\", @P, 1, 8);", 1, "pcopen", "function 'pcopen' is not supported");
    expectError("@P.x = 1 $ 2;", 1, "$", "unexpected character '$'");
    expectError("@P.x = 1\n@P.y = 2;", 2, "@P", "expected ';'");
    expectError("@ptnum = 3;", 1, "@ptnum", "read-only");
    expectError("@opinput1_P = {0, 0, 0};", 1, "@opinput1_P", "read-only");
    expectError("matrix m = ident();\nvector v = m * @P;", 2, "*", "vector * matrix");
    expectError("f@a = 1;\nv@a = {1, 2, 3};", 2, "v@a", "already bound");
    expectError("float g() { @a = 1; }", 1, "g(", "never returns");
    expectError("float f(float x) { return f(x); }\n@a = f(1);", 1, "f(x)", "recursion");
    expectError("matrix2 m;", 1, "matrix2", "not supported");
    expectError("@P = normalize(\"x\");", 1, "\"x\"", "'normalize' takes a vector");
    expectError("string s = \"P\"; vector q = point(1, s, @ptnum);", 1, "s, @", "must be a literal");
    expectError("@P.q = 1;", 1, "q", "no component 'q'");
    expectError("int i = 0;\nbreak;", 2, "break", "only allowed inside a loop");
    expectError("float x = @missing;", 1, "@missing", "no attribute 'missing' here");
    expectError("float x = 1;\nfloat x = 2;", 2, "x", "already declared");
    expectError("vector v = 1;\nv = v + \"s\";", 2, "+", "cannot be applied");
}

TEST(KernelParserTest, KernelDirectoryIncludesNoGeometryHeaders)
{
    const std::filesystem::path kernel = std::filesystem::path(NATRON_TESTS_ENGINE_DIR) / "Kernel";
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(kernel)) {
        files.push_back(entry.path());
    }
    ASSERT_GE(files.size(), 6u);

    for (const std::filesystem::path& path : files) {
        const std::vector<std::string> includes = readIncludeLines(path);
        ASSERT_FALSE(includes.empty()) << path << " could not be read";
        for (const std::string& include : includes) {
            EXPECT_EQ(std::string::npos, include.find("GeoDetail")) << path << ": " << include;
            EXPECT_EQ(std::string::npos, include.find("Geo/")) << path << ": " << include;
        }
    }
}

TEST(KernelParserTest, IndirectRecursionIsAnError)
{
    expectError("float f(float x) { return g(x); }\nfloat g(float x) { return f(x); }\n@a = f(1);", 2, "f(x)", "calls itself");
}

TEST(KernelParserTest, IntegerLiteralsOutsideIntRangeAreErrors)
{
    Compiled lowest;
    compile("i@a = -2147483648;", lowest);
    EXPECT_TRUE(lowest.ok) << lowest.error.message;
    expectError("i@a = 2147483648;", 1, "2147483648", "out of range");
    expectError("i@a = 2147483648 - 1;", 1, "2147483648", "out of range");
    expectError("i@a = 99999999999999999999999;", 1, "9999", "out of range");
    expectError("i@a = 0x100000000;", 1, "0x", "out of range");
    Compiled hex;
    compile("i@a = 0xFFFFFFFF;", hex);
    EXPECT_TRUE(hex.ok) << hex.error.message;
}

TEST(KernelParserTest, AHugeInputNumberIsAnError)
{
    expectError("float x = @opinput99999999999999999999999_P.x;", 1, "@opinput", "out of range");
}

TEST(KernelParserTest, DeepNestingIsAnErrorNotACrash)
{
    const auto nested = [](const std::string& open, const std::string& middle, const std::string& close, int depth) {
        std::string s;
        for (int i = 0; i < depth; ++i) {
            s += open;
        }
        s += middle;
        for (int i = 0; i < depth; ++i) {
            s += close;
        }
        return s;
    };
    const auto sum = [](int terms) {
        std::string s = "a";
        for (int i = 1; i < terms; ++i) {
            s += "+a";
        }
        return s;
    };

    Compiled shallow;
    compile("float x = " + nested("(", "1", ")", 200) + ";\nfloat a = 1;\nfloat y = " + sum(200) + ";", shallow);
    EXPECT_TRUE(shallow.ok) << shallow.error.message;

    for (int depth : { 300, 100000 }) {
        Compiled parens;
        compile("float x = " + nested("(", "1", ")", depth) + ";", parens);
        EXPECT_FALSE(parens.ok);
        EXPECT_NE(std::string::npos, parens.error.message.find("nests more than")) << parens.error.message;

        Compiled unary;
        compile("int x = " + nested("!", "1", "", depth) + ";", unary);
        EXPECT_FALSE(unary.ok);
        EXPECT_NE(std::string::npos, unary.error.message.find("nests more than")) << unary.error.message;

        Compiled blocks;
        compile(nested("{", "@a = 1;", "}", depth), blocks);
        EXPECT_FALSE(blocks.ok);
        EXPECT_NE(std::string::npos, blocks.error.message.find("nests more than")) << blocks.error.message;

        Compiled chain;
        compile("float a = 1;\nfloat y = " + sum(depth) + ";", chain);
        EXPECT_FALSE(chain.ok);
        EXPECT_NE(std::string::npos, chain.error.message.find("nests more than")) << chain.error.message;
    }
}

TEST(KernelParserTest, InliningThatDoublesPerCallIsAnErrorAtTheCallSite)
{
    std::string source = "float f0(float x) { return x + 1; }\n";
    for (int k = 1; k < 40; ++k) {
        source += "float f" + std::to_string(k) + "(float x) { return f" + std::to_string(k - 1) + "(x) + f" + std::to_string(k - 1) + "(x); }\n";
    }
    source += "@a = f39(1);";
    expectError(source, 41, "f39(1)", "larger than");

    std::string empty = "void g0() { }\n";
    for (int k = 1; k < 40; ++k) {
        empty += "void g" + std::to_string(k) + "() { g" + std::to_string(k - 1) + "(); g" + std::to_string(k - 1) + "(); }\n";
    }
    empty += "g39();";
    expectError(empty, 41, "g39()", "larger than");
}
