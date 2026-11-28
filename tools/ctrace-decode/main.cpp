#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ctrace_json.hpp"

#if defined(CTRACE_HAVE_ZSTD)
#include <zstd.h>
#include <zstd_errors.h>
#endif
#if defined(CTRACE_HAVE_SQLITE)
#include <sqlite3.h>
#endif

using json = ctrace::json;

// Event id 0 is the clock anchor written by the runtime at init:
// uint16_t id, uint64_t trace_ns, uint64_t wall_ns (CLOCK_REALTIME).
static const uint16_t CLOCK_ANCHOR_ID = 0;
static const size_t CLOCK_ANCHOR_SIZE = 18;

struct ClockAnchor {
    bool valid = false;
    uint64_t trace_ns = 0;
    uint64_t wall_ns = 0;
};

// "[2026-11-28 14:03:22.123456789]" with an anchor, "[12.345678901]" seconds without one,
// raw nanoseconds with --ns. Returns the number of characters written.
static int formatTimestamp(char *buf, size_t cap, uint64_t ts_ns, const ClockAnchor &anchor, bool raw_ns) {
    if (raw_ns) {
        return std::snprintf(buf, cap, "%llu", (unsigned long long)ts_ns);
    }
    if (anchor.valid) {
        int64_t wall = (int64_t)anchor.wall_ns + ((int64_t)ts_ns - (int64_t)anchor.trace_ns);
        if (wall < 0) wall = 0;
        time_t secs = (time_t)(wall / 1000000000LL);
        unsigned nanos = (unsigned)(wall % 1000000000LL);
        struct tm tmv;
        localtime_r(&secs, &tmv);
        size_t n = std::strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tmv);
        return (int)n + std::snprintf(buf + n, cap - n, ".%09u", nanos);
    }
    return std::snprintf(buf, cap, "%llu.%09u", (unsigned long long)(ts_ns / 1000000000ULL),
                         (unsigned)(ts_ns % 1000000000ULL));
}

// The stream is always little-endian; swap on big-endian hosts.
template <typename T>
static inline T read_unaligned(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    if constexpr (sizeof(T) == 2) {
        uint16_t u; std::memcpy(&u, &v, 2); u = __builtin_bswap16(u); std::memcpy(&v, &u, 2);
    } else if constexpr (sizeof(T) == 4) {
        uint32_t u; std::memcpy(&u, &v, 4); u = __builtin_bswap32(u); std::memcpy(&v, &u, 4);
    } else if constexpr (sizeof(T) == 8) {
        uint64_t u; std::memcpy(&u, &v, 8); u = __builtin_bswap64(u); std::memcpy(&v, &u, 8);
    }
#endif
    return v;
}

class FastOutput {
public:
    // Writes to 'fd', or appends to '*sink' when one is given (used to reorder events)
    FastOutput(int fd = STDOUT_FILENO, std::string *sink = nullptr, size_t cap = 262144)
        : fd_(fd), sink_(sink), cap_(cap), pos_(0) {
        buf_ = new char[cap_];
    }
    ~FastOutput() {
        flush();
        delete[] buf_;
    }
    FastOutput(const FastOutput&) = delete;
    FastOutput& operator=(const FastOutput&) = delete;

    void flush() {
        if (pos_ > 0) {
            if (sink_) {
                sink_->append(buf_, pos_);
                pos_ = 0;
                return;
            }
            size_t total = 0;
            while (total < pos_) {
                ssize_t w = ::write(fd_, buf_ + total, pos_ - total);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (w == 0) break;
                total += (size_t)w;
            }
            pos_ = 0;
        }
    }
    void write_str(const char *s, size_t len) {
        if (len > cap_ - pos_) {
            flush();
            if (len > cap_ && sink_) {
                sink_->append(s, len);
                return;
            }
            if (len > cap_) {
                size_t total = 0;
                while (total < len) {
                    ssize_t w = ::write(fd_, s + total, len - total);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (w == 0) break;
                    total += (size_t)w;
                }
                return;
            }
        }
        std::memcpy(buf_ + pos_, s, len);
        pos_ += len;
    }
    void write_str(const std::string &s) {
        write_str(s.data(), s.size());
    }
    void write_char(char c) {
        if (pos_ >= cap_) flush();
        buf_[pos_++] = c;
    }
    void write_u64(uint64_t v) {
        if (pos_ + 32 > cap_) flush();
        auto res = std::to_chars(buf_ + pos_, buf_ + cap_, v);
        pos_ = res.ptr - buf_;
    }
    void write_i64(int64_t v) {
        if (pos_ + 32 > cap_) flush();
        auto res = std::to_chars(buf_ + pos_, buf_ + cap_, v);
        pos_ = res.ptr - buf_;
    }
    void write_double(double v) {
        if (std::isnan(v)) {
            write_str("nan");
            return;
        }
        if (std::isinf(v)) {
            write_str(v < 0 ? "-inf" : "inf");
            return;
        }
        char tmp[64];
        int n = 0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
        {
            auto res = std::to_chars(tmp, tmp + sizeof(tmp) - 2, v);
            n = (res.ec == std::errc()) ? (int)(res.ptr - tmp) : 0;
        }
#endif
        if (n == 0) {
            n = std::snprintf(tmp, sizeof(tmp), "%.17g", v);
        }
        if (n > 0) {
            bool hasDotOrExp = false;
            for (int i = 0; i < n; ++i) {
                if (tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E') {
                    hasDotOrExp = true;
                    break;
                }
            }
            if (!hasDotOrExp && (size_t)(n + 2) < sizeof(tmp)) {
                tmp[n++] = '.';
                tmp[n++] = '0';
            }
            write_str(tmp, n);
        }
    }
    void write_hex(const uint8_t *data, size_t len) {
        static const char hexChars[] = "0123456789abcdef";
        for (size_t i = 0; i < len; ++i) {
            write_char(hexChars[(data[i] >> 4) & 0xF]);
            write_char(hexChars[data[i] & 0xF]);
        }
    }
    // Bytes handed to the sink so far (the sink owns everything flushed to it)
    size_t sink_size() const { return sink_ ? sink_->size() + pos_ : 0; }
private:
    int fd_;
    std::string *sink_;
    size_t cap_;
    size_t pos_;
    char *buf_;
};

