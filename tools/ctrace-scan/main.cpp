#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>

#include "ctrace_json.hpp"

using json = ctrace::json;
using namespace clang;
using namespace clang::tooling;

namespace {

struct EnumConstant {
    std::string name;
    int64_t value = 0;
};

struct FieldInfo {
    std::string name;
    std::string type_kind; // int8..int64, uint8..uint64, float, double, bool, string, bytes, enum, struct, counted_array
    std::string c_type;
    uint64_t size = 0;
    bool is_signed = false;
    bool is_pointer = false;
    int array_index = -1;
    std::string array_parent;
    std::string counted_by;

    // For enums
    std::string enum_name;
    std::vector<EnumConstant> enum_constants;

    // For structs / counted_array
    std::string struct_name;
    std::vector<FieldInfo> struct_fields;
};

struct EventInfo {
    std::string name;
    std::string file;
    unsigned line = 0;
    unsigned column = 0;
    std::vector<FieldInfo> fields;
};

std::map<std::tuple<std::string, std::string, unsigned, unsigned>, EventInfo> g_collected_events;

struct UnionCaseBinding {
    std::string tag_field;
    std::string tag_val_str;
    int64_t tag_val = 0;
    bool has_numeric_tag = false;
    std::string member_name;
};

std::map<std::pair<std::string, std::string>, std::string> g_bound_counts;
std::map<std::string, std::vector<UnionCaseBinding>> g_bound_unions;

std::string stripStructPrefix(const std::string &s) {
    if (s.rfind("struct ", 0) == 0) return s.substr(7);
    if (s.rfind("union ", 0) == 0) return s.substr(6);
    return s;
}

std::string getCountedByField(const FieldDecl *FD) {
    if (!FD) return "";
    for (const auto *A : FD->specific_attrs<AnnotateAttr>()) {
        std::string ann = A->getAnnotation().str();
        if (ann.rfind("ctrace:counted_by:", 0) == 0) {
            return ann.substr(18);
        }
    }
    return "";
}

std::string getSanitizedMemberPath(const Expr *e) {
    if (!e) return "";
    e = e->IgnoreParenImpCasts();
    if (const auto *ME = dyn_cast<MemberExpr>(e)) {
        std::string base = getSanitizedMemberPath(ME->getBase());
        std::string member = ME->getMemberDecl()->getNameAsString();
        if (base.empty()) return member;
        return base + "_" + member;
    }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(e)) {
        return DRE->getDecl()->getNameAsString();
    }
    return "";
}

std::string extractFieldName(const Expr *rawExpr, unsigned fieldIdx) {
    if (!rawExpr) return "arg_" + std::to_string(fieldIdx);
    const Expr *e = rawExpr->IgnoreParenImpCasts();

    if (const auto *UO = dyn_cast<UnaryOperator>(e)) {
        if (UO->getOpcode() == UO_Deref || UO->getOpcode() == UO_AddrOf) {
            return extractFieldName(UO->getSubExpr(), fieldIdx);
        }
    }

    if (const auto *DRE = dyn_cast<DeclRefExpr>(e)) {
        std::string name = DRE->getDecl()->getNameAsString();
        if (!name.empty()) return name;
    }

    std::string memberPath = getSanitizedMemberPath(e);
    if (!memberPath.empty()) {
        return memberPath;
    }

    return "arg_" + std::to_string(fieldIdx);
}

bool isCtraceBytesType(QualType QT, ASTContext &Ctx) {
    std::string s1 = QT.getAsString();
    if (s1.find("ctrace_bytes_t") != std::string::npos) return true;
    std::string s2 = QT.getDesugaredType(Ctx).getAsString();
    if (s2.find("ctrace_bytes_t") != std::string::npos) return true;
    if (const auto *RT = QT->getAs<RecordType>()) {
        if (RT->getDecl()->getNameAsString() == "ctrace_bytes_t") return true;
    }
    return false;
}

// Returns true ONLY for plain 'char' (Char_S / Char_U), NOT 'signed char' (int8_t) or 'unsigned char' (uint8_t)
bool isPlainCharType(QualType QT) {
    QualType canon = QT.getCanonicalType();
    if (const auto *BT = canon->getAs<BuiltinType>()) {
        return BT->getKind() == BuiltinType::Char_S || BT->getKind() == BuiltinType::Char_U;
    }
    return false;
}

