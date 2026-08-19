//===- abi_query.cpp - record layout as structured data -------------------===//
//
// One entry point: JSON in, JSON out. Everything a layout viewer needs about a
// translation unit, taken from clang's own AST rather than reconstructed from
// its diagnostic output.
//
// The design rule throughout: if clang knows it, report it. The pipeline this
// replaces re-derived member sizes by compiling probe structs, joined the
// layout dump to the AST dump by matching printed type names, and could not
// locate a base specifier at all. All three are direct field reads here.
//
//===----------------------------------------------------------------------===//

#include "abi_query.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/PreprocessorOptions.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>
#include <string>
#include <vector>

using namespace clang;

// Where the shipped headers live. The wasm build mounts its pack at /usr and
// puts the resource directory beside it; the native build points these at the
// LLVM tree it was compiled against, so both resolve the same way.
#ifndef ABI_DEFAULT_SYSROOT
#define ABI_DEFAULT_SYSROOT ""
#endif
#ifndef ABI_DEFAULT_RESOURCE_DIR
#define ABI_DEFAULT_RESOURCE_DIR ""
#endif

namespace {

// ------------------------------------------------------------ diagnostics --

/// Records diagnostics as data. The previous pipeline asked clang for coloured
/// text and then stripped the ANSI escapes back off with a regex.
class JsonDiagnostics : public DiagnosticConsumer {
public:
  llvm::json::Array Diags;

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level, Info);

    const char *Severity = nullptr;
    switch (Level) {
    case DiagnosticsEngine::Ignored:
      return;
    case DiagnosticsEngine::Note:    Severity = "note";    break;
    case DiagnosticsEngine::Remark:  Severity = "remark";  break;
    case DiagnosticsEngine::Warning: Severity = "warning"; break;
    case DiagnosticsEngine::Error:   Severity = "error";   break;
    case DiagnosticsEngine::Fatal:   Severity = "fatal";   break;
    }

    llvm::SmallString<256> Message;
    Info.FormatDiagnostic(Message);

    llvm::json::Object D{{"severity", Severity},
                         {"message", std::string(Message)}};

    if (Info.hasSourceManager()) {
      const SourceManager &SM = Info.getSourceManager();
      D["location"] = locationToJson(SM, Info.getLocation());

      llvm::json::Array Ranges;
      for (const CharSourceRange &R : Info.getRanges())
        if (auto Span = rangeToJson(SM, R.getAsRange()))
          Ranges.push_back(std::move(*Span));
      D["ranges"] = std::move(Ranges);
    } else {
      D["location"] = nullptr;
      D["ranges"] = llvm::json::Array{};
    }

    // The flag that controls the diagnostic, so a UI can offer to turn it off.
    if (const DiagnosticsEngine *Engine = Info.getDiags()) {
      llvm::StringRef Opt =
          Engine->getDiagnosticIDs()->getWarningOptionForDiag(Info.getID());
      D["option"] = Opt.empty() ? llvm::json::Value(nullptr)
                                : llvm::json::Value(("-W" + Opt).str());
    } else {
      D["option"] = nullptr;
    }

    Diags.push_back(std::move(D));
  }

  static llvm::json::Value locationToJson(const SourceManager &SM,
                                          SourceLocation Loc) {
    if (Loc.isInvalid())
      return nullptr;
    SourceLocation Spelling = SM.getSpellingLoc(Loc);
    PresumedLoc PL = SM.getPresumedLoc(Spelling);
    if (PL.isInvalid())
      return nullptr;
    // End column: one past the token, so a UI can underline exactly the token.
    unsigned TokLen = Lexer::MeasureTokenLength(Spelling, SM, LangOptions());
    return llvm::json::Object{
        {"file", PL.getFilename()},
        {"line", PL.getLine()},
        {"col", PL.getColumn()},
        {"endCol", PL.getColumn() + (TokLen ? TokLen : 1)},
        {"isMainFile", SM.isWrittenInMainFile(Spelling)}};
  }

  static std::optional<llvm::json::Value> rangeToJson(const SourceManager &SM,
                                                      SourceRange R) {
    if (R.isInvalid())
      return std::nullopt;
    PresumedLoc B = SM.getPresumedLoc(SM.getSpellingLoc(R.getBegin()));
    PresumedLoc E = SM.getPresumedLoc(SM.getSpellingLoc(R.getEnd()));
    if (B.isInvalid() || E.isInvalid())
      return std::nullopt;
    unsigned TokLen =
        Lexer::MeasureTokenLength(SM.getSpellingLoc(R.getEnd()), SM, LangOptions());
    return llvm::json::Value(llvm::json::Object{
        {"line", B.getLine()},
        {"col", B.getColumn()},
        {"endLine", E.getLine()},
        {"endCol", E.getColumn() + TokLen}});
  }
};