enum class FieldType {
    U8, I8, U16, I16, U32, I32, U64, I64,
    Float, Double, Bool, Enum, String, Bytes,
    FixedArray, CountedArray, Union, Struct, Unknown
};

static FieldType mapFieldType(const std::string &t, bool is_signed) {
    if (t == "uint8") return FieldType::U8;
    if (t == "int8") return FieldType::I8;
    if (t == "uint16") return FieldType::U16;
    if (t == "int16") return FieldType::I16;
    if (t == "uint32") return FieldType::U32;
    if (t == "int32") return FieldType::I32;
    if (t == "uint64") return FieldType::U64;
    if (t == "int64") return FieldType::I64;
    if (t == "float") return FieldType::Float;
    if (t == "double") return FieldType::Double;
    if (t == "bool") return FieldType::Bool;
    if (t == "enum") return FieldType::Enum;
    if (t == "string") return FieldType::String;
    if (t == "bytes") return FieldType::Bytes;
    if (t == "fixed_array") return FieldType::FixedArray;
    if (t == "counted_array") return FieldType::CountedArray;
    if (t == "union") return FieldType::Union;
    if (t == "struct") return FieldType::Struct;
    return is_signed ? FieldType::I64 : FieldType::U64;
}

struct FieldDesc {
    std::string name;
    std::string type_str;
    FieldType type = FieldType::Unknown;
    size_t size = 0;
    bool is_signed = false;
    std::string counted_by;
    std::string tag_field;
    std::string tag_flat_name; // tag field name with the same flattening prefix as this union
    int64_t tag_val = 0;
    bool has_tag_val = false;
    std::unordered_map<int64_t, std::string> enum_map;
    std::vector<FieldDesc> sub_fields;
    std::vector<FieldDesc> union_cases;
};

static std::vector<FieldDesc> flattenSchemaFields(const json &fields);

// Parses one schema field. 'name' is its flattened name and 'prefix' the flattening
// prefix of its enclosing struct (used to locate a union's tag among its siblings).
static void parseFieldDesc(const json &f, FieldDesc &fd, const std::string &name, const std::string &prefix) {
    fd.name = name;
    fd.type_str = f.value("type", "uint64");
    fd.size = f.value("size", 0ULL);
    fd.is_signed = f.value("is_signed", false);
    fd.type = mapFieldType(fd.type_str, fd.is_signed);
    fd.counted_by = f.value("counted_by", "");
    fd.tag_field = f.value("tag_field", "");
    if (!fd.tag_field.empty()) {
        fd.tag_flat_name = prefix.empty() ? fd.tag_field : (prefix + "_" + fd.tag_field);
    }
    if (f.contains("constants") && f["constants"].is_array()) {
        for (const auto &c : f["constants"]) {
            fd.enum_map[c.value("value", 0LL)] = c.value("name", "");
        }
    }
    if (f.contains("fields") && f["fields"].is_array()) {
        if (fd.type == FieldType::Struct) {
            // Nested structs are flattened the same way as top-level event fields
            fd.sub_fields = flattenSchemaFields(f["fields"]);
        } else {
            // Array element descriptor(s)
            for (const auto &sf : f["fields"]) {
                FieldDesc sfd;
                parseFieldDesc(sf, sfd, sf.value("name", ""), "");
                fd.sub_fields.push_back(sfd);
            }
        }
    }
    if (f.contains("union_cases") && f["union_cases"].is_array()) {
        for (const auto &uc : f["union_cases"]) {
            FieldDesc ucd;
            parseFieldDesc(uc, ucd, uc.value("name", ""), "");
            if (uc.contains("tag_val")) {
                ucd.tag_val = uc.value("tag_val", 0LL);
                ucd.has_tag_val = true;
            } else if (uc.contains("array_index")) { // older schemas
                ucd.tag_val = uc.value("array_index", 0LL);
                ucd.has_tag_val = true;
            }
            fd.union_cases.push_back(ucd);
        }
    }
}

