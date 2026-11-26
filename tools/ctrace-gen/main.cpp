#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <cinttypes>

#include "ctrace_json.hpp"

using json = ctrace::json;

struct EnumConstant {
    std::string name;
    int64_t value = 0;
};

struct Field {
    std::string name;
    std::string type_kind;
    std::string c_type;
    uint64_t size = 0;
    bool is_signed = false;
    bool is_pointer = false;
    int array_index = -1;
    std::string array_parent;
    std::string counted_by;
    uint64_t array_len = 0;
    std::string tag_field;

    std::string enum_name;
    std::vector<EnumConstant> enum_constants;

    std::string struct_name;
    std::vector<Field> struct_fields;
};

struct Event {
    uint16_t id = 0;
    std::string name;
    std::string file;
    unsigned line = 0;
    std::vector<Field> fields;
};

void parseField(const json &jf, Field &f) {
    f.name = jf.value("name", "");
    f.type_kind = jf.value("type_kind", "");
    f.c_type = jf.value("c_type", "");
    f.size = jf.value("size", 0ULL);
    f.is_signed = jf.value("is_signed", false);
    f.is_pointer = jf.value("is_pointer", false);
    f.array_index = jf.value("array_index", -1);
    f.array_parent = jf.value("array_parent", "");
    f.counted_by = jf.value("counted_by", "");
    f.array_len = jf.value("array_len", 0ULL);
    f.tag_field = jf.value("tag_field", "");

    if (jf.contains("enum_name")) {
        f.enum_name = jf.value("enum_name", "");
    }
    if (jf.contains("enum_constants") && jf["enum_constants"].is_array()) {
        for (const auto &c : jf["enum_constants"]) {
            f.enum_constants.push_back({c.value("name", ""), c.value("value", 0LL)});
        }
    }
    if (jf.contains("struct_name")) {
        f.struct_name = jf.value("struct_name", "");
    }
    if (jf.contains("struct_fields") && jf["struct_fields"].is_array()) {
        for (const auto &sub : jf["struct_fields"]) {
            Field sf;
            parseField(sub, sf);
            f.struct_fields.push_back(sf);
        }
    }
    if (jf.contains("union_cases") && jf["union_cases"].is_array()) {
        for (const auto &sub : jf["union_cases"]) {
            Field sf;
            parseField(sub, sf);
            f.struct_fields.push_back(sf);
        }
    }
}

bool fieldsCompatible(const Field &a, const Field &b) {
    if (a.type_kind != b.type_kind) return false;
    if (a.size != b.size) return false;
    if (a.is_signed != b.is_signed) return false;
    if (a.is_pointer != b.is_pointer) return false;
    if (a.counted_by != b.counted_by) return false;
    if (a.type_kind == "enum") {
        if (a.enum_name != b.enum_name) return false;
        if (a.enum_constants.size() != b.enum_constants.size()) return false;
        for (size_t i = 0; i < a.enum_constants.size(); ++i) {
            if (a.enum_constants[i].name != b.enum_constants[i].name ||
                a.enum_constants[i].value != b.enum_constants[i].value) {
                return false;
            }
        }
    }
    if (a.type_kind == "struct" || a.type_kind == "counted_array") {
        if (a.struct_name != b.struct_name) return false;
        if (a.struct_fields.size() != b.struct_fields.size()) return false;
        for (size_t i = 0; i < a.struct_fields.size(); ++i) {
            if (!fieldsCompatible(a.struct_fields[i], b.struct_fields[i])) return false;
        }
    }
    return true;
}

struct FlatField {
    std::string flat_name;
    std::string accessor_expr; // e.g. "req.hdr.id"
    std::string type_kind;
    std::string c_type;
    uint64_t size = 0;
    bool is_signed = false;
    bool is_pointer = false;
    std::string enum_name;
    std::vector<EnumConstant> enum_constants;
    std::string counted_by;
    uint64_t array_len = 0;
    std::string tag_field;
    uint64_t elem_size = 0;
    std::vector<Field> struct_fields;
};

std::string getCountExpr(const std::string &accessor, const std::string &countField) {
    size_t lastArrow = accessor.rfind("->");
    size_t lastDot = accessor.rfind(".");
    if (lastArrow != std::string::npos && (lastDot == std::string::npos || lastArrow > lastDot)) {
        return accessor.substr(0, lastArrow + 2) + countField;
    }
    if (lastDot != std::string::npos) {
        return accessor.substr(0, lastDot + 1) + countField;
    }
    return countField;
}

void flattenFieldInternal(const Field &f, const std::string &prefix, const std::string &accessor, std::vector<FlatField> &out) {
    if (f.type_kind == "struct") {
        for (const auto &sub : f.struct_fields) {
            std::string nextPrefix = prefix.empty() ? sub.name : prefix + "_" + sub.name;
            std::string op = f.is_pointer ? "->" : ".";
            std::string nextAccessor;
            if (sub.array_index >= 0) {
                nextAccessor = accessor + op + sub.array_parent + "[" + std::to_string(sub.array_index) + "]";
            } else {
                nextAccessor = accessor + op + sub.name;
            }
            flattenFieldInternal(sub, nextPrefix, nextAccessor, out);
        }
    } else {
        FlatField ff;
        ff.flat_name = prefix.empty() ? f.name : prefix;
        ff.accessor_expr = accessor;
        ff.type_kind = f.type_kind;
        ff.c_type = f.c_type;
        ff.size = f.size;
        ff.is_signed = f.is_signed;
        ff.is_pointer = f.is_pointer;
        ff.enum_name = f.enum_name;
        ff.enum_constants = f.enum_constants;
        ff.counted_by = f.counted_by;
        ff.array_len = f.array_len;
        ff.tag_field = f.tag_field;
        ff.struct_fields = f.struct_fields;
        if (!f.struct_fields.empty()) {
            ff.elem_size = f.struct_fields[0].size;
        }
        out.push_back(ff);
    }
}

std::vector<FlatField> flattenEventFields(const std::vector<Field> &fields, bool useSafePointers = false, const std::string &varPrefix = "") {
    std::vector<FlatField> raw;
    for (const auto &f : fields) {
        std::string baseAcc;
        if (useSafePointers && f.type_kind == "struct" && f.is_pointer) {
            baseAcc = "_safe_" + f.name;
        } else {
            baseAcc = varPrefix + f.name;
        }
        flattenFieldInternal(f, f.name, baseAcc, raw);
    }

    // Ensure all flat_name identifiers are unique within the event
    std::set<std::string> used;
    std::vector<FlatField> result;
    for (auto ff : raw) {
        std::string base = ff.flat_name;
        std::string uniqueName = base;
        int counter = 2;
        while (used.count(uniqueName)) {
            uniqueName = base + "_" + std::to_string(counter++);
        }
        used.insert(uniqueName);
        ff.flat_name = uniqueName;
        result.push_back(ff);
    }
    return result;
}

bool eventHasStructArgs(const Event &ev) {
    for (const auto &f : ev.fields) {
        if (f.type_kind == "struct" || f.type_kind == "counted_array" || f.type_kind == "fixed_array" || f.type_kind == "union") {
            return true;
        }
    }
    return false;
}

std::string safeParamCType(const Field &f) {
    if (f.type_kind == "enum") {
        return f.is_signed ? "int64_t" : "uint64_t";
    }
    if (f.type_kind == "string") {
        return "const char *";
    }
    if (f.type_kind == "bytes") {
        return "ctrace_bytes_t";
    }
    if (f.is_pointer && f.type_kind != "struct") {
        return "const void *";
    }
    return f.c_type;
}

std::string sqliteColumnType(const std::string &type_kind) {
    if (type_kind == "string") return "TEXT";
    if (type_kind == "bytes" || type_kind == "counted_array" || type_kind == "fixed_array" || type_kind == "union") return "BLOB";
    if (type_kind == "float" || type_kind == "double") return "REAL";
    return "INTEGER"; // int, uint, bool, enum
}