// --------------------------------------------------------------- collector --

/// Every complete record definition in the TU, in source order.
class RecordCollector : public RecursiveASTVisitor<RecordCollector> {
public:
  std::vector<const RecordDecl *> Order;
  llvm::SmallPtrSet<const RecordDecl *, 32> Seen;

  bool VisitRecordDecl(RecordDecl *RD) {
    if (!RD->isCompleteDefinition() || RD->isInvalidDecl())
      return true;
    // Injected class names repeat the record inside itself.
    if (RD->isImplicit())
      return true;
    if (Seen.insert(RD).second)
      Order.push_back(RD);
    return true;
  }
};

// ------------------------------------------------------------------ writer --

class LayoutWriter {
public:
  LayoutWriter(ASTContext &Ctx, bool IncludeSystem)
      : Ctx(Ctx), SM(Ctx.getSourceManager()), IncludeSystem(IncludeSystem) {}

  llvm::json::Array run(const std::vector<const RecordDecl *> &All) {
    // Which records to report. A record from a header is reported only when
    // something the user wrote refers to it — otherwise a single `#include
    // <string>` contributes over a thousand of them.
    for (const RecordDecl *RD : All)
      if (IncludeSystem || isUserCode(RD))
        markReachable(RD);

    // Ids are indices into the emitted array, so a consumer resolves a
    // reference with `records[id]` and never searches by name.
    for (const RecordDecl *RD : All)
      if (Reachable.count(RD))
        Ids[RD] = Ids.size();

    llvm::json::Array Out;
    for (const RecordDecl *RD : All)
      if (Reachable.count(RD))
        Out.push_back(emitRecord(RD));
    return Out;
  }

private:
  ASTContext &Ctx;
  const SourceManager &SM;
  bool IncludeSystem;
  llvm::SmallPtrSet<const RecordDecl *, 32> Reachable;
  llvm::DenseMap<const RecordDecl *, unsigned> Ids;

  bool isUserCode(const RecordDecl *RD) const {
    SourceLocation Loc = RD->getLocation();
    return Loc.isValid() && SM.isWrittenInMainFile(SM.getSpellingLoc(Loc));
  }

  /// A reported record drags in the records it refers to, so every `recordId`
  /// in the output resolves.
  void markReachable(const RecordDecl *RD) {
    if (!RD || !RD->isCompleteDefinition() || !Reachable.insert(RD).second)
      return;
    if (const auto *CXX = dyn_cast<CXXRecordDecl>(RD))
      for (const CXXBaseSpecifier &B : CXX->bases())
        markReachable(B.getType()->getAsRecordDecl());
    for (const FieldDecl *F : RD->fields())
      markReachable(F->getType()->getAsRecordDecl());
  }

  llvm::json::Value idOf(const RecordDecl *RD) const {
    if (!RD)
      return nullptr;
    auto It = Ids.find(RD);
    return It == Ids.end() ? llvm::json::Value(nullptr)
                           : llvm::json::Value(int64_t(It->second));
  }

  static const char *accessName(AccessSpecifier AS) {
    switch (AS) {
    case AS_public:    return "public";
    case AS_protected: return "protected";
    case AS_private:   return "private";
    case AS_none:      return "public";
    }
    return "public";
  }

  const char *kindName(const RecordDecl *RD) const {
    if (RD->isUnion())      return "union";
    if (RD->isInterface())  return "interface";
    if (RD->isClass())      return "class";
    return "struct";
  }

