#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cinttypes>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

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

    if (jf.contains("enum_name")) {
        f.enum_name = jf["enum_name"];
    }
    if (jf.contains("enum_constants") && jf["enum_constants"].is_array()) {
        for (const auto &c : jf["enum_constants"]) {
            f.enum_constants.push_back({c.value("name", ""), c.value("value", 0LL)});
        }
    }
    if (jf.contains("struct_name")) {
        f.struct_name = jf["struct_name"];
    }
    if (jf.contains("struct_fields") && jf["struct_fields"].is_array()) {
        for (const auto &sub : jf["struct_fields"]) {
            Field sf;
            parseField(sub, sf);
            f.struct_fields.push_back(sf);
        }
    }
}

bool fieldsCompatible(const Field &a, const Field &b) {
    if (a.type_kind != b.type_kind) return false;
    if (a.size != b.size) return false;
    if (a.type_kind == "struct") {
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
    std::string enum_name;
    std::vector<EnumConstant> enum_constants;
    std::string counted_by;
    uint64_t elem_size = 0;
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
        ff.enum_name = f.enum_name;
        ff.enum_constants = f.enum_constants;
        ff.counted_by = f.counted_by;
        if (!f.struct_fields.empty()) {
            ff.elem_size = f.struct_fields[0].size;
        }
        out.push_back(ff);
    }
}

std::vector<FlatField> flattenEventFields(const std::vector<Field> &fields, bool useSafePointers = false) {
    std::vector<FlatField> raw;
    for (const auto &f : fields) {
        std::string baseAcc = (useSafePointers && f.is_pointer) ? ("_safe_" + f.name) : f.name;
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

std::string sqliteColumnType(const std::string &type_kind) {
    if (type_kind == "string") return "TEXT";
    if (type_kind == "bytes" || type_kind == "counted_array") return "BLOB";
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
            consts.push_back({{"name", c.name}, {"value", c.value}});
        }
        j["constants"] = consts;
    } else if (f.type_kind == "struct" || f.type_kind == "counted_array") {
        if (!f.struct_name.empty()) j["struct_name"] = f.struct_name;
        json subs = json::array();
        for (const auto &sub : f.struct_fields) {
            subs.push_back(schemaFieldToJson(sub));
        }
        j["fields"] = subs;
    }
    return j;
}

