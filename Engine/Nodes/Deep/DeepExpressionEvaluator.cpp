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

#include "DeepExpressionEvaluator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace {
struct BuiltinFunction {
    const char* name;
    int arity;
    int op;
};

const BuiltinFunction kFunctions[] = {
    { "abs", 1, DeepExpressionEvaluator::eOpAbs },
    { "floor", 1, DeepExpressionEvaluator::eOpFloor },
    { "ceil", 1, DeepExpressionEvaluator::eOpCeil },
    { "round", 1, DeepExpressionEvaluator::eOpRound },
    { "sqrt", 1, DeepExpressionEvaluator::eOpSqrt },
    { "exp", 1, DeepExpressionEvaluator::eOpExp },
    { "log", 1, DeepExpressionEvaluator::eOpLog },
    { "sin", 1, DeepExpressionEvaluator::eOpSin },
    { "cos", 1, DeepExpressionEvaluator::eOpCos },
    { "tan", 1, DeepExpressionEvaluator::eOpTan },
    { "pow", 2, DeepExpressionEvaluator::eOpPower },
    { "min", 2, DeepExpressionEvaluator::eOpMin },
    { "max", 2, DeepExpressionEvaluator::eOpMax },
    { "step", 2, DeepExpressionEvaluator::eOpStep },
    { "atan2", 2, DeepExpressionEvaluator::eOpAtan2 },
    { "clamp", 3, DeepExpressionEvaluator::eOpClamp },
    { "lerp", 3, DeepExpressionEvaluator::eOpLerp },
    { "smoothstep", 3, DeepExpressionEvaluator::eOpSmoothstep },
};

struct BuiltinValue {
    const char* name;
    int op;
};

const BuiltinValue kValues[] = {
    { "x", DeepExpressionEvaluator::eOpX },
    { "y", DeepExpressionEvaluator::eOpY },
    { "sampleIndex", DeepExpressionEvaluator::eOpSampleIndex },
    { "sampleCount", DeepExpressionEvaluator::eOpSampleCount },
    { "frame", DeepExpressionEvaluator::eOpFrame },
    { "pi", DeepExpressionEvaluator::eOpConstant },
    { "Z", DeepExpressionEvaluator::eOpZ },
    { "ZBack", DeepExpressionEvaluator::eOpZBack },
};

bool
isIdentifierStart(char c)
{
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_');
}

bool
isDigit(char c)
{
    return (c >= '0') && (c <= '9');
}

bool
isIdentifierChar(char c)
{
    return isIdentifierStart(c) || isDigit(c) || (c == '.');
}

float
truth(bool b)
{
    return b ? 1.f : 0.f;
}
} // anonymous namespace

class DeepExpressionEvaluator::Parser {
public:
    Parser(const std::string& source,
           const std::vector<std::string>& channelNames,
           std::vector<Op>* ops,
           CompileError* error)
        : _source(source)
        , _channelNames(channelNames)
        , _ops(ops)
        , _error(error)
        , _cursor(0)
        , _token()
        , _depth(0)
        , _maxDepth(0)
        , _nesting(0)
    {
    }

    bool parse()
    {
        if (!next()) {
            return false;
        }
        if (_token.kind == eKindEnd) {
            return fail(_token.column, "empty expression");
        }
        if (!parseTernary()) {
            return false;
        }
        if (_token.kind != eKindEnd) {
            return fail(_token.column, "unexpected '" + _token.text + "'");
        }

        return true;
    }

    int getMaxDepth() const
    {
        return _maxDepth;
    }

private:
    enum KindEnum {
        eKindNumber,
        eKindIdentifier,
        eKindOperator,
        eKindEnd
    };

    struct Token {
        KindEnum kind;
        std::string text;
        float value;
        int column;

        Token()
            : kind(eKindEnd)
            , text()
            , value(0.f)
            , column(0)
        {
        }
    };

    bool fail(int column,
              const std::string& message)
    {
        _error->column = column;
        _error->message = message;

        return false;
    }