bool isStringType(QualType QT, ASTContext &Ctx) {
    if (QT->isPointerType()) {
        QualType p = QT->getPointeeType();
        if (isPlainCharType(p)) return true;
    }
    if (const ArrayType *AT = Ctx.getAsArrayType(QT)) {
        if (isPlainCharType(AT->getElementType())) return true;
    }
    return false;
}

FieldInfo resolveFieldType(QualType qualType, const std::string &name, ASTContext &Ctx, int depth = 0) {
    FieldInfo f;
    f.name = name;
    f.c_type = qualType.getAsString();
    QualType canon = qualType.getCanonicalType();

    // 1. Check ctrace_bytes_t
    if (isCtraceBytesType(qualType, Ctx)) {
        f.type_kind = "bytes";
        f.c_type = "ctrace_bytes_t";
        f.size = Ctx.getTypeSize(canon) / 8;
        return f;
    }

    // 2. Check string (char*, const char*, char[N], typedef char[N])
    if (isStringType(qualType, Ctx)) {
        f.type_kind = "string";
        f.c_type = "const char *";
        if (const auto *CAT = Ctx.getAsConstantArrayType(qualType)) {
            f.is_pointer = false;
            f.size = CAT->getSize().getZExtValue();
        } else {
            f.is_pointer = true;
            f.size = Ctx.getTypeSize(Ctx.VoidPtrTy) / 8;
        }
        return f;
    }

    // 3. Check bool
    if (canon->isBooleanType()) {
        f.type_kind = "bool";
        f.c_type = "bool";
        f.size = 1;
        return f;
    }

    // 4. Check enum
    if (canon->isEnumeralType()) {
        f.type_kind = "enum";
        const auto *ET = canon->getAs<EnumType>();
        const EnumDecl *ED = ET->getDecl()->getDefinition();
        if (!ED) ED = ET->getDecl();

        f.enum_name = ED->getNameAsString();
        if (f.enum_name.empty()) {
            if (const auto *TND = ED->getTypedefNameForAnonDecl()) {
                f.enum_name = TND->getNameAsString();
            }
        }
        if (f.enum_name.empty()) {
            if (const auto *TT = qualType->getAs<TypedefType>()) {
                f.enum_name = TT->getDecl()->getNameAsString();
            }
        }
        QualType intTy = ED->getIntegerType();
        if (intTy.isNull()) {
            intTy = Ctx.IntTy;
        }
        f.c_type = f.enum_name.empty() ? intTy.getAsString() : (ED->getNameAsString().empty() ? f.enum_name : "enum " + f.enum_name);
        f.size = Ctx.getTypeSize(canon) / 8;
        f.is_signed = intTy->isSignedIntegerType();

        for (auto it = ED->enumerator_begin(); it != ED->enumerator_end(); ++it) {
            EnumConstant ec;
            ec.name = it->getNameAsString();
            const llvm::APSInt &val = it->getInitVal();
            ec.value = val.isSigned() ? val.getSExtValue() : (int64_t)val.getZExtValue();
            f.enum_constants.push_back(ec);
        }
        return f;
    }

    // 5. Check struct (exclude union via isStructureType())
    if (canon->isStructureType() && depth < 4) {
        f.type_kind = "struct";
        const auto *RT = canon->getAs<RecordType>();
        const RecordDecl *RD = RT->getDecl()->getDefinition();
        if (!RD) RD = RT->getDecl();

        f.struct_name = RD->getNameAsString();
        if (f.struct_name.empty()) {
            if (const auto *TND = RD->getTypedefNameForAnonDecl()) {
                f.struct_name = TND->getNameAsString();
            }
        }
        if (f.struct_name.empty()) {
            if (const auto *TT = qualType->getAs<TypedefType>()) {
                f.struct_name = TT->getDecl()->getNameAsString();
            }
        }
        f.c_type = f.struct_name.empty() ? qualType.getAsString() : (RD->getNameAsString().empty() ? f.struct_name : "struct " + f.struct_name);
        f.size = Ctx.getTypeSize(canon) / 8;

        std::string sName = stripStructPrefix(f.struct_name);

        for (const FieldDecl *FD : RD->fields()) {
            if (FD->isBitField()) {
                continue;
            }
            std::string fieldName = FD->getNameAsString();
            QualType ft = FD->getType();
            QualType ftCanon = ft.getCanonicalType();

            // Check if field is a union (named or anonymous)
            if (ftCanon->isUnionType()) {
                std::string uName = fieldName.empty() ? "u" : fieldName;
                const auto *RT = ftCanon->getAs<RecordType>();
                const RecordDecl *UD = RT ? RT->getDecl() : nullptr;
                if (UD && UD->getDefinition()) UD = UD->getDefinition();
                uint64_t unionSize = Ctx.getTypeSize(ftCanon) / 8;

                auto itU = g_bound_unions.find(sName);
                if (itU != g_bound_unions.end() && !itU->second.empty()) {
                    FieldInfo uf;
                    uf.name = uName;
                    uf.type_kind = "union";
                    uf.c_type = ft.getAsString();
                    uf.size = unionSize;
                    uf.counted_by = itU->second[0].tag_field;

                    if (UD) {
                        for (const auto &ub : itU->second) {
                            for (const FieldDecl *UFD : UD->fields()) {
                                if (UFD->getNameAsString() == ub.member_name) {
                                    FieldInfo mInfo = resolveFieldType(UFD->getType(), ub.member_name, Ctx, depth + 1);
                                    mInfo.array_parent = ub.tag_val_str;
                                    mInfo.array_index = (int)ub.tag_val;
                                    uf.struct_fields.push_back(mInfo);
                                    break;
                                }
                            }
                        }
                    }
                    f.struct_fields.push_back(uf);
                    continue;
                } else {
                    // Fallback: unannotated union is preserved as raw bytes of sizeof(union)
                    FieldInfo uf;
                    uf.name = uName;
                    uf.type_kind = "bytes";
                    uf.c_type = ft.getAsString();
                    uf.size = unionSize;
                    f.struct_fields.push_back(uf);
                    continue;
                }
            }

            if (fieldName.empty()) {
                continue;
            }

            // Check count binding (CTRACE_COUNTED_BY or CTRACE_BIND_COUNT)
            std::string countField = getCountedByField(FD);
            if (countField.empty() && !sName.empty()) {
                auto itC = g_bound_counts.find({sName, fieldName});
                if (itC != g_bound_counts.end()) {
                    countField = itC->second;
                }
            }

            if (!countField.empty()) {
                QualType elemType;
                if (ft->isPointerType()) {
                    elemType = ft->getPointeeType();
                } else if (ftCanon->isIncompleteArrayType()) {
                    const auto *IAT = Ctx.getAsIncompleteArrayType(ftCanon);
                    elemType = IAT->getElementType();
                } else if (ftCanon->isConstantArrayType()) {
                    const auto *CAT = Ctx.getAsConstantArrayType(ftCanon);
                    elemType = CAT->getElementType();
                }
                if (!elemType.isNull()) {
                    FieldInfo elem = resolveFieldType(elemType, fieldName + "_elem", Ctx, depth + 1);
                    FieldInfo cf;
                    cf.name = fieldName;
                    cf.type_kind = "counted_array";
                    cf.c_type = ft.getAsString();
                    cf.counted_by = countField;
                    cf.is_pointer = ft->isPointerType();
                    cf.size = elem.size;
                    cf.struct_fields.push_back(elem);
                    f.struct_fields.push_back(cf);
                    continue;
                }
            }

            // Skip raw pointers inside struct (unless string)
            if (ft->isPointerType() && !isStringType(ft, Ctx)) {
                continue;
            }
            // Skip flexible array members without CTRACE_COUNTED_BY
            if (ftCanon->isIncompleteArrayType()) {
                continue;
            }
            if (ftCanon->isStructureType() && depth + 1 >= 4 && !isCtraceBytesType(ft, Ctx)) {
                continue;
            }

            // Fixed-size arrays
            if (const auto *CAT = Ctx.getAsConstantArrayType(ft)) {
                QualType elemType = CAT->getElementType();
                if (!isPlainCharType(elemType)) {
                    // Skip multi-dimensional arrays inside structs
                    if (elemType->isArrayType() || elemType->isUnionType()) {
                        continue;
                    }
                    uint64_t arrLen = CAT->getSize().getZExtValue();
                    if (arrLen > 4) {
                        // Fixed array (Gap 3): preserve as contiguous array rather than flattening
                        FieldInfo elem = resolveFieldType(elemType, fieldName + "_elem", Ctx, depth + 1);
                        FieldInfo af;
                        af.name = fieldName;
                        af.type_kind = "fixed_array";
                        af.c_type = ft.getAsString();
                        af.size = arrLen * elem.size;
                        af.array_index = -1;
                        af.array_parent = std::to_string(arrLen);
                        af.struct_fields.push_back(elem);
                        f.struct_fields.push_back(af);
                        continue;
                    } else {
                        // Small arrays (<= 4 elements) unroll for convenient scalar field access
                        for (uint64_t idx = 0; idx < arrLen; ++idx) {
                            std::string elemName = fieldName + "_" + std::to_string(idx);
                            FieldInfo sub = resolveFieldType(elemType, elemName, Ctx, depth + 1);
                            sub.array_index = (int)idx;
                            sub.array_parent = fieldName;
                            f.struct_fields.push_back(sub);
                        }
                        continue;
                    }
                }
            }

            FieldInfo sub = resolveFieldType(ft, fieldName, Ctx, depth + 1);
            f.struct_fields.push_back(sub);
        }
        return f;
    }

    // 6. Check floating point
    if (canon->isRealFloatingType()) {
        uint64_t bits = Ctx.getTypeSize(canon);
        if (bits == 32) {
            f.type_kind = "float";
            f.c_type = "float";
            f.size = 4;
        } else {
            f.type_kind = "double";
            f.c_type = "double";
            f.size = 8;
        }
        return f;
    }

    // 7. Check integer
    if (canon->isIntegerType()) {
        uint64_t bits = Ctx.getTypeSize(canon);
        bool isSigned = canon->isSignedIntegerType();
        f.is_signed = isSigned;
        f.size = bits / 8;

        if (bits <= 8) {
            f.type_kind = isSigned ? "int8" : "uint8";
            f.c_type = isSigned ? "int8_t" : "uint8_t";
        } else if (bits <= 16) {
            f.type_kind = isSigned ? "int16" : "uint16";
            f.c_type = isSigned ? "int16_t" : "uint16_t";
        } else if (bits <= 32) {
            f.type_kind = isSigned ? "int32" : "uint32";
            f.c_type = isSigned ? "int32_t" : "uint32_t";
        } else {
            f.type_kind = isSigned ? "int64" : "uint64";
            f.c_type = isSigned ? "int64_t" : "uint64_t";
        }
        return f;
    }

    // 8. Pointers (struct pointers or raw data pointers like void*, uint8_t*, int*)
    if (canon->isPointerType()) {
        QualType pointee = canon->getPointeeType();
        QualType pCanon = pointee.getCanonicalType();
        if (pCanon->isStructureType() && depth < 4) {
            FieldInfo s = resolveFieldType(pointee, name, Ctx, depth);
            s.is_pointer = true;
            s.c_type = qualType.getAsString();
            return s;
        }
        uint64_t ptrBytes = Ctx.getTypeSize(canon) / 8;
        f.type_kind = (ptrBytes == 4) ? "uint32" : "uint64";
        f.c_type = "const void *";
        f.is_pointer = true;
        f.size = ptrBytes;
        return f;
    }

    // Generic fallback
    f.type_kind = "uint64";
    f.c_type = "uint64_t";
    f.size = 8;
    return f;
}