std::string generateCtfTsdl(const std::map<std::string, Event> &events) {
    std::ostringstream m;
    m << "/* CTF 1.8 */\n\n";
    m << "typealias integer { size = 8; align = 8; signed = false; } := uint8_t;\n";
    m << "typealias integer { size = 16; align = 8; signed = false; } := uint16_t;\n";
    m << "typealias integer { size = 32; align = 8; signed = false; } := uint32_t;\n";
    m << "typealias integer { size = 64; align = 8; signed = false; } := uint64_t;\n";
    m << "typealias integer { size = 8; align = 8; signed = true; } := int8_t;\n";
    m << "typealias integer { size = 16; align = 8; signed = true; } := int16_t;\n";
    m << "typealias integer { size = 32; align = 8; signed = true; } := int32_t;\n";
    m << "typealias integer { size = 64; align = 8; signed = true; } := int64_t;\n";
    m << "typealias floating_point { exp_dig = 8; mant_dig = 24; align = 32; } := float;\n";
    m << "typealias floating_point { exp_dig = 11; mant_dig = 53; align = 64; } := double;\n\n";

    m << "trace {\n";
    m << "    major = 1;\n";
    m << "    minor = 8;\n";
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
            } else if (ff.type_kind == "bool") {
                m << "        uint8_t " << ff.flat_name << ";\n";
            } else if (ff.type_kind == "enum") {
                if (!ff.enum_constants.empty()) {
                    m << "        enum : int32_t {\n";
                    for (size_t c_idx = 0; c_idx < ff.enum_constants.size(); ++c_idx) {
                        m << "            \"" << ff.enum_constants[c_idx].name << "\" = " << ff.enum_constants[c_idx].value;
                        if (c_idx + 1 < ff.enum_constants.size()) m << ",";
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

    if (outHeader.empty()) {
        std::cerr << "ctrace-gen error: --out-header is required\n";
        return 1;
    }

    if (!outHeader.empty()) {
        std::filesystem::create_directories(std::filesystem::path(outHeader).parent_path());
    }
    if (!outSchema.empty()) {
        std::filesystem::create_directories(std::filesystem::path(outSchema).parent_path());
    }
    if (!outMetadata.empty()) {
        std::filesystem::create_directories(std::filesystem::path(outMetadata).parent_path());
    }

    // Map: event_name -> Event
    std::map<std::string, Event> events;
    // Map: event_name -> list of (file, line) for validation
    std::map<std::string, std::vector<std::pair<std::string, unsigned>>> eventCallSites;

    for (const auto &sidecarPath : sidecarFiles) {
        std::ifstream f(sidecarPath);
        if (!f.is_open()) {
            std::cerr << "ctrace-gen warning: Could not open sidecar file: " << sidecarPath << "\n";
            continue;
        }
        json j;
        try {
            f >> j;
        } catch (const std::exception &e) {
            std::cerr << "ctrace-gen error: Failed parsing JSON in " << sidecarPath << ": " << e.what() << "\n";
            return 1;
        }

        if (!j.contains("events") || !j["events"].is_array()) continue;

        for (const auto &evJson : j["events"]) {
            std::string name = evJson.value("name", "");
            std::string file = evJson.value("file", "");
            unsigned line = evJson.value("line", 0U);

            Event ev;
            ev.name = name;
            ev.file = file;
            ev.line = line;

            if (evJson.contains("fields") && evJson["fields"].is_array()) {
                for (const auto &jf : evJson["fields"]) {
                    Field fld;
                    parseField(jf, fld);
                    ev.fields.push_back(fld);
                }
            }

            // Check if already registered
            auto it = events.find(name);
            if (it == events.end()) {
                events[name] = ev;
                eventCallSites[name].push_back({file, line});
            } else {
                // Validate signature compatibility
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
                    std::cerr << "  Conflicting call at:  " << file << ":" << line << "\n";
                    std::cerr << "  Reason: " << reason << "\n";
                    return 1;
                }

                eventCallSites[name].push_back({file, line});
            }
        }
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
        if (sf.is_open()) {
            sf << schema.dump(2) << "\n";
        } else {
            std::cerr << "ctrace-gen warning: Could not open schema output file: " << outSchema << "\n";
        }
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

    // Common monotonic time helper declaration
    h << "uint64_t ctrace_now_ns(void);\n\n";

    if (backend == "stdout") {
        h << "#include <stdio.h>\n\n";
        h << "FILE *ctrace_get_stdout_file(void);\n\n";

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

        for (const auto &ep : allEnums) {
            h << "static inline const char *__ctrace_enum_" << ep.first << "_to_str(int64_t val) {\n";
            h << "    switch (val) {\n";
            for (const auto &c : ep.second) {
                h << "        case " << c.value << "LL: return \"" << c.name << "\";\n";
            }
            h << "        default: return \"UNKNOWN\";\n";
            h << "    }\n";
            h << "}\n\n";
        }

        // Helper for printing structured fields in stdout
        auto generateStdoutFieldPrinter = [&](auto &self, const Field &f, const std::string &accessor, bool isTopLevel, bool inArray, int loopDepth) -> void {
            std::string prefix = isTopLevel ? " " : "";
            std::string namePrefix = inArray ? "" : (prefix + f.name + "=");
            if (f.type_kind == "counted_array") {
                std::string cnt = getCountExpr(accessor, f.counted_by);
                std::string lvar = "_i" + std::to_string(loopDepth);
                h << "    fprintf(_f, \"" << prefix << f.name << "=[\");\n";
                h << "    for (size_t " << lvar << " = 0; " << lvar << " < (size_t)(" << cnt << "); ++" << lvar << ") {\n";
                h << "        if (" << lvar << " > 0) fprintf(_f, \", \");\n";
                std::string elemAccessor = accessor + "[" + lvar + "]";
                if (!f.struct_fields.empty()) {
                    const auto &elem = f.struct_fields[0];
                    self(self, elem, elemAccessor, false, true, loopDepth + 1);
                }
                h << "    }\n";
                h << "    fprintf(_f, \"]\");\n";
            } else if (f.type_kind == "struct") {
                std::string op = f.is_pointer ? "->" : ".";
                if (f.is_pointer) {
                    h << "    if (!" << accessor << ") {\n";
                    h << "        fprintf(_f, \"" << (inArray ? "(null)" : (prefix + f.name + "=(null)")) << "\");\n";
                    h << "    } else {\n";
                }
                h << "    fprintf(_f, \"" << (inArray ? "{" : (prefix + f.name + "={")) << "\");\n";
                for (size_t i = 0; i < f.struct_fields.size(); ++i) {
                    const auto &sub = f.struct_fields[i];
                    std::string subAccessor;
                    if (sub.array_index >= 0) {
                        subAccessor = accessor + op + sub.array_parent + "[" + std::to_string(sub.array_index) + "]";
                    } else {
                        subAccessor = accessor + op + sub.name;
                    }
                    if (i > 0) {
                        h << "    fprintf(_f, \", \");\n";
                    }
                    self(self, sub, subAccessor, false, false, loopDepth);
                }
                h << "    fprintf(_f, \"}\");\n";
                if (f.is_pointer) {
                    h << "    }\n";
                }
            } else if (f.type_kind == "string") {
                h << "    fprintf(_f, \"" << namePrefix << "\\\"%s\\\"\", " << accessor << " ? " << accessor << " : \"(null)\");\n";
            } else if (f.type_kind == "bytes") {
                h << "    fprintf(_f, \"" << namePrefix << "[len=%zu,hex=\", (size_t)" << accessor << ".len);\n";
                h << "    for (size_t _i = 0; _i < " << accessor << ".len && _i < 16; ++_i) {\n";
                h << "        fprintf(_f, \"%02x\", ((const unsigned char *)" << accessor << ".ptr)[_i]);\n";
                h << "    }\n";
                if (f.size > 0) {
                    h << "    if (" << accessor << ".len > 16) fprintf(_f, \"...\");\n";
                }
                h << "    fprintf(_f, \"]\");\n";
            } else if (f.type_kind == "bool") {
                h << "    fprintf(_f, \"" << namePrefix << "%s\", " << accessor << " ? \"true\" : \"false\");\n";
            } else if (f.type_kind == "enum") {
                if (!f.enum_name.empty()) {
                    h << "    fprintf(_f, \"" << namePrefix << "%s\", __ctrace_enum_" << f.enum_name << "_to_str((int64_t)" << accessor << "));\n";
                } else {
                    h << "    fprintf(_f, \"" << namePrefix << "%\" PRId64, (int64_t)" << accessor << ");\n";
                }
            } else if (f.type_kind == "float" || f.type_kind == "double") {
                h << "    fprintf(_f, \"" << namePrefix << "%.6g\", (double)" << accessor << ");\n";
            } else {
                if (f.is_signed) {
                    h << "    fprintf(_f, \"" << namePrefix << "%\" PRId64, (int64_t)" << accessor << ");\n";
                } else {
                    h << "    fprintf(_f, \"" << namePrefix << "%\" PRIu64, (uint64_t)" << accessor << ");\n";
                }
            }
        };

        // Generate stdout emitters
        for (const auto &pair : events) {
            const Event &ev = pair.second;
            h << "static inline void __ctrace_emit_" << ev.name << "(";
            for (size_t i = 0; i < ev.fields.size(); ++i) {
                if (i > 0) h << ", ";
                h << ev.fields[i].c_type << " " << ev.fields[i].name;
            }
            h << ") {\n";
            h << "    uint64_t _ts = ctrace_now_ns();\n";
            h << "    FILE *_f = ctrace_get_stdout_file();\n";
            h << "    flockfile(_f);\n";
            h << "    fprintf(_f, \"[%\" PRIu64 \"] " << ev.name << "\", _ts);\n";

            for (const auto &fld : ev.fields) {
                generateStdoutFieldPrinter(generateStdoutFieldPrinter, fld, fld.name, true, false, 0);
            }

            h << "    fprintf(_f, \"\\n\");\n";
            h << "    funlockfile(_f);\n";
            h << "}\n\n";
        }
    } else if (backend == "bin" || backend == "bin-zstd") {
        h << "void *ctrace_bin_reserve(size_t len);\n";
        h << "void ctrace_bin_commit(void *ptr, size_t len);\n\n";

        for (const auto &pair : events) {
            const Event &ev = pair.second;
            std::vector<FlatField> flatList = flattenEventFields(ev.fields, true);

            h << "static inline void __ctrace_emit_" << ev.name << "(";
            for (size_t i = 0; i < ev.fields.size(); ++i) {
                if (i > 0) h << ", ";
                h << ev.fields[i].c_type << " " << ev.fields[i].name;
            }
            h << ") {\n";
            for (const auto &fld : ev.fields) {
                if (fld.is_pointer) {
                    std::string ptype = fld.c_type;
                    if (ptype.rfind("const ", 0) == 0) ptype = ptype.substr(6);
                    if (!ptype.empty() && ptype.back() == '*') ptype.pop_back();
                    while (!ptype.empty() && ptype.back() == ' ') ptype.pop_back();
                    h << "    " << fld.c_type << " _safe_" << fld.name << " = " << fld.name << ";\n";
                    h << "    if (!_safe_" << fld.name << ") {\n";
                    h << "        static const " << ptype << " _dummy_" << fld.name << " = {0};\n";
                    h << "        _safe_" << fld.name << " = &_dummy_" << fld.name << ";\n";
                    h << "    }\n";
                }
            }
            h << "    uint16_t _ev_id = " << ev.id << ";\n";
            h << "    uint64_t _ts = ctrace_now_ns();\n";

            // Compute variable lengths
            for (const auto &ff : flatList) {
                if (ff.type_kind == "string") {
                    h << "    uint32_t _len_" << ff.flat_name << " = " << ff.accessor_expr << " ? (uint32_t)strlen(" << ff.accessor_expr << ") : 0;\n";
                } else if (ff.type_kind == "bytes") {
                    h << "    uint32_t _len_" << ff.flat_name << " = (uint32_t)" << ff.accessor_expr << ".len;\n";
                } else if (ff.type_kind == "counted_array") {
                    std::string cnt = getCountExpr(ff.accessor_expr, ff.counted_by);
                    uint64_t esz = ff.elem_size > 0 ? ff.elem_size : 1;
                    h << "    uint32_t _len_" << ff.flat_name << " = (uint32_t)(" << cnt << ") * " << esz << ";\n";
                }
            }

            // Total size computation
            h << "    size_t _total_len = sizeof(uint16_t) + sizeof(uint64_t)";
            for (const auto &ff : flatList) {
                if (ff.type_kind == "string") {
                    h << " + sizeof(uint32_t) + _len_" << ff.flat_name;
                } else if (ff.type_kind == "bytes" || ff.type_kind == "counted_array") {
                    h << " + sizeof(uint32_t) + _len_" << ff.flat_name;
                } else {
                    h << " + " << ff.size;
                }
            }
            h << ";\n";

            h << "    char *_buf = (char *)ctrace_bin_reserve(_total_len);\n";
            h << "    if (!_buf) return;\n";
            h << "    char *_p = _buf;\n";
            h << "    memcpy(_p, &_ev_id, sizeof(_ev_id)); _p += sizeof(_ev_id);\n";
            h << "    memcpy(_p, &_ts, sizeof(_ts)); _p += sizeof(_ts);\n";

            // Copy field data
            for (const auto &ff : flatList) {
                if (ff.type_kind == "string") {
                    h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;\n";
                    h << "    if (_len_" << ff.flat_name << " > 0) { memcpy(_p, " << ff.accessor_expr << ", _len_" << ff.flat_name << "); _p += _len_" << ff.flat_name << "; }\n";
                } else if (ff.type_kind == "bytes") {
                    h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;\n";
                    h << "    if (_len_" << ff.flat_name << " > 0 && " << ff.accessor_expr << ".ptr) { memcpy(_p, " << ff.accessor_expr << ".ptr, _len_" << ff.flat_name << "); _p += _len_" << ff.flat_name << "; }\n";
                } else if (ff.type_kind == "counted_array") {
                    h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;\n";
                    h << "    if (_len_" << ff.flat_name << " > 0 && " << ff.accessor_expr << ") { memcpy(_p, " << ff.accessor_expr << ", _len_" << ff.flat_name << "); _p += _len_" << ff.flat_name << "; }\n";
                } else if (ff.type_kind == "bool") {
                    h << "    uint8_t _val_" << ff.flat_name << " = " << ff.accessor_expr << " ? 1 : 0;\n";
                    h << "    memcpy(_p, &_val_" << ff.flat_name << ", 1); _p += 1;\n";
                } else {
                    h << "    memcpy(_p, &(" << ff.accessor_expr << "), " << ff.size << "); _p += " << ff.size << ";\n";
                }
            }

            h << "    ctrace_bin_commit(_buf, _total_len);\n";
            h << "}\n\n";
        }
    } else if (backend == "ctf") {
        h << "void *ctrace_ctf_reserve(size_t len);\n";
        h << "void ctrace_ctf_commit(void *ptr, size_t len);\n";
        h << "void ctrace_ctf_set_metadata(const char *metadata);\n\n";

        // Embed CTF TSDL metadata as a byte array
        h << "static const char __ctrace_ctf_metadata[] = {\n    ";
        for (size_t i = 0; i < ctfTsdl.size(); ++i) {
            h << (int)(unsigned char)ctfTsdl[i] << ", ";
            if ((i + 1) % 20 == 0) h << "\n    ";
        }
        h << "0\n};\n\n";

        h << "__attribute__((constructor)) static void __ctrace_ctf_auto_init(void) {\n";
        h << "    ctrace_ctf_set_metadata(__ctrace_ctf_metadata);\n";
        h << "}\n\n";

        for (const auto &pair : events) {
            const Event &ev = pair.second;
            std::vector<FlatField> flatList = flattenEventFields(ev.fields, true);

            h << "static inline void __ctrace_emit_" << ev.name << "(";
            for (size_t i = 0; i < ev.fields.size(); ++i) {
                if (i > 0) h << ", ";
                h << ev.fields[i].c_type << " " << ev.fields[i].name;
            }
            h << ") {\n";
            for (const auto &fld : ev.fields) {
                if (fld.is_pointer) {
                    std::string ptype = fld.c_type;
                    if (ptype.rfind("const ", 0) == 0) ptype = ptype.substr(6);
                    if (!ptype.empty() && ptype.back() == '*') ptype.pop_back();
                    while (!ptype.empty() && ptype.back() == ' ') ptype.pop_back();
                    h << "    " << fld.c_type << " _safe_" << fld.name << " = " << fld.name << ";\n";
                    h << "    if (!_safe_" << fld.name << ") {\n";
                    h << "        static const " << ptype << " _dummy_" << fld.name << " = {0};\n";
                    h << "        _safe_" << fld.name << " = &_dummy_" << fld.name << ";\n";
                    h << "    }\n";
                }
            }
            h << "    uint16_t _ev_id = " << ev.id << ";\n";
            h << "    uint64_t _ts = ctrace_now_ns();\n";

            // Compute variable lengths
            for (const auto &ff : flatList) {
                if (ff.type_kind == "string") {
                    h << "    uint32_t _len_" << ff.flat_name << " = " << ff.accessor_expr << " ? (uint32_t)strlen(" << ff.accessor_expr << ") : 0;\n";
                } else if (ff.type_kind == "bytes") {
                    h << "    uint32_t _len_" << ff.flat_name << " = (uint32_t)" << ff.accessor_expr << ".len;\n";
                } else if (ff.type_kind == "counted_array") {
                    std::string cnt = getCountExpr(ff.accessor_expr, ff.counted_by);
                    uint64_t esz = ff.elem_size > 0 ? ff.elem_size : 1;
                    h << "    uint32_t _len_" << ff.flat_name << " = (uint32_t)(" << cnt << ") * " << esz << ";\n";
                }
            }

            // Total size computation
            h << "    size_t _total_len = sizeof(uint16_t) + sizeof(uint64_t)";
            for (const auto &ff : flatList) {
                if (ff.type_kind == "string") {
                    // In CTF, null-terminated string = len + 1
                    h << " + _len_" << ff.flat_name << " + 1";
                } else if (ff.type_kind == "bytes" || ff.type_kind == "counted_array") {
                    // In CTF, 4 bytes len + payload
                    h << " + sizeof(uint32_t) + _len_" << ff.flat_name;
                } else if (ff.type_kind == "enum") {
                    h << " + 4";
                } else if (ff.type_kind == "bool") {
                    h << " + 1";
                } else {
                    h << " + " << ff.size;
                }
            }
            h << ";\n";

            h << "    char *_buf = (char *)ctrace_ctf_reserve(_total_len);\n";
            h << "    if (!_buf) return;\n";
            h << "    char *_p = _buf;\n";
            h << "    memcpy(_p, &_ev_id, sizeof(_ev_id)); _p += sizeof(_ev_id);\n";
            h << "    memcpy(_p, &_ts, sizeof(_ts)); _p += sizeof(_ts);\n";

            // Copy field data
            for (const auto &ff : flatList) {
                if (ff.type_kind == "string") {
                    h << "    if (_len_" << ff.flat_name << " > 0) {\n";
                    h << "        memcpy(_p, " << ff.accessor_expr << ", _len_" << ff.flat_name << ");\n";
                    h << "        _p += _len_" << ff.flat_name << ";\n";
                    h << "    }\n";
                    h << "    *_p++ = '\\0';\n";
                } else if (ff.type_kind == "bytes") {
                    h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;\n";
                    h << "    if (_len_" << ff.flat_name << " > 0 && " << ff.accessor_expr << ".ptr) {\n";
                    h << "        memcpy(_p, " << ff.accessor_expr << ".ptr, _len_" << ff.flat_name << ");\n";
                    h << "        _p += _len_" << ff.flat_name << ";\n";
                    h << "    }\n";
                } else if (ff.type_kind == "counted_array") {
                    h << "    memcpy(_p, &_len_" << ff.flat_name << ", 4); _p += 4;\n";
                    h << "    if (_len_" << ff.flat_name << " > 0 && " << ff.accessor_expr << ") {\n";
                    h << "        memcpy(_p, " << ff.accessor_expr << ", _len_" << ff.flat_name << ");\n";
                    h << "        _p += _len_" << ff.flat_name << ";\n";
                    h << "    }\n";
                } else if (ff.type_kind == "bool") {
                    h << "    uint8_t _val_" << ff.flat_name << " = " << ff.accessor_expr << " ? 1 : 0;\n";
                    h << "    memcpy(_p, &_val_" << ff.flat_name << ", 1); _p += 1;\n";
                } else if (ff.type_kind == "enum") {
                    h << "    int32_t _val_" << ff.flat_name << " = (int32_t)" << ff.accessor_expr << ";\n";
                    h << "    memcpy(_p, &_val_" << ff.flat_name << ", 4); _p += 4;\n";
                } else {
                    h << "    memcpy(_p, &(" << ff.accessor_expr << "), " << ff.size << "); _p += " << ff.size << ";\n";
                }
            }

            h << "    ctrace_ctf_commit(_buf, _total_len);\n";
            h << "}\n\n";
        }
    } else if (backend == "sqlite") {
        h << "#include <sqlite3.h>\n\n";
        h << "void ctrace_sqlite_lock(void);\n";
        h << "void ctrace_sqlite_unlock(void);\n";
        h << "void ctrace_sqlite_step_done(void);\n";
        h << "void ctrace_sqlite_register_init(void (*fn)(sqlite3 *db));\n\n";

        // Declare statement pointers
        for (const auto &pair : events) {
            h << "extern sqlite3_stmt *__ctrace_stmt_" << pair.second.name << ";\n";
        }
        h << "\n";

        // Define statement pointers as weak symbols
        for (const auto &pair : events) {
            h << "__attribute__((weak)) sqlite3_stmt *__ctrace_stmt_" << pair.second.name << " = NULL;\n";
        }
        h << "\n";

        // Table creation and statement prep function
        h << "__attribute__((weak)) void __ctrace_sqlite_init_events(sqlite3 *db) {\n";
        h << "    if (!db) return;\n";
        for (const auto &pair : events) {
            const Event &ev = pair.second;
            std::vector<FlatField> flatList = flattenEventFields(ev.fields);

            // DDL: CREATE TABLE IF NOT EXISTS ctrace_<name> (ts_ns INTEGER, f1 TYPE1, ...)
            std::ostringstream ddl;
            ddl << "CREATE TABLE IF NOT EXISTS ctrace_" << ev.name << " (ts_ns INTEGER";
            for (const auto &ff : flatList) {
                ddl << ", " << ff.flat_name << " " << sqliteColumnType(ff.type_kind);
            }
            ddl << ");";

            h << "    sqlite3_exec(db, \"" << ddl.str() << "\", NULL, NULL, NULL);\n";

            // Statement: INSERT INTO ctrace_<name> VALUES (?, ?, ...)
            std::ostringstream sql;
            sql << "INSERT INTO ctrace_" << ev.name << " VALUES (?";
            for (size_t i = 0; i < flatList.size(); ++i) {
                sql << ", ?";
            }
            sql << ");";

            h << "    if (!__ctrace_stmt_" << ev.name << ") {\n";
            h << "        sqlite3_prepare_v2(db, \"" << sql.str() << "\", -1, &__ctrace_stmt_" << ev.name << ", NULL);\n";
            h << "    }\n";
        }
        h << "}\n\n";

        h << "__attribute__((constructor)) static void __ctrace_sqlite_auto_register(void) {\n";
        h << "    ctrace_sqlite_register_init(__ctrace_sqlite_init_events);\n";
        h << "}\n\n";

        // Generate sqlite emitters
        for (const auto &pair : events) {
            const Event &ev = pair.second;
            std::vector<FlatField> flatList = flattenEventFields(ev.fields, true);

            h << "static inline void __ctrace_emit_" << ev.name << "(";
            for (size_t i = 0; i < ev.fields.size(); ++i) {
                if (i > 0) h << ", ";
                h << ev.fields[i].c_type << " " << ev.fields[i].name;
            }
            h << ") {\n";
            for (const auto &fld : ev.fields) {
                if (fld.is_pointer) {
                    std::string ptype = fld.c_type;
                    if (ptype.rfind("const ", 0) == 0) ptype = ptype.substr(6);
                    if (!ptype.empty() && ptype.back() == '*') ptype.pop_back();
                    while (!ptype.empty() && ptype.back() == ' ') ptype.pop_back();
                    h << "    " << fld.c_type << " _safe_" << fld.name << " = " << fld.name << ";\n";
                    h << "    if (!_safe_" << fld.name << ") {\n";
                    h << "        static const " << ptype << " _dummy_" << fld.name << " = {0};\n";
                    h << "        _safe_" << fld.name << " = &_dummy_" << fld.name << ";\n";
                    h << "    }\n";
                }
            }
            h << "    uint64_t _ts = ctrace_now_ns();\n";
            h << "    ctrace_sqlite_lock();\n";
            h << "    sqlite3_stmt *_st = __ctrace_stmt_" << ev.name << ";\n";
            h << "    if (_st) {\n";
            h << "        sqlite3_bind_int64(_st, 1, (sqlite3_int64)_ts);\n";

            int col = 2;
            for (const auto &ff : flatList) {
                if (ff.type_kind == "string") {
                    h << "        sqlite3_bind_text(_st, " << col++ << ", " << ff.accessor_expr << " ? " << ff.accessor_expr << " : \"\", -1, SQLITE_TRANSIENT);\n";
                } else if (ff.type_kind == "bytes") {
                    h << "        sqlite3_bind_blob(_st, " << col++ << ", " << ff.accessor_expr << ".ptr, (int)" << ff.accessor_expr << ".len, SQLITE_TRANSIENT);\n";
                } else if (ff.type_kind == "counted_array") {
                    std::string cnt = getCountExpr(ff.accessor_expr, ff.counted_by);
                    uint64_t esz = ff.elem_size > 0 ? ff.elem_size : 1;
                    h << "        sqlite3_bind_blob(_st, " << col++ << ", " << ff.accessor_expr << ", (int)((size_t)(" << cnt << ") * " << esz << "), SQLITE_TRANSIENT);\n";
                } else if (ff.type_kind == "float" || ff.type_kind == "double") {
                    h << "        sqlite3_bind_double(_st, " << col++ << ", (double)" << ff.accessor_expr << ");\n";
                } else if (ff.type_kind == "bool") {
                    h << "        sqlite3_bind_int(_st, " << col++ << ", " << ff.accessor_expr << " ? 1 : 0);\n";
                } else {
                    // Integer or Enum
                    if (ff.is_signed) {
                        h << "        sqlite3_bind_int64(_st, " << col++ << ", (sqlite3_int64)" << ff.accessor_expr << ");\n";
                    } else {
                        h << "        sqlite3_bind_int64(_st, " << col++ << ", (sqlite3_int64)(uint64_t)" << ff.accessor_expr << ");\n";
                    }
                }
            }

            h << "        sqlite3_step(_st);\n";
            h << "        sqlite3_reset(_st);\n";
            h << "        ctrace_sqlite_step_done();\n";
            h << "    }\n";
            h << "    ctrace_sqlite_unlock();\n";
            h << "}\n\n";
        }
    }

    if (events.empty()) {
        if (backend == "sqlite") {
            h << "static inline void __ctrace_sqlite_init_events(void *db) { (void)db; }\n";
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