static void flattenFieldsRaw(const json &fields, const std::string &prefix, std::vector<FieldDesc> &out) {
    if (!fields.is_array()) return;
    for (const auto &f : fields) {
        std::string fname = f.value("name", "");
        std::string name = prefix.empty() ? fname : (prefix + "_" + fname);
        std::string ftype = f.value("type", "uint64");
        if (ftype == "struct") {
            if (f.contains("fields")) {
                flattenFieldsRaw(f["fields"], name, out);
            }
        } else {
            FieldDesc fd;
            parseFieldDesc(f, fd, name, prefix);
            if (!f.contains("size")) fd.size = 8;
            out.push_back(std::move(fd));
        }
    }
}

static std::vector<FieldDesc> flattenSchemaFields(const json &fields) {
    std::vector<FieldDesc> raw;
    flattenFieldsRaw(fields, "", raw);
    std::set<std::string> used;
    std::vector<FieldDesc> result;
    for (auto &ff : raw) {
        std::string base = ff.name;
        std::string unique = base;
        int counter = 2;
        while (used.find(unique) != used.end()) {
            unique = base + "_" + std::to_string(counter);
            counter++;
        }
        used.insert(unique);
        ff.name = unique;
        result.push_back(std::move(ff));
    }
    return result;
}

struct EventDesc {
    uint16_t id = 0;
    std::string name;
    std::vector<FieldDesc> flat_fields;
};

// Optional SQLite output: one table per event type, ctrace_<event>(ts_ns, ts, <flat fields...>).
// Scalars are stored as numbers, strings and enum names as TEXT, bytes as BLOB, and
// arrays or unions as their decoded text.
class SqliteSink {
public:
    // Quotes an identifier so that field names such as "order" or "group" are usable
    static std::string ident(const std::string &name) {
        std::string q = "\"";
        for (char c : name) {
            if (c == '"') q += '"';
            q += c;
        }
        return q + "\"";
    }

    bool open(const std::string &path, const std::unordered_map<uint16_t, EventDesc> &decoders) {
#if defined(CTRACE_HAVE_SQLITE)
        unlink(path.c_str());
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            std::cerr << "ctrace-decode error: cannot open " << path << ": " << (db_ ? sqlite3_errmsg(db_) : "?") << "\n";
            return false;
        }
        sqlite3_exec(db_, "PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY; BEGIN;", nullptr, nullptr, nullptr);
        for (const auto &kv : decoders) {
            const EventDesc &ev = kv.second;
            std::string ddl = "CREATE TABLE " + ident("ctrace_" + ev.name) + " (ts_ns INTEGER, ts TEXT";
            std::string sql = "INSERT INTO " + ident("ctrace_" + ev.name) + " VALUES (?, ?";
            for (const auto &f : ev.flat_fields) {
                ddl += ", " + ident(f.name) + " " + columnType(f.type);
                sql += ", ?";
            }
            ddl += ");";
            sql += ");";
            if (sqlite3_exec(db_, ddl.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
                std::cerr << "ctrace-decode error: " << sqlite3_errmsg(db_) << "\n";
                return false;
            }
            sqlite3_stmt *st = nullptr;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                std::cerr << "ctrace-decode error: " << sqlite3_errmsg(db_) << "\n";
                return false;
            }
            stmts_[ev.id] = st;
        }
        return true;
#else
        (void)path; (void)decoders;
        std::cerr << "ctrace-decode error: compiled without SQLite support, --sqlite is unavailable\n";
        return false;
#endif
    }

    // 'values' holds each flat field's decoded text (top-level form, no quotes)
    void insert(const EventDesc &ev, uint64_t ts_ns, const std::vector<std::pair<const char *, size_t>> &values) {
#if defined(CTRACE_HAVE_SQLITE)
        sqlite3_stmt *st = stmts_[ev.id];
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ts_ns);
        sqlite3_bind_null(st, 2);
        for (size_t i = 0; i < ev.flat_fields.size(); ++i) {
            bindValue(st, (int)i + 3, ev.flat_fields[i].type, values[i].first, values[i].second);
        }
        sqlite3_step(st);
        sqlite3_reset(st);
#else
        (void)ev; (void)ts_ns; (void)values;
#endif
    }

    // Fills the readable 'ts' column once the whole stream (and so the clock anchor) is known
    void finish(const std::unordered_map<uint16_t, EventDesc> &decoders, const ClockAnchor &anchor, bool raw_ns) {
#if defined(CTRACE_HAVE_SQLITE)
        if (!db_) return;
        std::string expr;
        if (raw_ns) {
            expr = "CAST(ts_ns AS TEXT)";
        } else if (anchor.valid) {
            long long off = (long long)anchor.wall_ns - (long long)anchor.trace_ns;
            std::string w = "(ts_ns + (" + std::to_string(off) + "))";
            expr = "strftime('%Y-%m-%d %H:%M:%S', " + w + " / 1000000000, 'unixepoch', 'localtime') || printf('.%09d', " + w + " % 1000000000)";
        } else {
            expr = "printf('%d.%09d', ts_ns / 1000000000, ts_ns % 1000000000)";
        }
        for (const auto &kv : decoders) {
            std::string sql = "UPDATE " + ident("ctrace_" + kv.second.name) + " SET ts = " + expr + ";";
            sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
        }
#else
        (void)decoders; (void)anchor; (void)raw_ns;
#endif
    }

    void close() {
#if defined(CTRACE_HAVE_SQLITE)
        if (!db_) return;
        for (auto &kv : stmts_) sqlite3_finalize(kv.second);
        stmts_.clear();
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(db_);
        db_ = nullptr;
#endif
    }

    ~SqliteSink() { close(); }