  std::string printed(QualType T) const {
    PrintingPolicy P = Ctx.getPrintingPolicy();
    P.SuppressTagKeyword = false;
    return T.getAsString(P);
  }

  int64_t bits(CharUnits CU) const { return Ctx.toBits(CU); }

  /// Size and alignment of a type, in bits. Incomplete types (the element type
  /// of a flexible array member) have neither; they report zero rather than
  /// tripping the assertion in getTypeInfo.
  std::pair<int64_t, int64_t> typeInfoBits(QualType T) const {
    if (T->isIncompleteType() || T->isDependentType())
      return {0, 0};
    TypeInfo TI = Ctx.getTypeInfo(T);
    return {int64_t(TI.Width), int64_t(TI.Align)};
  }

  llvm::json::Value emitRecord(const RecordDecl *RD) {
    const ASTRecordLayout &L = Ctx.getASTRecordLayout(RD);
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);

    llvm::json::Object O;
    O["id"] = int64_t(Ids[RD]);
    O["kind"] = kindName(RD);
    O["name"] = RD->getNameAsString();
    O["qualifiedName"] = RD->getQualifiedNameAsString();
    O["printedName"] = printed(Ctx.getCanonicalTagType(RD));
    O["isAnonymous"] = RD->getDeclName().isEmpty();
    O["isUserCode"] = isUserCode(RD);
    O["location"] = JsonDiagnostics::locationToJson(SM, RD->getLocation());

    O["sizeBits"] = bits(L.getSize());
    O["alignBits"] = bits(L.getAlignment());
    O["dataSizeBits"] = bits(L.getDataSize());
    O["preferredAlignBits"] = bits(L.getPreferredAlignment());

    if (CXX) {
      O["isEmpty"] = CXX->isEmpty();
      O["isPolymorphic"] = CXX->isPolymorphic();
      O["isStandardLayout"] = CXX->isStandardLayout();
      O["nonVirtualSizeBits"] = bits(L.getNonVirtualSize());
      O["nonVirtualAlignBits"] = bits(L.getNonVirtualAlignment());
    } else {
      O["isEmpty"] = RD->field_empty();
      O["isPolymorphic"] = false;
      O["isStandardLayout"] = true;
      O["nonVirtualSizeBits"] = bits(L.getSize());
      O["nonVirtualAlignBits"] = bits(L.getAlignment());
    }

    // Track which bits are occupied so padding is reported, not inferred.
    std::vector<bool> Occupied(size_t(bits(L.getSize())), false);
    auto occupy = [&](int64_t Start, int64_t Width) {
      for (int64_t B = std::max<int64_t>(Start, 0);
           B < std::min<int64_t>(Start + Width, (int64_t)Occupied.size()); ++B)
        Occupied[size_t(B)] = true;
    };

    O["vtableSlots"] = emitVTableSlots(RD, CXX, L, occupy);
    O["bases"] = emitBases(CXX, L, occupy);
    O["fields"] = emitFields(RD, L, occupy);

    llvm::json::Array Runs;
    int64_t PaddingBits = 0;
    for (size_t B = 0; B < Occupied.size();) {
      if (Occupied[B]) { ++B; continue; }
      size_t Start = B;
      while (B < Occupied.size() && !Occupied[B]) ++B;
      Runs.push_back(llvm::json::Object{{"startBits", int64_t(Start)},
                                        {"endBits", int64_t(B)}});
      PaddingBits += int64_t(B - Start);
    }
    O["paddingRuns"] = std::move(Runs);
    O["paddingBits"] = PaddingBits;

