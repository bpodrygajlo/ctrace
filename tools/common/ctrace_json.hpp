#pragma once

#if !defined(CTRACE_FORCE_BUILTIN_JSON) && __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
namespace ctrace {
using json = nlohmann::json;
} // namespace ctrace
#else

#include <cctype>
#include <cstdint>
#include <istream>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ctrace {

class json {
public:
    enum class Kind { Null, Bool, Int, Uint, Float, String, Array, Object };

    using array_t = std::vector<json>;
    using object_t = std::map<std::string, json>;

    json() : kind_(Kind::Object) {}
    json(std::nullptr_t) : kind_(Kind::Null) {}
    json(bool v) : kind_(Kind::Bool), bool_val_(v) {}
    json(int v) : kind_(Kind::Int), int_val_(v) {}
    json(long v) : kind_(Kind::Int), int_val_(v) {}
    json(long long v) : kind_(Kind::Int), int_val_(v) {}
    json(unsigned int v) : kind_(Kind::Uint), uint_val_(v) {}
    json(unsigned long v) : kind_(Kind::Uint), uint_val_(v) {}
    json(unsigned long long v) : kind_(Kind::Uint), uint_val_(v) {}
    json(double v) : kind_(Kind::Float), float_val_(v) {}
    json(const char *v) : kind_(Kind::String), str_val_(v ? v : "") {}
    json(std::string v) : kind_(Kind::String), str_val_(std::move(v)) {}

    static json object() {
        json j;
        j.kind_ = Kind::Object;
        return j;
    }

    static json array() {
        json j;
        j.kind_ = Kind::Array;
        return j;
    }