private:
    static const char *columnType(FieldType t) {
        switch (t) {
            case FieldType::Float: case FieldType::Double: return "REAL";
            case FieldType::String: case FieldType::Enum: return "TEXT";
            case FieldType::Bytes: return "BLOB";
            case FieldType::U8: case FieldType::I8: case FieldType::U16: case FieldType::I16:
            case FieldType::U32: case FieldType::I32: case FieldType::U64: case FieldType::I64:
            case FieldType::Bool: return "INTEGER";
            default: return "TEXT";
        }
    }

#if defined(CTRACE_HAVE_SQLITE)
    static void bindValue(sqlite3_stmt *st, int col, FieldType t, const char *text, size_t len) {
        std::string tmp(text, len);
        switch (t) {
            case FieldType::U8: case FieldType::I8: case FieldType::U16: case FieldType::I16:
            case FieldType::U32: case FieldType::I32: case FieldType::I64:
                sqlite3_bind_int64(st, col, (sqlite3_int64)std::strtoll(tmp.c_str(), nullptr, 10));
                break;
            case FieldType::U64:
                sqlite3_bind_int64(st, col, (sqlite3_int64)std::strtoull(tmp.c_str(), nullptr, 10));
                break;
            case FieldType::Bool:
                sqlite3_bind_int(st, col, tmp == "True" ? 1 : 0);
                break;
            case FieldType::Float: case FieldType::Double:
                sqlite3_bind_double(st, col, std::strtod(tmp.c_str(), nullptr));
                break;
            case FieldType::Bytes: {
                if (len == 0) {
                    sqlite3_bind_zeroblob(st, col, 0); // an empty payload is an empty BLOB, not NULL
                    break;
                }
                std::vector<uint8_t> raw(len / 2);
                for (size_t i = 0; i + 1 < len; i += 2) {
                    raw[i / 2] = (uint8_t)std::strtoul(tmp.substr(i, 2).c_str(), nullptr, 16);
                }
                sqlite3_bind_blob(st, col, raw.data(), (int)raw.size(), SQLITE_TRANSIENT);
                break;
            }
            default:
                sqlite3_bind_text(st, col, text, (int)len, SQLITE_TRANSIENT);
                break;
        }
    }
    sqlite3 *db_ = nullptr;
    std::unordered_map<uint16_t, sqlite3_stmt *> stmts_;
#endif
};