    return llvm::json::Value(std::move(O));
  }

  template <typename OccupyFn>
  llvm::json::Array emitVTableSlots(const RecordDecl *RD,
                                    const CXXRecordDecl *CXX,
                                    const ASTRecordLayout &L, OccupyFn occupy) {
    llvm::json::Array Slots;
    if (!CXX)
      return Slots;
    const int64_t PtrBits = Ctx.getTargetInfo().getPointerWidth(LangAS::Default);

    // Itanium: a polymorphic class with no primary base owns the vptr at 0.
    // Microsoft: hasOwnVFPtr() says so directly.
    const bool OwnsVPtr =
        L.hasOwnVFPtr() || (CXX->isDynamicClass() && !L.getPrimaryBase());
    if (OwnsVPtr) {
      Slots.push_back(llvm::json::Object{
          {"kind", "vptr"},
          {"label", RD->getNameAsString() + " vtable pointer"},
          {"offsetBits", int64_t(0)},
          {"sizeBits", PtrBits}});
      occupy(0, PtrBits);
    }
    if (L.hasOwnVBPtr()) {
      const int64_t Off = bits(L.getVBPtrOffset());
      Slots.push_back(llvm::json::Object{
          {"kind", "vbptr"},
          {"label", RD->getNameAsString() + " vbtable pointer"},
          {"offsetBits", Off},
          {"sizeBits", PtrBits}});
      occupy(Off, PtrBits);
    }
    return Slots;
  }

  template <typename OccupyFn>
  llvm::json::Array emitBases(const CXXRecordDecl *CXX,
                              const ASTRecordLayout &L, OccupyFn occupy) {
    llvm::json::Array Bases;
    if (!CXX)
      return Bases;

    auto addBase = [&](const CXXBaseSpecifier &B, bool Virtual) {
      const auto *BD = B.getType()->getAsCXXRecordDecl();
      if (!BD || !BD->isCompleteDefinition())
        return;
      const ASTRecordLayout &BL = Ctx.getASTRecordLayout(BD);
      const int64_t Off =
          bits(Virtual ? L.getVBaseClassOffset(BD) : L.getBaseClassOffset(BD));
      // A base occupies its non-virtual size here; the derived class may reuse
      // its tail padding, so this is not always sizeof.
      const int64_t Occupies = BD->isEmpty() ? 0 : bits(BL.getNonVirtualSize());

      Bases.push_back(llvm::json::Object{
          {"recordId", idOf(BD)},
          {"typeSpelling", printed(B.getType())},
          {"offsetBits", Off},
          {"sizeBits", Occupies},
          {"typeSizeBits", bits(BL.getSize())},
          {"isVirtual", Virtual},
          {"isPrimary", L.getPrimaryBase() == BD},
          {"isEmpty", BD->isEmpty()},
          {"access", accessName(B.getAccessSpecifier())},
          // The whole reason this library exists: no clang dump format emits
          // the source range of a base specifier.
          {"location", JsonDiagnostics::rangeToJson(SM, B.getSourceRange())
                           .value_or(llvm::json::Value(nullptr))}});
      occupy(Off, Occupies);
    };

    for (const CXXBaseSpecifier &B : CXX->bases())
      if (!B.isVirtual())
        addBase(B, false);
    for (const CXXBaseSpecifier &B : CXX->vbases())
      addBase(B, true);
    return Bases;
  }

  template <typename OccupyFn>
  llvm::json::Array emitFields(const RecordDecl *RD, const ASTRecordLayout &L,
                               OccupyFn occupy) {
    llvm::json::Array Fields;
    unsigned Index = 0;
    for (const FieldDecl *F : RD->fields()) {
      const int64_t Off = int64_t(L.getFieldOffset(Index));
      QualType T = F->getType();

      llvm::json::Object O;
      O["id"] = int64_t(Index);
      O["name"] = F->getNameAsString();
      O["offsetBits"] = Off;
      O["typeSpelling"] = printed(T);
      O["canonicalTypeSpelling"] = printed(T.getCanonicalType());
      O["access"] = accessName(F->getAccess());
      O["location"] = JsonDiagnostics::locationToJson(SM, F->getLocation());
      O["recordId"] = idOf(T->getAsRecordDecl());
      O["isAnonymousMember"] = F->isAnonymousStructOrUnion();
      O["isNoUniqueAddress"] = F->hasAttr<NoUniqueAddressAttr>();

      int64_t Width = 0;
      if (F->isBitField()) {
        const unsigned BW = F->getBitWidthValue();
        O["isBitField"] = true;
        O["bitWidth"] = int64_t(BW);
        O["isZeroWidthBitField"] = F->isZeroLengthBitField();
        Width = int64_t(BW);
        O["sizeBits"] = Width;
        O["alignBits"] = int64_t(0);
        O["isFlexibleArrayMember"] = false;
      } else {
        O["isBitField"] = false;
        O["bitWidth"] = nullptr;
        O["isZeroWidthBitField"] = false;
        // A trailing `T x[]` sits at sizeof and occupies nothing. The previous
        // pipeline could not measure it and guessed one byte, which the byte
        // grid then refused to draw.
        const bool Flexible =
            T->isIncompleteArrayType() && RD->hasFlexibleArrayMember();
        O["isFlexibleArrayMember"] = Flexible;
        auto [SizeBits, AlignBits] = typeInfoBits(T);
        Width = Flexible ? 0 : SizeBits;
        O["sizeBits"] = Width;
        O["alignBits"] = AlignBits;
      }

      // An explicit alignas lives on the declaration, not on the type.
      if (unsigned MaxAlign = F->getMaxAlignment())
        O["explicitAlignBits"] = int64_t(MaxAlign);
      else
        O["explicitAlignBits"] = nullptr;

      // A member sharing an address occupies nothing of its own.
      if (F->isZeroSize(Ctx))
        Width = 0;

      occupy(Off, Width);
      Fields.push_back(std::move(O));
      ++Index;
    }
    return Fields;
  }
};