class TraceASTVisitor : public RecursiveASTVisitor<TraceASTVisitor> {
public:
    explicit TraceASTVisitor(ASTContext *Context) : Ctx(Context) {}

    bool VisitCallExpr(CallExpr *Call) {
        const FunctionDecl *FD = Call->getDirectCallee();
        if (!FD) return true;

        if (FD->getNameAsString() != "__ctrace_hook") {
            return true;
        }

        if (Call->getNumArgs() < 1) return true;

        // Arg 0 is the event name string literal
        const Expr *arg0 = Call->getArg(0)->IgnoreParenImpCasts();
        std::string eventName;
        if (const auto *SL = dyn_cast<StringLiteral>(arg0)) {
            eventName = SL->getString().str();
        } else {
            return true;
        }

        SourceManager &SM = Ctx->getSourceManager();
        SourceLocation loc = Call->getBeginLoc();
        SourceLocation expLoc = SM.getExpansionLoc(loc);
        PresumedLoc pLoc = SM.getPresumedLoc(expLoc);

        std::string filename = pLoc.isValid() ? pLoc.getFilename() : "";
        unsigned line = pLoc.isValid() ? pLoc.getLine() : 0;
        unsigned column = pLoc.isValid() ? pLoc.getColumn() : 0;

        // Canonicalize file path
        llvm::SmallString<256> realPath;
        if (!filename.empty() && !llvm::sys::fs::real_path(filename, realPath)) {
            filename = realPath.str().str();
        }

        EventInfo ev;
        ev.name = eventName;
        ev.file = filename;
        ev.line = line;
        ev.column = column;

        unsigned numArgs = Call->getNumArgs();
        std::set<std::string> usedNames;

        for (unsigned i = 1; i < numArgs; ++i) {
            const Expr *argExpr = Call->getArg(i);
            const Expr *unpromoted = argExpr->IgnoreParenImpCasts();
            std::string fieldName = extractFieldName(argExpr, i - 1);

            // Ensure field name is unique
            std::string uniqueName = fieldName;
            int counter = 1;
            while (usedNames.count(uniqueName)) {
                uniqueName = fieldName + "_" + std::to_string(counter++);
            }
            usedNames.insert(uniqueName);

            // Use the unpromoted expression's type to accurately detect enums, bools, etc.
            QualType typeToResolve = unpromoted->getType();
            if (isStringType(argExpr->getType(), *Ctx)) {
                typeToResolve = argExpr->getType();
            }

            FieldInfo f = resolveFieldType(typeToResolve, uniqueName, *Ctx);
            // Top-level string arguments always decay to const char * at call site
            if (f.type_kind == "string") {
                f.is_pointer = true;
                f.size = Ctx->getTypeSize(Ctx->VoidPtrTy) / 8;
            }
            ev.fields.push_back(f);
        }

        // Deduplicate by (event_name, canonical_file, line, column)
        auto key = std::make_tuple(ev.name, ev.file, ev.line, ev.column);
        g_collected_events[key] = ev;

        return true;
    }

private:
    ASTContext *Ctx;
};

