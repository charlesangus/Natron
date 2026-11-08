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

#include "KernelParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <utility>

NATRON_NAMESPACE_ENTER

namespace {

using B = KernelBaseType;

struct Loc {
    int line = 0;
    int column = 0;
};

struct Failure {
    KernelCompileError error;
};

[[noreturn]] void
fail(Loc at, std::string message)
{
    throw Failure { KernelCompileError { at.line, at.column, std::move(message) } };
}

Loc
locOf(const KernelToken& t)
{
    return Loc { t.line, t.column };
}

std::optional<KernelBaseType>
typeKeyword(std::string_view s)
{
    static const std::pair<std::string_view, KernelBaseType> table[] = {
        { "void", B::eVoid },
        { "int", B::eInt },
        { "float", B::eFloat },
        { "vector2", B::eVector2 },
        { "vector", B::eVector },
        { "vector4", B::eVector4 },
        { "matrix3", B::eMatrix3 },
        { "matrix", B::eMatrix },
        { "string", B::eString },
    };
    for (const auto& entry : table) {
        if (entry.first == s) {
            return entry.second;
        }
    }
    return std::nullopt;
}

bool
isUnsupportedTypeKeyword(std::string_view s)
{
    return s == "matrix2" || s == "dict" || s == "bsdf";
}

bool
isReservedWord(std::string_view s)
{
    static const std::set<std::string_view> words = { "if", "else", "for", "foreach", "while", "do", "return", "break",
                                                      "continue", "function", "const", "export" };
    return words.count(s) || typeKeyword(s) || isUnsupportedTypeKeyword(s);
}

// Names the language reserves for functions it does not implement, so a snippet using one gets a
// clear message rather than "unknown function". A binding may still provide any of them.
bool
isUnsupportedFunction(std::string_view s)
{
    static const std::set<std::string_view> names = {
        "addpoint",
        "addprim",
        "addvertex",
        "anoise",
        "argsort",
        "attribsize",
        "attribtype",
        "ch",
        "ch2",
        "ch3",
        "ch4",
        "chf",
        "chi",
        "chp",
        "chramp",
        "chs",
        "chu",
        "chv",
        "cracktransform",
        "curlnoise",
        "curlnoise2d",
        "detail",
        "dihedral",
        "error",
        "eulertoquaternion",
        "expandpointgroup",
        "expandprimgroup",
        "find",
        "findattribval",
        "findattribvalcount",
        "flownoise",
        "fresnel",
        "ftoa",
        "getbbox",
        "getbbox_center",
        "getbbox_max",
        "getbbox_min",
        "getbbox_size",
        "hasattrib",
        "haspointattrib",
        "hasprimattrib",
        "hasvertexattrib",
        "idtopoint",
        "idtoprim",
        "inpointgroup",
        "inprimgroup",
        "insert",
        "intersect",
        "intersect_all",
        "invertexgroup",
        "isvalidindex",
        "join",
        "lookat",
        "lstrip",
        "maketransform",
        "nametopoint",
        "nametoprim",
        "nearpoint",
        "nearpoints",
        "neighbour",
        "neighbourcount",
        "neighbours",
        "npoints",
        "nprimitives",
        "nvertices",
        "onoise",
        "pcclose",
        "pcfilter",
        "pcfind",
        "pcimport",
        "pciterate",
        "pcnumfound",
        "pcopen",
        "pnoise",
        "point",
        "pointprims",
        "pointvertices",
        "polardecomp",
        "polyneighbours",
        "pop",
        "prim",
        "primintrinsic",
        "primpoints",
        "primuv",
        "primvertexcount",
        "print_once",
        "printf",
        "qconvert",
        "qinvert",
        "quaterniontoeuler",
        "random",
        "re_find",
        "re_match",
        "re_replace",
        "reflect",
        "refract",
        "relbbox",
        "relpointbbox",
        "removeindex",
        "removepoint",
        "removeprim",
        "removevalue",
        "removevertex",
        "replace",
        "reverse",
        "rotate",
        "rstrip",
        "scale",
        "setdetailattrib",
        "setpointattrib",
        "setpointgroup",
        "setprimattrib",
        "setprimgroup",
        "setprimintrinsic",
        "setprimvertex",
        "setvertexattrib",
        "setvertexgroup",
        "shuffle",
        "slerp",
        "snoise",
        "sort",
        "split",
        "spline",
        "strip",
        "tolower",
        "toupper",
        "translate",
        "uvdist",
        "uvsample",
        "vertex",
        "vertexpoint",
        "vertexprimindex",
        "vnoise",
        "volumegradient",
        "volumeindex",
        "volumesample",
        "volumesamplev",
        "warning",
        "wnoise",
        "xnoise",
        "xyzdist",
    };
    return names.count(s) != 0;
}

KernelType
knownAttributeType(std::string_view name)
{
    static const std::map<std::string_view, KernelBaseType> table = {
        { "P", B::eVector },
        { "N", B::eVector },
        { "v", B::eVector },
        { "Cd", B::eVector },
        { "uv", B::eVector },
        { "up", B::eVector },
        { "accel", B::eVector },
        { "rest", B::eVector },
        { "force", B::eVector },
        { "scale", B::eVector },
        { "pivot", B::eVector },
        { "trans", B::eVector },
        { "center", B::eVector },
        { "w", B::eVector },
        { "orient", B::eVector4 },
        { "rot", B::eVector4 },
        { "id", B::eInt },
        { "name", B::eString },
        { "instance", B::eString },
    };
    auto it = table.find(name);
    return KernelType(it == table.end() ? B::eFloat : it->second);
}

// ---------------------------------------------------------------------------------------------
// Syntax tree
// ---------------------------------------------------------------------------------------------

enum class ExprKind : std::uint8_t {
    eInt,
    eFloat,
    eString,
    eBraceList,
    eVariable,
    eBinding,
    eUnary,
    eBinary,
    eAssign,
    eIncDec,
    eTernary,
    eCall,
    eMember,
    eIndex,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind = ExprKind::eInt;
    Loc loc;
    // Operator, variable, function or component name, string value, or binding name.
    std::string text;
    std::string prefix;
    int input = 0;
    std::int64_t intValue = 0;
    double floatValue = 0.;
    bool prefixForm = false;
    std::vector<ExprPtr> args;
};

enum class StmtKind : std::uint8_t {
    eEmpty,
    eExpr,
    eDecl,
    eBlock,
    eIf,
    eFor,
    eForeach,
    eWhile,
    eDoWhile,
    eReturn,
    eBreak,
    eContinue,
};

struct Declarator {
    std::string name;
    Loc loc;
    // void for an untyped foreach variable.
    KernelType type;
    ExprPtr init;
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
    StmtKind kind = StmtKind::eEmpty;
    Loc loc;
    // The expression, condition, returned value, or foreach array.
    ExprPtr expr;
    ExprPtr step;
    StmtPtr init;
    // Declarations; a foreach loop's [index,] value.
    std::vector<Declarator> decls;
    bool foreachHasIndex = false;
    // A block's statements; if: then [, else]; loops: the body.
    std::vector<StmtPtr> body;
};

struct Param {
    KernelType type;
    std::string name;
    Loc loc;
    bool isConst = false;
};

struct Function {
    KernelType result;
    std::string name;
    Loc loc;
    std::vector<Param> params;
    std::vector<StmtPtr> body;
};

struct Unit {
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<StmtPtr> main;
};

template <class F>
void
visitExpr(const Expr& e, F& f)
{
    f(e);
    for (const ExprPtr& a : e.args) {
        visitExpr(*a, f);
    }
}

template <class F>
void
visitStmt(const Stmt& s, F& f)
{
    if (s.expr) {
        visitExpr(*s.expr, f);
    }
    if (s.step) {
        visitExpr(*s.step, f);
    }
    for (const Declarator& d : s.decls) {
        if (d.init) {
            visitExpr(*d.init, f);
        }
    }
    if (s.init) {
        visitStmt(*s.init, f);
    }
    for (const StmtPtr& b : s.body) {
        visitStmt(*b, f);
    }
}

const Expr&
lvalueRoot(const Expr& e)
{
    const Expr* r = &e;
    while ((r->kind == ExprKind::eMember || r->kind == ExprKind::eIndex) && !r->args.empty()) {
        r = r->args[0].get();
    }
    return *r;
}

int
countReturns(const Stmt& s)
{
    int n = s.kind == StmtKind::eReturn ? 1 : 0;
    if (s.init) {
        n += countReturns(*s.init);
    }
    for (const StmtPtr& b : s.body) {
        n += countReturns(*b);
    }
    return n;
}

int
countReturns(const std::vector<StmtPtr>& body)
{
    int n = 0;
    for (const StmtPtr& s : body) {
        n += countReturns(*s);
    }
    return n;
}

// A return anywhere but as the function's last statement retires lanes early, which needs a mask.
bool
needsReturnMask(const Function& fn)
{
    const int n = countReturns(fn.body);
    if (n == 0) {
        return false;
    }
    return !(n == 1 && fn.body.back()->kind == StmtKind::eReturn);
}

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::vector<KernelToken> tokens)
        : _t(std::move(tokens))
    {
    }

    Unit parse()
    {
        Unit unit;
        while (cur().kind != KernelToken::Kind::eEnd) {
            if (atFunctionDefinition()) {
                unit.functions.push_back(function());
            } else {
                unit.main.push_back(statement());
            }
        }
        return unit;
    }