// ------------------------------------------------------------------ action --

class LayoutAction : public ASTFrontendAction {
public:
  LayoutAction(bool IncludeSystem, llvm::json::Array &Out)
      : IncludeSystem(IncludeSystem), Out(Out) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    class Consumer : public ASTConsumer {
    public:
      Consumer(bool IncludeSystem, llvm::json::Array &Out)
          : IncludeSystem(IncludeSystem), Out(Out) {}
      void HandleTranslationUnit(ASTContext &Ctx) override {
        RecordCollector C;
        C.TraverseDecl(Ctx.getTranslationUnitDecl());
        LayoutWriter W(Ctx, IncludeSystem);
        Out = W.run(C.Order);
      }

    private:
      bool IncludeSystem;
      llvm::json::Array &Out;
    };
    return std::make_unique<Consumer>(IncludeSystem, Out);
  }

private:
  bool IncludeSystem;
  llvm::json::Array &Out;
};

// ----------------------------------------------------------------- target --

llvm::json::Value emitTargetInfo(const TargetInfo &TI,
                                 llvm::StringRef Requested) {
  const auto &T = TI.getTriple();
  return llvm::json::Object{
      {"triple", Requested},
      {"normalizedTriple", T.str()},
      {"pointerSizeBits", int64_t(TI.getPointerWidth(LangAS::Default))},
      {"pointerAlignBits", int64_t(TI.getPointerAlign(LangAS::Default))},
      {"isLittleEndian", TI.isLittleEndian()},
      {"charSizeBits", int64_t(TI.getCharWidth())},
      {"shortSizeBits", int64_t(TI.getShortWidth())},
      {"intSizeBits", int64_t(TI.getIntWidth())},
      {"longSizeBits", int64_t(TI.getLongWidth())},
      {"longLongSizeBits", int64_t(TI.getLongLongWidth())},
      {"cxxAbi", T.isKnownWindowsMSVCEnvironment() ? "microsoft" : "itanium"}};
}

/// musl's per-architecture header tree for a triple, or "generic".
///
/// The names are musl's own directory names, which mostly but not always match
/// LLVM's architecture spelling.
std::string muslArch(llvm::StringRef Triple) {
  const llvm::Triple T(Triple);
  switch (T.getArch()) {
  case llvm::Triple::x86_64:   return T.isX32() ? "x32" : "x86_64";
  case llvm::Triple::x86:      return "i386";
  case llvm::Triple::aarch64:
  case llvm::Triple::aarch64_be: return "aarch64";
  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:  return "arm";
  case llvm::Triple::riscv32:  return "riscv32";
  case llvm::Triple::riscv64:  return "riscv64";
  case llvm::Triple::ppc:      return "powerpc";
  case llvm::Triple::ppc64:
  case llvm::Triple::ppc64le:  return "powerpc64";
  case llvm::Triple::mips:
  case llvm::Triple::mipsel:   return "mips";
  case llvm::Triple::mips64:
  case llvm::Triple::mips64el: return "mips64";
  case llvm::Triple::systemz:  return "s390x";
  case llvm::Triple::loongarch64: return "loongarch64";
  case llvm::Triple::m68k:     return "m68k";
  case llvm::Triple::wasm32:
  case llvm::Triple::wasm64:   return "wasm32";
  default:                     return "generic";
  }
}