static bool decodeFieldValue(const uint8_t *stream, size_t &offset, size_t total_len,
                             const FieldDesc &fld,
                             FastOutput &out,
                             const std::vector<std::pair<std::string, int64_t>> *current_tags,
                             std::vector<std::pair<std::string, int64_t>> *out_tags,
                             bool inside_dict) {
    switch (fld.type) {
        case FieldType::U8: {
            if (offset >= total_len) return false;
            uint8_t val = stream[offset++];
            out.write_u64((uint64_t)val);
            if (out_tags) out_tags->push_back({fld.name, (int64_t)val});
            return true;
        }
        case FieldType::I8: {
            if (offset >= total_len) return false;
            int8_t val = static_cast<int8_t>(stream[offset++]);
            out.write_i64((int64_t)val);
            if (out_tags) out_tags->push_back({fld.name, (int64_t)val});
            return true;
        }
        case FieldType::U16: {
            if (total_len - offset < 2) return false;
            uint16_t val = read_unaligned<uint16_t>(stream + offset);
            offset += 2;
            out.write_u64((uint64_t)val);
            if (out_tags) out_tags->push_back({fld.name, (int64_t)val});
            return true;
        }
        case FieldType::I16: {
            if (total_len - offset < 2) return false;
            int16_t val = read_unaligned<int16_t>(stream + offset);
            offset += 2;
            out.write_i64((int64_t)val);
            if (out_tags) out_tags->push_back({fld.name, (int64_t)val});
            return true;
        }
        case FieldType::U32: {
            if (total_len - offset < 4) return false;
            uint32_t val = read_unaligned<uint32_t>(stream + offset);
            offset += 4;
            out.write_u64((uint64_t)val);
            if (out_tags) out_tags->push_back({fld.name, (int64_t)val});
            return true;
        }
        case FieldType::I32: {
            if (total_len - offset < 4) return false;
            int32_t val = read_unaligned<int32_t>(stream + offset);
            offset += 4;
            out.write_i64((int64_t)val);
            if (out_tags) out_tags->push_back({fld.name, (int64_t)val});
            return true;
        }
        case FieldType::U64: {
            if (total_len - offset < 8) return false;
            uint64_t val = read_unaligned<uint64_t>(stream + offset);
            offset += 8;
            out.write_u64(val);
            if (out_tags) out_tags->push_back({fld.name, (int64_t)val});
            return true;
        }
        case FieldType::I64: {
            if (total_len - offset < 8) return false;
            int64_t val = read_unaligned<int64_t>(stream + offset);
            offset += 8;
            out.write_i64(val);
            if (out_tags) out_tags->push_back({fld.name, val});
            return true;
        }
        case FieldType::Float: {
            if (total_len - offset < 4) return false;
            float f = read_unaligned<float>(stream + offset);
            offset += 4;
            out.write_double((double)f);
            return true;
        }
        case FieldType::Double: {
            if (total_len - offset < 8) return false;
            double d = read_unaligned<double>(stream + offset);
            offset += 8;
            out.write_double(d);
            return true;
        }
        case FieldType::Bool: {
            if (offset >= total_len) return false;
            uint8_t v = stream[offset++];
            out.write_str(v ? "True" : "False");
            if (out_tags) out_tags->push_back({fld.name, v ? 1 : 0});
            return true;
        }
        case FieldType::Enum: {
            size_t sz = fld.size > 0 ? fld.size : 4;
            if (total_len - offset < sz) return false;
            int64_t val = 0;
            if (sz == 1) val = fld.is_signed ? (int64_t)static_cast<int8_t>(stream[offset]) : (int64_t)(stream[offset]);
            else if (sz == 2) val = fld.is_signed ? (int64_t)read_unaligned<int16_t>(stream + offset) : (int64_t)read_unaligned<uint16_t>(stream + offset);
            else if (sz == 4) val = fld.is_signed ? (int64_t)read_unaligned<int32_t>(stream + offset) : (int64_t)read_unaligned<uint32_t>(stream + offset);
            else if (sz == 8) val = fld.is_signed ? read_unaligned<int64_t>(stream + offset) : (int64_t)read_unaligned<uint64_t>(stream + offset);
            else return false;
            offset += sz;
            auto it = fld.enum_map.find(val);
            if (inside_dict) out.write_char('\'');
            if (it != fld.enum_map.end()) {
                out.write_str(it->second);
            } else {
                out.write_i64(val);
            }
            if (inside_dict) out.write_char('\'');
            if (out_tags) out_tags->push_back({fld.name, val});
            return true;
        }
        case FieldType::String: {
            if (total_len - offset < 4) return false;
            uint32_t slen = read_unaligned<uint32_t>(stream + offset);
            offset += 4;
            if (total_len - offset < slen) return false;
            const char *str_data = reinterpret_cast<const char*>(stream + offset);
            offset += slen;
            if (inside_dict) out.write_char('\'');
            out.write_str(str_data, slen);
            if (inside_dict) out.write_char('\'');
            return true;
        }
        case FieldType::Bytes: {
            if (total_len - offset < 4) return false;
            uint32_t blen = read_unaligned<uint32_t>(stream + offset);
            offset += 4;
            if (total_len - offset < blen) return false;
            const uint8_t *b_data = stream + offset;
            offset += blen;
            if (inside_dict) out.write_char('\'');
            out.write_hex(b_data, blen);
            if (inside_dict) out.write_char('\'');
            return true;
        }
        case FieldType::FixedArray: {
            size_t sz = fld.size;
            if (total_len - offset < sz) return false;
            const uint8_t *arr_bytes = stream + offset;
            offset += sz;
            out.write_char('[');
            if (!fld.sub_fields.empty()) {
                const auto &elem = fld.sub_fields[0];
                size_t esz = elem.size > 0 ? elem.size : 1;
                size_t cnt = sz / esz;
                size_t elem_off = 0;
                for (size_t i = 0; i < cnt; ++i) {
                    if (i > 0) out.write_str(", ");
                    size_t item_start = elem_off;
                    if (!decodeFieldValue(arr_bytes, elem_off, sz, elem, out, nullptr, nullptr, false)) {
                        return false;
                    }
                    if (elem_off == item_start) return false;
                }
            } else {
                for (size_t i = 0; i < sz; ++i) {
                    if (i > 0) out.write_str(", ");
                    out.write_u64((uint64_t)arr_bytes[i]);
                }
            }
            out.write_char(']');
            return true;
        }
        case FieldType::CountedArray: {
            if (total_len - offset < 4) return false;
            uint32_t blen = read_unaligned<uint32_t>(stream + offset);
            offset += 4;
            if (total_len - offset < blen) return false;
            size_t end_off = offset + blen;
            if (!fld.sub_fields.empty()) {
                const auto &elem = fld.sub_fields[0];
                if (elem.type == FieldType::Struct) {
                    out.write_char('[');
                    size_t idx = 0;
                    std::vector<std::pair<std::string, int64_t>> elem_tags;
                    while (offset < end_off) {
                        if (idx++ > 0) out.write_str(", ");
                        size_t item_start = offset;
                        out.write_char('{');
                        elem_tags.clear();
                        for (size_t j = 0; j < elem.sub_fields.size(); ++j) {
                            if (j > 0) out.write_str(", ");
                            out.write_char('\'');
                            out.write_str(elem.sub_fields[j].name);
                            out.write_str("': ");
                            if (!decodeFieldValue(stream, offset, end_off, elem.sub_fields[j], out, &elem_tags, &elem_tags, true)) {
                                return false;
                            }
                        }
                        out.write_char('}');
                        if (offset == item_start) return false; // zero-sized element: corrupt length
                    }
                    out.write_char(']');
                    return true;
                } else if (elem.size == 1 && (elem.type == FieldType::U8 || elem.type == FieldType::I8)) {
                    if (inside_dict) out.write_char('\'');
                    out.write_hex(stream + offset, blen);
                    if (inside_dict) out.write_char('\'');
                    offset = end_off;
                    return true;
                } else {
                    out.write_char('[');
                    size_t idx = 0;
                    while (offset < end_off) {
                        if (idx++ > 0) out.write_str(", ");
                        size_t item_start = offset;
                        if (!decodeFieldValue(stream, offset, end_off, elem, out, nullptr, nullptr, false)) {
                            return false;
                        }
                        if (offset == item_start) return false;
                    }
                    out.write_char(']');
                    return true;
                }
            } else {
                if (inside_dict) out.write_char('\'');
                out.write_hex(stream + offset, blen);
                if (inside_dict) out.write_char('\'');
                offset = end_off;
                return true;
            }
        }
        case FieldType::Union: {
            if (total_len - offset < 4) return false;
            uint32_t u_len = read_unaligned<uint32_t>(stream + offset);
            offset += 4;
            if (total_len - offset < u_len) return false;
            const uint8_t *u_data = stream + offset;
            offset += u_len;
            size_t sz = u_len;
            std::string tag_field = !fld.tag_field.empty() ? fld.tag_field : fld.counted_by;
            int64_t tag_val = -1;
            bool found_tag = false;
            if (current_tags && !fld.tag_flat_name.empty()) {
                for (auto it = current_tags->rbegin(); it != current_tags->rend(); ++it) {
                    if (it->first == fld.tag_flat_name) {
                        tag_val = it->second;
                        found_tag = true;
                        break;
                    }
                }
            }
            if (!found_tag && current_tags && !tag_field.empty()) {
                // Fallback: the most recently decoded field named like the tag
                for (auto it = current_tags->rbegin(); it != current_tags->rend(); ++it) {
                    if (it->first == tag_field ||
                        (it->first.size() > tag_field.size() &&
                         it->first.compare(it->first.size() - tag_field.size(), tag_field.size(), tag_field) == 0 &&
                         it->first[it->first.size() - tag_field.size() - 1] == '_')) {
                        tag_val = it->second;
                        found_tag = true;
                        break;
                    }
                }
            }
            const FieldDesc *matched_case = nullptr;
            if (found_tag) {
                for (const auto &c : fld.union_cases) {
                    if (c.has_tag_val && c.tag_val == tag_val) {
                        matched_case = &c;
                        break;
                    }
                }
            }
            if (matched_case) {
                out.write_str("{'");
                out.write_str(matched_case->name);
                out.write_str("': ");
                size_t u_off = 0;
                std::vector<std::pair<std::string, int64_t>> var_tags;
                if (current_tags) var_tags = *current_tags;
                if (matched_case->type == FieldType::Struct) {
                    out.write_char('{');
                    for (size_t j = 0; j < matched_case->sub_fields.size(); ++j) {
                        if (j > 0) out.write_str(", ");
                        out.write_char('\'');
                        out.write_str(matched_case->sub_fields[j].name);
                        out.write_str("': ");
                        if (!decodeFieldValue(u_data, u_off, sz, matched_case->sub_fields[j], out, &var_tags, &var_tags, true)) {
                            return false;
                        }
                    }
                    out.write_char('}');
                } else {
                    if (!decodeFieldValue(u_data, u_off, sz, *matched_case, out, &var_tags, &var_tags, true)) {
                        return false;
                    }
                }
                out.write_char('}');
            } else {
                if (inside_dict) out.write_char('\'');
                out.write_hex(u_data, sz);
                if (inside_dict) out.write_char('\'');
            }
            return true;
        }
        case FieldType::Struct: {
            out.write_char('{');
            for (size_t j = 0; j < fld.sub_fields.size(); ++j) {
                if (j > 0) out.write_str(", ");
                out.write_char('\'');
                out.write_str(fld.sub_fields[j].name);
                out.write_str("': ");
                if (!decodeFieldValue(stream, offset, total_len, fld.sub_fields[j], out, current_tags, out_tags, true)) {
                    return false;
                }
            }
            out.write_char('}');
            return true;
        }
        default:
            return false;
    }
}