    bool next()
    {
        while ((_cursor < _source.size()) && std::isspace((unsigned char)_source[_cursor])) {
            ++_cursor;
        }
        _token = Token();
        _token.column = (int)_cursor + 1;
        if (_cursor >= _source.size()) {
            _token.kind = eKindEnd;

            return true;
        }

        const char c = _source[_cursor];
        if (isDigit(c) || ((c == '.') && (_cursor + 1 < _source.size()) && isDigit(_source[_cursor + 1]))) {
            std::size_t end = _cursor;
            while ((end < _source.size()) && (isDigit(_source[end]) || (_source[end] == '.'))) {
                ++end;
            }
            if ((end < _source.size()) && ((_source[end] == 'e') || (_source[end] == 'E'))) {
                std::size_t exponent = end + 1;
                if ((exponent < _source.size()) && ((_source[exponent] == '+') || (_source[exponent] == '-'))) {
                    ++exponent;
                }
                if ((exponent < _source.size()) && isDigit(_source[exponent])) {
                    end = exponent;
                    while ((end < _source.size()) && isDigit(_source[end])) {
                        ++end;
                    }
                }
            }
            _token.kind = eKindNumber;
            _token.text = _source.substr(_cursor, end - _cursor);
            char* parsedEnd = nullptr;
            _token.value = (float)std::strtod(_token.text.c_str(), &parsedEnd);
            if (!parsedEnd || (*parsedEnd != '\0')) {
                return fail(_token.column, "malformed number '" + _token.text + "'");
            }
            _cursor = end;

            return true;
        }
        if (isIdentifierStart(c)) {
            std::size_t end = _cursor;
            while ((end < _source.size()) && isIdentifierChar(_source[end])) {
                ++end;
            }
            _token.kind = eKindIdentifier;
            _token.text = _source.substr(_cursor, end - _cursor);
            _cursor = end;

            return true;
        }

        static const char* const twoCharOperators[] = { "<=", ">=", "==", "!=", "&&", "||" };
        for (std::size_t i = 0; i < sizeof(twoCharOperators) / sizeof(twoCharOperators[0]); ++i) {
            if (_source.compare(_cursor, 2, twoCharOperators[i]) == 0) {
                _token.kind = eKindOperator;
                _token.text = twoCharOperators[i];
                _cursor += 2;

                return true;
            }
        }
        if (std::strchr("+-*/%^<>!?:(),", c)) {
            _token.kind = eKindOperator;
            _token.text = std::string(1, c);
            ++_cursor;

            return true;
        }

        return fail(_token.column, std::string("unexpected character '") + c + "'");
    }

    bool isOperator(const char* text) const
    {
        return (_token.kind == eKindOperator) && (_token.text == text);
    }

    // Consumes the current token if it is the operator text.
    bool accept(const char* text,
                bool* ok)
    {
        if (!isOperator(text)) {
            return false;
        }
        *ok = next();

        return true;
    }

    bool expect(const char* text)
    {
        if (!isOperator(text)) {
            return fail(_token.column, (_token.kind == eKindEnd) ? std::string("expected '") + text + "' before the end of the expression" : std::string("expected '") + text + "' but found '" + _token.text + "'");
        }

        return next();
    }

    static int arity(OpEnum op)
    {
        switch (op) {
        case eOpConstant:
        case eOpChannel:
        case eOpZ:
        case eOpZBack:
        case eOpX:
        case eOpY:
        case eOpSampleIndex:
        case eOpSampleCount:
        case eOpFrame:

            return 0;
        case eOpNegate:
        case eOpNot:
        case eOpAbs:
        case eOpFloor:
        case eOpCeil:
        case eOpRound:
        case eOpSqrt:
        case eOpExp:
        case eOpLog:
        case eOpSin:
        case eOpCos:
        case eOpTan:

            return 1;
        case eOpSelect:
        case eOpClamp:
        case eOpLerp:
        case eOpSmoothstep:

            return 3;
        default:

            return 2;
        }
    }

    bool emit(OpEnum op,
              int column,
              int index = 0,
              float value = 0.f)
    {
        Op o;

        o.op = op;
        o.index = index;
        o.value = value;
        _ops->push_back(o);
        _depth = _depth - arity(op) + 1;
        _maxDepth = std::max(_maxDepth, _depth);
        if (_depth > kMaxStackDepth) {
            return fail(column, "expression too deep");
        }

        return true;
    }