private:
    std::vector<KernelToken> _t;
    std::size_t _i = 0;

    const KernelToken& cur() const { return _t[_i]; }
    const KernelToken& peek(std::size_t n) const { return _t[std::min(_i + n, _t.size() - 1)]; }
    bool isPunct(std::string_view p) const { return cur().isPunct(p); }

    bool accept(std::string_view p)
    {
        if (isPunct(p)) {
            ++_i;
            return true;
        }
        return false;
    }

    std::string found() const
    {
        const KernelToken& t = cur();
        switch (t.kind) {
        case KernelToken::Kind::eEnd:
            return " at the end of the snippet";
        case KernelToken::Kind::eString:
            return " before a string";
        case KernelToken::Kind::eBinding:
            return " before '" + t.prefix + "@" + t.text + "'";
        default:
            return " before '" + t.text + "'";
        }
    }

    KernelToken expect(std::string_view p)
    {
        if (!isPunct(p)) {
            fail(locOf(cur()), "expected '" + std::string(p) + "'" + found());
        }
        return _t[_i++];
    }

    bool typeAt(std::size_t n) const
    {
        const KernelToken& t = peek(n);
        return t.kind == KernelToken::Kind::eIdentifier && (typeKeyword(t.text) || isUnsupportedTypeKeyword(t.text));
    }

    std::size_t afterType(std::size_t n) const
    {
        ++n;
        if (peek(n).isPunct("[") && peek(n + 1).isPunct("]")) {
            n += 2;
        }
        return n;
    }

    bool atFunctionDefinition() const
    {
        if (cur().isIdentifier("function")) {
            return true;
        }
        if (!typeAt(0)) {
            return false;
        }
        const std::size_t k = afterType(0);
        return peek(k).kind == KernelToken::Kind::eIdentifier && peek(k + 1).isPunct("(");
    }

    bool atDeclaration() const
    {
        std::size_t k = 0;
        while (peek(k).isIdentifier("const") || peek(k).isIdentifier("export")) {
            ++k;
        }
        if (!typeAt(k)) {
            return false;
        }
        const KernelToken& next = peek(k + 1);
        return next.kind == KernelToken::Kind::eIdentifier || (next.isPunct("[") && peek(k + 2).isPunct("]"));
    }

    KernelType type(bool allowVoid, const std::string& what)
    {
        const KernelToken t = cur();
        if (t.kind == KernelToken::Kind::eIdentifier && isUnsupportedTypeKeyword(t.text)) {
            fail(locOf(t), "type '" + t.text + "' is not supported");
        }
        const std::optional<KernelBaseType> base = t.kind == KernelToken::Kind::eIdentifier ? typeKeyword(t.text) : std::nullopt;
        if (!base) {
            fail(locOf(t), "expected a type" + found());
        }
        ++_i;
        KernelType result(*base);
        if (isPunct("[") && peek(1).isPunct("]")) {
            _i += 2;
            result.array = true;
        }
        if (result.base == B::eVoid && (!allowVoid || result.array)) {
            fail(locOf(t), what + " cannot be void");
        }
        return result;
    }

    std::string identifier(const std::string& what)
    {
        const KernelToken& t = cur();
        if (t.kind != KernelToken::Kind::eIdentifier) {
            fail(locOf(t), "expected " + what + found());
        }
        if (isReservedWord(t.text)) {
            fail(locOf(t), "'" + t.text + "' is a reserved word and cannot name " + what);
        }
        ++_i;
        return t.text;
    }

    bool startsTypedParameter(std::size_t n) const
    {
        return peek(n).isIdentifier("const") || peek(n).isIdentifier("export") || typeAt(n);
    }

    std::unique_ptr<Function> function()
    {
        if (cur().isIdentifier("function")) {
            ++_i;
        }
        auto fn = std::make_unique<Function>();
        fn->result = type(true, "an array result");
        fn->loc = locOf(cur());
        fn->name = identifier("a function");
        expect("(");
        if (!isPunct(")")) {
            while (true) {
                bool isConst = false;
                while (cur().isIdentifier("const") || cur().isIdentifier("export")) {
                    isConst |= cur().text == "const";
                    ++_i;
                }
                const KernelType paramType = type(false, "a parameter");
                while (true) {
                    Param p;
                    p.loc = locOf(cur());
                    p.name = identifier("a parameter");
                    p.type = paramType;
                    p.isConst = isConst;
                    if (isPunct("[") && peek(1).isPunct("]")) {
                        if (paramType.array) {
                            fail(locOf(cur()), "arrays of arrays are not supported");
                        }
                        _i += 2;
                        p.type.array = true;
                    }
                    fn->params.push_back(std::move(p));
                    if (isPunct(",") && !startsTypedParameter(1)) {
                        ++_i;
                        continue;
                    }
                    break;
                }
                if (accept(";") || accept(",")) {
                    continue;
                }
                break;
            }
        }
        expect(")");
        if (!isPunct("{")) {
            fail(locOf(cur()), "expected '{'" + found());
        }
        fn->body = blockBody();
        return fn;
    }

    std::vector<StmtPtr> blockBody()
    {
        expect("{");
        std::vector<StmtPtr> body;
        while (!isPunct("}")) {
            if (cur().kind == KernelToken::Kind::eEnd) {
                fail(locOf(cur()), "expected '}'" + found());
            }
            body.push_back(statement());
        }
        ++_i;
        return body;
    }

    void declaration(Stmt& s)
    {
        s.kind = StmtKind::eDecl;
        while (cur().isIdentifier("const") || cur().isIdentifier("export")) {
            ++_i;
        }
        const KernelType base = type(false, "a variable");
        while (true) {
            Declarator d;
            d.loc = locOf(cur());
            d.name = identifier("a variable");
            d.type = base;
            if (isPunct("(")) {
                fail(d.loc, "functions can only be defined at the top level");
            }
            if (accept("[")) {
                expect("]");
                if (base.array) {
                    fail(d.loc, "arrays of arrays are not supported");
                }
                d.type.array = true;
            }
            if (accept("=")) {
                d.init = assignment();
            }
            s.decls.push_back(std::move(d));
            if (!accept(",")) {
                break;
            }
        }
    }

    Declarator loopVariable()
    {
        Declarator d;
        if (typeAt(0) && peek(1).kind == KernelToken::Kind::eIdentifier) {
            d.type = type(false, "a loop variable");
        }
        d.loc = locOf(cur());
        d.name = identifier("a loop variable");
        return d;
    }

    StmtPtr statement()
    {
        const KernelToken t = cur();
        auto s = std::make_unique<Stmt>();
        s->loc = locOf(t);
        if (accept(";")) {
            return s;
        }
        if (isPunct("{")) {
            s->kind = StmtKind::eBlock;
            s->body = blockBody();
            return s;
        }
        if (t.kind == KernelToken::Kind::eIdentifier) {
            const std::string& w = t.text;
            if (w == "if") {
                ++_i;
                s->kind = StmtKind::eIf;
                expect("(");
                s->expr = expression();
                expect(")");
                s->body.push_back(statement());
                if (cur().isIdentifier("else")) {
                    ++_i;
                    s->body.push_back(statement());
                }
                return s;
            }
            if (w == "while") {
                ++_i;
                s->kind = StmtKind::eWhile;
                expect("(");
                s->expr = expression();
                expect(")");
                s->body.push_back(statement());
                return s;
            }
            if (w == "do") {
                ++_i;
                s->kind = StmtKind::eDoWhile;
                s->body.push_back(statement());
                if (!cur().isIdentifier("while")) {
                    fail(locOf(cur()), "expected 'while'" + found());
                }
                ++_i;
                expect("(");
                s->expr = expression();
                expect(")");
                expect(";");
                return s;
            }
            if (w == "for") {
                ++_i;
                s->kind = StmtKind::eFor;
                expect("(");
                if (!isPunct(";")) {
                    auto init = std::make_unique<Stmt>();
                    init->loc = locOf(cur());
                    if (atDeclaration()) {
                        declaration(*init);
                    } else {
                        init->kind = StmtKind::eExpr;
                        init->expr = expression();
                    }
                    s->init = std::move(init);
                }
                expect(";");
                if (!isPunct(";")) {
                    s->expr = expression();
                }
                expect(";");
                if (!isPunct(")")) {
                    s->step = expression();
                }
                expect(")");
                s->body.push_back(statement());
                return s;
            }
            if (w == "foreach") {
                ++_i;
                s->kind = StmtKind::eForeach;
                expect("(");
                Declarator first = loopVariable();
                if (accept(",")) {
                    if (!first.type.isVoid() && !first.type.is(B::eInt)) {
                        fail(first.loc, "the index of a foreach loop is an int");
                    }
                    first.type = KernelType(B::eInt);
                    s->foreachHasIndex = true;
                    s->decls.push_back(std::move(first));
                    s->decls.push_back(loopVariable());
                } else {
                    s->decls.push_back(std::move(first));
                }
                expect(";");
                s->expr = expression();
                expect(")");
                s->body.push_back(statement());
                return s;
            }
            if (w == "return") {
                ++_i;
                s->kind = StmtKind::eReturn;
                if (!isPunct(";")) {
                    s->expr = expression();
                }
                expect(";");
                return s;
            }
            if (w == "break" || w == "continue") {
                ++_i;
                s->kind = w == "break" ? StmtKind::eBreak : StmtKind::eContinue;
                expect(";");
                return s;
            }
            if (w == "else") {
                fail(s->loc, "'else' without a matching 'if'");
            }
            if (w == "function") {
                fail(s->loc, "functions can only be defined at the top level");
            }
            if (atDeclaration()) {
                declaration(*s);
                expect(";");
                return s;
            }
        }
        s->kind = StmtKind::eExpr;
        s->expr = expression();
        expect(";");
        return s;
    }

    static ExprPtr make(ExprKind kind, Loc at)
    {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->loc = at;
        return e;
    }

    ExprPtr expression() { return assignment(); }

    ExprPtr assignment()
    {
        ExprPtr lhs = ternary();
        static const char* const ops[] = { "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=" };
        for (const char* op : ops) {
            if (isPunct(op)) {
                const Loc at = locOf(cur());
                ++_i;
                ExprPtr e = make(ExprKind::eAssign, at);
                e->text = op;
                e->args.push_back(std::move(lhs));
                e->args.push_back(assignment());
                return e;
            }
        }
        return lhs;
    }

    ExprPtr ternary()
    {
        ExprPtr c = binary(0);
        if (!isPunct("?")) {
            return c;
        }
        const Loc at = locOf(cur());
        ++_i;
        ExprPtr e = make(ExprKind::eTernary, at);
        e->args.push_back(std::move(c));
        e->args.push_back(expression());
        expect(":");
        e->args.push_back(ternary());
        return e;
    }

    ExprPtr binary(std::size_t level)
    {
        static const std::vector<std::vector<std::string_view>> levels = {
            { "||" },
            { "&&" },
            { "|" },
            { "^" },
            { "&" },
            { "==", "!=" },
            { "<", "<=", ">", ">=" },
            { "<<", ">>" },
            { "+", "-" },
            { "*", "/", "%" },
        };
        if (level == levels.size()) {
            return unary();
        }
        ExprPtr a = binary(level + 1);
        while (true) {
            const KernelToken& t = cur();
            if (t.kind != KernelToken::Kind::ePunct
                || std::find(levels[level].begin(), levels[level].end(), t.text) == levels[level].end()) {
                break;
            }
            ExprPtr e = make(ExprKind::eBinary, locOf(t));
            e->text = t.text;
            ++_i;
            e->args.push_back(std::move(a));
            e->args.push_back(binary(level + 1));
            a = std::move(e);
        }
        return a;
    }

    ExprPtr unary()
    {
        const KernelToken t = cur();
        if (t.isPunct("-") || t.isPunct("+") || t.isPunct("!") || t.isPunct("~")) {
            ++_i;
            ExprPtr a = unary();
            if (t.text == "+") {
                return a;
            }
            if (t.text == "-" && (a->kind == ExprKind::eInt || a->kind == ExprKind::eFloat)) {
                a->intValue = -a->intValue;
                a->floatValue = -a->floatValue;
                a->loc = locOf(t);
                return a;
            }
            ExprPtr e = make(ExprKind::eUnary, locOf(t));
            e->text = t.text;
            e->args.push_back(std::move(a));
            return e;
        }
        if (t.isPunct("++") || t.isPunct("--")) {
            ++_i;
            ExprPtr e = make(ExprKind::eIncDec, locOf(t));
            e->text = t.text;
            e->prefixForm = true;
            e->args.push_back(unary());
            return e;
        }
        return postfix();
    }

    ExprPtr postfix()
    {
        ExprPtr e = primary();
        while (true) {
            const KernelToken t = cur();
            if (t.isPunct("[")) {
                ++_i;
                ExprPtr n = make(ExprKind::eIndex, locOf(t));
                n->args.push_back(std::move(e));
                n->args.push_back(expression());
                expect("]");
                e = std::move(n);
            } else if (t.isPunct(".")) {
                ++_i;
                if (cur().kind != KernelToken::Kind::eIdentifier) {
                    fail(locOf(cur()), "expected a component name after '.'");
                }
                ExprPtr n = make(ExprKind::eMember, locOf(cur()));
                n->text = cur().text;
                ++_i;
                n->args.push_back(std::move(e));
                e = std::move(n);
            } else if (t.isPunct("++") || t.isPunct("--")) {
                ++_i;
                ExprPtr n = make(ExprKind::eIncDec, locOf(t));
                n->text = t.text;
                n->args.push_back(std::move(e));
                e = std::move(n);
            } else {
                return e;
            }
        }
    }

    ExprPtr primary()
    {
        const KernelToken t = cur();
        switch (t.kind) {
        case KernelToken::Kind::eInt: {
            ++_i;
            ExprPtr e = make(ExprKind::eInt, locOf(t));
            e->intValue = t.intValue;
            e->floatValue = static_cast<double>(t.intValue);
            return e;
        }
        case KernelToken::Kind::eFloat: {
            ++_i;
            ExprPtr e = make(ExprKind::eFloat, locOf(t));
            e->floatValue = t.floatValue;
            return e;
        }
        case KernelToken::Kind::eString: {
            ++_i;
            ExprPtr e = make(ExprKind::eString, locOf(t));
            e->text = t.text;
            while (cur().kind == KernelToken::Kind::eString) {
                e->text += cur().text;
                ++_i;
            }
            return e;
        }
        case KernelToken::Kind::eBinding: {
            ++_i;
            ExprPtr e = make(ExprKind::eBinding, locOf(t));
            e->prefix = t.prefix;
            e->text = t.text;
            const std::string_view lead = "opinput";
            if (t.text.compare(0, lead.size(), lead) == 0) {
                std::size_t k = lead.size();
                while (k < t.text.size() && std::isdigit(static_cast<unsigned char>(t.text[k]))) {
                    ++k;
                }
                if (k > lead.size() && k + 1 < t.text.size() && t.text[k] == '_') {
                    e->input = std::stoi(t.text.substr(lead.size(), k - lead.size()));
                    e->text = t.text.substr(k + 1);
                }
            }
            return e;
        }
        case KernelToken::Kind::eIdentifier: {
            if (isReservedWord(t.text) && !typeKeyword(t.text)) {
                fail(locOf(t), "unexpected '" + t.text + "'");
            }
            ++_i;
            if (isPunct("(")) {
                ++_i;
                ExprPtr e = make(ExprKind::eCall, locOf(t));
                e->text = t.text;
                if (!isPunct(")")) {
                    do {
                        e->args.push_back(assignment());
                    } while (accept(","));
                }
                expect(")");
                return e;
            }
            if (typeKeyword(t.text)) {
                fail(locOf(t), "expected an expression, found the type '" + t.text + "'");
            }
            ExprPtr e = make(ExprKind::eVariable, locOf(t));
            e->text = t.text;
            return e;
        }
        case KernelToken::Kind::ePunct:
            if (t.text == "(") {
                ++_i;
                ExprPtr e = expression();
                expect(")");
                return e;
            }
            if (t.text == "{") {
                ++_i;
                ExprPtr e = make(ExprKind::eBraceList, locOf(t));
                if (!isPunct("}")) {
                    do {
                        e->args.push_back(assignment());
                    } while (accept(","));
                }
                expect("}");
                return e;
            }
            break;
        case KernelToken::Kind::eEnd:
            break;
        }
        fail(locOf(t), "expected an expression" + found());
    }
};

// ---------------------------------------------------------------------------------------------
// Instruction table
// ---------------------------------------------------------------------------------------------

// Banks of d, a, b, c ('f', 'i', 's', 'a', or 0 when unused) and the immediate's meaning: 'F' fimm,
// 'I' an int, 'S' a string constant, 'T' a jump target, 'E' an element type, 'N' a function.
struct OpInfo {
    const char* name;
    char d, a, b, c;
    char imm;
    bool operands;
};