class BindingASTVisitor : public RecursiveASTVisitor<BindingASTVisitor> {
public:
    explicit BindingASTVisitor(ASTContext *Context) : Ctx(Context) {}

    bool VisitVarDecl(VarDecl *VD) {
        if (!VD) return true;
        for (const auto *A : VD->specific_attrs<AnnotateAttr>()) {
            std::string ann = A->getAnnotation().str();
            if (ann.rfind("ctrace:bind_count:", 0) == 0) {
                // format: ctrace:bind_count:type:array_field:count_field
                std::vector<std::string> parts;
                std::stringstream ss(ann);
                std::string item;
                while (std::getline(ss, item, ':')) parts.push_back(item);
                if (parts.size() >= 5) {
                    std::string typeName = stripStructPrefix(parts[2]);
                    g_bound_counts[{typeName, parts[3]}] = parts[4];
                }
            } else if (ann.rfind("ctrace:bind_union:", 0) == 0) {
                // format: ctrace:bind_union:type:tag_field:tag_val:member_name
                std::vector<std::string> parts;
                std::stringstream ss(ann);
                std::string item;
                while (std::getline(ss, item, ':')) parts.push_back(item);
                if (parts.size() >= 6) {
                    std::string typeName = stripStructPrefix(parts[2]);
                    UnionCaseBinding ub;
                    ub.tag_field = parts[3];
                    ub.tag_val_str = parts[4];
                    ub.member_name = parts[5];
                    try {
                        ub.tag_val = std::stoll(ub.tag_val_str, nullptr, 0);
                        ub.has_numeric_tag = true;
                    } catch (...) {}
                    g_bound_unions[typeName].push_back(ub);
                }
            } else if (ann.rfind("ctrace:bind_union_val:", 0) == 0) {
                // evaluate integer value if available
                if (VD->hasInit()) {
                    Expr::EvalResult res;
                    if (VD->getInit()->EvaluateAsInt(res, *Ctx)) {
                        int64_t val = res.Val.getInt().getExtValue();
                        std::vector<std::string> parts;
                        std::stringstream ss(ann);
                        std::string item;
                        while (std::getline(ss, item, ':')) parts.push_back(item);
                        if (parts.size() >= 5) {
                            std::string typeName = stripStructPrefix(parts[2]);
                            std::string memberName = parts[4];
                            for (auto &ub : g_bound_unions[typeName]) {
                                if (ub.member_name == memberName) {
                                    ub.tag_val = val;
                                    ub.has_numeric_tag = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

private:
    ASTContext *Ctx;
};

class TraceASTConsumer : public ASTConsumer {
public:
    explicit TraceASTConsumer(ASTContext *Context) : BindingVisitor(Context), Visitor(Context) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        BindingVisitor.TraverseDecl(Context.getTranslationUnitDecl());
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    BindingASTVisitor BindingVisitor;
    TraceASTVisitor Visitor;
};

class TraceFrontendAction : public ASTFrontendAction {
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
        return std::make_unique<TraceASTConsumer>(&CI.getASTContext());
    }
};

static llvm::cl::OptionCategory CtraceScanCategory("ctrace-scan options");
static llvm::cl::opt<std::string> OutputFilename("o", llvm::cl::desc("Output JSON trace file"), llvm::cl::value_desc("filename"), llvm::cl::cat(CtraceScanCategory));
static llvm::cl::opt<bool> ForceScan("force-scan", llvm::cl::desc("Force full AST scan even if file has no trace symbols"), llvm::cl::cat(CtraceScanCategory));

bool fileHasTraceSymbols(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return true;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (content.find("TRACE_EVENT") != std::string::npos ||
        content.find("__ctrace_hook") != std::string::npos ||
        content.find("CTRACE_") != std::string::npos) {
        return true;
    }
    return false;
}

json fieldInfoToJson(const FieldInfo &f) {
    json j;
    j["name"] = f.name;
    j["type_kind"] = f.type_kind;
    j["c_type"] = f.c_type;
    j["size"] = f.size;
    j["is_signed"] = f.is_signed;
    j["is_pointer"] = f.is_pointer;
    if (!f.counted_by.empty()) {
        j["counted_by"] = f.counted_by;
    }
    if (f.array_index >= 0) {
        j["array_index"] = f.array_index;
        j["array_parent"] = f.array_parent;
    }

    if (f.type_kind == "enum") {
        j["enum_name"] = f.enum_name;
        json constants = json::array();
        for (const auto &c : f.enum_constants) {
            json cj = json::object();
            cj["name"] = c.name;
            cj["value"] = c.value;
            constants.push_back(cj);
        }
        j["enum_constants"] = constants;
    } else if (f.type_kind == "struct" || f.type_kind == "counted_array") {
        if (!f.struct_name.empty()) {
            j["struct_name"] = f.struct_name;
        }
        json subfields = json::array();
        for (const auto &sub : f.struct_fields) {
            subfields.push_back(fieldInfoToJson(sub));
        }
        j["struct_fields"] = subfields;
    } else if (f.type_kind == "fixed_array") {
        j["array_len"] = f.array_index;
        json subfields = json::array();
        for (const auto &sub : f.struct_fields) {
            subfields.push_back(fieldInfoToJson(sub));
        }
        j["struct_fields"] = subfields;
    } else if (f.type_kind == "union") {
        j["tag_field"] = f.counted_by;
        json cases = json::array();
        for (const auto &sub : f.struct_fields) {
            json sj = fieldInfoToJson(sub);
            sj["tag_val"] = sub.array_index;
            sj["tag_val_str"] = sub.array_parent;
            cases.push_back(sj);
        }
        j["union_cases"] = cases;
    }
    return j;
}

class FallbackCompilationDatabase : public CompilationDatabase {
public:
    FallbackCompilationDatabase(const CompilationDatabase &Base, std::vector<std::string> FallbackFlags)
        : BaseDb(Base), Flags(std::move(FallbackFlags)) {}

    std::vector<CompileCommand> getCompileCommands(StringRef FilePath) const override {
        auto cmds = BaseDb.getCompileCommands(FilePath);
        if (!cmds.empty()) return cmds;
        std::vector<std::string> args = {"clang"};
        args.insert(args.end(), Flags.begin(), Flags.end());
        args.push_back(FilePath.str());
        return {CompileCommand(".", FilePath.str(), args, "clang")};
    }

    std::vector<std::string> getAllFiles() const override {
        return BaseDb.getAllFiles();
    }

    std::vector<CompileCommand> getAllCompileCommands() const override {
        return BaseDb.getAllCompileCommands();
    }

private:
    const CompilationDatabase &BaseDb;
    std::vector<std::string> Flags;
};

} // namespace

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, CtraceScanCategory, llvm::cl::ZeroOrMore);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }

    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    // Fast-skip: If source file doesn't mention TRACE_EVENT or CTRACE_ or __ctrace_hook, emit empty JSON immediately
    bool anySymbols = false;
    if (ForceScan) {
        anySymbols = true;
    } else {
        for (const auto &src : OptionsParser.getSourcePathList()) {
            if (fileHasTraceSymbols(src)) {
                anySymbols = true;
                break;
            }
        }
    }

    if (!anySymbols && !OptionsParser.getSourcePathList().empty()) {
        json root = json::object();
        root["events"] = json::array();
        std::string outPath = OutputFilename;
        if (outPath.size() >= 2 && outPath.front() == '"' && outPath.back() == '"') {
            outPath = outPath.substr(1, outPath.size() - 2);
        }
        if (!outPath.empty()) {
            auto parent = std::filesystem::path(outPath).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            std::ofstream outFile(outPath);
            if (outFile) {
                outFile << root.dump(2) << std::endl;
                return 0;
            }
        }
        std::cout << root.dump(2) << std::endl;
        return 0;
    }

    // Extract extra-arg values passed from command line
    std::vector<std::string> extraFlags = {"-DCTRACE_AST_SCAN"};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--extra-arg=", 0) == 0) {
            extraFlags.push_back(a.substr(12));
        }
    }

    FallbackCompilationDatabase FallbackDb(OptionsParser.getCompilations(), extraFlags);
    ClangTool Tool(FallbackDb, OptionsParser.getSourcePathList());

    int result = Tool.run(newFrontendActionFactory<TraceFrontendAction>().get());
    if (result != 0) {
        llvm::errs() << "ctrace-scan error: ClangTool failed with code: " << result << "\n";
        return result;
    }

    json root = json::object();
    json eventsArray = json::array();

    for (const auto &entry : g_collected_events) {
        const EventInfo &ev = entry.second;
        json evObj = json::object();
        evObj["name"] = ev.name;
        evObj["file"] = ev.file;
        evObj["line"] = ev.line;
        evObj["column"] = ev.column;

        json fields = json::array();
        for (const auto &f : ev.fields) {
            fields.push_back(fieldInfoToJson(f));
        }
        evObj["fields"] = fields;
        eventsArray.push_back(evObj);
    }

    root["events"] = eventsArray;

    std::string outPath = OutputFilename;
    if (outPath.size() >= 2 && outPath.front() == '"' && outPath.back() == '"') {
        outPath = outPath.substr(1, outPath.size() - 2);
    }
    if (!outPath.empty()) {
        auto parent = std::filesystem::path(outPath).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
    }
    if (outPath.empty()) {
        std::cout << root.dump(2) << std::endl;
    } else {
        std::ofstream outFile(outPath);
        if (!outFile) {
            llvm::errs() << "Error: Could not open output file: " << outPath << "\n";
            return 1;
        }
        outFile << root.dump(2) << std::endl;
    }

    return 0;
}