    bool enterNesting(int column)
    {
        if (++_nesting > kMaxStackDepth) {
            return fail(column, "expression nested too deeply");
        }

        return true;
    }

    void leaveNesting()
    {
        --_nesting;
    }

    bool parseTernary()
    {
        if (!parseOr()) {
            return false;
        }
        bool ok = true;
        const int column = _token.column;
        if (!accept("?", &ok)) {
            return true;
        }
        if (!ok || !enterNesting(column) || !parseTernary() || !expect(":") || !parseTernary()) {
            return false;
        }
        leaveNesting();

        return emit(eOpSelect, column);
    }

    bool parseOr()
    {
        if (!parseAnd()) {
            return false;
        }
        for (;;) {
            bool ok = true;
            const int column = _token.column;
            if (!accept("||", &ok)) {
                return true;
            }
            if (!ok || !parseAnd() || !emit(eOpOr, column)) {
                return false;
            }
        }
    }

    bool parseAnd()
    {
        if (!parseEquality()) {
            return false;
        }
        for (;;) {
            bool ok = true;
            const int column = _token.column;
            if (!accept("&&", &ok)) {
                return true;
            }
            if (!ok || !parseEquality() || !emit(eOpAnd, column)) {
                return false;
            }
        }
    }

    bool parseEquality()
    {
        if (!parseRelational()) {
            return false;
        }
        for (;;) {
            bool ok = true;
            const int column = _token.column;
            OpEnum op;
            if (accept("==", &ok)) {
                op = eOpEqual;
            } else if (accept("!=", &ok)) {
                op = eOpNotEqual;
            } else {
                return true;
            }
            if (!ok || !parseRelational() || !emit(op, column)) {
                return false;
            }
        }
    }

    bool parseRelational()
    {
        if (!parseAdditive()) {
            return false;
        }
        for (;;) {
            bool ok = true;
            const int column = _token.column;
            OpEnum op;
            if (accept("<=", &ok)) {
                op = eOpLessEqual;
            } else if (accept(">=", &ok)) {
                op = eOpGreaterEqual;
            } else if (accept("<", &ok)) {
                op = eOpLess;
            } else if (accept(">", &ok)) {
                op = eOpGreater;
            } else {
                return true;
            }
            if (!ok || !parseAdditive() || !emit(op, column)) {
                return false;
            }
        }
    }

    bool parseAdditive()
    {
        if (!parseMultiplicative()) {
            return false;
        }
        for (;;) {
            bool ok = true;
            const int column = _token.column;
            OpEnum op;
            if (accept("+", &ok)) {
                op = eOpAdd;
            } else if (accept("-", &ok)) {
                op = eOpSubtract;
            } else {
                return true;
            }
            if (!ok || !parseMultiplicative() || !emit(op, column)) {
                return false;
            }
        }
    }

    bool parseMultiplicative()
    {
        if (!parseUnary()) {
            return false;
        }
        for (;;) {
            bool ok = true;
            const int column = _token.column;
            OpEnum op;
            if (accept("*", &ok)) {
                op = eOpMultiply;
            } else if (accept("/", &ok)) {
                op = eOpDivide;
            } else if (accept("%", &ok)) {
                op = eOpModulo;
            } else {
                return true;
            }
            if (!ok || !parseUnary() || !emit(op, column)) {
                return false;
            }
        }
    }

    bool parseUnary()
    {
        bool ok = true;
        const int column = _token.column;
        OpEnum op;

        if (accept("-", &ok)) {
            op = eOpNegate;
        } else if (accept("!", &ok)) {
            op = eOpNot;
        } else {
            return parsePower();
        }
        if (!ok || !enterNesting(column) || !parseUnary()) {
            return false;
        }
        leaveNesting();

        return emit(op, column);
    }

    bool parsePower()
    {
        if (!parsePrimary()) {
            return false;
        }
        bool ok = true;
        const int column = _token.column;
        if (!accept("^", &ok)) {
            return true;
        }

        return ok && parseUnary() && emit(eOpPower, column);
    }

