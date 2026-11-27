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

#include "KernelLexer.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace {

bool
isIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool
isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool
isDigit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

class Lexer {
public:
    Lexer(std::string_view source, std::vector<KernelToken>& tokens, KernelCompileError& error)
        : _s(source)
        , _out(tokens)
        , _error(error)
    {
    }

    bool run()
    {
        _out.clear();
        while (true) {
            if (!skipSpaceAndComments()) {
                return false;
            }
            KernelToken t;
            t.line = _line;
            t.column = column();
            if (_i >= _s.size()) {
                _out.push_back(t);
                return true;
            }
            if (!token(t)) {
                return false;
            }
            _out.push_back(std::move(t));
        }
    }

private:
    std::string_view _s;
    std::vector<KernelToken>& _out;
    KernelCompileError& _error;
    std::size_t _i = 0;
    int _line = 1;
    std::size_t _lineStart = 0;

    int column() const { return static_cast<int>(_i - _lineStart) + 1; }
    char at(std::size_t k) const { return k < _s.size() ? _s[k] : '\0'; }

    bool fail(int line, int col, std::string message)
    {
        _error = KernelCompileError { line, col, std::move(message) };
        return false;
    }

    void newline(std::size_t nextLineStart)
    {
        ++_line;
        _lineStart = nextLineStart;
    }

    bool skipSpaceAndComments()
    {
        while (_i < _s.size()) {
            const char c = _s[_i];
            if (c == '\n') {
                newline(++_i);
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++_i;
            } else if (c == '/' && at(_i + 1) == '/') {
                while (_i < _s.size() && _s[_i] != '\n') {
                    ++_i;
                }
            } else if (c == '/' && at(_i + 1) == '*') {
                const int line = _line;
                const int col = column();
                _i += 2;
                while (_i < _s.size() && !(_s[_i] == '*' && at(_i + 1) == '/')) {
                    if (_s[_i] == '\n') {
                        newline(_i + 1);
                    }
                    ++_i;
                }
                if (_i >= _s.size()) {
                    return fail(line, col, "unterminated comment");
                }
                _i += 2;
            } else {
                break;
            }
        }
        return true;
    }

    // A type prefix before '@': one of f i v p s u 2 3 4 d, optionally followed by "[]".
    std::size_t prefixLength() const
    {
        if (!std::strchr("fivpsu234d", _s[_i])) {
            return 0;
        }
        if (_i > 0 && isIdentChar(_s[_i - 1])) {
            return 0;
        }
        if (at(_i + 1) == '@') {
            return 1;
        }
        if (at(_i + 1) == '[' && at(_i + 2) == ']' && at(_i + 3) == '@') {
            return 3;
        }
        return 0;
    }

    bool token(KernelToken& t)
    {
        const char c = _s[_i];
        const std::size_t prefix = prefixLength();
        if (c == '@' || prefix) {
            t.kind = KernelToken::Kind::eBinding;
            t.prefix = std::string(_s.substr(_i, prefix));
            _i += prefix + 1;
            const std::size_t b = _i;
            while (_i < _s.size() && isIdentChar(_s[_i])) {
                ++_i;
            }
            if (b == _i) {
                return fail(t.line, t.column, "expected a name after '@'");
            }
            t.text = std::string(_s.substr(b, _i - b));
            return true;
        }
        if (isIdentStart(c)) {
            const std::size_t b = _i;
            while (_i < _s.size() && isIdentChar(_s[_i])) {
                ++_i;
            }
            t.kind = KernelToken::Kind::eIdentifier;
            t.text = std::string(_s.substr(b, _i - b));
            return true;
        }
        if (isDigit(c) || (c == '.' && isDigit(at(_i + 1)))) {
            return number(t);
        }
        if (c == '"' || c == '\'') {
            return string(t, c);
        }
        return punct(t);
    }

