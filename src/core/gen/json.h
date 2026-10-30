// QuattroStagioni - minimal JSON reader for our own data files (data/vegetation_classes_v1.json).
//
// Small DOM: null, bool, number (kept as int64 when integral, else double), string (UTF-8, \uXXXX
// escapes incl. surrogate pairs), array, object (insertion order kept). Strict RFC 8259 syntax,
// depth limited. Not a general purpose library: no writer, no comments, no streaming.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace qs::json {

class Value {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Int || type_ == Type::Double; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    int64_t asInt(int64_t def = 0) const;
    double asDouble(double def = 0) const;
    const std::string& asString() const { return s_; }  // empty unless String

    const std::vector<Value>& items() const { return arr_; }                       // Array
    const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }  // Object
    size_t size() const { return type_ == Type::Array ? arr_.size() : obj_.size(); }

    // Object member lookup (linear; objects in our files are small). Returns a Null value if missing.
    const Value& operator[](const std::string& key) const;
    const Value& operator[](size_t i) const;
    bool has(const std::string& key) const;

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

// Parses a whole document. On error returns false and sets `error` ("line L col C: ...").
bool parse(const std::string& text, Value& out, std::string* error = nullptr);

}  // namespace qs::json