    bool parsePrimary()
    {
        const Token token = _token;
        bool ok = true;

        if (token.kind == eKindNumber) {
            return next() && emit(eOpConstant, token.column, 0, token.value);
        }
        if (token.kind == eKindIdentifier) {
            if (!next()) {
                return false;
            }
            if (accept("(", &ok)) {
                return ok && parseCall(token);
            }

            return parseIdentifier(token);
        }
        if (accept("(", &ok)) {
            if (!ok || !enterNesting(token.column) || !parseTernary() || !expect(")")) {
                return false;
            }
            leaveNesting();

            return true;
        }
        if (token.kind == eKindEnd) {
            return fail(token.column, "unexpected end of expression");
        }

        return fail(token.column, "unexpected '" + token.text + "'");
    }

    bool parseIdentifier(const Token& token)
    {
        for (std::size_t i = 0; i < sizeof(kValues) / sizeof(kValues[0]); ++i) {
            if (token.text == kValues[i].name) {
                return emit((OpEnum)kValues[i].op, token.column, 0, (float)M_PI);
            }
        }
        const std::vector<std::string>::const_iterator found = std::find(_channelNames.begin(), _channelNames.end(), token.text);
        if (found != _channelNames.end()) {
            return emit(eOpChannel, token.column, (int)(found - _channelNames.begin()));
        }

        return fail(token.column, "unknown identifier '" + token.text + "'");
    }

    // Parses the arguments of a call to name, the opening parenthesis already consumed.
    bool parseCall(const Token& name)
    {
        const BuiltinFunction* function = nullptr;

        for (std::size_t i = 0; i < sizeof(kFunctions) / sizeof(kFunctions[0]); ++i) {
            if (name.text == kFunctions[i].name) {
                function = &kFunctions[i];
                break;
            }
        }
        if (!function) {
            return fail(name.column, "unknown function '" + name.text + "'");
        }
        if (!enterNesting(name.column)) {
            return false;
        }

        int count = 0;
        bool ok = true;
        if (!accept(")", &ok)) {
            for (;;) {
                if (!parseTernary()) {
                    return false;
                }
                ++count;
                if (accept(",", &ok)) {
                    if (!ok) {
                        return false;
                    }
                    continue;
                }
                if (!expect(")")) {
                    return false;
                }
                break;
            }
        }
        if (!ok) {
            return false;
        }
        if (count != function->arity) {
            return fail(name.column, "'" + name.text + "' takes " + std::to_string(function->arity) + (function->arity == 1 ? " argument" : " arguments") + ", not " + std::to_string(count));
        }
        leaveNesting();

        return emit((OpEnum)function->op, name.column);
    }

    const std::string& _source;
    const std::vector<std::string>& _channelNames;
    std::vector<Op>* _ops;
    CompileError* _error;
    std::size_t _cursor;
    Token _token;
    int _depth;
    int _maxDepth;
    int _nesting;
};

DeepExpressionEvaluator::DeepExpressionEvaluator()
    : _ops()
    , _stackDepth(0)
{
}

bool
DeepExpressionEvaluator::compile(const std::string& source,
                                 const std::vector<std::string>& channelNames,
                                 CompileError* error)
{
    CompileError localError;

    _ops.clear();
    _stackDepth = 0;

    Parser parser(source, channelNames, &_ops, error ? error : &localError);
    if (!parser.parse()) {
        _ops.clear();

        return false;
    }
    _stackDepth = parser.getMaxDepth();

    return true;
}