json schemaFieldToJson(const Field &f) {
    json j;
    j["name"] = f.name;
    j["type"] = f.type_kind;
    j["c_type"] = f.c_type;
    j["size"] = f.size;
    j["is_signed"] = f.is_signed;
    if (f.is_pointer) j["is_pointer"] = true;
    if (!f.counted_by.empty()) j["counted_by"] = f.counted_by;
    if (f.array_index >= 0) {
        j["array_index"] = f.array_index;
        j["array_parent"] = f.array_parent;
    }
    if (f.type_kind == "enum") {
        j["enum_name"] = f.enum_name;
        json consts = json::array();
        for (const auto &c : f.enum_constants) {
            json cj = json::object();
            cj["name"] = c.name;
            cj["value"] = c.value;
            consts.push_back(cj);
        }
        j["constants"] = consts;
    } else if (f.type_kind == "struct" || f.type_kind == "counted_array") {
        if (!f.struct_name.empty()) j["struct_name"] = f.struct_name;
        json subs = json::array();
        for (const auto &sub : f.struct_fields) {
            subs.push_back(schemaFieldToJson(sub));
        }
        j["fields"] = subs;
    } else if (f.type_kind == "fixed_array") {
        j["array_len"] = f.array_len;
        json subs = json::array();
        for (const auto &sub : f.struct_fields) {
            subs.push_back(schemaFieldToJson(sub));
        }
        j["fields"] = subs;
    } else if (f.type_kind == "union") {
        j["tag_field"] = f.tag_field;
        json cases = json::array();
        for (const auto &sub : f.struct_fields) {
            cases.push_back(schemaFieldToJson(sub));
        }
        j["union_cases"] = cases;
    }
    return j;
}

std::string generateCtfTsdl(const std::map<std::string, Event> &events) {
    std::ostringstream m;
    m << "/* CTF 1.8 */\n";
    m << "typealias integer { size = 8;  align = 8; signed = false; } := uint8_t;\n";
    m << "typealias integer { size = 16; align = 8; signed = false; } := uint16_t;\n";
    m << "typealias integer { size = 32; align = 8; signed = false; } := uint32_t;\n";
    m << "typealias integer { size = 64; align = 8; signed = false; } := uint64_t;\n";
    m << "typealias integer { size = 8;  align = 8; signed = true;  } := int8_t;\n";
    m << "typealias integer { size = 16; align = 8; signed = true;  } := int16_t;\n";
    m << "typealias integer { size = 32; align = 8; signed = true;  } := int32_t;\n";
    m << "typealias integer { size = 64; align = 8; signed = true;  } := int64_t;\n\n";

    // Use align = 8 to match byte-packed emission
    m << "typealias floating_point {\n";
    m << "    exp_dig = 8; mant_dig = 24; align = 8;\n";
    m << "} := float;\n\n";

    m << "typealias floating_point {\n";
    m << "    exp_dig = 11; mant_dig = 53; align = 8;\n";
    m << "} := double;\n\n";

    m << "trace {\n";
    m << "    major = 1;\n";
    m << "    minor = 8;\n";
    m << "    uuid = \"2a8562a0-bb2a-466c-947e-8c38bb4e4276\";\n";
    m << "    byte_order = le;\n";
    m << "    packet.header := struct {\n";
    m << "        uint32_t magic;\n";
    m << "        uint8_t  uuid[16];\n";
    m << "        uint32_t stream_id;\n";
    m << "    };\n";
    m << "};\n\n";

    m << "clock {\n";
    m << "    name = monotonic;\n";
    m << "    uuid = \"2a8562a0-bb2a-466c-947e-8c38bb4e4276\";\n";
    m << "    description = \"Monotonic Clock\";\n";
    m << "    freq = 1000000000;\n";
    m << "};\n\n";

    m << "typealias integer {\n";
    m << "    size = 64; align = 8; signed = false;\n";
    m << "    map = clock.monotonic.value;\n";
    m << "} := uint64_clock_t;\n\n";

    m << "stream {\n";
    m << "    id = 0;\n";
    m << "    packet.context := struct {\n";
    m << "        uint64_clock_t timestamp_begin;\n";
    m << "        uint64_clock_t timestamp_end;\n";
    m << "        uint64_t content_size;\n";
    m << "        uint64_t packet_size;\n";
    m << "        uint64_t events_discarded;\n";
    m << "        uint32_t cpu_id;\n";
    m << "    };\n";
    m << "    event.header := struct {\n";
    m << "        uint16_t id;\n";
    m << "        uint64_clock_t timestamp;\n";
    m << "    };\n";
    m << "};\n\n";

    for (const auto &pair : events) {
        const Event &ev = pair.second;
        m << "event {\n";
        m << "    name = \"" << ev.name << "\";\n";
        m << "    id = " << ev.id << ";\n";
        m << "    stream_id = 0;\n";
        m << "    fields := struct {\n";

        std::vector<FlatField> flatList = flattenEventFields(ev.fields);
        for (const auto &ff : flatList) {
            if (ff.type_kind == "string") {
                m << "        string " << ff.flat_name << ";\n";
            } else if (ff.type_kind == "bytes" || ff.type_kind == "counted_array") {
                m << "        uint32_t " << ff.flat_name << "_len;\n";
                m << "        uint8_t " << ff.flat_name << "[" << ff.flat_name << "_len];\n";
            } else if (ff.type_kind == "fixed_array" || ff.type_kind == "union") {
                m << "        uint8_t " << ff.flat_name << "[" << ff.size << "];\n";
            } else if (ff.type_kind == "bool") {
                m << "        uint8_t " << ff.flat_name << ";\n";
            } else if (ff.type_kind == "enum") {
                if (!ff.enum_constants.empty()) {
                    m << "        enum : int32_t {\n";
                    std::set<int64_t> seenVals;
                    std::vector<EnumConstant> uniqueConsts;
                    for (const auto &ec : ff.enum_constants) {
                        if (!seenVals.count(ec.value)) {
                            seenVals.insert(ec.value);
                            uniqueConsts.push_back(ec);
                        }
                    }
                    for (size_t c_idx = 0; c_idx < uniqueConsts.size(); ++c_idx) {
                        m << "            \"" << uniqueConsts[c_idx].name << "\" = " << uniqueConsts[c_idx].value;
                        if (c_idx + 1 < uniqueConsts.size()) m << ",";
                        m << "\n";
                    }
                    m << "        } " << ff.flat_name << ";\n";
                } else {
                    m << "        int32_t " << ff.flat_name << ";\n";
                }
            } else if (ff.type_kind == "float") {
                m << "        float " << ff.flat_name << ";\n";
            } else if (ff.type_kind == "double") {
                m << "        double " << ff.flat_name << ";\n";
            } else {
                std::string t = (ff.is_signed ? "int" : "uint") + std::to_string(ff.size * 8) + "_t";
                m << "        " << t << " " << ff.flat_name << ";\n";
            }
        }
        m << "    };\n";
        m << "};\n\n";
    }

    return m.str();
}

// Helper for generating string length expression (handling both const char * and char[N])
std::string stringLenExpr(const FlatField &ff) {
    if (!ff.is_pointer && ff.size > 0) {
        return "(uint32_t)strnlen(" + ff.accessor_expr + ", " + std::to_string(ff.size) + ")";
    }
    return "(" + ff.accessor_expr + " ? (uint32_t)strlen(" + ff.accessor_expr + ") : 0U)";
}