std::string fail(llvm::StringRef Message) {
  llvm::json::Object O{{"ok", false},
                       {"error", Message},
                       {"clangVersion", clang::getClangFullVersion()},
                       {"exitCode", 1},
                       {"target", nullptr},
                       {"diagnostics", llvm::json::Array{}},
                       {"records", llvm::json::Array{}}};
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << llvm::json::Value(std::move(O));
  return S;
}

/// Every architecture this build can lay records out for.
///
/// Clang exposes this to no command-line flag — `-print-targets` lists the
/// *LLVM backends* that were built, which is a different and much shorter list
/// (a build with only the wasm backends still lays out records for 35 of 41
/// exotic triples). So the list is produced the only way it can be: by asking
/// TargetInfo to construct one per architecture and keeping those that work.
std::string listTargets() {
  DiagnosticOptions DiagOpts;
  auto DiagIDs = llvm::makeIntrusiveRefCnt<DiagnosticIDs>();
  IgnoringDiagConsumer Ignore;
  DiagnosticsEngine DE(DiagIDs, DiagOpts, &Ignore, /*ShouldOwnClient=*/false);

  llvm::json::Array Targets;
  for (unsigned A = llvm::Triple::UnknownArch + 1;
       A <= llvm::Triple::LastArchType; ++A) {
    llvm::StringRef Name =
        llvm::Triple::getArchTypeName(static_cast<llvm::Triple::ArchType>(A));
    if (Name.empty())
      continue;
    auto Opts = std::make_shared<TargetOptions>();
    Opts->Triple = (Name + "-unknown-unknown").str();
    if (TargetInfo::CreateTargetInfo(DE, *Opts))
      Targets.push_back(Name);
  }

  llvm::json::Object O{{"ok", true},
                       {"error", nullptr},
                       {"clangVersion", clang::getClangFullVersion()},
                       {"targets", std::move(Targets)}};
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << llvm::json::Value(std::move(O));
  return S;
}