float
DeepExpressionEvaluator::evaluate(const DeepPixelView& pixel,
                                  int sampleIndex,
                                  int x,
                                  int y,
                                  float frame) const
{
    float stack[kMaxStackDepth];
    int sp = 0;

    for (std::vector<Op>::const_iterator it = _ops.begin(); it != _ops.end(); ++it) {
        float* const top = stack + sp;
        switch (it->op) {
        case eOpConstant:
            stack[sp++] = it->value;
            break;
        case eOpChannel:
            stack[sp++] = pixel.channels[it->index][sampleIndex];
            break;
        case eOpZ:
            stack[sp++] = pixel.z[sampleIndex];
            break;
        case eOpZBack:
            stack[sp++] = pixel.zbackAt(sampleIndex);
            break;
        case eOpX:
            stack[sp++] = (float)x;
            break;
        case eOpY:
            stack[sp++] = (float)y;
            break;
        case eOpSampleIndex:
            stack[sp++] = (float)sampleIndex;
            break;
        case eOpSampleCount:
            stack[sp++] = (float)pixel.numSamples;
            break;
        case eOpFrame:
            stack[sp++] = frame;
            break;
        case eOpNegate:
            top[-1] = -top[-1];
            break;
        case eOpNot:
            top[-1] = truth(top[-1] == 0.f);
            break;
        case eOpAbs:
            top[-1] = std::fabs(top[-1]);
            break;
        case eOpFloor:
            top[-1] = std::floor(top[-1]);
            break;
        case eOpCeil:
            top[-1] = std::ceil(top[-1]);
            break;
        case eOpRound:
            top[-1] = std::round(top[-1]);
            break;
        case eOpSqrt:
            top[-1] = std::sqrt(top[-1]);
            break;
        case eOpExp:
            top[-1] = std::exp(top[-1]);
            break;
        case eOpLog:
            top[-1] = std::log(top[-1]);
            break;
        case eOpSin:
            top[-1] = std::sin(top[-1]);
            break;
        case eOpCos:
            top[-1] = std::cos(top[-1]);
            break;
        case eOpTan:
            top[-1] = std::tan(top[-1]);
            break;
        case eOpAdd:
            top[-2] = top[-2] + top[-1];
            --sp;
            break;
        case eOpSubtract:
            top[-2] = top[-2] - top[-1];
            --sp;
            break;
        case eOpMultiply:
            top[-2] = top[-2] * top[-1];
            --sp;
            break;
        case eOpDivide:
            top[-2] = top[-2] / top[-1];
            --sp;
            break;
        case eOpModulo:
            top[-2] = std::fmod(top[-2], top[-1]);
            --sp;
            break;
        case eOpPower:
            top[-2] = std::pow(top[-2], top[-1]);
            --sp;
            break;
        case eOpLess:
            top[-2] = truth(top[-2] < top[-1]);
            --sp;
            break;
        case eOpLessEqual:
            top[-2] = truth(top[-2] <= top[-1]);
            --sp;
            break;
        case eOpGreater:
            top[-2] = truth(top[-2] > top[-1]);
            --sp;
            break;
        case eOpGreaterEqual:
            top[-2] = truth(top[-2] >= top[-1]);
            --sp;
            break;
        case eOpEqual:
            top[-2] = truth(top[-2] == top[-1]);
            --sp;
            break;
        case eOpNotEqual:
            top[-2] = truth(top[-2] != top[-1]);
            --sp;
            break;
        case eOpAnd:
            top[-2] = truth((top[-2] != 0.f) && (top[-1] != 0.f));
            --sp;
            break;
        case eOpOr:
            top[-2] = truth((top[-2] != 0.f) || (top[-1] != 0.f));
            --sp;
            break;
        case eOpMin:
            top[-2] = std::min(top[-2], top[-1]);
            --sp;
            break;
        case eOpMax:
            top[-2] = std::max(top[-2], top[-1]);
            --sp;
            break;
        case eOpStep:
            top[-2] = truth(top[-1] >= top[-2]);
            --sp;
            break;
        case eOpAtan2:
            top[-2] = std::atan2(top[-2], top[-1]);
            --sp;
            break;
        case eOpSelect:
            top[-3] = (top[-3] != 0.f) ? top[-2] : top[-1];
            sp -= 2;
            break;
        case eOpClamp:
            top[-3] = std::min(std::max(top[-3], top[-2]), top[-1]);
            sp -= 2;
            break;
        case eOpLerp:
            top[-3] = top[-3] + (top[-2] - top[-3]) * top[-1];
            sp -= 2;
            break;
        case eOpSmoothstep: {
            const float t = std::min(std::max((top[-1] - top[-3]) / (top[-2] - top[-3]), 0.f), 1.f);
            top[-3] = t * t * (3.f - 2.f * t);
            sp -= 2;
            break;
        }
        }
    }

    return (sp > 0) ? stack[sp - 1] : 0.f;
} // DeepExpressionEvaluator::evaluate

NATRON_NAMESPACE_EXIT