// Recursive size calculation for counted_array (supports nested structs & nested counted_arrays)
void emitCountedArraySizeCalc(std::ostringstream &h, const FlatField &ff, const std::string &lenVar, int depth, bool isCtf, const std::string &eol) {
    std::string cntExpr = getCountExpr(ff.accessor_expr, ff.counted_by);
    std::string cntVar = "_cnt_" + ff.flat_name + "_" + std::to_string(depth);
    if (ff.is_pointer) {
        h << "    size_t " << cntVar << " = (((int64_t)(" << cntExpr << ") > 0) && ((const void *)(" << ff.accessor_expr << ") != NULL)) ? (size_t)(" << cntExpr << ") : 0;" << eol;
    } else {
        h << "    size_t " << cntVar << " = ((int64_t)(" << cntExpr << ") > 0) ? (size_t)(" << cntExpr << ") : 0;" << eol;
    }

    if (!ff.struct_fields.empty() && ff.struct_fields[0].type_kind == "struct") {
        const Field &elemStruct = ff.struct_fields[0];
        std::string idxVar = "_i" + std::to_string(depth);
        std::string elemAcc = "(" + ff.accessor_expr + ")[" + idxVar + "]";
        std::vector<FlatField> subFlat;
        flattenFieldInternal(elemStruct, ff.flat_name, elemAcc, subFlat);

        h << "    uint32_t " << lenVar << " = 0;" << eol;
        h << "    for (size_t " << idxVar << " = 0; " << idxVar << " < " << cntVar << "; ++" << idxVar << ") {" << eol;
        for (const auto &sf : subFlat) {
            if (sf.type_kind == "string") {
                std::string sl = stringLenExpr(sf);
                h << "        " << lenVar << " += " << (isCtf ? (sl + " + 1U") : ("4U + " + sl)) << ";" << eol;
            } else if (sf.type_kind == "bytes") {
                h << "        " << lenVar << " += 4U + (((" << sf.accessor_expr << ").ptr && (" << sf.accessor_expr << ").len > 0) ? (uint32_t)(" << sf.accessor_expr << ").len : 0U);" << eol;
            } else if (sf.type_kind == "counted_array") {
                std::string subLenVar = "_sublen_" + sf.flat_name + "_" + std::to_string(depth + 1);
                emitCountedArraySizeCalc(h, sf, subLenVar, depth + 1, isCtf, eol);
                h << "        " << lenVar << " += 4U + " << subLenVar << ";" << eol;
            } else if (isCtf && sf.type_kind == "enum") {
                h << "        " << lenVar << " += 4U;" << eol;
            } else if (sf.type_kind == "bool") {
                h << "        " << lenVar << " += 1U;" << eol;
            } else {
                h << "        " << lenVar << " += " << sf.size << "U;" << eol;
            }
        }
        h << "    }" << eol;
    } else {
        uint64_t esz = ff.elem_size > 0 ? ff.elem_size : 1;
        h << "    uint32_t " << lenVar << " = (uint32_t)(" << cntVar << " * " << esz << "U);" << eol;
    }
}

// Recursive payload serializer for counted_array into char *_p
void emitCountedArraySerialize(std::ostringstream &h, const FlatField &ff, const std::string &lenVar, int depth, bool isCtf, const std::string &eol) {
    h << "    memcpy(_p, &" << lenVar << ", 4); _p += 4;" << eol;
    std::string cntVar = "_cnt_" + ff.flat_name + "_" + std::to_string(depth);

    if (!ff.struct_fields.empty() && ff.struct_fields[0].type_kind == "struct") {
        const Field &elemStruct = ff.struct_fields[0];
        std::string idxVar = "_i" + std::to_string(depth);
        std::string elemAcc = "(" + ff.accessor_expr + ")[" + idxVar + "]";
        std::vector<FlatField> subFlat;
        flattenFieldInternal(elemStruct, ff.flat_name, elemAcc, subFlat);

        h << "    for (size_t " << idxVar << " = 0; " << idxVar << " < " << cntVar << "; ++" << idxVar << ") {" << eol;
        for (const auto &sf : subFlat) {
            if (sf.type_kind == "string") {
                std::string slVar = "_sl_" + sf.flat_name + "_" + std::to_string(depth);
                h << "        uint32_t " << slVar << " = " << stringLenExpr(sf) << ";" << eol;
                if (isCtf) {
                    h << "        if (" << slVar << " > 0) { memcpy(_p, " << sf.accessor_expr << ", " << slVar << "); _p += " << slVar << "; }" << eol;
                    h << "        *_p++ = '\\0';" << eol;
                } else {
                    h << "        memcpy(_p, &" << slVar << ", 4); _p += 4;" << eol;
                    h << "        if (" << slVar << " > 0) { memcpy(_p, " << sf.accessor_expr << ", " << slVar << "); _p += " << slVar << "; }" << eol;
                }
            } else if (sf.type_kind == "bytes") {
                std::string blVar = "_bl_" + sf.flat_name + "_" + std::to_string(depth);
                h << "        uint32_t " << blVar << " = ((" << sf.accessor_expr << ").ptr && (" << sf.accessor_expr << ").len > 0) ? (uint32_t)(" << sf.accessor_expr << ").len : 0U;" << eol;
                h << "        memcpy(_p, &" << blVar << ", 4); _p += 4;" << eol;
                h << "        if (" << blVar << " > 0) { memcpy(_p, (" << sf.accessor_expr << ").ptr, " << blVar << "); _p += " << blVar << "; }" << eol;
            } else if (sf.type_kind == "counted_array") {
                std::string subLenVar = "_sublen_" + sf.flat_name + "_" + std::to_string(depth + 1);
                emitCountedArraySizeCalc(h, sf, subLenVar, depth + 1, isCtf, eol);
                emitCountedArraySerialize(h, sf, subLenVar, depth + 1, isCtf, eol);
            } else if (sf.type_kind == "bool") {
                h << "        uint8_t _bv = (" << sf.accessor_expr << ") ? 1 : 0; memcpy(_p, &_bv, 1); _p += 1;" << eol;
            } else if (isCtf && sf.type_kind == "enum") {
                h << "        int32_t _ev = (int32_t)(" << sf.accessor_expr << "); memcpy(_p, &_ev, 4); _p += 4;" << eol;
            } else {
                h << "        { __typeof__(" << sf.accessor_expr << ") _tmp = (" << sf.accessor_expr << "); memcpy(_p, &_tmp, " << sf.size << "); _p += " << sf.size << "; }" << eol;
            }
        }
        h << "    }" << eol;
    } else {
        h << "    if (" << lenVar << " > 0) { memcpy(_p, (const void *)(" << ff.accessor_expr << "), " << lenVar << "); _p += " << lenVar << "; }" << eol;
    }
}