    bool is_null() const { return kind_ == Kind::Null; }
    bool is_bool() const { return kind_ == Kind::Bool; }
    bool is_number() const { return kind_ == Kind::Int || kind_ == Kind::Uint || kind_ == Kind::Float; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_object() const { return kind_ == Kind::Object; }

    void push_back(const json &v) {
        if (kind_ != Kind::Array) {
            kind_ = Kind::Array;
            arr_val_.clear();
        }
        arr_val_.push_back(v);
    }

    json &operator[](const std::string &key) {
        if (kind_ != Kind::Object) {
            kind_ = Kind::Object;
            obj_val_.clear();
        }
        return obj_val_[key];
    }

    const json &operator[](const std::string &key) const {
        static const json kNull(nullptr);
        if (kind_ != Kind::Object) return kNull;
        auto it = obj_val_.find(key);
        return (it != obj_val_.end()) ? it->second : kNull;
    }

    bool contains(const std::string &key) const {
        if (kind_ != Kind::Object) return false;
        return obj_val_.find(key) != obj_val_.end();
    }

    json value(const std::string &key, const json &def) const {
        if (!contains(key)) return def;
        return (*this)[key];
    }

    std::string value(const std::string &key, const char *def) const {
        return value(key, std::string(def ? def : ""));
    }

    std::string value(const std::string &key, const std::string &def) const {
        if (!contains(key)) return def;
        const json &v = (*this)[key];
        return (v.kind_ == Kind::String) ? v.str_val_ : def;
    }

    bool value(const std::string &key, bool def) const {
        if (!contains(key)) return def;
        const json &v = (*this)[key];
        if (v.kind_ == Kind::Bool) return v.bool_val_;
        if (v.kind_ == Kind::Int) return v.int_val_ != 0;
        if (v.kind_ == Kind::Uint) return v.uint_val_ != 0;
        return def;
    }

    int value(const std::string &key, int def) const {
        if (!contains(key)) return def;
        return static_cast<int>((*this)[key].as_int64(def));
    }

    unsigned int value(const std::string &key, unsigned int def) const {
        if (!contains(key)) return def;
        return static_cast<unsigned int>((*this)[key].as_uint64(def));
    }

    long long value(const std::string &key, long long def) const {
        if (!contains(key)) return def;
        return static_cast<long long>((*this)[key].as_int64(def));
    }

    unsigned long long value(const std::string &key, unsigned long long def) const {
        if (!contains(key)) return def;
        return static_cast<unsigned long long>((*this)[key].as_uint64(def));
    }

    explicit operator std::string() const {
        return (kind_ == Kind::String) ? str_val_ : "";
    }

    array_t::const_iterator begin() const { return arr_val_.begin(); }
    array_t::const_iterator end() const { return arr_val_.end(); }
    array_t::iterator begin() { return arr_val_.begin(); }
    array_t::iterator end() { return arr_val_.end(); }

    std::string dump(int indent = -1) const {
        std::ostringstream os;
        dump_internal(os, indent, 0);
        return os.str();
    }

    static json parse(const std::string &s) {
        size_t pos = 0;
        json res = parse_value(s, pos);
        skip_ws(s, pos);
        if (pos != s.size()) {
            throw std::runtime_error("Trailing characters in JSON input");
        }
        return res;
    }

    friend std::istream &operator>>(std::istream &is, json &j) {
        std::ostringstream ss;
        ss << is.rdbuf();
        j = json::parse(ss.str());
        return is;
    }

    friend std::ostream &operator<<(std::ostream &os, const json &j) {
        os << j.dump();
        return os;
    }

private:
    Kind kind_ = Kind::Object;
    bool bool_val_ = false;
    int64_t int_val_ = 0;
    uint64_t uint_val_ = 0;
    double float_val_ = 0.0;
    std::string str_val_;
    array_t arr_val_;
    object_t obj_val_;

    int64_t as_int64(int64_t def) const {
        if (kind_ == Kind::Int) return int_val_;
        if (kind_ == Kind::Uint) return static_cast<int64_t>(uint_val_);
        if (kind_ == Kind::Float) return static_cast<int64_t>(float_val_);
        return def;
    }

    uint64_t as_uint64(uint64_t def) const {
        if (kind_ == Kind::Uint) return uint_val_;
        if (kind_ == Kind::Int) return static_cast<uint64_t>(int_val_);
        if (kind_ == Kind::Float) return static_cast<uint64_t>(float_val_);
        return def;
    }

    static void escape_string(std::ostream &os, const std::string &s) {
        os << '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"':  os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\b': os << "\\b";  break;
                case '\f': os << "\\f";  break;
                case '\n': os << "\\n";  break;
                case '\r': os << "\\r";  break;
                case '\t': os << "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        os << buf;
                    } else {
                        os << static_cast<char>(c);
                    }
                    break;
            }
        }
        os << '"';
    }

    void dump_internal(std::ostream &os, int indent, int depth) const {
        std::string pad = (indent > 0) ? std::string(depth * indent, ' ') : "";
        std::string inner_pad = (indent > 0) ? std::string((depth + 1) * indent, ' ') : "";
        std::string nl = (indent > 0) ? "\n" : "";
        std::string sp = (indent > 0) ? " " : "";

        switch (kind_) {
            case Kind::Null:
                os << "null";
                break;
            case Kind::Bool:
                os << (bool_val_ ? "true" : "false");
                break;
            case Kind::Int:
                os << int_val_;
                break;
            case Kind::Uint:
                os << uint_val_;
                break;
            case Kind::Float:
                os << float_val_;
                break;
            case Kind::String:
                escape_string(os, str_val_);
                break;
            case Kind::Array:
                if (arr_val_.empty()) {
                    os << "[]";
                } else {
                    os << "[" << nl;
                    for (size_t i = 0; i < arr_val_.size(); ++i) {
                        os << inner_pad;
                        arr_val_[i].dump_internal(os, indent, depth + 1);
                        if (i + 1 < arr_val_.size()) os << ",";
                        os << nl;
                    }
                    os << pad << "]";
                }
                break;
            case Kind::Object:
                if (obj_val_.empty()) {
                    os << "{}";
                } else {
                    os << "{" << nl;
                    size_t i = 0;
                    for (const auto &kv : obj_val_) {
                        os << inner_pad;
                        escape_string(os, kv.first);
                        os << ":" << sp;
                        kv.second.dump_internal(os, indent, depth + 1);
                        if (++i < obj_val_.size()) os << ",";
                        os << nl;
                    }
                    os << pad << "}";
                }
                break;
        }
    }

    static void skip_ws(const std::string &s, size_t &pos) {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
            ++pos;
        }
    }

    static std::string parse_string(const std::string &s, size_t &pos) {
        if (pos >= s.size() || s[pos] != '"') {
            throw std::runtime_error("Expected '\"' at start of JSON string");
        }
        ++pos;
        std::string out;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (pos >= s.size()) throw std::runtime_error("Unterminated escape in JSON string");
                char esc = s[pos++];
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (pos + 4 > s.size()) throw std::runtime_error("Invalid \\u escape in JSON");
                        unsigned int cp = std::stoul(s.substr(pos, 4), nullptr, 16);
                        pos += 4;
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        }
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("Unterminated JSON string");
    }

    static json parse_number(const std::string &s, size_t &pos) {
        size_t start = pos;
        bool is_neg = false;
        bool is_float = false;
        if (s[pos] == '-') {
            is_neg = true;
            ++pos;
        }
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            ++pos;
        }
        if (pos < s.size() && s[pos] == '.') {
            is_float = true;
            ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
                ++pos;
            }
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            is_float = true;
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
                ++pos;
            }
        }
        std::string tok = s.substr(start, pos - start);
        if (is_float) {
            return json(std::stod(tok));
        }
        if (is_neg) {
            return json(std::stoll(tok));
        }
        return json(std::stoull(tok));
    }

    static json parse_value(const std::string &s, size_t &pos) {
        skip_ws(s, pos);
        if (pos >= s.size()) {
            throw std::runtime_error("Unexpected end of JSON input");
        }
        char c = s[pos];
        if (c == '"') {
            return json(parse_string(s, pos));
        }
        if (c == '{') {
            ++pos;
            json obj = json::object();
            skip_ws(s, pos);
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return obj;
            }
            while (pos < s.size()) {
                skip_ws(s, pos);
                std::string key = parse_string(s, pos);
                skip_ws(s, pos);
                if (pos >= s.size() || s[pos] != ':') {
                    throw std::runtime_error("Expected ':' in JSON object");
                }
                ++pos;
                obj[key] = parse_value(s, pos);
                skip_ws(s, pos);
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == '}') {
                    ++pos;
                    return obj;
                }
                throw std::runtime_error("Expected ',' or '}' in JSON object");
            }
            throw std::runtime_error("Unterminated JSON object");
        }
        if (c == '[') {
            ++pos;
            json arr = json::array();
            skip_ws(s, pos);
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return arr;
            }
            while (pos < s.size()) {
                arr.push_back(parse_value(s, pos));
                skip_ws(s, pos);
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == ']') {
                    ++pos;
                    return arr;
                }
                throw std::runtime_error("Expected ',' or ']' in JSON array");
            }
            throw std::runtime_error("Unterminated JSON array");
        }
        if (c == 't' && s.compare(pos, 4, "true") == 0) {
            pos += 4;
            return json(true);
        }
        if (c == 'f' && s.compare(pos, 5, "false") == 0) {
            pos += 5;
            return json(false);
        }
        if (c == 'n' && s.compare(pos, 4, "null") == 0) {
            pos += 4;
            return json(nullptr);
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number(s, pos);
        }
        throw std::runtime_error(std::string("Unexpected character in JSON: ") + c);
    }
};

} // namespace ctrace
#endif