static std::vector<uint8_t> decompressZstdFrames(const uint8_t *data, size_t total) {
    std::vector<uint8_t> result;
#if defined(CTRACE_HAVE_ZSTD)
    size_t offset = 0;
    while (offset + 4 <= total) {
        uint32_t comp_len = read_unaligned<uint32_t>(data + offset);
        offset += 4;
        if (comp_len == 0 || comp_len > total - offset) {
            std::cerr << "ctrace-decode error: Corrupt ZSTD frame length at offset " << (offset - 4) << "\n";
            return {};
        }
        const void *src = data + offset;
        offset += comp_len;

        unsigned long long rsz = ZSTD_getFrameContentSize(src, comp_len);
        if (rsz == ZSTD_CONTENTSIZE_ERROR) {
            std::cerr << "ctrace-decode error: Invalid ZSTD frame content\n";
            return {};
        }
        size_t initial_cap = (rsz == ZSTD_CONTENTSIZE_UNKNOWN) ? (comp_len * 4 + 65536) : (size_t)rsz;
        std::vector<uint8_t> chunk_buf(initial_cap);
        size_t dsz = ZSTD_decompress(chunk_buf.data(), chunk_buf.size(), src, comp_len);
        while (ZSTD_isError(dsz) && ZSTD_getErrorCode(dsz) == ZSTD_error_dstSize_tooSmall) {
            chunk_buf.resize(chunk_buf.size() * 2);
            dsz = ZSTD_decompress(chunk_buf.data(), chunk_buf.size(), src, comp_len);
        }
        if (ZSTD_isError(dsz)) {
            std::cerr << "ctrace-decode error: ZSTD_decompress failed: " << ZSTD_getErrorName(dsz) << "\n";
            return {};
        }
        size_t old_sz = result.size();
        result.resize(old_sz + dsz);
        std::memcpy(result.data() + old_sz, chunk_buf.data(), dsz);
    }
#else
    std::cerr << "ctrace-decode error: compiled without Zstandard support, cannot decompress .zst\n";
#endif
    return result;
}