int main(int argc, char **argv) {
    std::string backend = "stdout";
    std::string outHeader = "";
    std::string outSchema = "";
    std::string outMetadata = "";
    std::vector<std::string> sidecarFiles;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--backend=", 0) == 0) {
            backend = arg.substr(10);
        } else if (arg.rfind("--out-header=", 0) == 0) {
            outHeader = arg.substr(13);
        } else if (arg.rfind("--out-schema=", 0) == 0) {
            outSchema = arg.substr(13);
        } else if (arg.rfind("--out-metadata=", 0) == 0) {
            outMetadata = arg.substr(15);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: ctrace-gen --backend=<backend> --out-header=<hdr> --out-schema=<json> [--out-metadata=<meta>] <sidecars...>\n";
            return 0;
        } else if (arg[0] != '-') {
            sidecarFiles.push_back(arg);
        }
    }

    auto stripQuotes = [](std::string &s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            s = s.substr(1, s.size() - 2);
        }
    };
    stripQuotes(backend);
    stripQuotes(outHeader);
    stripQuotes(outSchema);
    stripQuotes(outMetadata);
    for (auto &sf : sidecarFiles) {
        stripQuotes(sf);
    }

    if (backend != "stdout" && backend != "bin" && backend != "bin-zstd" && backend != "sqlite" && backend != "ctf" && backend != "runtime" && backend != "multi") {
        std::cerr << "ctrace-gen error: Unknown backend '" << backend << "'. Valid backends: stdout, bin, bin-zstd, sqlite, ctf, runtime\n";
        return 1;
    }

    if (outHeader.empty()) {
        std::cerr << "ctrace-gen error: --out-header is required\n";
        return 1;
    }

    auto ensureParentDir = [](const std::string &p) {
        if (!p.empty()) {
            auto parent = std::filesystem::path(p).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
        }
    };
    ensureParentDir(outHeader);
    ensureParentDir(outSchema);
    ensureParentDir(outMetadata);

    std::map<std::string, Event> events;
    std::map<std::string, std::vector<std::pair<std::string, unsigned>>> eventCallSites;

    for (const auto &file : sidecarFiles) {
        std::ifstream ifs(file);
        if (!ifs.is_open()) {
            std::cerr << "ctrace-gen error: Failed to open sidecar file: " << file << "\n";
            return 1;
        }
        json j;
        try {
            ifs >> j;
        } catch (const std::exception &e) {
            std::cerr << "ctrace-gen error: Invalid JSON in " << file << ": " << e.what() << "\n";
            return 1;
        }

        if (!j.contains("events") || !j["events"].is_array()) {
            continue;
        }

        for (const auto &evJson : j["events"]) {
            std::string name = evJson.value("name", "");
            std::string evFile = evJson.value("file", "");
            unsigned line = evJson.value("line", 0U);

            Event ev;
            ev.name = name;
            ev.file = evFile;
            ev.line = line;

            if (evJson.contains("fields") && evJson["fields"].is_array()) {
                for (const auto &jf : evJson["fields"]) {
                    Field fld;
                    parseField(jf, fld);
                    ev.fields.push_back(fld);
                }
            }

            auto it = events.find(name);
            if (it == events.end()) {
                events[name] = ev;
                eventCallSites[name].push_back({evFile, line});
            } else {
                const Event &existing = it->second;
                bool mismatch = false;
                std::string reason;

                if (existing.fields.size() != ev.fields.size()) {
                    mismatch = true;
                    reason = "argument count mismatch (expected " + std::to_string(existing.fields.size()) +
                             ", got " + std::to_string(ev.fields.size()) + ")";
                } else {
                    for (size_t i = 0; i < existing.fields.size(); ++i) {
                        if (!fieldsCompatible(existing.fields[i], ev.fields[i])) {
                            mismatch = true;
                            reason = "field " + std::to_string(i) + " type mismatch: '" +
                                     existing.fields[i].c_type + "' vs '" + ev.fields[i].c_type + "'";
                            break;
                        }
                    }
                }

                if (mismatch) {
                    std::cerr << "ctrace-gen error: Signature mismatch for event '" << name << "':\n";
                    std::cerr << "  First declaration at: " << existing.file << ":" << existing.line << "\n";
                    std::cerr << "  Conflicting call at:  " << evFile << ":" << line << "\n";
                    std::cerr << "  Reason: " << reason << "\n";
                    return 1;
                }

                eventCallSites[name].push_back({evFile, line});
            }
        }
    }

    if (events.size() > 65534) {
        std::cerr << "ctrace-gen error: Too many distinct trace events (> 65534)\n";
        return 1;
    }

    // Assign deterministic 16-bit IDs (alphabetical order)
    uint16_t nextId = 1;
    for (auto &pair : events) {
        pair.second.id = nextId++;
    }

    // Write trace_schema.json
    if (!outSchema.empty()) {
        json schema = json::object();
        schema["version"] = "1.0";
        schema["backend"] = backend;
        schema["endianness"] = "little";

        json eventsArray = json::array();
        for (const auto &pair : events) {
            const Event &ev = pair.second;
            json ej;
            ej["id"] = ev.id;
            ej["name"] = ev.name;
            json fieldsJson = json::array();
            for (const auto &fld : ev.fields) {
                fieldsJson.push_back(schemaFieldToJson(fld));
            }
            ej["fields"] = fieldsJson;
            eventsArray.push_back(ej);
        }
        schema["events"] = eventsArray;

        std::ofstream sf(outSchema);
        if (!sf.is_open()) {
            std::cerr << "ctrace-gen error: Could not open schema output file: " << outSchema << "\n";
            return 1;
        }
        sf << schema.dump(2) << "\n";
    }

    std::string ctfTsdl = generateCtfTsdl(events);
    if (!outMetadata.empty()) {
        std::ofstream mf(outMetadata);
        if (mf.is_open()) {
            mf << ctfTsdl;
        } else {
            std::cerr << "ctrace-gen warning: Could not open metadata output file: " << outMetadata << "\n";
        }
    }

    // Generate trace_generated.h
    std::ostringstream h;
    h << "// Generated by ctrace-gen. DO NOT EDIT.\n";
    h << "#pragma once\n\n";
    h << "#include <stddef.h>\n";
    h << "#include <stdint.h>\n";
    h << "#include <stdbool.h>\n";
    h << "#include <string.h>\n";
    h << "#include <time.h>\n";
    h << "#include <inttypes.h>\n\n";

    h << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

    h << "uint64_t ctrace_now_ns(void);\n\n";

    // Helper to emit function or macro header & argument bindings
    auto emitEmitterHeader = [&](const Event &ev, bool isMacro, const std::string &eol, const std::string &fnPrefix) {
        if (isMacro) {
            h << "#define " << fnPrefix << ev.name << "(";
            for (size_t i = 0; i < ev.fields.size(); ++i) {
                if (i > 0) h << ", ";
                h << "_ctrace_a" << i;
            }
            h << ") do {" << eol;
            for (size_t i = 0; i < ev.fields.size(); ++i) {
                if (ev.fields[i].type_kind == "struct") {
                    h << "    __typeof__(_ctrace_a" << i << ") __ctrace_v_" << ev.fields[i].name << " = (_ctrace_a" << i << ");" << eol;
                } else {
                    h << "    " << safeParamCType(ev.fields[i]) << " __ctrace_v_" << ev.fields[i].name << " = (_ctrace_a" << i << ");" << eol;
                }
            }
        } else {
            h << "static inline void " << fnPrefix << ev.name << "(";
            if (ev.fields.empty()) {
                h << "void";
            } else {
                for (size_t i = 0; i < ev.fields.size(); ++i) {
                    if (i > 0) h << ", ";
                    h << safeParamCType(ev.fields[i]) << " __ctrace_v_" << ev.fields[i].name;
                }
            }
            h << ") {" << eol;
        }
    };

    auto emitEmitterFooter = [&](bool isMacro) {
        if (isMacro) {
            h << "} while (0)\n\n";
        } else {
            h << "}\n\n";
        }
    };

    auto emitSafePointerGuards = [&](const Event &ev, const std::string &eol) {
        for (const auto &fld : ev.fields) {
            if (fld.type_kind == "struct" && fld.is_pointer) {
                std::string vname = "__ctrace_v_" + fld.name;
                h << "    static const __typeof__(*(" << vname << ")) _dummy_" << fld.name << " = {0};" << eol;
                h << "    const __typeof__(*(" << vname << ")) *_safe_" << fld.name << " = (" << vname << ") ? (" << vname << ") : &_dummy_" << fld.name << ";" << eol;
            }
        }
    };

    // Generate enum string helper functions recursively
    std::map<std::string, std::vector<EnumConstant>> allEnums;
    auto collectEnums = [&](auto &self, const std::vector<Field> &flds) -> void {
        for (const auto &f : flds) {
            if (f.type_kind == "enum" && !f.enum_name.empty()) {
                if (allEnums.find(f.enum_name) == allEnums.end() || (!f.enum_constants.empty() && allEnums[f.enum_name].empty())) {
                    allEnums[f.enum_name] = f.enum_constants;
                }
            }
            if (!f.struct_fields.empty()) {
                self(self, f.struct_fields);
            }
        }
    };

    for (const auto &pair : events) {
        collectEnums(collectEnums, pair.second.fields);
    }

    auto emitEnumHelpers = [&]() {
        for (const auto &ep : allEnums) {
            h << "static inline const char *__ctrace_enum_" << ep.first << "_to_str(int64_t val) {\n";
            h << "    switch (val) {\n";
            std::set<int64_t> seenValues;
            for (const auto &c : ep.second) {
                if (!seenValues.count(c.value)) {
                    seenValues.insert(c.value);
                    h << "        case " << c.value << "LL: return \"" << c.name << "\";\n";
                }
            }
            h << "        default: return \"UNKNOWN\";\n";
            h << "    }\n";
            h << "}\n\n";
        }
    };

    if (backend == "stdout" || backend == "runtime" || backend == "multi") {
        h << "#include <stdio.h>\n";
        h << "#include <stdarg.h>\n\n";
        h << "static inline void __ctrace_buf_append(char **p, size_t *rem, const char *fmt, ...) {\n";
        h << "    va_list ap;\n";
        h << "    va_start(ap, fmt);\n";
        h << "    int w = vsnprintf(*p, *rem, fmt, ap);\n";
        h << "    va_end(ap);\n";
        h << "    if (w > 0 && (size_t)w < *rem) {\n";
        h << "        *p += (size_t)w;\n";
        h << "        *rem -= (size_t)w;\n";
        h << "    } else if (*rem > 0) {\n";
        h << "        *rem = 0;\n";
        h << "    }\n";
        h << "}\n\n";
    }

    // Helper for printing structured fields in stdout
    auto generateStdoutFieldPrinter = [&](auto &self, const Field &f, const std::string &accessor, bool isTopLevel, bool inArray, int loopDepth, const std::string &eol) -> void {
        std::string prefix = isTopLevel ? " " : "";
        std::string namePrefix = inArray ? "" : (prefix + f.name + "=");
        if (f.type_kind == "counted_array") {
            std::string cnt = getCountExpr(accessor, f.counted_by);
            std::string lvar = "_i" + std::to_string(loopDepth);
            std::string cvar = "_cnt" + std::to_string(loopDepth);
            h << "    {" << eol;
            if (f.is_pointer) {
                h << "    size_t " << cvar << " = (((int64_t)(" << cnt << ") > 0) && ((const void *)(" << accessor << ") != NULL)) ? (size_t)(" << cnt << ") : 0;" << eol;
            } else {
                h << "    size_t " << cvar << " = ((int64_t)(" << cnt << ") > 0) ? (size_t)(" << cnt << ") : 0;" << eol;
            }
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << prefix << f.name << "=[\");" << eol;
            h << "    for (size_t " << lvar << " = 0; " << lvar << " < " << cvar << "; ++" << lvar << ") {" << eol;
            h << "        if (" << lvar << " > 0) __ctrace_buf_append(&_p, &_rem, \", \");" << eol;
            std::string elemAccessor = "(" + accessor + ")[" + lvar + "]";
            if (!f.struct_fields.empty()) {
                const auto &elem = f.struct_fields[0];
                self(self, elem, elemAccessor, false, true, loopDepth + 1, eol);
            }
            h << "    }" << eol;
            h << "    __ctrace_buf_append(&_p, &_rem, \"]\");" << eol;
            h << "    }" << eol;
        } else if (f.type_kind == "struct") {
            std::string op = f.is_pointer ? "->" : ".";
            if (f.is_pointer) {
                h << "    if (!(" << accessor << ")) {" << eol;
                h << "        __ctrace_buf_append(&_p, &_rem, \"" << (inArray ? "(null)" : (prefix + f.name + "=(null)")) << "\");" << eol;
                h << "    } else {" << eol;
            }
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << (inArray ? "{" : (prefix + f.name + "={")) << "\");" << eol;
            for (size_t i = 0; i < f.struct_fields.size(); ++i) {
                const auto &sub = f.struct_fields[i];
                std::string subAccessor;
                if (sub.array_index >= 0) {
                    subAccessor = "(" + accessor + ")" + op + sub.array_parent + "[" + std::to_string(sub.array_index) + "]";
                } else {
                    subAccessor = "(" + accessor + ")" + op + sub.name;
                }
                if (i > 0) {
                    h << "    __ctrace_buf_append(&_p, &_rem, \", \");" << eol;
                }
                self(self, sub, subAccessor, false, false, loopDepth, eol);
            }
            h << "    __ctrace_buf_append(&_p, &_rem, \"}\");" << eol;
            if (f.is_pointer) {
                h << "    }" << eol;
            }
        } else if (f.type_kind == "fixed_array") {
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "[len=%\" PRIu64 \",hex=\", (uint64_t)" << f.size << ");" << eol;
            h << "    for (size_t _i = 0; _i < (size_t)" << f.size << " && _i < 16; ++_i) {" << eol;
            h << "        __ctrace_buf_append(&_p, &_rem, \"%02x\", ((const unsigned char *)(" << accessor << "))[_i]);" << eol;
            h << "    }" << eol;
            if ((size_t)f.size > 16) h << "    __ctrace_buf_append(&_p, &_rem, \"...\");" << eol;
            h << "    __ctrace_buf_append(&_p, &_rem, \"]\");" << eol;
        } else if (f.type_kind == "union") {
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "[union_len=%\" PRIu64 \",hex=\", (uint64_t)" << f.size << ");" << eol;
            h << "    for (size_t _i = 0; _i < (size_t)" << f.size << " && _i < 16; ++_i) {" << eol;
            h << "        __ctrace_buf_append(&_p, &_rem, \"%02x\", ((const unsigned char *)(&(" << accessor << ")))[_i]);" << eol;
            h << "    }" << eol;
            if ((size_t)f.size > 16) h << "    __ctrace_buf_append(&_p, &_rem, \"...\");" << eol;
            h << "    __ctrace_buf_append(&_p, &_rem, \"]\");" << eol;
        } else if (f.type_kind == "string") {
            if (!f.is_pointer && f.size > 0) {
                h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "\\\"%.*s\\\"\", (int)" << f.size << ", " << accessor << ");" << eol;
            } else {
                h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "\\\"%s\\\"\", (" << accessor << ") ? (" << accessor << ") : \"(null)\");" << eol;
            }
        } else if (f.type_kind == "bytes") {
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "[len=%zu,hex=\", (size_t)(" << accessor << ").len);" << eol;
            h << "    if ((" << accessor << ").ptr) {" << eol;
            h << "        for (size_t _i = 0; _i < (" << accessor << ").len && _i < 16; ++_i) {" << eol;
            h << "            __ctrace_buf_append(&_p, &_rem, \"%02x\", ((const unsigned char *)(" << accessor << ").ptr)[_i]);" << eol;
            h << "        }" << eol;
            h << "        if ((" << accessor << ").len > 16) __ctrace_buf_append(&_p, &_rem, \"...\");" << eol;
            h << "    }" << eol;
            h << "    __ctrace_buf_append(&_p, &_rem, \"]\");" << eol;
        } else if (f.type_kind == "bool") {
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "%s\", (" << accessor << ") ? \"true\" : \"false\");" << eol;
        } else if (f.type_kind == "enum") {
            if (!f.enum_name.empty()) {
                h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "%s\", __ctrace_enum_" << f.enum_name << "_to_str((int64_t)(" << accessor << ")));" << eol;
            } else {
                h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "%\" PRId64, (int64_t)(" << accessor << "));" << eol;
            }
        } else if (f.type_kind == "float" || f.type_kind == "double") {
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "%.6g\", (double)(" << accessor << "));" << eol;
        } else if (f.is_pointer) {
            h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "%\" PRIu64, (uint64_t)(uintptr_t)(" << accessor << "));" << eol;
        } else {
            if (f.is_signed) {
                h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "%\" PRId64, (int64_t)(" << accessor << "));" << eol;
            } else {
                h << "    __ctrace_buf_append(&_p, &_rem, \"" << namePrefix << "%\" PRIu64, (uint64_t)(" << accessor << "));" << eol;
            }
        }
    };

    auto emitStdoutEvent = [&](const Event &ev, const std::string &fnPrefix) {
        bool isMacro = eventHasStructArgs(ev);
        std::string eol = isMacro ? " \\\n" : "\n";

        emitEmitterHeader(ev, isMacro, eol, fnPrefix);
        h << "    uint64_t _ts = ctrace_now_ns();" << eol;
        h << "    char _line_buf[4096];" << eol;
        h << "    char *_p = _line_buf;" << eol;
        h << "    size_t _rem = sizeof(_line_buf);" << eol;
        h << "    __ctrace_buf_append(&_p, &_rem, \"[%" PRIu64 "] " << ev.name << "\", _ts);" << eol;

        for (const auto &fld : ev.fields) {
            generateStdoutFieldPrinter(generateStdoutFieldPrinter, fld, "__ctrace_v_" + fld.name, true, false, 0, eol);
        }

        h << "    __ctrace_buf_append(&_p, &_rem, \"\\n\");" << eol;
        h << "    FILE *_f = ctrace_stdout_acquire();" << eol;
        h << "    if (_f) { fputs(_line_buf, _f); }" << eol;
        h << "    ctrace_stdout_release(_f);" << eol;
        emitEmitterFooter(isMacro);
    };

    auto emitBinEvent = [&](const Event &ev, const std::string &fnPrefix) {
        bool isMacro = eventHasStructArgs(ev);
        std::string eol = isMacro ? " \\\n" : "\n";
        std::vector<FlatField> flatList = flattenEventFields(ev.fields, true, "__ctrace_v_");

        emitEmitterHeader(ev, isMacro, eol, fnPrefix);
        emitSafePointerGuards(ev, eol);
        h << "    uint16_t _ev_id = " << ev.id << ";" << eol;
        h << "    uint64_t _ts = ctrace_now_ns();" << eol;

        for (const auto &ff : flatList) {
            if (ff.type_kind == "string") {
                h << "    uint32_t _len_" << ff.flat_name << " = " << stringLenExpr(ff) << ";" << eol;
            } else if (ff.type_kind == "bytes") {
                h << "    uint32_t _len_" << ff.flat_name << " = ((" << ff.accessor_expr << ").ptr && (" << ff.accessor_expr << ").len > 0) ? (uint32_t)(" << ff.accessor_expr << ").len : 0U;" << eol;
            } else if (ff.type_kind == "counted_array") {
                emitCountedArraySizeCalc(h, ff, "_len_" + ff.flat_name, 0, false, eol);
            }
        }

        h << "    size_t _total_len = sizeof(uint16_t) + sizeof(uint64_t)";
        for (const auto &ff : flatList) {
            if (ff.type_kind == "string" || ff.type_kind == "bytes" || ff.type_kind == "counted_array") {
                h << " + sizeof(uint32_t) + _len_" << ff.flat_name;
            } else if (ff.type_kind == "fixed_array" || ff.type_kind == "union") {
                h << " + " << ff.size;
            } else {
                h << " + " << ff.size;
            }
        }
        h << ";" << eol;

        h << "    char *_buf = (char *)ctrace_bin_reserve(_total_len);" << eol;
        h << "    if (_buf) {" << eol;
        h << "    char *_p = _buf;" << eol;
        h << "    memcpy(_p, &_ev_id, sizeof(_ev_id)); _p += sizeof(_ev_id);" << eol;
        h << "    memcpy(_p, &_ts, sizeof(_ts)); _p += sizeof(_ts);" << eol;

        for (const auto &ff : flatList) {
            if (ff.type_kind == "string") {
                h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;" << eol;
                h << "    if (_len_" << ff.flat_name << " > 0) { memcpy(_p, " << ff.accessor_expr << ", _len_" << ff.flat_name << "); _p += _len_" << ff.flat_name << "; }" << eol;
            } else if (ff.type_kind == "bytes") {
                h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;" << eol;
                h << "    if (_len_" << ff.flat_name << " > 0) { memcpy(_p, (" << ff.accessor_expr << ").ptr, _len_" << ff.flat_name << "); _p += _len_" << ff.flat_name << "; }" << eol;
            } else if (ff.type_kind == "counted_array") {
                emitCountedArraySerialize(h, ff, "_len_" + ff.flat_name, 0, false, eol);
            } else if (ff.type_kind == "fixed_array") {
                h << "    memcpy(_p, (const void *)(" << ff.accessor_expr << "), " << ff.size << "); _p += " << ff.size << ";" << eol;
            } else if (ff.type_kind == "union") {
                h << "    memcpy(_p, (const void *)(&(" << ff.accessor_expr << ")), " << ff.size << "); _p += " << ff.size << ";" << eol;
            } else if (ff.type_kind == "bool") {
                h << "    uint8_t _val_" << ff.flat_name << " = (" << ff.accessor_expr << ") ? 1 : 0;" << eol;
                h << "    memcpy(_p, &_val_" << ff.flat_name << ", 1); _p += 1;" << eol;
            } else if (ff.is_pointer) {
                h << "    uint" << (ff.size * 8) << "_t _val_" << ff.flat_name << " = (uint" << (ff.size * 8) << "_t)(uintptr_t)(" << ff.accessor_expr << ");" << eol;
                h << "    memcpy(_p, &_val_" << ff.flat_name << ", " << ff.size << "); _p += " << ff.size << ";" << eol;
            } else {
                h << "    { __typeof__(" << ff.accessor_expr << ") _val_" << ff.flat_name << " = (" << ff.accessor_expr << "); memcpy(_p, &_val_" << ff.flat_name << ", " << ff.size << "); _p += " << ff.size << "; }" << eol;
            }
        }

        h << "    ctrace_bin_commit(_buf, _total_len);" << eol;
        h << "    }" << eol;
        emitEmitterFooter(isMacro);
    };

    auto emitCtfEvent = [&](const Event &ev, const std::string &fnPrefix) {
        bool isMacro = eventHasStructArgs(ev);
        std::string eol = isMacro ? " \\\n" : "\n";
        std::vector<FlatField> flatList = flattenEventFields(ev.fields, true, "__ctrace_v_");

        emitEmitterHeader(ev, isMacro, eol, fnPrefix);
        emitSafePointerGuards(ev, eol);
        h << "    uint16_t _ev_id = " << ev.id << ";" << eol;
        h << "    uint64_t _ts = ctrace_now_ns();" << eol;

        for (const auto &ff : flatList) {
            if (ff.type_kind == "string") {
                h << "    uint32_t _len_" << ff.flat_name << " = " << stringLenExpr(ff) << ";" << eol;
            } else if (ff.type_kind == "bytes") {
                h << "    uint32_t _len_" << ff.flat_name << " = ((" << ff.accessor_expr << ").ptr && (" << ff.accessor_expr << ").len > 0) ? (uint32_t)(" << ff.accessor_expr << ").len : 0U;" << eol;
            } else if (ff.type_kind == "counted_array") {
                emitCountedArraySizeCalc(h, ff, "_len_" + ff.flat_name, 0, true, eol);
            }
        }

        h << "    size_t _total_len = sizeof(uint16_t) + sizeof(uint64_t)";
        for (const auto &ff : flatList) {
            if (ff.type_kind == "string") {
                h << " + _len_" << ff.flat_name << " + 1";
            } else if (ff.type_kind == "bytes" || ff.type_kind == "counted_array") {
                h << " + sizeof(uint32_t) + _len_" << ff.flat_name;
            } else if (ff.type_kind == "fixed_array" || ff.type_kind == "union") {
                h << " + " << ff.size;
            } else if (ff.type_kind == "enum") {
                h << " + 4";
            } else if (ff.type_kind == "bool") {
                h << " + 1";
            } else {
                h << " + " << ff.size;
            }
        }
        h << ";" << eol;

        h << "    char *_buf = (char *)ctrace_ctf_reserve(_total_len);" << eol;
        h << "    if (_buf) {" << eol;
        h << "    char *_p = _buf;" << eol;
        h << "    memcpy(_p, &_ev_id, sizeof(_ev_id)); _p += sizeof(_ev_id);" << eol;
        h << "    memcpy(_p, &_ts, sizeof(_ts)); _p += sizeof(_ts);" << eol;

        for (const auto &ff : flatList) {
            if (ff.type_kind == "string") {
                h << "    if (_len_" << ff.flat_name << " > 0) {" << eol;
                h << "        memcpy(_p, " << ff.accessor_expr << ", _len_" << ff.flat_name << ");" << eol;
                h << "        _p += _len_" << ff.flat_name << ";" << eol;
                h << "    }" << eol;
                h << "    *_p++ = '\\0';" << eol;
            } else if (ff.type_kind == "bytes") {
                h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;" << eol;
                h << "    if (_len_" << ff.flat_name << " > 0) {" << eol;
                h << "        memcpy(_p, (" << ff.accessor_expr << ").ptr, _len_" << ff.flat_name << ");" << eol;
                h << "        _p += _len_" << ff.flat_name << ";" << eol;
                h << "    }" << eol;
            } else if (ff.type_kind == "counted_array") {
                emitCountedArraySerialize(h, ff, "_len_" + ff.flat_name, 0, true, eol);
            } else if (ff.type_kind == "fixed_array") {
                h << "    memcpy(_p, (const void *)(" << ff.accessor_expr << "), " << ff.size << "); _p += " << ff.size << ";" << eol;
            } else if (ff.type_kind == "union") {
                h << "    memcpy(_p, (const void *)(&(" << ff.accessor_expr << ")), " << ff.size << "); _p += " << ff.size << ";" << eol;
            } else if (ff.type_kind == "bool") {
                h << "    uint8_t _val_" << ff.flat_name << " = (" << ff.accessor_expr << ") ? 1 : 0;" << eol;
                h << "    memcpy(_p, &_val_" << ff.flat_name << ", 1); _p += 1;" << eol;
            } else if (ff.type_kind == "enum") {
                h << "    int32_t _val_" << ff.flat_name << " = (int32_t)(" << ff.accessor_expr << ");" << eol;
                h << "    memcpy(_p, &_val_" << ff.flat_name << ", 4); _p += 4;" << eol;
            } else if (ff.is_pointer) {
                h << "    uint" << (ff.size * 8) << "_t _val_" << ff.flat_name << " = (uint" << (ff.size * 8) << "_t)(uintptr_t)(" << ff.accessor_expr << ");" << eol;
                h << "    memcpy(_p, &_val_" << ff.flat_name << ", " << ff.size << "); _p += " << ff.size << ";" << eol;
            } else {
                h << "    { __typeof__(" << ff.accessor_expr << ") _val_" << ff.flat_name << " = (" << ff.accessor_expr << "); memcpy(_p, &_val_" << ff.flat_name << ", " << ff.size << "); _p += " << ff.size << "; }" << eol;
            }
        }

        h << "    ctrace_ctf_commit(_buf, _total_len);" << eol;
        h << "    }" << eol;
        emitEmitterFooter(isMacro);
    };

    auto emitSqliteEvent = [&](const Event &ev, const std::string &fnPrefix) {
        bool isMacro = eventHasStructArgs(ev);
        std::string eol = isMacro ? " \\\n" : "\n";
        std::vector<FlatField> flatList = flattenEventFields(ev.fields, true, "__ctrace_v_");

        emitEmitterHeader(ev, isMacro, eol, fnPrefix);
        emitSafePointerGuards(ev, eol);
        h << "    uint64_t _ts = ctrace_now_ns();" << eol;
        h << "    ctrace_sqlite_lock();" << eol;
        h << "    sqlite3_stmt *_st = __ctrace_stmt_" << ev.name << ";" << eol;
        h << "    if (_st) {" << eol;
        h << "        sqlite3_bind_int64(_st, 1, (sqlite3_int64)_ts);" << eol;

        int col = 2;
        for (const auto &ff : flatList) {
            if (ff.type_kind == "string") {
                if (!ff.is_pointer && ff.size > 0) {
                    h << "        sqlite3_bind_text(_st, " << col++ << ", " << ff.accessor_expr << ", (int)strnlen(" << ff.accessor_expr << ", " << ff.size << "), SQLITE_TRANSIENT);" << eol;
                } else {
                    h << "        sqlite3_bind_text(_st, " << col++ << ", (" << ff.accessor_expr << ") ? (" << ff.accessor_expr << ") : \"\", -1, SQLITE_TRANSIENT);" << eol;
                }
            } else if (ff.type_kind == "bytes") {
                h << "        sqlite3_bind_blob(_st, " << col++ << ", (" << ff.accessor_expr << ").ptr, ((" << ff.accessor_expr << ").ptr && (" << ff.accessor_expr << ").len > 0) ? (int)(" << ff.accessor_expr << ").len : 0, SQLITE_TRANSIENT);" << eol;
            } else if (ff.type_kind == "fixed_array") {
                h << "        sqlite3_bind_blob(_st, " << col++ << ", (const void *)(" << ff.accessor_expr << "), (int)" << ff.size << ", SQLITE_TRANSIENT);" << eol;
            } else if (ff.type_kind == "union") {
                h << "        sqlite3_bind_blob(_st, " << col++ << ", (const void *)(&(" << ff.accessor_expr << ")), (int)" << ff.size << ", SQLITE_TRANSIENT);" << eol;
            } else if (ff.type_kind == "counted_array") {
                if (!ff.struct_fields.empty() && ff.struct_fields[0].type_kind == "struct") {
                    std::string lenVar = "_len_" + ff.flat_name;
                    emitCountedArraySizeCalc(h, ff, lenVar, 0, false, eol);
                    h << "        char *_ca_buf = (char *)malloc((size_t)4 + (size_t)" << lenVar << ");" << eol;
                    h << "        if (_ca_buf) {" << eol;
                    h << "            char *_p = _ca_buf;" << eol;
                    emitCountedArraySerialize(h, ff, lenVar, 0, false, eol);
                    h << "            sqlite3_bind_blob(_st, " << col++ << ", _ca_buf + 4, (int)" << lenVar << ", SQLITE_TRANSIENT);" << eol;
                    h << "            free(_ca_buf);" << eol;
                    h << "        } else {" << eol;
                    h << "            sqlite3_bind_null(_st, " << col++ << ");" << eol;
                    h << "        }" << eol;
                } else {
                    std::string cnt = getCountExpr(ff.accessor_expr, ff.counted_by);
                    uint64_t esz = ff.elem_size > 0 ? ff.elem_size : 1;
                    if (ff.is_pointer) {
                        h << "        sqlite3_bind_blob(_st, " << col++ << ", (const void *)(" << ff.accessor_expr << "), (((int64_t)(" << cnt << ") > 0) && ((const void *)(" << ff.accessor_expr << ") != NULL)) ? (int)((size_t)(" << cnt << ") * " << esz << ") : 0, SQLITE_TRANSIENT);" << eol;
                    } else {
                        h << "        sqlite3_bind_blob(_st, " << col++ << ", (const void *)(" << ff.accessor_expr << "), ((int64_t)(" << cnt << ") > 0) ? (int)((size_t)(" << cnt << ") * " << esz << ") : 0, SQLITE_TRANSIENT);" << eol;
                    }
                }
            } else if (ff.type_kind == "float" || ff.type_kind == "double") {
                h << "        sqlite3_bind_double(_st, " << col++ << ", (double)(" << ff.accessor_expr << "));" << eol;
            } else if (ff.type_kind == "bool") {
                h << "        sqlite3_bind_int(_st, " << col++ << ", (" << ff.accessor_expr << ") ? 1 : 0);" << eol;
            } else if (ff.is_pointer) {
                h << "        sqlite3_bind_int64(_st, " << col++ << ", (sqlite3_int64)(uint64_t)(uintptr_t)(" << ff.accessor_expr << "));" << eol;
            } else {
                if (ff.is_signed) {
                    h << "        sqlite3_bind_int64(_st, " << col++ << ", (sqlite3_int64)(" << ff.accessor_expr << "));" << eol;
                } else {
                    h << "        sqlite3_bind_int64(_st, " << col++ << ", (sqlite3_int64)(uint64_t)(" << ff.accessor_expr << "));" << eol;
                }
            }
        }

        h << "        sqlite3_step(_st);" << eol;
        h << "        sqlite3_reset(_st);" << eol;
        h << "        ctrace_sqlite_step_done();" << eol;
        h << "    }" << eol;
        h << "    ctrace_sqlite_unlock();" << eol;
        emitEmitterFooter(isMacro);
    };

    if (backend == "stdout") {
        h << "#include <stdio.h>\n\n";
        h << "FILE *ctrace_get_stdout_file(void);\n";
        h << "FILE *ctrace_stdout_acquire(void);\n";
        h << "void ctrace_stdout_release(FILE *f);\n\n";

        emitEnumHelpers();

        for (const auto &pair : events) {
            emitStdoutEvent(pair.second, "__ctrace_emit_");
        }
    } else if (backend == "bin" || backend == "bin-zstd") {
        h << "void *ctrace_bin_reserve(size_t len);\n";
        h << "void ctrace_bin_commit(void *ptr, size_t len);\n\n";

        for (const auto &pair : events) {
            emitBinEvent(pair.second, "__ctrace_emit_");
        }
    } else if (backend == "ctf") {
        h << "void *ctrace_ctf_reserve(size_t len);\n";
        h << "void ctrace_ctf_commit(void *ptr, size_t len);\n";
        h << "void ctrace_ctf_set_metadata(const char *metadata);\n\n";

        h << "static const char __ctrace_ctf_metadata[] = {\n    ";
        for (size_t i = 0; i < ctfTsdl.size(); ++i) {
            h << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)ctfTsdl[i] << std::dec << ", ";
            if ((i + 1) % 12 == 0) h << "\n    ";
        }
        h << "0x00\n};\n\n";

        h << "__attribute__((constructor)) static void __ctrace_ctf_register_meta(void) {\n";
        h << "    ctrace_ctf_set_metadata(__ctrace_ctf_metadata);\n";
        h << "}\n\n";

        for (const auto &pair : events) {
            emitCtfEvent(pair.second, "__ctrace_emit_");
        }
    } else if (backend == "sqlite") {
        h << "#include <sqlite3.h>\n";
        h << "#include <stdlib.h>\n\n";
        h << "void ctrace_sqlite_lock(void);\n";
        h << "void ctrace_sqlite_unlock(void);\n";
        h << "void ctrace_sqlite_step_done(void);\n";
        h << "void ctrace_sqlite_register_init(void (*fn)(sqlite3 *db));\n";
        h << "void ctrace_sqlite_register_cleanup(void (*fn)(void));\n\n";

        for (const auto &pair : events) {
            h << "extern sqlite3_stmt *__ctrace_stmt_" << pair.second.name << ";\n";
        }
        h << "\n";

        for (const auto &pair : events) {
            h << "__attribute__((weak)) sqlite3_stmt *__ctrace_stmt_" << pair.second.name << " = NULL;\n";
        }
        h << "\n";

        h << "__attribute__((weak)) void __ctrace_sqlite_finalize_events(void) {\n";
        for (const auto &pair : events) {
            const Event &ev = pair.second;
            h << "    if (__ctrace_stmt_" << ev.name << ") {\n";
            h << "        sqlite3_finalize(__ctrace_stmt_" << ev.name << ");\n";
            h << "        __ctrace_stmt_" << ev.name << " = NULL;\n";
            h << "    }\n";
        }
        h << "}\n\n";

        h << "__attribute__((weak)) void __ctrace_sqlite_init_events(sqlite3 *db) {\n";
        h << "    if (!db) return;\n";
        h << "    __ctrace_sqlite_finalize_events();\n";
        for (const auto &pair : events) {
            const Event &ev = pair.second;
            std::vector<FlatField> flatList = flattenEventFields(ev.fields);

            std::ostringstream ddl;
            ddl << "CREATE TABLE IF NOT EXISTS ctrace_" << ev.name << " (ts_ns INTEGER";
            for (const auto &ff : flatList) {
                ddl << ", " << ff.flat_name << " " << sqliteColumnType(ff.type_kind);
            }
            ddl << ");";

            h << "    sqlite3_exec(db, \"" << ddl.str() << "\", NULL, NULL, NULL);\n";

            std::ostringstream sql;
            sql << "INSERT INTO ctrace_" << ev.name << " VALUES (?";
            for (size_t i = 0; i < flatList.size(); ++i) {
                sql << ", ?";
            }
            sql << ");";

            h << "    sqlite3_prepare_v2(db, \"" << sql.str() << "\", -1, &__ctrace_stmt_" << ev.name << ", NULL);\n";
        }
        h << "}\n\n";

        h << "__attribute__((constructor)) static void __ctrace_sqlite_auto_register(void) {\n";
        h << "    ctrace_sqlite_register_cleanup(__ctrace_sqlite_finalize_events);\n";
        h << "    ctrace_sqlite_register_init(__ctrace_sqlite_init_events);\n";
        h << "}\n\n";

        for (const auto &pair : events) {
            emitSqliteEvent(pair.second, "__ctrace_emit_");
        }
    } else if (backend == "runtime" || backend == "multi") {
        h << "#include <stdio.h>\n";
        h << "#include <stdlib.h>\n\n";
        h << "FILE *ctrace_get_stdout_file(void);\n";
        h << "FILE *ctrace_stdout_acquire(void);\n";
        h << "void ctrace_stdout_release(FILE *f);\n";
        h << "void *ctrace_bin_reserve(size_t len);\n";
        h << "void ctrace_bin_commit(void *ptr, size_t len);\n";
        h << "void *ctrace_ctf_reserve(size_t len);\n";
        h << "void ctrace_ctf_commit(void *ptr, size_t len);\n";
        h << "void ctrace_ctf_set_metadata(const char *metadata);\n\n";

        h << "static const char __ctrace_ctf_metadata[] = {\n    ";
        for (size_t i = 0; i < ctfTsdl.size(); ++i) {
            h << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)ctfTsdl[i] << std::dec << ", ";
            if ((i + 1) % 12 == 0) h << "\n    ";
        }
        h << "0x00\n};\n\n";

        h << "__attribute__((constructor)) static void __ctrace_ctf_register_meta(void) {\n";
        h << "    ctrace_ctf_set_metadata(__ctrace_ctf_metadata);\n";
        h << "}\n\n";

        emitEnumHelpers();

        for (const auto &pair : events) {
            emitStdoutEvent(pair.second, "__ctrace_emit_stdout_");
            emitBinEvent(pair.second, "__ctrace_emit_bin_");
            emitCtfEvent(pair.second, "__ctrace_emit_ctf_");
        }

        h << "extern int g_ctrace_active_backend;\n\n";
        for (const auto &pair : events) {
            const Event &ev = pair.second;
            h << "#define __ctrace_emit_" << ev.name << "(...) do { \\\n";
            h << "    switch (__atomic_load_n(&g_ctrace_active_backend, __ATOMIC_RELAXED)) { \\\n";
            h << "        case 1: __ctrace_emit_stdout_" << ev.name << "(__VA_ARGS__); break; \\\n";
            h << "        case 2: __ctrace_emit_bin_" << ev.name << "(__VA_ARGS__); break; \\\n";
            h << "        case 3: __ctrace_emit_ctf_" << ev.name << "(__VA_ARGS__); break; \\\n";
            h << "        default: break; \\\n";
            h << "    } \\\n";
            h << "} while (0)\n\n";
        }
    }

    if (events.empty()) {
        if (backend == "sqlite") {
            h << "static inline void __ctrace_sqlite_init_events(void *db) { (void)db; }\n";
            h << "static inline void __ctrace_sqlite_finalize_events(void) {}\n";
        }
        h << "static inline void __ctrace_stub_noop(void) {}\n";
    }

    h << "#ifdef __cplusplus\n}\n#endif\n";

    std::ofstream outH(outHeader);
    if (!outH.is_open()) {
        std::cerr << "ctrace-gen error: Could not open output header file: " << outHeader << "\n";
        return 1;
    }
    outH << h.str();

    return 0;
}
