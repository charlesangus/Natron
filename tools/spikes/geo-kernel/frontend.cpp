// A tiny typed front end: just enough of a VEX-like snippet language for the backend comparison
// (declarations, @P/@N/@Cd/@ptnum/@numpt/@Time, arithmetic, comparisons, one level of if/else
// or more, and a handful of builtins).

#include "gk.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>

namespace gk {
namespace {

    struct Token {
        enum Kind { End,
                    Ident,
                    Int,
                    Float,
                    String,
                    AttrRef,
                    Punct } kind
            = End;
        std::string text;
        char prefix = 0; // type prefix of an @ reference (f, i, v, p) or 0
        long ival = 0;
        double fval = 0;
        int line = 1;
        int col = 1;
    };

    struct Failure {
        CompileError err;
    };

    [[noreturn]] void
    fail(const Token& at, const std::string& message)
    {
        throw Failure { CompileError { at.line, at.col, message } };
    }

    std::vector<Token>
    lex(const std::string& s)
    {
        std::vector<Token> out;
        std::size_t i = 0;
        int line = 1;
        std::size_t lineStart = 0;
        auto here = [&](Token t) {
            t.line = line;
            t.col = int(i - lineStart) + 1;
            return t;
        };
        while (true) {
            while (i < s.size()) {
                if (s[i] == '\n') {
                    ++line;
                    lineStart = ++i;
                } else if (std::isspace((unsigned char)s[i])) {
                    ++i;
                } else if (s.compare(i, 2, "//") == 0) {
                    while (i < s.size() && s[i] != '\n') {
                        ++i;
                    }
                } else if (s.compare(i, 2, "/*") == 0) {
                    std::size_t e = s.find("*/", i + 2);
                    for (std::size_t k = i; k < (e == std::string::npos ? s.size() : e); ++k) {
                        if (s[k] == '\n') {
                            ++line;
                            lineStart = k + 1;
                        }
                    }
                    i = e == std::string::npos ? s.size() : e + 2;
                } else {
                    break;
                }
            }
            Token t = here(Token {});
            if (i >= s.size()) {
                out.push_back(t);
                return out;
            }
            const char c = s[i];
            const bool prefixedAttr = std::strchr("fivp", c) && i + 1 < s.size() && s[i + 1] == '@';
            if (c == '@' || prefixedAttr) {
                if (prefixedAttr) {
                    t.prefix = c;
                    ++i;
                }
                ++i;
                std::size_t b = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) {
                    ++i;
                }
                if (b == i) {
                    fail(t, "expected an attribute name after '@'");
                }
                t.kind = Token::AttrRef;
                t.text = s.substr(b, i - b);
            } else if (std::isalpha((unsigned char)c) || c == '_') {
                std::size_t b = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) {
                    ++i;
                }
                t.kind = Token::Ident;
                t.text = s.substr(b, i - b);
            } else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
                std::size_t b = i;
                bool isFloat = false;
                while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) {
                    isFloat |= !std::isdigit((unsigned char)s[i]);
                    ++i;
                }
                t.text = s.substr(b, i - b);
                if (isFloat) {
                    t.kind = Token::Float;
                    t.fval = std::strtod(t.text.c_str(), nullptr);
                } else {
                    t.kind = Token::Int;
                    t.ival = std::strtol(t.text.c_str(), nullptr, 10);
                }
            } else if (c == '"') {
                std::size_t e = s.find('"', i + 1);
                if (e == std::string::npos) {
                    fail(t, "unterminated string");
                }
                t.kind = Token::String;
                t.text = s.substr(i + 1, e - i - 1);
                i = e + 1;
            } else {
                static const char* const two[] = { "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "&&", "||" };
                t.kind = Token::Punct;
                for (const char* p : two) {
                    if (s.compare(i, 2, p) == 0) {
                        t.text = p;
                    }
                }
                if (t.text.empty()) {
                    if (!std::strchr("+-*/%<>=!(){};,.", c)) {
                        fail(t, std::string("unexpected character '") + c + "'");
                    }
                    t.text = std::string(1, c);
                }
                i += t.text.size();
            }
            out.push_back(t);
        }
    }

    using ExprPtr = std::unique_ptr<Expr>;

    ExprPtr
    make(Op op, Type type)
    {
        auto e = std::make_unique<Expr>();
        e->op = op;
        e->type = type;
        return e;
    }

    ExprPtr
    make(Op op, Type type, ExprPtr a, ExprPtr b = nullptr, ExprPtr c = nullptr)
    {
        auto e = make(op, type);
        for (ExprPtr* p : { &a, &b, &c }) {
            if (*p) {
                e->args.push_back(std::move(*p));
            }
        }
        return e;
    }

    const char*
    typeName(Type t)
    {
        switch (t) {
        case Type::Int:
            return "int";
        case Type::Float:
            return "float";
        case Type::Vec:
            return "vector";
        }
        return "?";
    }

    class Parser {
    public:
        Parser(std::vector<Token> toks, Program& prog)
            : _t(std::move(toks))
            , _p(prog)
        {
        }

        void parseProgram()
        {
            while (cur().kind != Token::End) {
                statement(_p.body);
            }
        }

    private:
        std::vector<Token> _t;
        std::size_t _i = 0;
        Program& _p;
        std::map<std::string, int> _locals;

        const Token& cur() const { return _t[_i]; }
        bool isPunct(const char* p) const { return cur().kind == Token::Punct && cur().text == p; }
        bool isIdent(const char* p) const { return cur().kind == Token::Ident && cur().text == p; }

        Token expect(const char* p)
        {
            if (!isPunct(p)) {
                fail(cur(), std::string("expected '") + p + "'");
            }
            return _t[_i++];
        }

        ExprPtr convert(ExprPtr e, Type to, const Token& at)
        {
            if (e->type == to) {
                return e;
            }
            if (e->type == Type::Int && to == Type::Float) {
                if (e->op == Op::ConstI) {
                    auto c = make(Op::ConstF, Type::Float);
                    c->f = float(e->i);
                    return c;
                }
                return make(Op::I2F, Type::Float, std::move(e));
            }
            if (e->type == Type::Float && to == Type::Int) {
                return make(Op::F2I, Type::Int, std::move(e));
            }
            if (to == Type::Vec) {
                return make(Op::MakeVec, Type::Vec, convert(std::move(e), Type::Float, at));
            }
            fail(at, std::string("cannot convert ") + typeName(e->type) + " to " + typeName(to));
        }

        ExprPtr truthy(ExprPtr e, const Token& at)
        {
            if (e->type == Type::Vec) {
                fail(at, "a vector cannot be used as a condition");
            }
            if (e->type == Type::Float) {
                auto zero = make(Op::ConstF, Type::Float);
                return make(Op::Ne, Type::Int, std::move(e), std::move(zero));
            }
            return e;
        }

        ExprPtr arith(Op op, ExprPtr a, ExprPtr b, const Token& at)
        {
            if (op == Op::Mod) {
                if (a->type != Type::Int || b->type != Type::Int) {
                    fail(at, "'%' takes ints in this spike");
                }
                return make(op, Type::Int, std::move(a), std::move(b));
            }
            Type t = (a->type == Type::Vec || b->type == Type::Vec)  ? Type::Vec
                : (a->type == Type::Float || b->type == Type::Float) ? Type::Float
                                                                     : Type::Int;
            a = convert(std::move(a), t, at);
            b = convert(std::move(b), t, at);
            return make(op, t, std::move(a), std::move(b));
        }

        ExprPtr compare(Op op, ExprPtr a, ExprPtr b, const Token& at)
        {
            if (a->type == Type::Vec || b->type == Type::Vec) {
                fail(at, "vector comparison is not supported in this spike");
            }
            Type t = (a->type == Type::Float || b->type == Type::Float) ? Type::Float : Type::Int;
            a = convert(std::move(a), t, at);
            b = convert(std::move(b), t, at);
            return make(op, Type::Int, std::move(a), std::move(b));
        }

        // Binary levels, loosest first.
        ExprPtr expr() { return orExpr(); }

        ExprPtr orExpr()
        {
            ExprPtr a = andExpr();
            while (isPunct("||")) {
                Token at = _t[_i++];
                ExprPtr b = andExpr();
                a = make(Op::Or, Type::Int, truthy(std::move(a), at), truthy(std::move(b), at));
            }
            return a;
        }

        ExprPtr andExpr()
        {
            ExprPtr a = eqExpr();
            while (isPunct("&&")) {
                Token at = _t[_i++];
                ExprPtr b = eqExpr();
                a = make(Op::And, Type::Int, truthy(std::move(a), at), truthy(std::move(b), at));
            }
            return a;
        }

        ExprPtr eqExpr()
        {
            ExprPtr a = relExpr();
            while (isPunct("==") || isPunct("!=")) {
                Token at = _t[_i++];
                a = compare(at.text == "==" ? Op::Eq : Op::Ne, std::move(a), relExpr(), at);
            }
            return a;
        }

        ExprPtr relExpr()
        {
            ExprPtr a = addExpr();
            while (isPunct("<") || isPunct("<=") || isPunct(">") || isPunct(">=")) {
                Token at = _t[_i++];
                Op op = at.text == "<" ? Op::Lt : at.text == "<=" ? Op::Le
                    : at.text == ">"                              ? Op::Gt
                                                                  : Op::Ge;
                a = compare(op, std::move(a), addExpr(), at);
            }
            return a;
        }

        ExprPtr addExpr()
        {
            ExprPtr a = mulExpr();
            while (isPunct("+") || isPunct("-")) {
                Token at = _t[_i++];
                a = arith(at.text == "+" ? Op::Add : Op::Sub, std::move(a), mulExpr(), at);
            }
            return a;
        }

        ExprPtr mulExpr()
        {
            ExprPtr a = unary();
            while (isPunct("*") || isPunct("/") || isPunct("%")) {
                Token at = _t[_i++];
                Op op = at.text == "*" ? Op::Mul : at.text == "/" ? Op::Div
                                                                  : Op::Mod;
                a = arith(op, std::move(a), unary(), at);
            }
            return a;
        }

        ExprPtr unary()
        {
            if (isPunct("-")) {
                ++_i;
                ExprPtr a = unary();
                if (a->op == Op::ConstF) {
                    a->f = -a->f;
                    return a;
                }
                if (a->op == Op::ConstI) {
                    a->i = -a->i;
                    return a;
                }
                Type t = a->type;
                return make(Op::Neg, t, std::move(a));
            }
            if (isPunct("!")) {
                Token at = _t[_i++];
                return make(Op::Not, Type::Int, truthy(unary(), at));
            }
            return postfix();
        }

        int component(const Token& at)
        {
            if (cur().kind != Token::Ident || cur().text.size() != 1 || !std::strchr("xyzrgb", cur().text[0])) {
                fail(at, "expected a component x, y or z");
            }
            const char c = _t[_i++].text[0];
            return (c == 'x' || c == 'r') ? 0 : (c == 'y' || c == 'g') ? 1
                                                                       : 2;
        }

        ExprPtr postfix()
        {
            ExprPtr a = primary();
            while (isPunct(".")) {
                Token at = _t[_i++];
                if (a->type != Type::Vec) {
                    fail(at, "component access on a non-vector");
                }
                auto c = make(Op::Comp, Type::Float, std::move(a));
                c->index = component(at);
                a = std::move(c);
            }
            return a;
        }

        void bindAttr(const Token& at, Type& type, int& attr, Op& op)
        {
            static const std::map<std::string, std::pair<Op, Type>> builtins = {
                { "ptnum", { Op::PtNum, Type::Int } },
                { "numpt", { Op::NumPt, Type::Int } },
                { "Time", { Op::Time, Type::Float } },
            };
            static const std::map<std::string, Attr> vectors = { { "P", Attr::P }, { "N", Attr::N }, { "Cd", Attr::Cd } };
            if (auto b = builtins.find(at.text); b != builtins.end()) {
                op = b->second.first;
                type = b->second.second;
                attr = -1;
                return;
            }
            auto v = vectors.find(at.text);
            if (v == vectors.end()) {
                fail(at, "unknown attribute '@" + at.text + "' (this spike binds P, N, Cd, ptnum, numpt, Time)");
            }
            if (at.prefix && at.prefix != 'v' && at.prefix != 'p') {
                fail(at, "'@" + at.text + "' is a vector attribute");
            }
            op = Op::AttrRead;
            type = Type::Vec;
            attr = int(v->second);
        }

        ExprPtr call(const Token& name)
        {
            expect("(");
            std::vector<ExprPtr> args;
            std::vector<Token> where;
            if (!isPunct(")")) {
                while (true) {
                    where.push_back(cur());
                    if (cur().kind == Token::String) {
                        auto s = make(Op::ConstI, Type::Int);
                        s->index = -1;
                        s->i = cur().text == "P" ? 0 : -1;
                        ++_i;
                        args.push_back(std::move(s));
                    } else {
                        args.push_back(expr());
                    }
                    if (!isPunct(",")) {
                        break;
                    }
                    ++_i;
                }
            }
            expect(")");
            auto arity = [&](std::size_t n) {
                if (args.size() != n) {
                    fail(name, "'" + name.text + "' takes " + std::to_string(n) + " argument(s)");
                }
            };
            auto f1 = [&](Op op) {
                arity(1);
                return make(op, Type::Float, convert(std::move(args[0]), Type::Float, where[0]));
            };
            auto inputOne = [&]() {
                if (args[0]->op != Op::ConstI || args[0]->i != 1) {
                    fail(where[0], "this spike only reads input 1");
                }
                _p.usesInput1 = true;
            };
            const std::string& n = name.text;
            if (n == "sin")
                return f1(Op::Sin);
            if (n == "cos")
                return f1(Op::Cos);
            if (n == "sqrt")
                return f1(Op::Sqrt);
            if (n == "abs")
                return f1(Op::Abs);
            if (n == "floor")
                return f1(Op::Floor);
            if (n == "min" || n == "max") {
                arity(2);
                return make(n == "min" ? Op::Min : Op::Max, Type::Float, convert(std::move(args[0]), Type::Float, where[0]),
                            convert(std::move(args[1]), Type::Float, where[1]));
            }
            if (n == "length") {
                arity(1);
                return make(Op::Length, Type::Float, convert(std::move(args[0]), Type::Vec, where[0]));
            }
            if (n == "dot") {
                arity(2);
                return make(Op::Dot, Type::Float, convert(std::move(args[0]), Type::Vec, where[0]),
                            convert(std::move(args[1]), Type::Vec, where[1]));
            }
            if (n == "set") {
                arity(3);
                return make(Op::MakeVec, Type::Vec, convert(std::move(args[0]), Type::Float, where[0]),
                            convert(std::move(args[1]), Type::Float, where[1]), convert(std::move(args[2]), Type::Float, where[2]));
            }
            if (n == "noise") {
                arity(1);
                if (args[0]->type != Type::Vec) {
                    fail(where[0], "noise() takes a vector in this spike");
                }
                return make(Op::Noise, Type::Float, std::move(args[0]));
            }
            if (n == "nearpoint") {
                arity(2);
                inputOne();
                return make(Op::NearPoint, Type::Int, convert(std::move(args[1]), Type::Vec, where[1]));
            }
            if (n == "point") {
                arity(3);
                inputOne();
                if (args[1]->op != Op::ConstI || args[1]->index != -1 || args[1]->i != 0) {
                    fail(where[1], "point() reads \"P\" only in this spike");
                }
                return make(Op::PointP, Type::Vec, convert(std::move(args[2]), Type::Int, where[2]));
            }
            fail(name, "unsupported function '" + n + "'");
        }

        ExprPtr primary()
        {
            const Token t = cur();
            switch (t.kind) {
            case Token::Int: {
                ++_i;
                auto e = make(Op::ConstI, Type::Int);
                e->i = int(t.ival);
                return e;
            }
            case Token::Float: {
                ++_i;
                auto e = make(Op::ConstF, Type::Float);
                e->f = float(t.fval);
                return e;
            }
            case Token::AttrRef: {
                ++_i;
                Type type;
                int attr;
                Op op;
                bindAttr(t, type, attr, op);
                auto e = make(op, type);
                e->index = attr;
                if (op == Op::AttrRead) {
                    _p.attrRead[attr] = true;
                }
                return e;
            }
            case Token::Ident: {
                ++_i;
                if (isPunct("(")) {
                    return call(t);
                }
                auto l = _locals.find(t.text);
                if (l == _locals.end()) {
                    fail(t, "unknown variable '" + t.text + "'");
                }
                auto e = make(Op::Local, _p.locals[l->second]);
                e->index = l->second;
                return e;
            }
            case Token::Punct:
                if (t.text == "(") {
                    ++_i;
                    ExprPtr e = expr();
                    expect(")");
                    return e;
                }
                break;
            default:
                break;
            }
            fail(t, "expected an expression");
        }

        void block(std::vector<Stmt>& out)
        {
            if (isPunct("{")) {
                ++_i;
                while (!isPunct("}")) {
                    if (cur().kind == Token::End) {
                        fail(cur(), "expected '}'");
                    }
                    statement(out);
                }
                ++_i;
            } else {
                statement(out);
            }
        }

        void assign(std::vector<Stmt>& out, bool toAttr, int target, int comp, Type targetType, const Token& op, ExprPtr rhs)
        {
            Stmt s;
            s.toAttr = toAttr;
            s.target = target;
            s.comp = comp;
            if (op.text != "=") {
                ExprPtr lhs;
                if (toAttr) {
                    lhs = make(Op::AttrRead, Type::Vec);
                    _p.attrRead[target] = true;
                } else {
                    lhs = make(Op::Local, _p.locals[target]);
                }
                lhs->index = target;
                if (comp >= 0) {
                    auto c = make(Op::Comp, Type::Float, std::move(lhs));
                    c->index = comp;
                    lhs = std::move(c);
                }
                const char k = op.text[0];
                Op bop = k == '+' ? Op::Add : k == '-' ? Op::Sub
                    : k == '*'                         ? Op::Mul
                                                       : Op::Div;
                rhs = arith(bop, std::move(lhs), std::move(rhs), op);
            }
            s.value = convert(std::move(rhs), targetType, op);
            if (toAttr) {
                _p.attrWritten[target] = true;
            }
            out.push_back(std::move(s));
        }

        void statement(std::vector<Stmt>& out)
        {
            const Token t = cur();
            if (isPunct(";")) {
                ++_i;
                return;
            }
            if (isPunct("{")) {
                block(out);
                return;
            }
            if (isIdent("if")) {
                ++_i;
                expect("(");
                Stmt s;
                s.kind = Stmt::If;
                s.cond = truthy(expr(), t);
                expect(")");
                block(s.thenBody);
                if (isIdent("else")) {
                    ++_i;
                    block(s.elseBody);
                }
                out.push_back(std::move(s));
                return;
            }
            if (isIdent("int") || isIdent("float") || isIdent("vector")) {
                const Type type = t.text == "int" ? Type::Int : t.text == "float" ? Type::Float
                                                                                  : Type::Vec;
                ++_i;
                const Token name = cur();
                if (name.kind != Token::Ident) {
                    fail(name, "expected a variable name");
                }
                if (_locals.count(name.text)) {
                    fail(name, "'" + name.text + "' is already declared");
                }
                ++_i;
                ExprPtr init;
                Token op = name;
                op.text = "=";
                if (isPunct("=")) {
                    op = _t[_i++];
                    init = expr();
                } else {
                    init = make(Op::ConstI, Type::Int);
                }
                expect(";");
                const int slot = int(_p.locals.size());
                _p.locals.push_back(type);
                _locals[name.text] = slot;
                assign(out, false, slot, -1, type, op, std::move(init));
                return;
            }
            bool toAttr = false;
            int target = 0;
            Type type;
            if (t.kind == Token::AttrRef) {
                Op op;
                bindAttr(t, type, target, op);
                if (op != Op::AttrRead) {
                    fail(t, "'@" + t.text + "' is read-only");
                }
                toAttr = true;
            } else if (t.kind == Token::Ident) {
                auto l = _locals.find(t.text);
                if (l == _locals.end()) {
                    fail(t, "unknown variable '" + t.text + "'");
                }
                target = l->second;
                type = _p.locals[target];
            } else {
                fail(t, "expected a statement");
            }
            ++_i;
            int comp = -1;
            if (isPunct(".")) {
                Token at = _t[_i++];
                if (type != Type::Vec) {
                    fail(at, "component access on a non-vector");
                }
                comp = component(at);
                type = Type::Float;
            }
            const Token op = cur();
            if (!(isPunct("=") || isPunct("+=") || isPunct("-=") || isPunct("*=") || isPunct("/="))) {
                fail(op, "expected an assignment");
            }
            ++_i;
            ExprPtr rhs = expr();
            expect(";");
            assign(out, toAttr, target, comp, type, op, std::move(rhs));
        }
    };

} // namespace

bool
compileSource(const std::string& source, Program& out, CompileError& err)
{
    out = Program {};
    try {
        Parser p(lex(source), out);
        p.parseProgram();
    } catch (const Failure& f) {
        err = f.err;
        return false;
    }
    return true;
}

} // namespace gk