OpInfo
opInfo(KernelOp op)
{
    switch (op) {
    case KernelOp::eFConst:
        return { "fconst", 'f', 0, 0, 0, 'F', false };
    case KernelOp::eIConst:
        return { "iconst", 'i', 0, 0, 0, 'I', false };
    case KernelOp::eSConst:
        return { "sconst", 's', 0, 0, 0, 'S', false };
    case KernelOp::eFMov:
        return { "fmov", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eIMov:
        return { "imov", 'i', 'i', 0, 0, 0, false };
    case KernelOp::eSMov:
        return { "smov", 's', 's', 0, 0, 0, false };
    case KernelOp::eACopy:
        return { "acopy", 'a', 'a', 0, 0, 0, false };
    case KernelOp::eFSel:
        return { "fsel", 'f', 'i', 'f', 'f', 0, false };
    case KernelOp::eISel:
        return { "isel", 'i', 'i', 'i', 'i', 0, false };
    case KernelOp::eSSel:
        return { "ssel", 's', 'i', 's', 's', 0, false };
    case KernelOp::eASel:
        return { "asel", 'a', 'i', 'a', 'a', 0, false };
    case KernelOp::eFAdd:
        return { "fadd", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFSub:
        return { "fsub", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFMul:
        return { "fmul", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFDiv:
        return { "fdiv", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFMod:
        return { "fmod", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFMulAdd:
        return { "fmuladd", 'f', 'f', 'f', 'f', 0, false };
    case KernelOp::eFNeg:
        return { "fneg", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFMin:
        return { "fmin", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFMax:
        return { "fmax", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFPow:
        return { "fpow", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFAtan2:
        return { "fatan2", 'f', 'f', 'f', 0, 0, false };
    case KernelOp::eFAbs:
        return { "fabs", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFFloor:
        return { "ffloor", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFCeil:
        return { "fceil", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFRound:
        return { "fround", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFTrunc:
        return { "ftrunc", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFFrac:
        return { "ffrac", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFSign:
        return { "fsign", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFSqrt:
        return { "fsqrt", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFExp:
        return { "fexp", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFLog:
        return { "flog", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFLog10:
        return { "flog10", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFSin:
        return { "fsin", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFCos:
        return { "fcos", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFTan:
        return { "ftan", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFAsin:
        return { "fasin", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFAcos:
        return { "facos", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFAtan:
        return { "fatan", 'f', 'f', 0, 0, 0, false };
    case KernelOp::eFLt:
        return { "flt", 'i', 'f', 'f', 0, 0, false };
    case KernelOp::eFLe:
        return { "fle", 'i', 'f', 'f', 0, 0, false };
    case KernelOp::eFGt:
        return { "fgt", 'i', 'f', 'f', 0, 0, false };
    case KernelOp::eFGe:
        return { "fge", 'i', 'f', 'f', 0, 0, false };
    case KernelOp::eFEq:
        return { "feq", 'i', 'f', 'f', 0, 0, false };
    case KernelOp::eFNe:
        return { "fne", 'i', 'f', 'f', 0, 0, false };
    case KernelOp::eIAdd:
        return { "iadd", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eISub:
        return { "isub", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIMul:
        return { "imul", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIDiv:
        return { "idiv", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIMod:
        return { "imod", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eINeg:
        return { "ineg", 'i', 'i', 0, 0, 0, false };
    case KernelOp::eIAbs:
        return { "iabs", 'i', 'i', 0, 0, 0, false };
    case KernelOp::eIMin:
        return { "imin", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIMax:
        return { "imax", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIBitAnd:
        return { "ibitand", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIBitOr:
        return { "ibitor", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIBitXor:
        return { "ibitxor", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIBitNot:
        return { "ibitnot", 'i', 'i', 0, 0, 0, false };
    case KernelOp::eIShl:
        return { "ishl", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIShr:
        return { "ishr", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eILt:
        return { "ilt", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eILe:
        return { "ile", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIGt:
        return { "igt", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIGe:
        return { "ige", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIEq:
        return { "ieq", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eINe:
        return { "ine", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIBool:
        return { "ibool", 'i', 'i', 0, 0, 0, false };
    case KernelOp::eINot:
        return { "inot", 'i', 'i', 0, 0, 0, false };
    case KernelOp::eIAnd:
        return { "iand", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIOr:
        return { "ior", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eIAndNot:
        return { "iandnot", 'i', 'i', 'i', 0, 0, false };
    case KernelOp::eI2F:
        return { "i2f", 'f', 'i', 0, 0, 0, false };
    case KernelOp::eF2I:
        return { "f2i", 'i', 'f', 0, 0, 0, false };
    case KernelOp::eSEq:
        return { "seq", 'i', 's', 's', 0, 0, false };
    case KernelOp::eSNe:
        return { "sne", 'i', 's', 's', 0, 0, false };
    case KernelOp::eSConcat:
        return { "sconcat", 's', 's', 's', 0, 0, false };
    case KernelOp::eANew:
        return { "anew", 'a', 0, 0, 0, 'E', false };
    case KernelOp::eALen:
        return { "alen", 'i', 'a', 0, 0, 0, false };
    case KernelOp::eAGet:
        return { "aget", 0, 'a', 'i', 0, 'E', true };
    case KernelOp::eASet:
        return { "aset", 0, 'i', 'a', 'i', 'E', true };
    case KernelOp::eAPush:
        return { "apush", 0, 'i', 'a', 0, 'E', true };
    case KernelOp::eAResize:
        return { "aresize", 0, 'i', 'a', 'i', 'E', false };
    case KernelOp::eJump:
        return { "jump", 0, 0, 0, 0, 'T', false };
    case KernelOp::eJumpIfNone:
        return { "jumpifnone", 0, 'i', 0, 0, 'T', false };
    case KernelOp::eCall:
        return { "call", 0, 'i', 0, 0, 'N', true };
    }
    return { "?", 0, 0, 0, 0, 0, false };
}

char
bankLetter(KernelBank bank)
{
    switch (bank) {
    case KernelBank::eFloat:
        return 'f';
    case KernelBank::eInt:
        return 'i';
    case KernelBank::eString:
        return 's';
    case KernelBank::eArray:
        return 'a';
    }
    return '?';
}

KernelOp
movOp(KernelBank bank)
{
    switch (bank) {
    case KernelBank::eFloat:
        return KernelOp::eFMov;
    case KernelBank::eInt:
        return KernelOp::eIMov;
    case KernelBank::eString:
        return KernelOp::eSMov;
    case KernelBank::eArray:
        return KernelOp::eACopy;
    }
    return KernelOp::eFMov;
}

KernelOp
selOp(KernelBank bank)
{
    switch (bank) {
    case KernelBank::eFloat:
        return KernelOp::eFSel;
    case KernelBank::eInt:
        return KernelOp::eISel;
    case KernelBank::eString:
        return KernelOp::eSSel;
    case KernelBank::eArray:
        return KernelOp::eASel;
    }
    return KernelOp::eFSel;
}

// ---------------------------------------------------------------------------------------------
// Lowering to bytecode
// ---------------------------------------------------------------------------------------------

struct Value {
    KernelType type;
    std::array<std::int32_t, 16> r {};
    // The registers are temporaries of the current statement that nothing else refers to.
    bool temporary = false;
    bool literal = false;
    // An int known to be 0 or 1.
    bool boolean = false;
    double number = 0.;

    int count() const { return type.array ? 1 : kernelComponentCount(type.base); }
    KernelBank bank() const { return kernelBankOf(type); }
};

bool
isScalar(KernelType t)
{
    return t.is(B::eInt) || t.is(B::eFloat);
}

bool
isFloatTuple(KernelType t)
{
    return !t.array && kernelIsFloatTuple(t.base);
}

KernelBaseType
tupleType(int n)
{
    switch (n) {
    case 1:
        return B::eFloat;
    case 2:
        return B::eVector2;
    case 3:
        return B::eVector;
    case 4:
        return B::eVector4;
    case 9:
        return B::eMatrix3;
    case 16:
        return B::eMatrix;
    default:
        return B::eVoid;
    }
}

int
matrixDim(KernelBaseType t)
{
    return t == B::eMatrix3 ? 3 : 4;
}

std::string
article(KernelType t)
{
    const std::string n = kernelTypeName(t);
    return (std::strchr("aeiou", n[0]) ? "an " : "a ") + n;
}

std::string
bindingDisplayName(const std::string& name, int input)
{
    return input ? "@opinput" + std::to_string(input) + "_" + name : "@" + name;
}

std::string
bindingKey(const std::string& name, int input)
{
    return std::to_string(input) + ":" + name;
}

bool
isLiteral(const Expr& e)
{
    return e.kind == ExprKind::eInt || e.kind == ExprKind::eFloat || e.kind == ExprKind::eString;
}

bool
hasSideEffects(const Expr& e)
{
    bool found = false;
    auto f = [&](const Expr& x) {
        found |= x.kind == ExprKind::eAssign || x.kind == ExprKind::eIncDec || x.kind == ExprKind::eCall;
    };
    visitExpr(e, f);
    return found;
}

// Cost of passing a value of type from where to is expected, or -1 when it cannot be passed.
int
conversionCost(KernelType from, KernelType to)
{
    if (from == to) {
        return 0;
    }
    if (from.array || to.array) {
        return -1;
    }
    if (from.base == B::eInt && to.base == B::eFloat) {
        return 1;
    }
    if (from.base == B::eFloat && to.base == B::eInt) {
        return 4;
    }
    if (from.base == B::eFloat && (kernelIsVector(to.base) || kernelIsMatrix(to.base))) {
        return 2;
    }
    if (from.base == B::eInt && (kernelIsVector(to.base) || kernelIsMatrix(to.base))) {
        return 3;
    }
    return -1;
}

std::string
typeList(const std::vector<KernelType>& types)
{
    std::string s = "(";
    for (std::size_t i = 0; i < types.size(); ++i) {
        s += (i ? ", " : "") + kernelTypeName(types[i]);
    }
    return s + ")";
}

class Compiler {
public:
    Compiler(const KernelBinding& binding, KernelProgram& program, const Unit& unit)
        : _binding(binding)
        , _p(program)
        , _unit(unit)
    {
        _p = KernelProgram {};
        for (const auto& fn : unit.functions) {
            _userFunctions[fn->name].push_back(fn.get());
        }
        collectWrittenBindings();
    }

    void compileMain()
    {
        checkFunctionDefinitions();
        bool mainReturns = false;
        for (const StmtPtr& s : _unit.main) {
            mainReturns |= countReturns(*s) > 0;
        }
        pushScope();
        beginStatement();
        if (mainReturns) {
            const std::int32_t m = temp(KernelBank::eInt);
            emit(KernelOp::eIMov, m, KernelProgram::kEntryMaskRegister);
            _maskChain.push_back(m);
            _mask = m;
            _masked = true;
        }
        for (const StmtPtr& s : _unit.main) {
            statement(*s);
        }
        endStatement();
        popScope();
        finish();
    }

    std::vector<const Function*> uncalledFunctions() const
    {
        std::vector<const Function*> out;
        for (const auto& fn : _unit.functions) {
            if (!_called.count(fn.get())) {
                out.push_back(fn.get());
            }
        }
        return out;
    }

    void checkFunction(const Function& fn)
    {
        pushScope();
        beginStatement();
        std::vector<Symbol> params;
        for (const Param& p : fn.params) {
            Symbol s;
            s.value = tempValue(p.type);
            s.value.temporary = false;
            s.readOnly = p.isConst;
            params.push_back(s);
        }
        inlineFunction(fn, params);
        endStatement();
        popScope();
    }

private:
    struct Symbol {
        Value value;
        std::int32_t binding = -1;
        bool readOnly = false;
    };

    struct Scope {
        std::map<std::string, Symbol> symbols;
        std::vector<KernelOperand> owned;
    };

    struct LoopContext {
        std::int32_t loopMask = 0;
        std::size_t chainDepth = 0;
        std::size_t functionDepth = 0;
    };

    struct FunctionContext {
        const Function* fn = nullptr;
        Value result;
        bool needsMask = false;
        std::size_t chainDepth = 0;
        std::size_t loopDepth = 0;
    };

    struct LValue {
        enum class Kind {
            eRegisters,
            eArrayElement,
            eDynamicComponent,
        };
        Kind kind = Kind::eRegisters;
        KernelType type;
        Value whole;
        // The components of whole written, empty for all of them.
        std::vector<int> comps;
        std::int32_t binding = -1;
        std::int32_t arrayReg = 0;
        std::int32_t indexReg = 0;
    };

    const KernelBinding& _binding;
    KernelProgram& _p;
    const Unit& _unit;

    std::vector<KernelInstruction> _prologue;
    std::vector<KernelInstruction> _code;
    std::map<std::uint32_t, std::int32_t> _fconst;
    std::map<std::int32_t, std::int32_t> _iconst;
    std::map<std::string, std::int32_t> _sconst;
    std::array<std::vector<std::int32_t>, 4> _free;
    std::vector<std::vector<KernelOperand>> _temps;
    std::vector<std::size_t> _tempStarts;
    std::vector<Scope> _scopes;
    std::size_t _scopeBase = 0;

    std::int32_t _mask = KernelProgram::kEntryMaskRegister;
    bool _masked = false;
    // Masks that later statements of enclosing blocks run under; retiring lanes clears them here.
    std::vector<std::int32_t> _maskChain;
    std::vector<LoopContext> _loops;
    std::vector<FunctionContext> _functions;
    std::vector<const Function*> _inlineStack;
    std::set<const Function*> _called;
    std::map<std::string, std::vector<const Function*>> _userFunctions;
    std::set<std::string> _writtenBindings;

    // ---- registers and emission

    std::int32_t& counter(KernelBank bank)
    {
        switch (bank) {
        case KernelBank::eFloat:
            return _p.floatRegisters;
        case KernelBank::eInt:
            return _p.intRegisters;
        case KernelBank::eString:
            return _p.stringRegisters;
        case KernelBank::eArray:
            break;
        }
        return _p.arrayRegisters;
    }

    std::int32_t newRegister(KernelBank bank) { return counter(bank)++; }

    std::int32_t reuseOrNew(KernelBank bank)
    {
        std::vector<std::int32_t>& free = _free[static_cast<std::size_t>(bank)];
        if (free.empty()) {
            return newRegister(bank);
        }
        const std::int32_t r = free.back();
        free.pop_back();
        return r;
    }

    std::int32_t temp(KernelBank bank)
    {
        const std::int32_t r = reuseOrNew(bank);
        _temps.back().push_back(KernelOperand { bank, r });
        return r;
    }

    std::int32_t local(KernelBank bank)
    {
        const std::int32_t r = reuseOrNew(bank);
        _scopes.back().owned.push_back(KernelOperand { bank, r });
        return r;
    }

    Value tempValue(KernelType t)
    {
        Value v;
        v.type = t;
        v.temporary = true;
        for (int k = 0; k < v.count(); ++k) {
            v.r[k] = temp(v.bank());
        }
        return v;
    }

    void beginStatement()
    {
        _temps.emplace_back();
        _tempStarts.push_back(_code.size());
    }

    void endStatement()
    {
        for (const KernelOperand& o : _temps.back()) {
            _free[static_cast<std::size_t>(o.bank)].push_back(o.reg);
        }
        _temps.pop_back();
        _tempStarts.pop_back();
    }

    void pushScope() { _scopes.emplace_back(); }

    void popScope()
    {
        for (const KernelOperand& o : _scopes.back().owned) {
            _free[static_cast<std::size_t>(o.bank)].push_back(o.reg);
        }
        _scopes.pop_back();
    }

    bool isStatementTemp(KernelBank bank, std::int32_t r) const
    {
        for (const KernelOperand& o : _temps.back()) {
            if (o.bank == bank && o.reg == r) {
                return true;
            }
        }
        return false;
    }

    std::size_t emit(KernelOp op, std::int32_t d, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0)
    {
        KernelInstruction in;
        in.op = op;
        in.d = d;
        in.a = a;
        in.b = b;
        in.c = c;
        _code.push_back(in);
        return _code.size() - 1;
    }

    void attachOperands(std::size_t at, const std::vector<KernelOperand>& operands)
    {
        _code[at].operandBegin = static_cast<std::uint32_t>(_p.operands.size());
        _code[at].operandCount = static_cast<std::uint32_t>(operands.size());
        _p.operands.insert(_p.operands.end(), operands.begin(), operands.end());
    }

    std::vector<KernelOperand> operandsOf(const Value& v) const
    {
        std::vector<KernelOperand> out;
        for (int k = 0; k < v.count(); ++k) {
            out.push_back(KernelOperand { v.bank(), v.r[k] });
        }
        return out;
    }

    std::int32_t maskRegister() const { return _masked ? _mask : KernelProgram::kEntryMaskRegister; }

    // ---- constants

    std::int32_t constF(float v)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        auto it = _fconst.find(bits);
        if (it != _fconst.end()) {
            return it->second;
        }
        KernelInstruction in;
        in.op = KernelOp::eFConst;
        in.d = newRegister(KernelBank::eFloat);
        in.fimm = v;
        _prologue.push_back(in);
        return _fconst[bits] = in.d;
    }

    std::int32_t constI(std::int32_t v)
    {
        auto it = _iconst.find(v);
        if (it != _iconst.end()) {
            return it->second;
        }
        KernelInstruction in;
        in.op = KernelOp::eIConst;
        in.d = newRegister(KernelBank::eInt);
        in.imm = v;
        _prologue.push_back(in);
        return _iconst[v] = in.d;
    }

    std::int32_t constS(const std::string& v)
    {
        auto it = _sconst.find(v);
        if (it != _sconst.end()) {
            return it->second;
        }
        KernelInstruction in;
        in.op = KernelOp::eSConst;
        in.d = newRegister(KernelBank::eString);
        in.imm = static_cast<std::int32_t>(_p.strings.size());
        _p.strings.push_back(v);
        _prologue.push_back(in);
        return _sconst[v] = in.d;
    }

    Value floatLiteral(double v)
    {
        Value x;
        x.type = KernelType(B::eFloat);
        x.r[0] = constF(static_cast<float>(v));
        x.literal = true;
        x.number = v;
        return x;
    }

    Value intLiteral(std::int64_t v)
    {
        Value x;
        x.type = KernelType(B::eInt);
        x.r[0] = constI(static_cast<std::int32_t>(v));
        x.literal = true;
        x.boolean = v == 0 || v == 1;
        x.number = static_cast<double>(v);
        return x;
    }

    Value stringLiteral(const std::string& s)
    {
        Value x;
        x.type = KernelType(B::eString);
        x.r[0] = constS(s);
        x.literal = true;
        return x;
    }

    Value floatValue(std::int32_t r)
    {
        Value v;
        v.type = KernelType(B::eFloat);
        v.r[0] = r;
        return v;
    }

    Value intTemp(KernelOp op, std::int32_t a, std::int32_t b = 0)
    {
        Value v = tempValue(KernelType(B::eInt));
        emit(op, v.r[0], a, b);
        return v;
    }

    // ---- conversions

    [[noreturn]] void cannotConvert(const Value& v, KernelType to, Loc at)
    {
        fail(at, "cannot convert " + article(v.type) + " to " + article(to));
    }

    Value convert(const Value& v, KernelType to, Loc at)
    {
        if (v.type == to) {
            return v;
        }
        if (v.type.array || to.array || v.type.isVoid() || to.isVoid()) {
            cannotConvert(v, to, at);
        }
        const B from = v.type.base;
        const B t = to.base;
        if (from == B::eInt && t == B::eFloat) {
            if (v.literal) {
                return floatLiteral(v.number);
            }
            Value out = tempValue(to);
            emit(KernelOp::eI2F, out.r[0], v.r[0]);
            return out;
        }
        if (from == B::eFloat && t == B::eInt) {
            if (v.literal) {
                return intLiteral(static_cast<std::int64_t>(std::trunc(v.number)));
            }
            Value out = tempValue(to);
            emit(KernelOp::eF2I, out.r[0], v.r[0]);
            return out;
        }
        if ((from == B::eInt || from == B::eFloat) && kernelIsVector(t)) {
            return broadcast(v, t, at);
        }
        if ((from == B::eInt || from == B::eFloat) && kernelIsMatrix(t)) {
            const Value f = convert(v, KernelType(B::eFloat), at);
            const std::int32_t zero = constF(0.f);
            const int n = matrixDim(t);
            Value out;
            out.type = to;
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) {
                    out.r[i * n + j] = i == j ? f.r[0] : zero;
                }
            }
            return out;
        }
        cannotConvert(v, to, at);
    }

    Value cast(const Value& v, KernelType to, Loc at)
    {
        if (conversionCost(v.type, to) >= 0) {
            return convert(v, to, at);
        }
        if (to.array || v.type.array) {
            cannotConvert(v, to, at);
        }
        const B from = v.type.base;
        const B t = to.base;
        if (kernelIsVector(from) && kernelIsVector(t)) {
            const std::int32_t zero = constF(0.f);
            Value out;
            out.type = to;
            for (int k = 0; k < kernelComponentCount(t); ++k) {
                out.r[k] = k < v.count() ? v.r[k] : zero;
            }
            return out;
        }
        if (kernelIsMatrix(from) && kernelIsMatrix(t)) {
            const int n = matrixDim(from);
            const int m = matrixDim(t);
            const std::int32_t zero = constF(0.f);
            const std::int32_t one = constF(1.f);
            Value out;
            out.type = to;
            for (int i = 0; i < m; ++i) {
                for (int j = 0; j < m; ++j) {
                    out.r[i * m + j] = (i < n && j < n) ? v.r[i * n + j] : (i == j ? one : zero);
                }
            }
            return out;
        }
        if (from == B::eString && t == B::eInt) {
            return coreCall(KernelCoreFunction::eAtoi, "atoi", to, { v.type }, { v });
        }
        if (from == B::eString && t == B::eFloat) {
            return coreCall(KernelCoreFunction::eAtof, "atof", to, { v.type }, { v });
        }
        if (from == B::eInt && t == B::eString) {
            return coreCall(KernelCoreFunction::eItoa, "itoa", to, { v.type }, { v });
        }
        cannotConvert(v, to, at);
    }

    // Repeats a scalar across every component of a vector or matrix (not a scaled identity).
    Value broadcast(const Value& v, B to, Loc at)
    {
        if (v.type.is(to)) {
            return v;
        }
        if (!isScalar(v.type)) {
            cannotConvert(v, KernelType(to), at);
        }
        const Value f = convert(v, KernelType(B::eFloat), at);
        Value out;
        out.type = KernelType(to);
        for (int k = 0; k < kernelComponentCount(to); ++k) {
            out.r[k] = f.r[0];
        }
        return out;
    }

    Value toInt(const Value& v, Loc at)
    {
        if (!isScalar(v.type)) {
            fail(at, "expected an int, not " + article(v.type));
        }
        return convert(v, KernelType(B::eInt), at);
    }

    Value floatTuple(const Value& v, Loc at, const std::string& what)
    {
        if (v.type.is(B::eInt)) {
            return convert(v, KernelType(B::eFloat), at);
        }
        if (!isFloatTuple(v.type)) {
            fail(at, what + " takes a float or vector, not " + article(v.type));
        }
        return v;
    }

    Value truthy(const Value& v, Loc at)
    {
        if (v.type.is(B::eInt)) {
            if (v.boolean) {
                return v;
            }
            Value out = intTemp(KernelOp::eIBool, v.r[0]);
            out.boolean = true;
            return out;
        }
        if (v.type.is(B::eFloat)) {
            Value out = intTemp(KernelOp::eFNe, v.r[0], constF(0.f));
            out.boolean = true;
            return out;
        }
        if (v.type.is(B::eString)) {
            Value out = intTemp(KernelOp::eSNe, v.r[0], constS(std::string()));
            out.boolean = true;
            return out;
        }
        fail(at, article(v.type) + " cannot be used as a condition");
    }

    // ---- componentwise helpers

    Value unaryF(KernelOp op, const Value& a)
    {
        Value out = tempValue(a.type);
        for (int k = 0; k < a.count(); ++k) {
            emit(op, out.r[k], a.r[k]);
        }
        return out;
    }

    Value binaryF(KernelOp op, const Value& a, const Value& b)
    {
        Value out = tempValue(a.type);
        for (int k = 0; k < a.count(); ++k) {
            emit(op, out.r[k], a.r[k], b.r[k]);
        }
        return out;
    }

    // The float tuple type every value can be brought to, scalars broadcasting.
    B commonTuple(const std::vector<Value>& values, const std::vector<Loc>& locs, const std::string& what)
    {
        B t = B::eFloat;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const KernelType vt = values[i].type;
            if (isScalar(vt)) {
                continue;
            }
            if (!isFloatTuple(vt)) {
                fail(locs[i], what + " takes floats, vectors or matrices, not " + article(vt));
            }
            if (t == B::eFloat) {
                t = vt.base;
            } else if (t != vt.base) {
                fail(locs[i], what + " cannot mix " + article(KernelType(t)) + " with " + article(vt));
            }
        }
        return t;
    }

    std::int32_t dotRegister(const Value& a, const Value& b)
    {
        const std::int32_t d = temp(KernelBank::eFloat);
        emit(KernelOp::eFMul, d, a.r[0], b.r[0]);
        for (int k = 1; k < a.count(); ++k) {
            emit(KernelOp::eFMulAdd, d, a.r[k], b.r[k], d);
        }
        return d;
    }

    Value cross(const Value& a, const Value& b)
    {
        Value out = tempValue(KernelType(B::eVector));
        for (int k = 0; k < 3; ++k) {
            const int i = (k + 1) % 3;
            const int j = (k + 2) % 3;
            const std::int32_t t = temp(KernelBank::eFloat);
            emit(KernelOp::eFMul, t, a.r[i], b.r[j]);
            const std::int32_t u = temp(KernelBank::eFloat);
            emit(KernelOp::eFMul, u, a.r[j], b.r[i]);
            emit(KernelOp::eFSub, out.r[k], t, u);
        }
        return out;
    }

    Value matrixProduct(const Value& a, const Value& b)
    {
        const int n = matrixDim(a.type.base);
        Value out = tempValue(a.type);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const std::int32_t d = out.r[i * n + j];
                emit(KernelOp::eFMul, d, a.r[i * n], b.r[j]);
                for (int k = 1; k < n; ++k) {
                    emit(KernelOp::eFMulAdd, d, a.r[i * n + k], b.r[k * n + j], d);
                }
            }
        }
        return out;
    }

    Value vectorTimesMatrix(const Value& v, const Value& m)
    {
        const int n = matrixDim(m.type.base);
        const int vn = v.count();
        Value out = tempValue(v.type);
        for (int j = 0; j < vn; ++j) {
            const std::int32_t d = out.r[j];
            emit(KernelOp::eFMul, d, v.r[0], m.r[j]);
            for (int i = 1; i < vn; ++i) {
                emit(KernelOp::eFMulAdd, d, v.r[i], m.r[i * n + j], d);
            }
            if (vn == 3 && n == 4) {
                emit(KernelOp::eFAdd, d, d, m.r[3 * n + j]);
            }
        }
        return out;
    }

    Value transpose(const Value& m)
    {
        const int n = matrixDim(m.type.base);
        Value out;
        out.type = m.type;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                out.r[i * n + j] = m.r[j * n + i];
            }
        }
        return out;
    }

    Value selectComponent(const Value& whole, std::int32_t index)
    {
        Value out = tempValue(KernelType(B::eFloat));
        emit(KernelOp::eFMov, out.r[0], whole.r[0]);
        for (int k = 1; k < whole.count(); ++k) {
            const std::int32_t m = temp(KernelBank::eInt);
            emit(KernelOp::eIEq, m, index, constI(k));
            emit(KernelOp::eFSel, out.r[0], m, whole.r[k], out.r[0]);
        }
        return out;
    }

    // ---- operators

    std::optional<double> fold(const std::string& op, double a, double b, bool integer)
    {
        if (integer) {
            const std::int64_t x = static_cast<std::int64_t>(a);
            const std::int64_t y = static_cast<std::int64_t>(b);
            if (op == "+") {
                return static_cast<double>(static_cast<std::int32_t>(x + y));
            }
            if (op == "-") {
                return static_cast<double>(static_cast<std::int32_t>(x - y));
            }
            if (op == "*") {
                return static_cast<double>(static_cast<std::int32_t>(x * y));
            }
            if (op == "/") {
                return y == 0 ? 0. : static_cast<double>(x / y);
            }
            if (op == "%") {
                return y == 0 ? 0. : static_cast<double>(x % y);
            }
            return std::nullopt;
        }
        const float x = static_cast<float>(a);
        const float y = static_cast<float>(b);
        if (op == "+") {
            return static_cast<double>(x + y);
        }
        if (op == "-") {
            return static_cast<double>(x - y);
        }
        if (op == "*") {
            return static_cast<double>(x * y);
        }
        if (op == "/") {
            return static_cast<double>(x / y);
        }
        return std::nullopt;
    }

    Value arithmetic(const std::string& op, const Value& a, const Value& b, Loc at)
    {
        if (a.type.array || b.type.array) {
            fail(at, "'" + op + "' cannot be applied to arrays");
        }
        if (a.type.is(B::eString) || b.type.is(B::eString)) {
            if (op == "+" && a.type.is(B::eString) && b.type.is(B::eString)) {
                Value out = tempValue(a.type);
                emit(KernelOp::eSConcat, out.r[0], a.r[0], b.r[0]);
                return out;
            }
            fail(at, "'" + op + "' cannot be applied to " + article(a.type) + " and " + article(b.type));
        }
        if (a.type.isVoid() || b.type.isVoid()) {
            fail(at, "'" + op + "' needs values, and a void function returns none");
        }
        const B ta = a.type.base;
        const B tb = b.type.base;
        if (ta == B::eInt && tb == B::eInt) {
            if (a.literal && b.literal) {
                if (std::optional<double> r = fold(op, a.number, b.number, true)) {
                    return intLiteral(static_cast<std::int64_t>(*r));
                }
            }
            static const std::map<std::string, KernelOp> ops = {
                { "+", KernelOp::eIAdd },
                { "-", KernelOp::eISub },
                { "*", KernelOp::eIMul },
                { "/", KernelOp::eIDiv },
                { "%", KernelOp::eIMod },
            };
            return intTemp(ops.at(op), a.r[0], b.r[0]);
        }
        if (op == "*" && (kernelIsMatrix(ta) || kernelIsMatrix(tb)) && !isScalar(a.type) && !isScalar(b.type)) {
            if (kernelIsMatrix(ta) && ta == tb) {
                return matrixProduct(a, b);
            }
            if ((ta == B::eVector && kernelIsMatrix(tb)) || (ta == B::eVector4 && tb == B::eMatrix)) {
                return vectorTimesMatrix(a, b);
            }
            if (kernelIsMatrix(ta) && kernelIsVector(tb)) {
                fail(at, "a vector multiplies a matrix from the left: write vector * matrix");
            }
            fail(at, "cannot multiply " + article(a.type) + " by " + article(b.type));
        }
        B t;
        if (isScalar(a.type) && isScalar(b.type)) {
            t = B::eFloat;
        } else if (isScalar(a.type)) {
            t = tb;
        } else if (isScalar(b.type) || ta == tb) {
            t = ta;
        } else {
            fail(at, "'" + op + "' cannot combine " + article(a.type) + " and " + article(b.type));
        }
        if (t == B::eFloat && a.literal && b.literal) {
            if (std::optional<double> r = fold(op, a.number, b.number, false)) {
                return floatLiteral(*r);
            }
        }
        static const std::map<std::string, KernelOp> ops = {
            { "+", KernelOp::eFAdd },
            { "-", KernelOp::eFSub },
            { "*", KernelOp::eFMul },
            { "/", KernelOp::eFDiv },
            { "%", KernelOp::eFMod },
        };
        return binaryF(ops.at(op), broadcast(a, t, at), broadcast(b, t, at));
    }

    Value comparison(const std::string& op, const Value& a, const Value& b, Loc at)
    {
        const bool equality = op == "==" || op == "!=";
        if (a.type.array || b.type.array || a.type.isVoid() || b.type.isVoid()) {
            fail(at, "'" + op + "' cannot compare " + article(a.type) + " and " + article(b.type));
        }
        Value out;
        if (a.type.is(B::eString) || b.type.is(B::eString)) {
            if (!equality || !(a.type.is(B::eString) && b.type.is(B::eString))) {
                fail(at, "'" + op + "' cannot compare " + article(a.type) + " and " + article(b.type));
            }
            out = intTemp(op == "==" ? KernelOp::eSEq : KernelOp::eSNe, a.r[0], b.r[0]);
        } else if (isScalar(a.type) && isScalar(b.type)) {
            static const std::map<std::string, std::pair<KernelOp, KernelOp>> ops = {
                { "<", { KernelOp::eILt, KernelOp::eFLt } },
                { "<=", { KernelOp::eILe, KernelOp::eFLe } },
                { ">", { KernelOp::eIGt, KernelOp::eFGt } },
                { ">=", { KernelOp::eIGe, KernelOp::eFGe } },
                { "==", { KernelOp::eIEq, KernelOp::eFEq } },
                { "!=", { KernelOp::eINe, KernelOp::eFNe } },
            };
            const auto& pair = ops.at(op);
            if (a.type.is(B::eInt) && b.type.is(B::eInt)) {
                out = intTemp(pair.first, a.r[0], b.r[0]);
            } else {
                const KernelType f(B::eFloat);
                out = intTemp(pair.second, convert(a, f, at).r[0], convert(b, f, at).r[0]);
            }
        } else {
            if (!equality) {
                fail(at, "'" + op + "' cannot order " + article(a.type) + " and " + article(b.type));
            }
            if (a.type != b.type) {
                fail(at, "'" + op + "' cannot compare " + article(a.type) + " and " + article(b.type));
            }
            out = intTemp(KernelOp::eFEq, a.r[0], b.r[0]);
            for (int k = 1; k < a.count(); ++k) {
                const std::int32_t t = temp(KernelBank::eInt);
                emit(KernelOp::eFEq, t, a.r[k], b.r[k]);
                emit(KernelOp::eIAnd, out.r[0], out.r[0], t);
            }
            if (op == "!=") {
                emit(KernelOp::eINot, out.r[0], out.r[0]);
            }
        }
        out.boolean = true;
        return out;
    }

    Value bitwise(const std::string& op, const Value& a, const Value& b, Loc at)
    {
        if (op == "^" && a.type.is(B::eVector) && b.type.is(B::eVector)) {
            return cross(a, b);
        }
        if (!a.type.is(B::eInt) || !b.type.is(B::eInt)) {
            fail(at, "'" + op + "' takes ints, not " + article(a.type.is(B::eInt) ? b.type : a.type));
        }
        static const std::map<std::string, KernelOp> ops = {
            { "&", KernelOp::eIBitAnd },
            { "|", KernelOp::eIBitOr },
            { "^", KernelOp::eIBitXor },
            { "<<", KernelOp::eIShl },
            { ">>", KernelOp::eIShr },
        };
        return intTemp(ops.at(op), a.r[0], b.r[0]);
    }

    Value binaryOp(const std::string& op, const Value& a, const Value& b, Loc at)
    {
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            return arithmetic(op, a, b, at);
        }
        if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
            return bitwise(op, a, b, at);
        }
        return comparison(op, a, b, at);
    }

    // ---- bindings

    KernelType prefixType(const std::string& prefix, Loc at)
    {
        KernelType t;
        switch (prefix[0]) {
        case 'f':
            t = KernelType(B::eFloat);
            break;
        case 'i':
            t = KernelType(B::eInt);
            break;
        case 'v':
            t = KernelType(B::eVector);
            break;
        case 'p':
            t = KernelType(B::eVector4);
            break;
        case 's':
            t = KernelType(B::eString);
            break;
        case 'u':
            t = KernelType(B::eVector2);
            break;
        case '3':
            t = KernelType(B::eMatrix3);
            break;
        case '4':
            t = KernelType(B::eMatrix);
            break;
        case '2':
            fail(at, "matrix2 bindings are not supported");
        default:
            fail(at, "dictionary bindings are not supported");
        }
        t.array = prefix.size() > 1;
        return t;
    }

    std::int32_t bindingIndex(const Expr& e)
    {
        std::optional<KernelType> prefixed;
        if (!e.prefix.empty()) {
            prefixed = prefixType(e.prefix, e.loc);
        }
        const std::string shown = bindingDisplayName(e.text, e.input);
        for (std::size_t i = 0; i < _p.bindings.size(); ++i) {
            const KernelProgramBinding& b = _p.bindings[i];
            if (b.name == e.text && b.input == e.input) {
                if (prefixed && *prefixed != b.type) {
                    fail(e.loc, "'" + shown + "' is already bound as " + article(b.type));
                }
                return static_cast<std::int32_t>(i);
            }
        }
        KernelProgramBinding b;
        b.name = e.text;
        b.input = e.input;
        std::optional<KernelVariable> builtin;
        if (e.input == 0) {
            builtin = _binding.builtinVariable(e.text);
        }
        if (builtin) {
            if (prefixed && *prefixed != builtin->type) {
                fail(e.loc, "'" + shown + "' is " + article(builtin->type));
            }
            builtin->kind = KernelVariableKind::eBuiltin;
            builtin->readOnly = true;
            b.variable = *builtin;
            b.type = builtin->type;
        } else {
            KernelAttributeRequest request;
            request.name = e.text;
            request.input = e.input;
            request.typeExplicit = prefixed.has_value();
            request.type = prefixed ? *prefixed : knownAttributeType(e.text);
            request.write = _writtenBindings.count(bindingKey(e.text, e.input)) != 0;
            KernelBindResult result = _binding.bindAttribute(request);
            if (!result.variable) {
                fail(e.loc, result.error.empty() ? "unknown attribute '" + shown + "'" : result.error);
            }
            b.variable = *result.variable;
            b.type = request.type;
        }
        const KernelBank bank = kernelBankOf(b.type);
        const int n = b.type.array ? 1 : kernelComponentCount(b.type.base);
        for (int k = 0; k < n; ++k) {
            b.registers.push_back(newRegister(bank));
        }
        _p.bindings.push_back(std::move(b));
        return static_cast<std::int32_t>(_p.bindings.size() - 1);
    }

    Value bindingValue(std::int32_t index) const
    {
        const KernelProgramBinding& b = _p.bindings[index];
        Value v;
        v.type = b.type;
        for (std::size_t k = 0; k < b.registers.size(); ++k) {
            v.r[k] = b.registers[k];
        }
        return v;
    }

    void markRead(std::int32_t index)
    {
        if (index >= 0) {
            _p.bindings[index].read = true;
            _p.bindings[index].needsGather = true;
        }
    }

    void markWrite(std::int32_t index, bool partial, Loc at)
    {
        if (index < 0) {
            return;
        }
        KernelProgramBinding& b = _p.bindings[index];
        if (b.variable.readOnly) {
            fail(at, "'" + bindingDisplayName(b.name, b.input) + "' is read-only");
        }
        b.written = true;
        if (_masked || partial) {
            b.needsGather = true;
        }
    }

    void collectWrittenBindings()
    {
        auto note = [&](const Expr& root) {
            if (root.kind == ExprKind::eBinding) {
                _writtenBindings.insert(bindingKey(root.text, root.input));
            }
        };
        auto f = [&](const Expr& e) {
            if ((e.kind == ExprKind::eAssign || e.kind == ExprKind::eIncDec) && !e.args.empty()) {
                note(lvalueRoot(*e.args[0]));
            }
            if (e.kind == ExprKind::eCall && !e.args.empty()) {
                if (e.text == "append" || e.text == "push" || e.text == "resize") {
                    note(lvalueRoot(*e.args[0]));
                }
                if (_userFunctions.count(e.text)) {
                    for (const ExprPtr& a : e.args) {
                        note(*a);
                    }
                }
            }
        };
        for (const StmtPtr& s : _unit.main) {
            visitStmt(*s, f);
        }
        for (const auto& fn : _unit.functions) {
            for (const StmtPtr& s : fn->body) {
                visitStmt(*s, f);
            }
        }
    }

    // ---- names

    Symbol* lookup(const std::string& name)
    {
        for (std::size_t i = _scopes.size(); i-- > _scopeBase;) {
            auto it = _scopes[i].symbols.find(name);
            if (it != _scopes[i].symbols.end()) {
                return &it->second;
            }
        }
        return nullptr;
    }

    std::vector<int> swizzle(const Expr& e, KernelType base)
    {
        if (base.array || !kernelIsVector(base.base)) {
            if (isFloatTuple(base) && kernelIsMatrix(base.base)) {
                fail(e.loc, "matrix components are read with getcomp(), not '." + e.text + "'");
            }
            fail(e.loc, article(base) + " has no component '" + e.text + "'");
        }
        if (e.text.size() > 4) {
            fail(e.loc, "a swizzle has at most 4 components");
        }
        std::vector<int> comps;
        for (char c : e.text) {
            int k;
            switch (c) {
            case 'x':
            case 'r':
            case 'u':
                k = 0;
                break;
            case 'y':
            case 'g':
            case 'v':
                k = 1;
                break;
            case 'z':
            case 'b':
                k = 2;
                break;
            case 'w':
            case 'a':
                k = 3;
                break;
            default:
                fail(e.loc, article(base) + " has no component '" + e.text + "'");
            }
            if (k >= kernelComponentCount(base.base)) {
                fail(e.loc, article(base) + " has no component '" + std::string(1, c) + "'");
            }
            comps.push_back(k);
        }
        return comps;
    }

    // ---- assignment

    LValue lvalue(const Expr& e)
    {
        switch (e.kind) {
        case ExprKind::eVariable: {
            Symbol* s = lookup(e.text);
            if (!s) {
                fail(e.loc, "unknown variable '" + e.text + "'");
            }
            if (s->readOnly) {
                fail(e.loc, "cannot assign to const parameter '" + e.text + "'");
            }
            LValue lv;
            lv.type = s->value.type;
            lv.whole = s->value;
            lv.whole.temporary = false;
            lv.binding = s->binding;
            return lv;
        }
        case ExprKind::eBinding: {
            const std::int32_t index = bindingIndex(e);
            const KernelProgramBinding& b = _p.bindings[index];
            if (b.variable.readOnly) {
                fail(e.loc, "'" + bindingDisplayName(b.name, b.input) + "' is read-only");
            }
            LValue lv;
            lv.type = b.type;
            lv.whole = bindingValue(index);
            lv.binding = index;
            return lv;
        }
        case ExprKind::eMember: {
            LValue base = lvalue(*e.args[0]);
            if (base.kind != LValue::Kind::eRegisters) {
                fail(e.loc, "cannot assign to a component of this expression");
            }
            const std::vector<int> s = swizzle(e, base.type);
            std::vector<int> comps;
            for (int k : s) {
                const int c = base.comps.empty() ? k : base.comps[k];
                if (std::find(comps.begin(), comps.end(), c) != comps.end()) {
                    fail(e.loc, "an assigned swizzle cannot repeat a component");
                }
                comps.push_back(c);
            }
            base.comps = comps;
            base.type = KernelType(tupleType(static_cast<int>(comps.size())));
            return base;
        }
        case ExprKind::eIndex: {
            LValue base = lvalue(*e.args[0]);
            if (base.kind != LValue::Kind::eRegisters) {
                fail(e.loc, "cannot assign to an element of this expression");
            }
            const Expr& ie = *e.args[1];
            if (base.type.array) {
                base.kind = LValue::Kind::eArrayElement;
                base.arrayReg = base.whole.r[0];
                base.indexReg = toInt(expr(ie), ie.loc).r[0];
                base.type = base.type.element();
                return base;
            }
            if (kernelIsVector(base.type.base)) {
                if (ie.kind == ExprKind::eInt) {
                    if (ie.intValue < 0 || ie.intValue >= kernelComponentCount(base.type.base)) {
                        fail(ie.loc, "index " + std::to_string(ie.intValue) + " is out of range for " + article(base.type));
                    }
                    const int k = static_cast<int>(ie.intValue);
                    base.comps = { base.comps.empty() ? k : base.comps[k] };
                    base.type = KernelType(B::eFloat);
                    return base;
                }
                if (!base.comps.empty()) {
                    fail(e.loc, "a computed index needs a whole vector");
                }
                base.kind = LValue::Kind::eDynamicComponent;
                base.indexReg = toInt(expr(ie), ie.loc).r[0];
                base.type = KernelType(B::eFloat);
                return base;
            }
            fail(e.loc, article(base.type) + " cannot be indexed");
        }
        default:
            fail(e.loc, "this expression cannot be assigned to");
        }
    }

    Value readLValue(const LValue& lv)
    {
        markRead(lv.binding);
        switch (lv.kind) {
        case LValue::Kind::eRegisters: {
            if (lv.comps.empty()) {
                return lv.whole;
            }
            Value v;
            v.type = lv.type;
            for (std::size_t k = 0; k < lv.comps.size(); ++k) {
                v.r[k] = lv.whole.r[lv.comps[k]];
            }
            return v;
        }
        case LValue::Kind::eArrayElement: {
            Value out = tempValue(lv.type);
            const std::size_t at = emit(KernelOp::eAGet, 0, lv.arrayReg, lv.indexReg);
            _code[at].imm = static_cast<std::int32_t>(lv.type.base);
            attachOperands(at, operandsOf(out));
            return out;
        }
        case LValue::Kind::eDynamicComponent:
            return selectComponent(lv.whole, lv.indexReg);
        }
        return Value {};
    }

    // Moves the newest write of temporary src into dst, so `x = a + b` costs one instruction.
    bool retarget(KernelBank bank, std::int32_t src, std::int32_t dst)
    {
        if (!isStatementTemp(bank, src)) {
            return false;
        }
        const char letter = bankLetter(bank);
        auto touches = [&](const KernelInstruction& in, const OpInfo& info, std::int32_t r) {
            return (info.d == letter && in.d == r) || (info.a == letter && in.a == r) || (info.b == letter && in.b == r)
                || (info.c == letter && in.c == r);
        };
        for (std::size_t w = _code.size(); w-- > _tempStarts.back();) {
            KernelInstruction& in = _code[w];
            const OpInfo info = opInfo(in.op);
            if (info.operands || in.op == KernelOp::eJump || in.op == KernelOp::eJumpIfNone) {
                return false;
            }
            const bool isSelect = in.op == KernelOp::eFSel || in.op == KernelOp::eISel || in.op == KernelOp::eSSel
                || in.op == KernelOp::eASel;
            if (info.d == letter && in.d == src && !isSelect && in.op != KernelOp::eACopy) {
                in.d = dst;
                return true;
            }
            if (touches(in, info, src) || touches(in, info, dst)) {
                return false;
            }
        }
        return false;
    }

    // Writes src into the registers dst, under the current mask.
    void assignRegisters(const std::vector<std::int32_t>& dst, Value src)
    {
        const KernelBank bank = src.bank();
        const std::size_t n = dst.size();
        bool hazard = false;
        for (std::size_t k = 0; k < n; ++k) {
            for (std::size_t j = 0; j < n; ++j) {
                hazard |= j != k && src.r[k] == dst[j];
            }
        }
        if (hazard) {
            Value copy = tempValue(src.type);
            for (std::size_t k = 0; k < n; ++k) {
                emit(movOp(bank), copy.r[k], src.r[k]);
            }
            src = copy;
            src.temporary = false;
        }
        for (std::size_t k = 0; k < n; ++k) {
            if (dst[k] == src.r[k]) {
                continue;
            }
            if (_masked) {
                emit(selOp(bank), dst[k], _mask, src.r[k], dst[k]);
                continue;
            }
            bool shared = false;
            for (std::size_t j = 0; j < n; ++j) {
                shared |= j != k && src.r[j] == src.r[k];
            }
            if (src.temporary && !shared && retarget(bank, src.r[k], dst[k])) {
                continue;
            }
            emit(movOp(bank), dst[k], src.r[k]);
        }
    }

    Value writeLValue(const LValue& lv, const Value& value, Loc at)
    {
        const Value v = convert(value, lv.type, at);
        switch (lv.kind) {
        case LValue::Kind::eRegisters: {
            const bool partial = !lv.comps.empty() && static_cast<int>(lv.comps.size()) < lv.whole.count();
            markWrite(lv.binding, partial, at);
            std::vector<std::int32_t> dst;
            Value written;
            written.type = lv.type;
            for (int k = 0; k < v.count(); ++k) {
                dst.push_back(lv.comps.empty() ? lv.whole.r[k] : lv.whole.r[lv.comps[k]]);
                written.r[k] = dst.back();
            }
            assignRegisters(dst, v);
            return written;
        }
        case LValue::Kind::eArrayElement: {
            markWrite(lv.binding, true, at);
            const std::size_t i = emit(KernelOp::eASet, 0, maskRegister(), lv.arrayReg, lv.indexReg);
            _code[i].imm = static_cast<std::int32_t>(lv.type.base);
            attachOperands(i, operandsOf(v));
            return v;
        }
        case LValue::Kind::eDynamicComponent: {
            markWrite(lv.binding, true, at);
            for (int k = 0; k < lv.whole.count(); ++k) {
                const std::int32_t m = temp(KernelBank::eInt);
                emit(KernelOp::eIEq, m, lv.indexReg, constI(k));
                if (_masked) {
                    emit(KernelOp::eIAnd, m, m, _mask);
                }
                emit(KernelOp::eFSel, lv.whole.r[k], m, v.r[0], lv.whole.r[k]);
            }
            return v;
        }
        }
        return v;
    }

    Value assignment(const Expr& e)
    {
        const LValue lv = lvalue(*e.args[0]);
        const Expr& rhs = *e.args[1];
        if (e.text == "=") {
            return writeLValue(lv, expr(rhs, lv.type), e.loc);
        }
        const Value current = readLValue(lv);
        const Value r = expr(rhs, lv.type);
        return writeLValue(lv, binaryOp(e.text.substr(0, e.text.size() - 1), current, r, e.loc), e.loc);
    }

    Value incDec(const Expr& e, bool discard)
    {
        const LValue lv = lvalue(*e.args[0]);
        if (!isScalar(lv.type)) {
            fail(e.loc, "'" + e.text + "' needs an int or float, not " + article(lv.type));
        }
        const Value current = readLValue(lv);
        Value old;
        if (!e.prefixForm && !discard) {
            old = tempValue(current.type);
            emit(movOp(current.bank()), old.r[0], current.r[0]);
        }
        const Value one = lv.type.is(B::eInt) ? intLiteral(1) : floatLiteral(1.);
        const Value next = arithmetic(e.text == "++" ? "+" : "-", current, one, e.loc);
        const Value written = writeLValue(lv, next, e.loc);
        return (e.prefixForm || discard) ? written : old;
    }

    // ---- expressions

    Value expr(const Expr& e, KernelType expected = {}, bool discard = false)
    {
        switch (e.kind) {
        case ExprKind::eInt:
            return intLiteral(e.intValue);
        case ExprKind::eFloat:
            return floatLiteral(e.floatValue);
        case ExprKind::eString:
            return stringLiteral(e.text);
        case ExprKind::eBraceList:
            return braceList(e, expected);
        case ExprKind::eVariable: {
            Symbol* s = lookup(e.text);
            if (!s) {
                fail(e.loc, "unknown variable '" + e.text + "'");
            }
            markRead(s->binding);
            Value v = s->value;
            v.temporary = false;
            return v;
        }
        case ExprKind::eBinding: {
            const std::int32_t index = bindingIndex(e);
            markRead(index);
            return bindingValue(index);
        }
        case ExprKind::eUnary:
            return unary(e, expected);
        case ExprKind::eBinary:
            return binary(e, expected);
        case ExprKind::eAssign:
            return assignment(e);
        case ExprKind::eIncDec:
            return incDec(e, discard);
        case ExprKind::eTernary:
            return ternary(e, expected);
        case ExprKind::eCall:
            return call(e, expected);
        case ExprKind::eMember: {
            const Value base = expr(*e.args[0]);
            const std::vector<int> comps = swizzle(e, base.type);
            Value v;
            v.type = KernelType(tupleType(static_cast<int>(comps.size())));
            for (std::size_t k = 0; k < comps.size(); ++k) {
                v.r[k] = base.r[comps[k]];
            }
            return v;
        }
        case ExprKind::eIndex: {
            const Value base = expr(*e.args[0]);
            const Expr& ie = *e.args[1];
            if (base.type.array) {
                const Value index = toInt(expr(ie), ie.loc);
                Value out = tempValue(base.type.element());
                const std::size_t at = emit(KernelOp::eAGet, 0, base.r[0], index.r[0]);
                _code[at].imm = static_cast<std::int32_t>(base.type.base);
                attachOperands(at, operandsOf(out));
                return out;
            }
            if (kernelIsVector(base.type.base)) {
                if (ie.kind == ExprKind::eInt) {
                    if (ie.intValue < 0 || ie.intValue >= base.count()) {
                        fail(ie.loc, "index " + std::to_string(ie.intValue) + " is out of range for " + article(base.type));
                    }
                    return floatValue(base.r[ie.intValue]);
                }
                return selectComponent(base, toInt(expr(ie), ie.loc).r[0]);
            }
            fail(e.loc, article(base.type) + " cannot be indexed");
        }
        }
        fail(e.loc, "unsupported expression");
    }

    Value unary(const Expr& e, KernelType expected)
    {
        const Expr& arg = *e.args[0];
        if (e.text == "!") {
            const Value v = truthy(expr(arg), arg.loc);
            Value out = intTemp(KernelOp::eINot, v.r[0]);
            out.boolean = true;
            return out;
        }
        const Value v = expr(arg, e.text == "-" ? expected : KernelType());
        if (e.text == "~") {
            if (!v.type.is(B::eInt)) {
                fail(e.loc, "'~' takes an int, not " + article(v.type));
            }
            return intTemp(KernelOp::eIBitNot, v.r[0]);
        }
        if (v.type.is(B::eInt)) {
            return intTemp(KernelOp::eINeg, v.r[0]);
        }
        if (!isFloatTuple(v.type)) {
            fail(e.loc, "'-' cannot be applied to " + article(v.type));
        }
        return unaryF(KernelOp::eFNeg, v);
    }

    Value withMask(std::int32_t mask, const Expr& e, KernelType expected)
    {
        const std::int32_t savedMask = _mask;
        const bool savedMasked = _masked;
        _mask = mask;
        _masked = true;
        Value v = expr(e, expected);
        _mask = savedMask;
        _masked = savedMasked;
        return v;
    }

    Value logical(const Expr& e)
    {
        const bool isAnd = e.text == "&&";
        const Value a = truthy(expr(*e.args[0]), e.args[0]->loc);
        const Expr& rhs = *e.args[1];
        Value b;
        if (hasSideEffects(rhs)) {
            const std::int32_t narrowed = temp(KernelBank::eInt);
            emit(isAnd ? KernelOp::eIAnd : KernelOp::eIAndNot, narrowed, maskRegister(), a.r[0]);
            b = truthy(withMask(narrowed, rhs, {}), rhs.loc);
        } else {
            b = truthy(expr(rhs), rhs.loc);
        }
        Value out = intTemp(isAnd ? KernelOp::eIAnd : KernelOp::eIOr, a.r[0], b.r[0]);
        out.boolean = true;
        return out;
    }

    Value binary(const Expr& e, KernelType expected)
    {
        if (e.text == "&&" || e.text == "||") {
            return logical(e);
        }
        const bool arith = e.text == "+" || e.text == "-" || e.text == "*" || e.text == "/" || e.text == "%";
        const KernelType hint = arith ? expected : KernelType();
        const Value a = expr(*e.args[0], hint);
        const Value b = expr(*e.args[1], hint);
        return binaryOp(e.text, a, b, e.loc);
    }

    Value ternary(const Expr& e, KernelType expected)
    {
        const Value c = truthy(expr(*e.args[0]), e.args[0]->loc);
        const Expr& x = *e.args[1];
        const Expr& y = *e.args[2];
        Value a;
        Value b;
        if (hasSideEffects(x) || hasSideEffects(y)) {
            const std::int32_t m = temp(KernelBank::eInt);
            emit(KernelOp::eIAnd, m, maskRegister(), c.r[0]);
            a = withMask(m, x, expected);
            const std::int32_t n = temp(KernelBank::eInt);
            emit(KernelOp::eIAndNot, n, maskRegister(), c.r[0]);
            b = withMask(n, y, expected);
        } else {
            a = expr(x, expected);
            b = expr(y, expected);
        }
        KernelType t = a.type;
        if (a.type != b.type) {
            if (isScalar(a.type) && isScalar(b.type)) {
                t = KernelType(B::eFloat);
            } else if (isScalar(a.type) && isFloatTuple(b.type)) {
                t = b.type;
            } else if (isScalar(b.type) && isFloatTuple(a.type)) {
                t = a.type;
            } else {
                fail(e.loc, "the two sides of '?:' differ: " + article(a.type) + " and " + article(b.type));
            }
        }
        if (t.isVoid()) {
            fail(e.loc, "the sides of '?:' have no value");
        }
        const Value ca = convert(a, t, x.loc);
        const Value cb = convert(b, t, y.loc);
        Value out = tempValue(t);
        for (int k = 0; k < out.count(); ++k) {
            emit(selOp(out.bank()), out.r[k], c.r[0], ca.r[k], cb.r[k]);
        }
        return out;
    }

    Value arrayOf(KernelType element, const std::vector<const Expr*>& items, Loc at)
    {
        if (element.array || element.isVoid()) {
            fail(at, "arrays of arrays are not supported");
        }
        Value out = tempValue(KernelType(element.base, true));
        const std::size_t i = emit(KernelOp::eANew, out.r[0]);
        _code[i].imm = static_cast<std::int32_t>(element.base);
        for (const Expr* item : items) {
            const Value v = convert(expr(*item, element), element, item->loc);
            const std::size_t p = emit(KernelOp::eAPush, 0, KernelProgram::kEntryMaskRegister, out.r[0]);
            _code[p].imm = static_cast<std::int32_t>(element.base);
            attachOperands(p, operandsOf(v));
        }
        return out;
    }

    Value braceList(const Expr& e, KernelType expected)
    {
        std::vector<const Expr*> items;
        for (const ExprPtr& a : e.args) {
            items.push_back(a.get());
        }
        if (expected.array) {
            return arrayOf(expected.element(), items, e.loc);
        }
        const int n = static_cast<int>(items.size());
        B t = tupleType(n);
        if (isFloatTuple(expected) && kernelComponentCount(expected.base) == n) {
            t = expected.base;
        }
        if (t == B::eVoid || t == B::eFloat) {
            fail(e.loc, "a {} literal has 2, 3, 4, 9 or 16 components, or makes an array, not " + std::to_string(n) + " components");
        }
        Value out;
        out.type = KernelType(t);
        for (int k = 0; k < n; ++k) {
            const Value v = expr(*items[k]);
            if (!isScalar(v.type)) {
                fail(items[k]->loc, "a {} literal holds numbers, not " + article(v.type));
            }
            out.r[k] = convert(v, KernelType(B::eFloat), items[k]->loc).r[0];
        }
        return out;
    }

    // ---- calls

    std::vector<Value> lowerArgs(const Expr& e)
    {
        std::vector<Value> args;
        for (const ExprPtr& a : e.args) {
            args.push_back(expr(*a));
        }
        return args;
    }

    std::vector<Loc> argLocs(const Expr& e) const
    {
        std::vector<Loc> locs;
        for (const ExprPtr& a : e.args) {
            locs.push_back(a->loc);
        }
        return locs;
    }

    std::int32_t functionIndex(KernelFunctionProvider provider, const KernelFunctionSignature& sig)
    {
        for (std::size_t i = 0; i < _p.functions.size(); ++i) {
            const KernelProgramFunction& f = _p.functions[i];
            if (f.provider == provider && f.signature.id == sig.id && f.signature.name == sig.name
                && f.signature.result == sig.result) {
                return static_cast<std::int32_t>(i);
            }
        }
        _p.functions.push_back(KernelProgramFunction { provider, sig });
        return static_cast<std::int32_t>(_p.functions.size() - 1);
    }

    Value emitCall(KernelFunctionProvider provider, const KernelFunctionSignature& sig, const std::vector<Value>& args,
                   const std::vector<Loc>& locs)
    {
        std::vector<Value> converted;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i < sig.params.size()) {
                converted.push_back(convert(args[i], sig.params[i].type, locs[i]));
            } else {
                if (args[i].type.isVoid()) {
                    fail(locs[i], "a void function returns no value to pass");
                }
                converted.push_back(args[i]);
            }
        }
        const std::int32_t index = functionIndex(provider, sig);
        Value out;
        if (!sig.result.isVoid()) {
            out = tempValue(sig.result);
        }
        std::vector<KernelOperand> operands = operandsOf(out);
        for (const Value& v : converted) {
            const std::vector<KernelOperand> more = operandsOf(v);
            operands.insert(operands.end(), more.begin(), more.end());
        }
        const std::size_t at = emit(KernelOp::eCall, 0, maskRegister(), out.count());
        _code[at].imm = index;
        attachOperands(at, operands);
        return out;
    }

    Value coreCall(KernelCoreFunction id, const char* name, KernelType result, std::vector<KernelType> params,
                   const std::vector<Value>& args, bool variadic = false)
    {
        KernelFunctionSignature sig;
        sig.name = name;
        sig.result = result;
        for (KernelType t : params) {
            sig.params.push_back(KernelParameter { t, false });
        }
        sig.variadic = variadic;
        sig.id = static_cast<std::int32_t>(id);
        return emitCall(KernelFunctionProvider::eCore, sig, args, std::vector<Loc>(args.size()));
    }

    Value coreCall(KernelCoreFunction id, const char* name, KernelType result, std::vector<KernelType> params,
                   const std::vector<Value>& args, const Expr& e, bool variadic = false)
    {
        KernelFunctionSignature sig;
        sig.name = name;
        sig.result = result;
        for (KernelType t : params) {
            sig.params.push_back(KernelParameter { t, false });
        }
        sig.variadic = variadic;
        sig.id = static_cast<std::int32_t>(id);
        return emitCall(KernelFunctionProvider::eCore, sig, args, argLocs(e));
    }

    template <class Signature>
    const Signature* resolve(const std::vector<Signature>& overloads, const std::vector<Value>& args,
                             KernelType expected, const Expr& e)
    {
        const Signature* best = nullptr;
        int bestCost = INT_MAX;
        for (const Signature& sig : overloads) {
            const std::vector<KernelType> params = paramTypes(sig);
            if (args.size() < params.size() || (args.size() > params.size() && !isVariadic(sig))) {
                continue;
            }
            int cost = 0;
            for (std::size_t i = 0; i < params.size() && cost >= 0; ++i) {
                const int c = conversionCost(args[i].type, params[i]);
                cost = c < 0 ? -1 : cost + c;
            }
            if (cost < 0) {
                continue;
            }
            const bool matches = !expected.isVoid() && resultOf(sig) == expected;
            const bool bestMatches = best && !expected.isVoid() && resultOf(*best) == expected;
            if (!best || cost < bestCost || (cost == bestCost && matches && !bestMatches)) {
                best = &sig;
                bestCost = cost;
            }
        }
        if (!best) {
            std::vector<KernelType> types;
            for (const Value& v : args) {
                types.push_back(v.type);
            }
            fail(e.loc, "no version of '" + e.text + "' takes " + typeList(types));
        }
        return best;
    }

    static std::vector<KernelType> paramTypes(const KernelFunctionSignature& sig)
    {
        std::vector<KernelType> out;
        for (const KernelParameter& p : sig.params) {
            out.push_back(p.type);
        }
        return out;
    }

    static std::vector<KernelType> paramTypes(const Function* fn)
    {
        std::vector<KernelType> out;
        for (const Param& p : fn->params) {
            out.push_back(p.type);
        }
        return out;
    }

    static bool isVariadic(const KernelFunctionSignature& sig) { return sig.variadic; }
    static bool isVariadic(const Function*) { return false; }
    static KernelType resultOf(const KernelFunctionSignature& sig) { return sig.result; }
    static KernelType resultOf(const Function* fn) { return fn->result; }

    void arity(const Expr& e, std::size_t lo, std::size_t hi)
    {
        const std::size_t n = e.args.size();
        if (n >= lo && n <= hi) {
            return;
        }
        std::string count = std::to_string(lo);
        if (hi != lo) {
            count += hi == SIZE_MAX ? " or more" : " to " + std::to_string(hi);
        }
        fail(e.loc, "'" + e.text + "' takes " + count + (lo == 1 && hi == 1 ? " argument" : " arguments") + ", not " + std::to_string(n));
    }

    Value call(const Expr& e, KernelType expected)
    {
        if (const std::optional<KernelBaseType> t = typeKeyword(e.text)) {
            if (*t == B::eVoid) {
                fail(e.loc, "cannot cast to void");
            }
            arity(e, 1, 1);
            return cast(expr(*e.args[0], KernelType(*t)), KernelType(*t), e.args[0]->loc);
        }
        auto user = _userFunctions.find(e.text);
        if (user != _userFunctions.end()) {
            return userCall(e, user->second, expected);
        }
        if (std::optional<Value> v = builtin(e, expected)) {
            return *v;
        }
        std::vector<KernelFunctionSignature> overloads;
        _binding.functions(e.text, overloads);
        if (!overloads.empty()) {
            const std::vector<Value> args = lowerArgs(e);
            const KernelFunctionSignature& sig = *resolve(overloads, args, expected, e);
            for (std::size_t i = 0; i < sig.params.size(); ++i) {
                if (!sig.params[i].constant) {
                    continue;
                }
                const Expr& arg = *e.args[i];
                if (!isLiteral(arg)) {
                    fail(arg.loc, "argument " + std::to_string(i + 1) + " of '" + e.text + "' must be a literal");
                }
                KernelLiteral literal;
                literal.type = arg.kind == ExprKind::eInt ? B::eInt : arg.kind == ExprKind::eFloat ? B::eFloat
                                                                                                   : B::eString;
                literal.intValue = arg.intValue;
                literal.floatValue = arg.floatValue;
                literal.text = arg.text;
                const std::string error = _binding.checkLiteralArgument(sig, i, literal);
                if (!error.empty()) {
                    fail(arg.loc, error);
                }
            }
            return emitCall(KernelFunctionProvider::eBinding, sig, args, argLocs(e));
        }
        if (isUnsupportedFunction(e.text)) {
            fail(e.loc, "function '" + e.text + "' is not supported");
        }
        fail(e.loc, "unknown function '" + e.text + "'");
    }

    std::optional<Value> builtin(const Expr& e, KernelType expected)
    {
        const std::string& n = e.text;
        const Loc at = e.loc;
        const std::vector<Loc> locs = argLocs(e);
        static const std::map<std::string, KernelOp> unaryMaths = {
            { "sin", KernelOp::eFSin },
            { "cos", KernelOp::eFCos },
            { "tan", KernelOp::eFTan },
            { "asin", KernelOp::eFAsin },
            { "acos", KernelOp::eFAcos },
            { "sqrt", KernelOp::eFSqrt },
            { "exp", KernelOp::eFExp },
            { "log", KernelOp::eFLog },
            { "log10", KernelOp::eFLog10 },
            { "floor", KernelOp::eFFloor },
            { "ceil", KernelOp::eFCeil },
            { "round", KernelOp::eFRound },
            { "trunc", KernelOp::eFTrunc },
            { "frac", KernelOp::eFFrac },
            { "sign", KernelOp::eFSign },
        };
        if (auto it = unaryMaths.find(n); it != unaryMaths.end()) {
            arity(e, 1, 1);
            return unaryF(it->second, floatTuple(expr(*e.args[0]), locs[0], "'" + n + "'"));
        }
        if (n == "atan") {
            arity(e, 1, 2);
            if (e.args.size() == 1) {
                return unaryF(KernelOp::eFAtan, floatTuple(expr(*e.args[0]), locs[0], "'atan'"));
            }
        }
        if (n == "atan" || n == "atan2") {
            arity(e, 2, 2);
            const KernelType f(B::eFloat);
            const Value y = convert(expr(*e.args[0]), f, locs[0]);
            const Value x = convert(expr(*e.args[1]), f, locs[1]);
            return binaryF(KernelOp::eFAtan2, y, x);
        }
        if (n == "abs") {
            arity(e, 1, 1);
            const Value v = expr(*e.args[0]);
            if (v.type.is(B::eInt)) {
                return intTemp(KernelOp::eIAbs, v.r[0]);
            }
            return unaryF(KernelOp::eFAbs, floatTuple(v, locs[0], "'abs'"));
        }
        if (n == "degrees" || n == "radians") {
            arity(e, 1, 1);
            const Value v = floatTuple(expr(*e.args[0]), locs[0], "'" + n + "'");
            const double k = n == "degrees" ? 180. / std::numbers::pi : std::numbers::pi / 180.;
            return binaryF(KernelOp::eFMul, v, broadcast(floatLiteral(k), v.type.base, at));
        }
        if (n == "min" || n == "max") {
            arity(e, 2, 2);
            const std::vector<Value> args = lowerArgs(e);
            const bool isMin = n == "min";
            if (args[0].type.is(B::eInt) && args[1].type.is(B::eInt)) {
                return intTemp(isMin ? KernelOp::eIMin : KernelOp::eIMax, args[0].r[0], args[1].r[0]);
            }
            const B t = commonTuple(args, locs, "'" + n + "'");
            return binaryF(isMin ? KernelOp::eFMin : KernelOp::eFMax, broadcast(args[0], t, at), broadcast(args[1], t, at));
        }
        if (n == "pow") {
            arity(e, 2, 2);
            const std::vector<Value> args = lowerArgs(e);
            const B t = commonTuple(args, locs, "'pow'");
            return binaryF(KernelOp::eFPow, broadcast(args[0], t, at), broadcast(args[1], t, at));
        }
        if (n == "clamp") {
            arity(e, 3, 3);
            const std::vector<Value> args = lowerArgs(e);
            if (args[0].type.is(B::eInt) && args[1].type.is(B::eInt) && args[2].type.is(B::eInt)) {
                const Value lo = intTemp(KernelOp::eIMax, args[0].r[0], args[1].r[0]);
                return intTemp(KernelOp::eIMin, lo.r[0], args[2].r[0]);
            }
            const B t = commonTuple(args, locs, "'clamp'");
            const Value lo = binaryF(KernelOp::eFMax, broadcast(args[0], t, at), broadcast(args[1], t, at));
            return binaryF(KernelOp::eFMin, lo, broadcast(args[2], t, at));
        }
        if (n == "lerp") {
            arity(e, 3, 3);
            const std::vector<Value> args = lowerArgs(e);
            const B t = commonTuple(args, locs, "'lerp'");
            const Value a = broadcast(args[0], t, at);
            const Value b = broadcast(args[1], t, at);
            const Value bias = broadcast(args[2], t, locs[2]);
            const Value d = binaryF(KernelOp::eFSub, b, a);
            Value out = tempValue(a.type);
            for (int k = 0; k < out.count(); ++k) {
                emit(KernelOp::eFMulAdd, out.r[k], d.r[k], bias.r[k], a.r[k]);
            }
            return out;
        }
        if (n == "fit" || n == "efit" || n == "fit01" || n == "fit10" || n == "fit11") {
            const bool full = n == "fit" || n == "efit";
            arity(e, full ? 5 : 3, full ? 5 : 3);
            std::vector<Value> args = lowerArgs(e);
            std::vector<Loc> argLoc = locs;
            if (!full) {
                const double lo = n == "fit10" ? 1. : n == "fit11" ? -1.
                                                                   : 0.;
                const double hi = n == "fit10" ? 0. : 1.;
                args.insert(args.begin() + 1, { floatLiteral(lo), floatLiteral(hi) });
                argLoc.insert(argLoc.begin() + 1, { at, at });
            }
            const B t = commonTuple(args, argLoc, "'" + n + "'");
            std::vector<Value> v;
            for (std::size_t i = 0; i < args.size(); ++i) {
                v.push_back(broadcast(args[i], t, argLoc[i]));
            }
            Value u = binaryF(KernelOp::eFDiv, binaryF(KernelOp::eFSub, v[0], v[1]), binaryF(KernelOp::eFSub, v[2], v[1]));
            if (n != "efit") {
                u = binaryF(KernelOp::eFMin, binaryF(KernelOp::eFMax, u, broadcast(floatLiteral(0.), t, at)),
                            broadcast(floatLiteral(1.), t, at));
            }
            const Value range = binaryF(KernelOp::eFSub, v[4], v[3]);
            Value out = tempValue(KernelType(t));
            for (int k = 0; k < out.count(); ++k) {
                emit(KernelOp::eFMulAdd, out.r[k], range.r[k], u.r[k], v[3].r[k]);
            }
            return out;
        }
        if (n == "smooth") {
            arity(e, 3, 3);
            const KernelType f(B::eFloat);
            const Value lo = convert(expr(*e.args[0]), f, locs[0]);
            const Value hi = convert(expr(*e.args[1]), f, locs[1]);
            const Value x = convert(expr(*e.args[2]), f, locs[2]);
            Value u = binaryF(KernelOp::eFDiv, binaryF(KernelOp::eFSub, x, lo), binaryF(KernelOp::eFSub, hi, lo));
            u = binaryF(KernelOp::eFMin, binaryF(KernelOp::eFMax, u, floatLiteral(0.)), floatLiteral(1.));
            const Value twoU = binaryF(KernelOp::eFMul, u, floatLiteral(2.));
            const Value s = binaryF(KernelOp::eFSub, floatLiteral(3.), twoU);
            return binaryF(KernelOp::eFMul, binaryF(KernelOp::eFMul, u, u), s);
        }
        if (n == "length" || n == "length2") {
            arity(e, 1, 1);
            const Value v = expr(*e.args[0]);
            if (!kernelIsVector(v.type.base) || v.type.array) {
                fail(locs[0], "'" + n + "' takes a vector, not " + article(v.type));
            }
            const std::int32_t d = dotRegister(v, v);
            if (n == "length") {
                emit(KernelOp::eFSqrt, d, d);
            }
            Value out = floatValue(d);
            out.temporary = true;
            return out;
        }
        if (n == "distance" || n == "distance2" || n == "dot") {
            arity(e, 2, 2);
            const std::vector<Value> args = lowerArgs(e);
            for (std::size_t i = 0; i < 2; ++i) {
                if (!kernelIsVector(args[i].type.base) || args[i].type.array) {
                    fail(locs[i], "'" + n + "' takes vectors, not " + article(args[i].type));
                }
            }
            if (args[0].type != args[1].type) {
                fail(at, "'" + n + "' takes two vectors of the same size");
            }
            std::int32_t d;
            if (n == "dot") {
                d = dotRegister(args[0], args[1]);
            } else {
                const Value diff = binaryF(KernelOp::eFSub, args[0], args[1]);
                d = dotRegister(diff, diff);
                if (n == "distance") {
                    emit(KernelOp::eFSqrt, d, d);
                }
            }
            Value out = floatValue(d);
            out.temporary = true;
            return out;
        }
        if (n == "normalize") {
            arity(e, 1, 1);
            const Value v = expr(*e.args[0]);
            if (!kernelIsVector(v.type.base) || v.type.array) {
                fail(locs[0], "'normalize' takes a vector, not " + article(v.type));
            }
            const std::int32_t len2 = dotRegister(v, v);
            const std::int32_t inv = temp(KernelBank::eFloat);
            emit(KernelOp::eFSqrt, inv, len2);
            emit(KernelOp::eFDiv, inv, constF(1.f), inv);
            const std::int32_t nonZero = temp(KernelBank::eInt);
            emit(KernelOp::eFGt, nonZero, len2, constF(0.f));
            emit(KernelOp::eFSel, inv, nonZero, inv, constF(0.f));
            return binaryF(KernelOp::eFMul, v, broadcast(floatValue(inv), v.type.base, at));
        }
        if (n == "cross") {
            arity(e, 2, 2);
            const std::vector<Value> args = lowerArgs(e);
            for (std::size_t i = 0; i < 2; ++i) {
                if (!args[i].type.is(B::eVector)) {
                    fail(locs[i], "'cross' takes vectors, not " + article(args[i].type));
                }
            }
            return cross(args[0], args[1]);
        }
        if (n == "set") {
            const B t = tupleType(static_cast<int>(e.args.size()));
            if (t == B::eVoid || t == B::eFloat) {
                fail(at, "'set' takes 2, 3, 4, 9 or 16 numbers, not " + std::to_string(e.args.size()));
            }
            Value out;
            out.type = KernelType(t);
            for (std::size_t k = 0; k < e.args.size(); ++k) {
                const Value v = expr(*e.args[k]);
                if (!isScalar(v.type)) {
                    fail(locs[k], "'set' takes numbers, not " + article(v.type));
                }
                out.r[k] = convert(v, KernelType(B::eFloat), locs[k]).r[0];
            }
            return out;
        }
        if (n == "ident") {
            arity(e, 0, 0);
            return convert(floatLiteral(1.), KernelType(expected.is(B::eMatrix3) ? B::eMatrix3 : B::eMatrix), at);
        }
        if (n == "transpose" || n == "invert" || n == "determinant") {
            arity(e, 1, 1);
            const Value m = expr(*e.args[0]);
            if (m.type.array || !kernelIsMatrix(m.type.base)) {
                fail(locs[0], "'" + n + "' takes a matrix, not " + article(m.type));
            }
            const bool three = m.type.base == B::eMatrix3;
            if (n == "transpose") {
                return transpose(m);
            }
            if (n == "invert") {
                return coreCall(three ? KernelCoreFunction::eInvert3 : KernelCoreFunction::eInvert4, "invert", m.type,
                                { m.type }, { m }, e);
            }
            return coreCall(three ? KernelCoreFunction::eDeterminant3 : KernelCoreFunction::eDeterminant4, "determinant",
                            KernelType(B::eFloat), { m.type }, { m }, e);
        }
        if (n == "noise" || n == "rand") {
            arity(e, 1, 1);
            Value v = expr(*e.args[0]);
            const bool wantsVector = expected.is(B::eVector);
            const bool isNoise = n == "noise";
            if (isScalar(v.type)) {
                v = convert(v, KernelType(B::eFloat), locs[0]);
                if (isNoise) {
                    return coreCall(KernelCoreFunction::eNoiseFloat, "noise", KernelType(B::eFloat), { v.type }, { v }, e);
                }
                return coreCall(wantsVector ? KernelCoreFunction::eRandFloatToVector : KernelCoreFunction::eRandFloat, "rand",
                                KernelType(wantsVector ? B::eVector : B::eFloat), { v.type }, { v }, e);
            }
            if (!v.type.is(B::eVector)) {
                fail(locs[0], "'" + n + "' takes a float or a vector, not " + article(v.type));
            }
            const KernelType result(wantsVector ? B::eVector : B::eFloat);
            const KernelCoreFunction id = isNoise
                ? (wantsVector ? KernelCoreFunction::eNoiseVectorToVector : KernelCoreFunction::eNoiseVector)
                : (wantsVector ? KernelCoreFunction::eRandVectorToVector : KernelCoreFunction::eRandVector);
            return coreCall(id, isNoise ? "noise" : "rand", result, { v.type }, { v }, e);
        }
        const KernelType s(B::eString);
        const KernelType i(B::eInt);
        const KernelType f(B::eFloat);
        if (n == "strlen") {
            arity(e, 1, 1);
            return coreCall(KernelCoreFunction::eStrlen, "strlen", i, { s }, lowerArgs(e), e);
        }
        if (n == "itoa") {
            arity(e, 1, 1);
            return coreCall(KernelCoreFunction::eItoa, "itoa", s, { i }, lowerArgs(e), e);
        }
        if (n == "atoi") {
            arity(e, 1, 1);
            return coreCall(KernelCoreFunction::eAtoi, "atoi", i, { s }, lowerArgs(e), e);
        }
        if (n == "atof") {
            arity(e, 1, 1);
            return coreCall(KernelCoreFunction::eAtof, "atof", f, { s }, lowerArgs(e), e);
        }
        if (n == "startswith" || n == "endswith") {
            arity(e, 2, 2);
            const bool starts = n == "startswith";
            return coreCall(starts ? KernelCoreFunction::eStartsWith : KernelCoreFunction::eEndsWith,
                            starts ? "startswith" : "endswith", i, { s, s }, lowerArgs(e), e);
        }
        if (n == "sprintf") {
            arity(e, 1, SIZE_MAX);
            const std::vector<Value> args = lowerArgs(e);
            for (std::size_t k = 1; k < args.size(); ++k) {
                if (args[k].type.array) {
                    fail(locs[k], "'sprintf' cannot format an array");
                }
            }
            return coreCall(KernelCoreFunction::eSprintf, "sprintf", s, { s }, args, e, true);
        }
        if (n == "concat") {
            arity(e, 1, SIZE_MAX);
            Value out = convert(expr(*e.args[0]), s, locs[0]);
            for (std::size_t k = 1; k < e.args.size(); ++k) {
                out = arithmetic("+", out, convert(expr(*e.args[k]), s, locs[k]), locs[k]);
            }
            return out;
        }
        if (n == "len") {
            arity(e, 1, 1);
            const Value v = expr(*e.args[0]);
            if (v.type.array) {
                return intTemp(KernelOp::eALen, v.r[0]);
            }
            if (v.type.is(B::eString)) {
                return coreCall(KernelCoreFunction::eStrlen, "strlen", i, { s }, { v }, e);
            }
            if (isFloatTuple(v.type) && !v.type.is(B::eFloat)) {
                return intLiteral(v.count());
            }
            fail(locs[0], "'len' takes an array, string or vector, not " + article(v.type));
        }
        if (n == "append" || n == "push" || n == "resize") {
            arity(e, 2, 2);
            const LValue lv = lvalue(*e.args[0]);
            if (lv.kind != LValue::Kind::eRegisters || !lv.type.array) {
                fail(locs[0], "'" + n + "' needs an array variable");
            }
            markRead(lv.binding);
            markWrite(lv.binding, true, locs[0]);
            if (n == "resize") {
                const Value size = toInt(expr(*e.args[1]), locs[1]);
                const std::size_t r = emit(KernelOp::eAResize, 0, maskRegister(), lv.whole.r[0], size.r[0]);
                _code[r].imm = static_cast<std::int32_t>(lv.type.base);
                return Value {};
            }
            const KernelType element = lv.type.element();
            const Value v = convert(expr(*e.args[1], element), element, locs[1]);
            const std::size_t p = emit(KernelOp::eAPush, 0, maskRegister(), lv.whole.r[0]);
            _code[p].imm = static_cast<std::int32_t>(element.base);
            attachOperands(p, operandsOf(v));
            return Value {};
        }
        if (n == "array") {
            std::vector<const Expr*> items;
            for (const ExprPtr& a : e.args) {
                items.push_back(a.get());
            }
            KernelType element = expected.array ? expected.element() : KernelType(B::eFloat);
            if (!expected.array && !items.empty()) {
                element = expr(*items[0]).type;
            }
            return arrayOf(element, items, at);
        }
        if (n == "quaternion") {
            arity(e, 2, 2);
            return coreCall(KernelCoreFunction::eQuaternionAxisAngle, "quaternion", KernelType(B::eVector4),
                            { f, KernelType(B::eVector) }, lowerArgs(e), e);
        }
        if (n == "qmultiply") {
            arity(e, 2, 2);
            const KernelType q(B::eVector4);
            return coreCall(KernelCoreFunction::eQuaternionMultiply, "qmultiply", q, { q, q }, lowerArgs(e), e);
        }
        if (n == "qrotate") {
            arity(e, 2, 2);
            return coreCall(KernelCoreFunction::eQuaternionRotate, "qrotate", KernelType(B::eVector),
                            { KernelType(B::eVector4), KernelType(B::eVector) }, lowerArgs(e), e);
        }
        if (n == "getcomp") {
            arity(e, 2, 3);
            const Value v = expr(*e.args[0]);
            const bool matrix = isFloatTuple(v.type) && kernelIsMatrix(v.type.base);
            if (!(isFloatTuple(v.type) && (kernelIsVector(v.type.base) || matrix)) || e.args.size() != (matrix ? 3u : 2u)) {
                fail(at, "'getcomp' takes a vector and an index, or a matrix, a row and a column");
            }
            if (!matrix) {
                const Expr& ie = *e.args[1];
                if (ie.kind == ExprKind::eInt && ie.intValue >= 0 && ie.intValue < v.count()) {
                    return floatValue(v.r[ie.intValue]);
                }
                return selectComponent(v, toInt(expr(ie), ie.loc).r[0]);
            }
            const int dim = matrixDim(v.type.base);
            const Expr& re = *e.args[1];
            const Expr& ce = *e.args[2];
            if (re.kind == ExprKind::eInt && ce.kind == ExprKind::eInt && re.intValue >= 0 && re.intValue < dim
                && ce.intValue >= 0 && ce.intValue < dim) {
                return floatValue(v.r[re.intValue * dim + ce.intValue]);
            }
            const Value row = toInt(expr(re), re.loc);
            const Value column = toInt(expr(ce), ce.loc);
            const Value index = intTemp(KernelOp::eIMul, row.r[0], constI(dim));
            emit(KernelOp::eIAdd, index.r[0], index.r[0], column.r[0]);
            return selectComponent(v, index.r[0]);
        }
        return std::nullopt;
    }

    bool paramWritten(const Function& fn, const std::string& name) const
    {
        bool written = false;
        auto f = [&](const Expr& e) {
            if ((e.kind == ExprKind::eAssign || e.kind == ExprKind::eIncDec) && !e.args.empty()) {
                const Expr& root = lvalueRoot(*e.args[0]);
                written |= root.kind == ExprKind::eVariable && root.text == name;
            }
            if (e.kind == ExprKind::eCall) {
                for (const ExprPtr& a : e.args) {
                    const Expr& root = lvalueRoot(*a);
                    written |= root.kind == ExprKind::eVariable && root.text == name;
                }
            }
        };
        for (const StmtPtr& s : fn.body) {
            visitStmt(*s, f);
        }
        return written;
    }

    Value userCall(const Expr& e, const std::vector<const Function*>& overloads, KernelType expected)
    {
        const std::vector<Value> args = lowerArgs(e);
        const Function* fn = *resolve(overloads, args, expected, e);
        if (std::find(_inlineStack.begin(), _inlineStack.end(), fn) != _inlineStack.end()) {
            fail(e.loc, "'" + fn->name + "' calls itself, and recursion is not supported");
        }
        std::vector<Symbol> params;
        for (std::size_t i = 0; i < fn->params.size(); ++i) {
            const Param& p = fn->params[i];
            const Expr& a = *e.args[i];
            Symbol s;
            s.readOnly = p.isConst;
            if ((a.kind == ExprKind::eVariable || a.kind == ExprKind::eBinding) && args[i].type == p.type) {
                s.value = args[i];
                s.value.temporary = false;
                if (a.kind == ExprKind::eVariable) {
                    const Symbol* source = lookup(a.text);
                    s.binding = source->binding;
                    s.readOnly |= source->readOnly;
                } else {
                    s.binding = bindingIndex(a);
                }
            } else {
                Value c = convert(args[i], p.type, a.loc);
                if (!c.temporary && paramWritten(*fn, p.name)) {
                    Value copy = tempValue(c.type);
                    for (int k = 0; k < c.count(); ++k) {
                        emit(movOp(c.bank()), copy.r[k], c.r[k]);
                    }
                    c = copy;
                }
                s.value = c;
                s.value.temporary = false;
            }
            params.push_back(s);
        }
        return inlineFunction(*fn, params);
    }

    Value inlineFunction(const Function& fn, const std::vector<Symbol>& params)
    {
        _inlineStack.push_back(&fn);
        _called.insert(&fn);
        FunctionContext ctx;
        ctx.fn = &fn;
        ctx.needsMask = needsReturnMask(fn);
        ctx.loopDepth = _loops.size();
        if (!fn.result.isVoid()) {
            ctx.result = tempValue(fn.result);
            if (ctx.needsMask) {
                zeroFill(ctx.result);
            }
        }
        const std::int32_t savedMask = _mask;
        const bool savedMasked = _masked;
        const std::size_t savedBase = _scopeBase;
        _scopeBase = _scopes.size();
        pushScope();
        for (std::size_t i = 0; i < params.size(); ++i) {
            _scopes.back().symbols[fn.params[i].name] = params[i];
        }
        if (ctx.needsMask) {
            const std::int32_t m = temp(KernelBank::eInt);
            emit(KernelOp::eIMov, m, maskRegister());
            _maskChain.push_back(m);
            _mask = m;
            _masked = true;
        }
        ctx.chainDepth = _maskChain.size() - (ctx.needsMask ? 1 : 0);
        _functions.push_back(ctx);
        for (const StmtPtr& s : fn.body) {
            statement(*s);
        }
        const Value result = _functions.back().result;
        _functions.pop_back();
        if (ctx.needsMask) {
            _maskChain.pop_back();
        }
        popScope();
        _scopeBase = savedBase;
        _mask = savedMask;
        _masked = savedMasked;
        _inlineStack.pop_back();
        Value out = result;
        out.temporary = !fn.result.isVoid();
        return out;
    }

    // ---- statements

    void zeroFill(const Value& v)
    {
        if (v.type.array) {
            const std::size_t i = emit(KernelOp::eANew, v.r[0]);
            _code[i].imm = static_cast<std::int32_t>(v.type.base);
            return;
        }
        std::int32_t zero;
        switch (v.bank()) {
        case KernelBank::eInt:
            zero = constI(0);
            break;
        case KernelBank::eString:
            zero = constS(std::string());
            break;
        default:
            zero = constF(0.f);
            break;
        }
        for (int k = 0; k < v.count(); ++k) {
            emit(movOp(v.bank()), v.r[k], zero);
        }
    }

    bool adoptable(const Value& v) const
    {
        if (!v.temporary) {
            return false;
        }
        for (int k = 0; k < v.count(); ++k) {
            if (!isStatementTemp(v.bank(), v.r[k])) {
                return false;
            }
            for (int j = 0; j < k; ++j) {
                if (v.r[j] == v.r[k]) {
                    return false;
                }
            }
        }
        return true;
    }

    Value declareRegisters(KernelType t, const Value* init)
    {
        if (init && adoptable(*init)) {
            std::vector<KernelOperand>& temps = _temps.back();
            for (int k = 0; k < init->count(); ++k) {
                const KernelOperand o { init->bank(), init->r[k] };
                temps.erase(std::find_if(temps.begin(), temps.end(),
                                         [&](const KernelOperand& x) { return x.bank == o.bank && x.reg == o.reg; }));
                _scopes.back().owned.push_back(o);
            }
            Value v = *init;
            v.temporary = false;
            v.literal = false;
            return v;
        }
        Value v;
        v.type = t;
        for (int k = 0; k < v.count(); ++k) {
            v.r[k] = local(v.bank());
        }
        if (init) {
            for (int k = 0; k < v.count(); ++k) {
                emit(movOp(v.bank()), v.r[k], init->r[k]);
            }
        } else {
            zeroFill(v);
        }
        return v;
    }

    void declare(const std::string& name, Loc at, Value value)
    {
        if (_scopes.back().symbols.count(name)) {
            fail(at, "'" + name + "' is already declared");
        }
        Symbol s;
        s.value = value;
        s.value.temporary = false;
        _scopes.back().symbols[name] = s;
    }

    void declaration(const Stmt& s)
    {
        for (const Declarator& d : s.decls) {
            if (d.init) {
                const Value init = convert(expr(*d.init, d.type), d.type, d.init->loc);
                declare(d.name, d.loc, declareRegisters(d.type, &init));
            } else {
                declare(d.name, d.loc, declareRegisters(d.type, nullptr));
            }
        }
    }

    void retireLanes(std::size_t chainFrom, std::size_t loopFrom)
    {
        const std::int32_t t = temp(KernelBank::eInt);
        emit(KernelOp::eIMov, t, _mask);
        for (std::size_t j = loopFrom; j < _loops.size(); ++j) {
            emit(KernelOp::eIAndNot, _loops[j].loopMask, _loops[j].loopMask, t);
        }
        for (std::size_t j = chainFrom; j < _maskChain.size(); ++j) {
            emit(KernelOp::eIAndNot, _maskChain[j], _maskChain[j], t);
        }
    }

    void branch(std::int32_t mask, const Stmt& body)
    {
        const std::size_t skip = emit(KernelOp::eJumpIfNone, 0, mask);
        const std::int32_t savedMask = _mask;
        const bool savedMasked = _masked;
        _maskChain.push_back(mask);
        _mask = mask;
        _masked = true;
        statement(body);
        _maskChain.pop_back();
        _mask = savedMask;
        _masked = savedMasked;
        _code[skip].imm = static_cast<std::int32_t>(_code.size());
    }

    void ifStatement(const Stmt& s)
    {
        const Value c = truthy(expr(*s.expr), s.expr->loc);
        const std::int32_t parent = maskRegister();
        const std::int32_t thenMask = temp(KernelBank::eInt);
        emit(KernelOp::eIAnd, thenMask, parent, c.r[0]);
        std::int32_t elseMask = -1;
        if (s.body.size() > 1) {
            elseMask = temp(KernelBank::eInt);
            emit(KernelOp::eIAndNot, elseMask, parent, c.r[0]);
        }
        branch(thenMask, *s.body[0]);
        if (elseMask >= 0) {
            branch(elseMask, *s.body[1]);
        }
    }

    // Lanes leave a loop by clearing their bit in the loop mask; the body runs under a per-iteration
    // copy so that continue only retires lanes for the rest of the iteration.
    void loop(const Expr* cond, bool condFirst, const Expr* step, const Stmt& body,
              const std::function<void(std::int32_t)>& beforeBody = {})
    {
        const std::int32_t savedMask = _mask;
        const bool savedMasked = _masked;
        const std::int32_t loopMask = temp(KernelBank::eInt);
        emit(KernelOp::eIMov, loopMask, maskRegister());
        const std::size_t top = _code.size();
        _mask = loopMask;
        _masked = true;
        auto condition = [&]() {
            if (cond) {
                const Value c = truthy(expr(*cond), cond->loc);
                emit(KernelOp::eIAnd, loopMask, loopMask, c.r[0]);
            }
        };
        if (condFirst) {
            condition();
        }
        const std::size_t exit = emit(KernelOp::eJumpIfNone, 0, loopMask);
        const std::int32_t iterMask = temp(KernelBank::eInt);
        emit(KernelOp::eIMov, iterMask, loopMask);
        _loops.push_back(LoopContext { loopMask, _maskChain.size(), _functions.size() });
        _maskChain.push_back(iterMask);
        _mask = iterMask;
        pushScope();
        if (beforeBody) {
            beforeBody(iterMask);
        }
        statement(body);
        popScope();
        _maskChain.pop_back();
        _loops.pop_back();
        _mask = loopMask;
        if (!condFirst) {
            condition();
        }
        if (step) {
            beginStatement();
            expr(*step, {}, true);
            endStatement();
        }
        const std::size_t back = emit(KernelOp::eJump, 0);
        _code[back].imm = static_cast<std::int32_t>(top);
        _code[exit].imm = static_cast<std::int32_t>(_code.size());
        _mask = savedMask;
        _masked = savedMasked;
    }

    void foreachStatement(const Stmt& s)
    {
        const Value array = expr(*s.expr);
        if (!array.type.array) {
            fail(s.expr->loc, "foreach loops over an array, not " + article(array.type));
        }
        std::int32_t arrayReg = array.r[0];
        if (!array.temporary) {
            arrayReg = temp(KernelBank::eArray);
            emit(KernelOp::eACopy, arrayReg, array.r[0]);
        }
        const std::int32_t length = temp(KernelBank::eInt);
        emit(KernelOp::eALen, length, arrayReg);
        const std::int32_t index = temp(KernelBank::eInt);
        emit(KernelOp::eIMov, index, constI(0));
        const std::int32_t inRange = temp(KernelBank::eInt);
        const Declarator& valueDecl = s.decls.back();
        const KernelType element = array.type.element();
        const KernelType valueType = valueDecl.type.isVoid() ? element : valueDecl.type;
        const std::int32_t savedMask = _mask;
        const bool savedMasked = _masked;
        const std::int32_t loopMask = temp(KernelBank::eInt);
        emit(KernelOp::eIMov, loopMask, maskRegister());
        const std::size_t top = _code.size();
        emit(KernelOp::eILt, inRange, index, length);
        emit(KernelOp::eIAnd, loopMask, loopMask, inRange);
        const std::size_t exit = emit(KernelOp::eJumpIfNone, 0, loopMask);
        const std::int32_t iterMask = temp(KernelBank::eInt);
        emit(KernelOp::eIMov, iterMask, loopMask);
        _loops.push_back(LoopContext { loopMask, _maskChain.size(), _functions.size() });
        _maskChain.push_back(iterMask);
        _mask = iterMask;
        _masked = true;
        pushScope();
        beginStatement();
        Value element_ = tempValue(element);
        const std::size_t get = emit(KernelOp::eAGet, 0, arrayReg, index);
        _code[get].imm = static_cast<std::int32_t>(element.base);
        attachOperands(get, operandsOf(element_));
        const Value converted = convert(element_, valueType, valueDecl.loc);
        Value value;
        value.type = valueType;
        for (int k = 0; k < value.count(); ++k) {
            value.r[k] = local(value.bank());
            emit(movOp(value.bank()), value.r[k], converted.r[k]);
        }
        endStatement();
        if (s.foreachHasIndex) {
            Value i;
            i.type = KernelType(B::eInt);
            i.r[0] = local(KernelBank::eInt);
            emit(KernelOp::eIMov, i.r[0], index);
            declare(s.decls[0].name, s.decls[0].loc, i);
        }
        declare(valueDecl.name, valueDecl.loc, value);
        statement(*s.body[0]);
        popScope();
        _maskChain.pop_back();
        _loops.pop_back();
        emit(KernelOp::eIAdd, index, index, constI(1));
        const std::size_t back = emit(KernelOp::eJump, 0);
        _code[back].imm = static_cast<std::int32_t>(top);
        _code[exit].imm = static_cast<std::int32_t>(_code.size());
        _mask = savedMask;
        _masked = savedMasked;
    }

    void returnStatement(const Stmt& s)
    {
        if (_functions.empty()) {
            if (s.expr) {
                fail(s.expr->loc, "the snippet's main body cannot return a value");
            }
            retireLanes(0, 0);
            return;
        }
        FunctionContext& f = _functions.back();
        const Function& fn = *f.fn;
        if (fn.result.isVoid()) {
            if (s.expr) {
                fail(s.expr->loc, "void function '" + fn.name + "' cannot return a value");
            }
        } else {
            if (!s.expr) {
                fail(s.loc, "function '" + fn.name + "' must return " + article(fn.result));
            }
            const Value v = convert(expr(*s.expr, fn.result), fn.result, s.expr->loc);
            std::vector<std::int32_t> dst(f.result.r.begin(), f.result.r.begin() + f.result.count());
            assignRegisters(dst, v);
        }
        if (f.needsMask) {
            retireLanes(f.chainDepth, f.loopDepth);
        }
    }

    void loopExit(const Stmt& s, bool isBreak)
    {
        if (_loops.empty() || _loops.back().functionDepth != _functions.size()) {
            fail(s.loc, std::string("'") + (isBreak ? "break" : "continue") + "' is only allowed inside a loop");
        }
        const LoopContext& l = _loops.back();
        retireLanes(l.chainDepth, isBreak ? _loops.size() - 1 : _loops.size());
    }

    void statement(const Stmt& s)
    {
        beginStatement();
        switch (s.kind) {
        case StmtKind::eEmpty:
            break;
        case StmtKind::eExpr:
            expr(*s.expr, {}, true);
            break;
        case StmtKind::eDecl:
            declaration(s);
            break;
        case StmtKind::eBlock:
            pushScope();
            for (const StmtPtr& b : s.body) {
                statement(*b);
            }
            popScope();
            break;
        case StmtKind::eIf:
            ifStatement(s);
            break;
        case StmtKind::eFor:
            pushScope();
            if (s.init) {
                statement(*s.init);
            }
            loop(s.expr.get(), true, s.step.get(), *s.body[0]);
            popScope();
            break;
        case StmtKind::eWhile:
            loop(s.expr.get(), true, nullptr, *s.body[0]);
            break;
        case StmtKind::eDoWhile:
            loop(s.expr.get(), false, nullptr, *s.body[0]);
            break;
        case StmtKind::eForeach:
            foreachStatement(s);
            break;
        case StmtKind::eReturn:
            returnStatement(s);
            break;
        case StmtKind::eBreak:
            loopExit(s, true);
            break;
        case StmtKind::eContinue:
            loopExit(s, false);
            break;
        }
        endStatement();
    }

    // ---- program

    void checkFunctionDefinitions()
    {
        for (const auto& fn : _unit.functions) {
            std::set<std::string> names;
            for (const Param& p : fn->params) {
                if (!names.insert(p.name).second) {
                    fail(p.loc, "parameter '" + p.name + "' is declared twice");
                }
            }
            if (!fn->result.isVoid() && countReturns(fn->body) == 0) {
                fail(fn->loc, "function '" + fn->name + "' never returns " + article(fn->result));
            }
            for (const Function* other : _userFunctions[fn->name]) {
                if (other == fn.get()) {
                    break;
                }
                if (paramTypes(other) == paramTypes(fn.get())) {
                    fail(fn->loc, "function '" + fn->name + "' is already defined with these parameters");
                }
            }
        }
    }

    // Literals are hoisted as they are lowered, and some (an int literal converted to float) end up
    // unread; dropping them leaves their registers allocated but never loaded.
    void dropUnusedConstants()
    {
        std::set<std::pair<char, std::int32_t>> used;
        for (const KernelInstruction& in : _code) {
            const OpInfo info = opInfo(in.op);
            const char banks[4] = { info.d, info.a, info.b, info.c };
            const std::int32_t regs[4] = { in.d, in.a, in.b, in.c };
            for (int k = 0; k < 4; ++k) {
                if (banks[k]) {
                    used.insert({ banks[k], regs[k] });
                }
            }
            for (std::uint32_t k = 0; info.operands && k < in.operandCount; ++k) {
                const KernelOperand& o = _p.operands[in.operandBegin + k];
                used.insert({ bankLetter(o.bank), o.reg });
            }
        }
        std::vector<KernelInstruction> kept;
        for (const KernelInstruction& in : _prologue) {
            if (used.count({ opInfo(in.op).d, in.d })) {
                kept.push_back(in);
            }
        }
        _prologue = std::move(kept);
    }

    void finish()
    {
        dropUnusedConstants();
        const std::int32_t offset = static_cast<std::int32_t>(_prologue.size());
        for (KernelInstruction& in : _code) {
            if (in.op == KernelOp::eJump || in.op == KernelOp::eJumpIfNone) {
                in.imm += offset;
            }
        }
        _p.prologueSize = static_cast<std::uint32_t>(_prologue.size());
        _p.instructions = std::move(_prologue);
        _p.instructions.insert(_p.instructions.end(), _code.begin(), _code.end());
    }
};

} // namespace

bool
kernelCompile(std::string_view source, const KernelBinding& binding, KernelProgram& program, KernelCompileError& error)
{
    program = KernelProgram {};
    std::vector<KernelToken> tokens;
    if (!kernelLex(source, tokens, error)) {
        return false;
    }
    try {
        const Unit unit = Parser(std::move(tokens)).parse();
        std::vector<const Function*> uncalled;
        {
            Compiler compiler(binding, program, unit);
            compiler.compileMain();
            uncalled = compiler.uncalledFunctions();
        }
        for (const Function* fn : uncalled) {
            KernelProgram scratch;
            Compiler checker(binding, scratch, unit);
            checker.checkFunction(*fn);
        }
    } catch (const Failure& f) {
        error = f.error;
        program = KernelProgram {};
        return false;
    }
    return true;
}

const char*
kernelOpName(KernelOp op)
{
    return opInfo(op).name;
}

std::string
kernelDisassemble(const KernelProgram& program)
{
    std::string out;
    auto reg = [](char bank, std::int32_t r) { return std::string(1, bank) + std::to_string(r); };
    for (std::size_t pc = 0; pc < program.instructions.size(); ++pc) {
        if (pc == program.prologueSize && pc != 0) {
            out += "--\n";
        }
        const KernelInstruction& in = program.instructions[pc];
        const OpInfo info = opInfo(in.op);
        std::string line = std::to_string(pc) + ": " + info.name;
        std::vector<std::string> fields;
        const char banks[4] = { info.d, info.a, info.b, info.c };
        const std::int32_t regs[4] = { in.d, in.a, in.b, in.c };
        for (int k = 0; k < 4; ++k) {
            if (banks[k]) {
                fields.push_back(reg(banks[k], regs[k]));
            }
        }
        char number[32];
        switch (info.imm) {
        case 'F':
            std::snprintf(number, sizeof number, "%g", static_cast<double>(in.fimm));
            fields.push_back(number);
            break;
        case 'I':
            fields.push_back(std::to_string(in.imm));
            break;
        case 'S':
            fields.push_back("\"" + (in.imm >= 0 && static_cast<std::size_t>(in.imm) < program.strings.size() ? program.strings[in.imm] : std::string()) + "\"");
            break;
        case 'T':
            fields.push_back("-> " + std::to_string(in.imm));
            break;
        case 'E':
            fields.push_back(kernelBaseTypeName(static_cast<KernelBaseType>(in.imm)));
            break;
        case 'N':
            if (in.imm >= 0 && static_cast<std::size_t>(in.imm) < program.functions.size()) {
                fields.push_back(program.functions[in.imm].signature.name);
            }
            break;
        default:
            break;
        }
        if (info.operands) {
            std::string list = "[";
            for (std::uint32_t k = 0; k < in.operandCount; ++k) {
                const KernelOperand& o = program.operands[in.operandBegin + k];
                if (in.op == KernelOp::eCall && k == static_cast<std::uint32_t>(in.b)) {
                    list += "; ";
                } else if (k) {
                    list += ", ";
                }
                list += reg(bankLetter(o.bank), o.reg);
            }
            fields.push_back(list + "]");
        }
        for (std::size_t k = 0; k < fields.size(); ++k) {
            line += (k ? ", " : " ") + fields[k];
        }
        out += line + "\n";
    }
    return out;
}

NATRON_NAMESPACE_EXIT