static void usage() {
    std::cerr << "Usage: ctrace-decode <trace_schema.json> <trace.bin|trace.bin.zst> [options]\n"
                 "  -o FILE       write the text output to FILE instead of stdout\n"
                 "  --sqlite DB   also write the decoded events to a SQLite database\n"
                 "  --ns          print raw nanosecond timestamps instead of dates\n"
                 "  -q            no text output\n";
}

int main(int argc, char **argv) {
    std::string schema_path;
    std::string data_path;
    std::string out_path;
    std::string sqlite_path;
    bool raw_ns = false;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--sqlite" && i + 1 < argc) {
            sqlite_path = argv[++i];
        } else if (arg == "--ns") {
            raw_ns = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "ctrace-decode error: unknown option " << arg << "\n";
            usage();
            return 1;
        } else if (schema_path.empty()) {
            schema_path = arg;
        } else if (data_path.empty()) {
            data_path = arg;
        } else {
            usage();
            return 1;
        }
    }
    if (schema_path.empty() || data_path.empty()) {
        usage();
        return 1;
    }

    std::ifstream ifs(schema_path);
    if (!ifs.is_open()) {
        std::cerr << "ctrace-decode error: Failed to open schema file: " << schema_path << "\n";
        return 1;
    }

    json schema;
    try {
        ifs >> schema;
    } catch (const std::exception &e) {
        std::cerr << "ctrace-decode error: Invalid JSON in " << schema_path << ": " << e.what() << "\n";
        return 1;
    }

    std::unordered_map<uint16_t, EventDesc> decoders;
    if (schema.contains("events") && schema["events"].is_array()) {
        for (const auto &ev : schema["events"]) {
            EventDesc ed;
            ed.id = ev.value("id", (uint16_t)0);
            ed.name = ev.value("name", "");
            ed.flat_fields = flattenSchemaFields(ev.value("fields", json::array()));
            decoders[ed.id] = std::move(ed);
        }
    }

    int fd = open(data_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "ctrace-decode error: Failed to open trace file: " << data_path << "\n";
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        std::cerr << "ctrace-decode error: Failed to stat " << data_path << "\n";
        return 1;
    }

    size_t file_len = (size_t)st.st_size;
    if (file_len == 0) {
        close(fd);
        return 0; // Empty trace is valid
    }

    const uint8_t *stream_ptr = nullptr;
    std::vector<uint8_t> decomp_buffer;
    bool is_mmap = false;

    bool is_zst = data_path.size() >= 4 && data_path.rfind(".zst") == data_path.size() - 4;
    if (!is_zst && schema.value("backend", "") == "bin-zstd") {
        is_zst = true;
    }

    if (is_zst) {
        std::vector<uint8_t> raw_buf(file_len);
        size_t total_read = 0;
        while (total_read < file_len) {
            ssize_t r = ::read(fd, raw_buf.data() + total_read, file_len - total_read);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) break;
            total_read += (size_t)r;
        }
        close(fd);
        if (total_read != file_len) {
            std::cerr << "ctrace-decode error: Failed to read " << data_path << "\n";
            return 1;
        }
        decomp_buffer = decompressZstdFrames(raw_buf.data(), file_len);
        if (decomp_buffer.empty() && file_len > 0) {
            return 1;
        }
        stream_ptr = decomp_buffer.data();
        file_len = decomp_buffer.size();
    } else {
        void *map = mmap(nullptr, file_len, PROT_READ, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            stream_ptr = reinterpret_cast<const uint8_t*>(map);
            is_mmap = true;
        } else {
            decomp_buffer.resize(file_len);
            size_t total_read = 0;
            while (total_read < file_len) {
                ssize_t r = ::read(fd, decomp_buffer.data() + total_read, file_len - total_read);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) break;
                total_read += (size_t)r;
            }
            if (total_read != file_len) {
                close(fd);
                std::cerr << "ctrace-decode error: Failed to read " << data_path << "\n";
                return 1;
            }
            stream_ptr = decomp_buffer.data();
        }
        close(fd);
    }

    int out_fd = STDOUT_FILENO;
    if (!out_path.empty()) {
        out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            std::cerr << "ctrace-decode error: Failed to open output file: " << out_path << "\n";
            if (is_mmap) munmap((void*)stream_ptr, file_len);
            return 1;
        }
    }

    SqliteSink sqlite_sink;
    if (!sqlite_path.empty() && !sqlite_sink.open(sqlite_path, decoders)) {
        if (is_mmap) munmap((void*)stream_ptr, file_len);
        return 1;
    }

    // The clock anchor is normally the first record but may sit anywhere (buffers drain per
    // thread); timestamps are therefore formatted only after the whole stream was decoded.
    ClockAnchor anchor;

    // Ring buffers are drained per thread, so the file is not globally time ordered.
    // Every event line (without its timestamp) is decoded into memory first and written
    // out sorted by timestamp.
    std::string lines;
    struct LineRef {
        uint64_t ts;
        size_t off;
        size_t len;
    };
    std::vector<LineRef> refs;
    FastOutput fast_out(-1, &lines);
    std::vector<std::pair<size_t, size_t>> value_ranges; // per field: [start, end) in 'lines'
    std::vector<std::pair<const char *, size_t>> values;
    char ts_buf[64];

    size_t offset = 0;
    std::vector<std::pair<std::string, int64_t>> current_tags;
    bool decode_ok = true;

    while (offset + 10 <= file_len) {
        uint16_t ev_id = read_unaligned<uint16_t>(stream_ptr + offset);
        uint64_t ts_ns = read_unaligned<uint64_t>(stream_ptr + offset + 2);

        if (ev_id == CLOCK_ANCHOR_ID) {
            if (file_len - offset < CLOCK_ANCHOR_SIZE) {
                std::cerr << "ctrace-decode error: Truncated clock anchor at offset " << offset << "\n";
                decode_ok = false;
                break;
            }
            if (!anchor.valid) {
                anchor.valid = true;
                anchor.trace_ns = ts_ns;
                anchor.wall_ns = read_unaligned<uint64_t>(stream_ptr + offset + 10);
            }
            offset += CLOCK_ANCHOR_SIZE;
            continue;
        }

        auto it = decoders.find(ev_id);
        if (it == decoders.end()) {
            std::cerr << "ctrace-decode error: Unknown event ID " << ev_id << " at offset " << offset << "\n";
            decode_ok = false;
            break;
        }

        const EventDesc &dec = it->second;
        offset += 10;

        size_t line_start = fast_out.sink_size();
        fast_out.write_str(dec.name);

        current_tags.clear();
        value_ranges.clear();
        bool truncated = false;

        for (const auto &fld : dec.flat_fields) {
            fast_out.write_char(' ');
            fast_out.write_str(fld.name);
            fast_out.write_char('=');
            size_t value_start = fast_out.sink_size();
            if (!decodeFieldValue(stream_ptr, offset, file_len, fld, fast_out, &current_tags, &current_tags, false)) {
                truncated = true;
                break;
            }
            value_ranges.push_back({value_start, fast_out.sink_size()});
        }
        fast_out.write_char('\n');
        refs.push_back({ts_ns, line_start, fast_out.sink_size() - line_start});

        if (truncated) {
            std::cerr << "ctrace-decode error: Truncated event '" << dec.name << "' at offset " << offset << "\n";
            decode_ok = false;
            break;
        }

        if (!sqlite_path.empty()) {
            fast_out.flush(); // the values must be in 'lines' to be read back
            values.clear();
            for (const auto &r : value_ranges) {
                values.push_back({lines.data() + r.first, r.second - r.first});
            }
            sqlite_sink.insert(dec, ts_ns, values);
        }
    }

    if (decode_ok && offset != file_len) {
        std::cerr << "ctrace-decode error: Trailing unparsed bytes at offset " << offset << " (file size " << file_len << ")\n";
        decode_ok = false;
    }

    fast_out.flush();
    sqlite_sink.finish(decoders, anchor, raw_ns);
    sqlite_sink.close();
    if (!quiet) {
        std::stable_sort(refs.begin(), refs.end(), [](const LineRef &a, const LineRef &b) { return a.ts < b.ts; });
        FastOutput file_out(out_fd);
        for (const auto &r : refs) {
            int n = formatTimestamp(ts_buf, sizeof(ts_buf), r.ts, anchor, raw_ns);
            file_out.write_char('[');
            file_out.write_str(ts_buf, (size_t)n);
            file_out.write_str("] ");
            file_out.write_str(lines.data() + r.off, r.len);
        }
        file_out.flush();
    }

    if (out_fd != STDOUT_FILENO) {
        close(out_fd);
    }
    if (is_mmap) {
        munmap((void*)stream_ptr, file_len);
    }

    return decode_ok ? 0 : 1;
}