std::string runQuery(llvm::StringRef RequestJson) {
  auto Parsed = llvm::json::parse(RequestJson);
  if (!Parsed)
    return fail("request is not valid JSON: " +
                llvm::toString(Parsed.takeError()));
  const llvm::json::Object *Req = Parsed->getAsObject();
  if (!Req)
    return fail("request must be a JSON object");

  if (Req->getBoolean("listTargets").value_or(false))
    return listTargets();

  const std::string Source = Req->getString("source").value_or("").str();
  const std::string Triple = Req->getString("triple").value_or("").str();
  if (Triple.empty())
    return fail("request is missing `triple`");

  std::string Lang = Req->getString("lang").value_or("c").str();
  const bool IsCxx = (Lang == "c++");
  const std::string Filename =
      Req->getString("filename").value_or(IsCxx ? "input.cc" : "input.c").str();
  const bool IncludeSystem =
      Req->getBoolean("includeSystemRecords").value_or(false);

  // Driver-level arguments, translated to a cc1 invocation below. Taking driver
  // flags rather than cc1 flags means callers pass what they would type.
  std::vector<std::string> Args{"clang",
                                "-fsyntax-only",
                                "--target=" + Triple,
                                IsCxx ? "-xc++" : "-xc"};

  // Header search is pinned to the shipped sysroot, never discovered.
  //
  // Left alone, the driver locates its resource directory relative to argv[0]
  // and then probes the real filesystem for system headers. Under wasm there is
  // no filesystem to probe, and natively it silently answers with *the host's*
  // headers — so `--target=aarch64-...` would report x86-64 glibc layouts, which
  // is precisely the class of wrong answer this library exists to avoid. Both
  // builds therefore run -nostdinc with explicit paths, and behave the same.
  const std::string Sysroot =
      Req->getString("sysroot").value_or(ABI_DEFAULT_SYSROOT).str();
  const std::string ResourceDir =
      Req->getString("resourceDir").value_or(ABI_DEFAULT_RESOURCE_DIR).str();

  if (!ResourceDir.empty()) {
    Args.push_back("-resource-dir=" + ResourceDir);
    Args.push_back("-nostdinc");
    // clang's own builtin headers: stddef.h, stdint.h, the intrinsics.
    Args.push_back("-isystem");
    Args.push_back(ResourceDir + "/include");
  }
  if (!Sysroot.empty()) {
    // libc++ before the C library: it reaches the C headers with #include_next,
    // so anything that shadows them has to come first.
    if (IsCxx) {
      Args.push_back("-isystem");
      Args.push_back(Sysroot + "/include/c++/v1");
    }
    // musl keeps the target-varying declarations in a per-architecture tree and
    // the rest in a shared one; the architecture tree must win.
    Args.push_back("-isystem");
    Args.push_back(Sysroot + "/include/musl-arch/" + muslArch(Triple));
    Args.push_back("-isystem");
    Args.push_back(Sysroot + "/include/musl");
  }
  if (auto Std = Req->getString("std"); Std && !Std->empty())
    Args.push_back(("-std=" + *Std).str());
  if (const llvm::json::Array *Flags = Req->getArray("flags"))
    for (const auto &F : *Flags)
      if (auto S = F.getAsString())
        Args.push_back(S->str());
  Args.push_back(Filename);

  std::vector<const char *> Argv;
  Argv.reserve(Args.size());
  for (const std::string &A : Args)
    Argv.push_back(A.c_str());

  auto Diags = std::make_unique<JsonDiagnostics>();
  DiagnosticOptions DiagOpts;
  auto DiagIDs = llvm::makeIntrusiveRefCnt<DiagnosticIDs>();
  DiagnosticsEngine DE(DiagIDs, DiagOpts, Diags.get(), /*ShouldOwnClient=*/false);

  // The driver turns `--target=... -xc++ -std=...` into the cc1 arguments the
  // frontend actually consumes. CreateFromArgs alone would reject these: it
  // parses cc1 flags, not driver flags.
  CreateInvocationOptions IOpts;
  IOpts.Diags = llvm::makeIntrusiveRefCnt<DiagnosticsEngine>(
      DiagIDs, DiagOpts, Diags.get(), /*ShouldOwnClient=*/false);
  IOpts.RecoverOnError = true;
  std::shared_ptr<CompilerInvocation> Invocation = createInvocation(Argv, IOpts);
  if (!Invocation)
    return fail("could not build a compiler invocation for target " + Triple);

  CompilerInstance CI(std::move(Invocation));
  CI.createDiagnostics(Diags.get(), /*ShouldOwnClient=*/false);

  // The source lives in memory; nothing is read from or written to a real path.
  CI.getPreprocessorOpts().addRemappedFile(
      Filename, llvm::MemoryBuffer::getMemBufferCopy(Source, Filename).release());

  llvm::json::Array Records;
  LayoutAction Action(IncludeSystem, Records);
  const bool Ok = CI.ExecuteAction(Action);

  llvm::json::Object O{
      {"ok", true},
      {"error", nullptr},
      {"clangVersion", clang::getClangFullVersion()},
      {"exitCode", (Ok && !CI.getDiagnostics().hasErrorOccurred()) ? 0 : 1},
      {"target", CI.hasTarget() ? emitTargetInfo(CI.getTarget(), Triple)
                                : llvm::json::Value(nullptr)},
      {"diagnostics", std::move(Diags->Diags)},
      {"records", std::move(Records)}};

  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << llvm::json::Value(std::move(O));
  return S;
}

/// Owns the strings handed back across the C boundary.
std::string &resultSlot() {
  static std::string Slot;
  return Slot;
}

} // namespace

extern "C" {

const char *abi_query(const char *RequestJson) {
  resultSlot() = runQuery(RequestJson ? RequestJson : "");
  return resultSlot().c_str();
}

const char *abi_version(void) {
  static const std::string V = clang::getClangFullVersion();
  return V.c_str();
}

} // extern "C"
