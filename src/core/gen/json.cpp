// QuattroStagioni - minimal JSON reader. See json.h.
#include "json.h"

#include <cmath>
#include <cstdlib>

namespace qs::json {

namespace {
const Value& nullValue() {
    static const Value v;
    return v;
}
}  // namespace

int64_t Value::asInt(int64_t def) const {
    if (type_ == Type::Int) return i_;
    if (type_ == Type::Double) return int64_t(d_);
    return def;
}

double Value::asDouble(double def) const {
    if (type_ == Type::Double) return d_;
    if (type_ == Type::Int) return double(i_);
    return def;
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ == Type::Object)
        for (const auto& kv : obj_)
            if (kv.first == key) return kv.second;
    return nullValue();
}

const Value& Value::operator[](size_t i) const {
    return type_ == Type::Array && i < arr_.size() ? arr_[i] : nullValue();
}

bool Value::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}

    bool document(Value& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        if (p_ != t_.size()) return fail("trailing characters");
        return true;
    }
    std::string error() const {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < errPos_ && i < t_.size(); ++i) {
            if (t_[i] == '\n') { ++line; col = 1; }
            else ++col;
        }
        return "line " + std::to_string(line) + " col " + std::to_string(col) + ": " + err_;
    }

private:
    const std::string& t_;
    size_t p_ = 0, errPos_ = 0;
    std::string err_;

    bool fail(const char* m) {
        if (err_.empty()) { err_ = m; errPos_ = p_; }
        return false;
    }
    void ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
    }
    bool lit(const char* s) {
        size_t n = 0;
        while (s[n]) ++n;
        if (t_.compare(p_, n, s) != 0) return fail("invalid literal");
        p_ += n;
        return true;
    }

    bool value(Value& v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        if (p_ >= t_.size()) return fail("unexpected end");
        char c = t_[p_];
        switch (c) {
            case 'n': v.type_ = Value::Type::Null; return lit("null");
            case 't': v.type_ = Value::Type::Bool; v.b_ = true; return lit("true");
            case 'f': v.type_ = Value::Type::Bool; v.b_ = false; return lit("false");
            case '"': v.type_ = Value::Type::String; return str(v.s_);
            case '[': return array(v, depth);
            case '{': return object(v, depth);
            default: return number(v);
        }
    }

    bool array(Value& v, int depth) {
        v.type_ = Value::Type::Array;
        ++p_;
        ws();
        if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
        for (;;) {
            v.arr_.emplace_back();
            ws();
            if (!value(v.arr_.back(), depth + 1)) return false;
            ws();
            if (p_ >= t_.size()) return fail("unterminated array");
            if (t_[p_] == ',') { ++p_; continue; }
            if (t_[p_] == ']') { ++p_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool object(Value& v, int depth) {
        v.type_ = Value::Type::Object;
        ++p_;
        ws();
        if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
        for (;;) {
            ws();
            if (p_ >= t_.size() || t_[p_] != '"') return fail("expected member name");
            std::string key;
            if (!str(key)) return false;
            ws();
            if (p_ >= t_.size() || t_[p_] != ':') return fail("expected ':'");
            ++p_;
            ws();
            v.obj_.emplace_back(std::move(key), Value());
            if (!value(v.obj_.back().second, depth + 1)) return false;
            ws();
            if (p_ >= t_.size()) return fail("unterminated object");
            if (t_[p_] == ',') { ++p_; continue; }
            if (t_[p_] == '}') { ++p_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    static int hexv(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool hex4(uint32_t& out) {
        if (p_ + 4 > t_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            int h = hexv(t_[p_ + i]);
            if (h < 0) return fail("bad \\u escape");
            out = out * 16 + uint32_t(h);
        }
        p_ += 4;
        return true;
    }
    static void utf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s.push_back(char(cp));
        else if (cp < 0x800) { s.push_back(char(0xC0 | (cp >> 6))); s.push_back(char(0x80 | (cp & 63))); }
        else if (cp < 0x10000) {
            s.push_back(char(0xE0 | (cp >> 12))); s.push_back(char(0x80 | ((cp >> 6) & 63))); s.push_back(char(0x80 | (cp & 63)));
        } else {
            s.push_back(char(0xF0 | (cp >> 18))); s.push_back(char(0x80 | ((cp >> 12) & 63)));
            s.push_back(char(0x80 | ((cp >> 6) & 63))); s.push_back(char(0x80 | (cp & 63)));
        }
    }

    bool str(std::string& s) {
        ++p_;  // opening quote
        for (;;) {
            if (p_ >= t_.size()) return fail("unterminated string");
            char c = t_[p_++];
            if (c == '"') return true;
            if (uint8_t(c) < 0x20) return fail("control character in string");
            if (c != '\\') { s.push_back(c); continue; }
            if (p_ >= t_.size()) return fail("unterminated escape");
            char e = t_[p_++];
            switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        uint32_t lo;
                        if (p_ + 2 > t_.size() || t_[p_] != '\\' || t_[p_ + 1] != 'u') return fail("lone surrogate");
                        p_ += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("lone surrogate");
                    }
                    utf8(s, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }

    bool number(Value& v) {
        size_t s = p_;
        if (p_ < t_.size() && t_[p_] == '-') ++p_;
        if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return fail("invalid value");
        if (t_[p_] == '0') ++p_;
        else while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        bool integral = true;
        if (p_ < t_.size() && t_[p_] == '.') {
            integral = false;
            ++p_;
            if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return fail("bad fraction");
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            integral = false;
            ++p_;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
            if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return fail("bad exponent");
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        }
        std::string num = t_.substr(s, p_ - s);
        if (integral && num.size() < 19) {
            v.type_ = Value::Type::Int;
            v.i_ = std::strtoll(num.c_str(), nullptr, 10);
        } else {
            v.type_ = Value::Type::Double;
            v.d_ = std::strtod(num.c_str(), nullptr);  // our files never depend on the C locale
        }
        return true;
    }
};

bool parse(const std::string& text, Value& out, std::string* error) {
    out = Value();
    Parser p(text);
    if (!p.document(out)) {
        if (error) *error = p.error();
        return false;
    }
    return true;
}

}  // namespace qs::json