    bool number(KernelToken& t)
    {
        const std::size_t b = _i;
        if (_s[_i] == '0' && (at(_i + 1) == 'x' || at(_i + 1) == 'X')) {
            _i += 2;
            while (_i < _s.size() && std::isxdigit(static_cast<unsigned char>(_s[_i]))) {
                ++_i;
            }
            t.text = std::string(_s.substr(b, _i - b));
            if (t.text.size() == 2) {
                return fail(t.line, t.column, "expected hexadecimal digits after '0x'");
            }
            t.kind = KernelToken::Kind::eInt;
            errno = 0;
            const unsigned long long bits = std::strtoull(t.text.c_str() + 2, nullptr, 16);
            if (errno == ERANGE || bits > UINT32_MAX) {
                return fail(t.line, t.column, "integer literal " + t.text + " is out of range");
            }
            t.intValue = static_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
            return true;
        }
        bool isFloat = false;
        while (_i < _s.size() && isDigit(_s[_i])) {
            ++_i;
        }
        if (at(_i) == '.') {
            isFloat = true;
            ++_i;
            while (_i < _s.size() && isDigit(_s[_i])) {
                ++_i;
            }
        }
        if (at(_i) == 'e' || at(_i) == 'E') {
            std::size_t k = _i + 1;
            if (at(k) == '+' || at(k) == '-') {
                ++k;
            }
            if (!isDigit(at(k))) {
                return fail(_line, column(), "expected an exponent");
            }
            isFloat = true;
            _i = k;
            while (_i < _s.size() && isDigit(_s[_i])) {
                ++_i;
            }
        }
        t.text = std::string(_s.substr(b, _i - b));
        if (at(_i) == 'f' || at(_i) == 'F') {
            isFloat = true;
            ++_i;
        }
        if (_i < _s.size() && isIdentChar(_s[_i])) {
            return fail(t.line, t.column, "malformed number '" + t.text + std::string(1, _s[_i]) + "'");
        }
        if (isFloat) {
            t.kind = KernelToken::Kind::eFloat;
            t.floatValue = std::strtod(t.text.c_str(), nullptr);
        } else {
            t.kind = KernelToken::Kind::eInt;
            errno = 0;
            t.intValue = std::strtoll(t.text.c_str(), nullptr, 10);
            // INT32_MAX + 1 is let through for a leading '-'; the parser rejects it otherwise.
            if (errno == ERANGE || t.intValue > std::int64_t(INT32_MAX) + 1) {
                return fail(t.line, t.column, "integer literal " + t.text + " is out of range");
            }
        }
        return true;
    }

    bool string(KernelToken& t, char quote)
    {
        ++_i;
        t.kind = KernelToken::Kind::eString;
        while (true) {
            if (_i >= _s.size() || _s[_i] == '\n') {
                return fail(t.line, t.column, "unterminated string");
            }
            const char c = _s[_i++];
            if (c == quote) {
                return true;
            }
            if (c != '\\') {
                t.text += c;
                continue;
            }
            if (_i >= _s.size()) {
                return fail(t.line, t.column, "unterminated string");
            }
            const char e = _s[_i++];
            switch (e) {
            case 'n':
                t.text += '\n';
                break;
            case 't':
                t.text += '\t';
                break;
            case 'r':
                t.text += '\r';
                break;
            case '0':
                t.text += '\0';
                break;
            case '\\':
            case '"':
            case '\'':
                t.text += e;
                break;
            default:
                return fail(_line, column() - 2, std::string("unknown escape sequence '\\") + e + "'");
            }
        }
    }

    bool punct(KernelToken& t)
    {
        static const char* const three[] = { "<<=", ">>=" };
        static const char* const two[] = { "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=",
                                           "&&", "||", "++", "--", "<<", ">>" };
        t.kind = KernelToken::Kind::ePunct;
        for (const char* p : three) {
            if (_s.compare(_i, 3, p) == 0) {
                t.text = p;
                _i += 3;
                return true;
            }
        }
        for (const char* p : two) {
            if (_s.compare(_i, 2, p) == 0) {
                t.text = p;
                _i += 2;
                return true;
            }
        }
        const char c = _s[_i];
        if (!std::strchr("+-*/%=!<>&|^~?:;,.(){}[]", c)) {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
                return fail(t.line, t.column, "unexpected character");
            }
            return fail(t.line, t.column, std::string("unexpected character '") + c + "'");
        }
        t.text = std::string(1, c);
        ++_i;
        return true;
    }
};

} // namespace

bool
kernelLex(std::string_view source, std::vector<KernelToken>& tokens, KernelCompileError& error)
{
    return Lexer(source, tokens, error).run();
}

NATRON_NAMESPACE_EXIT
