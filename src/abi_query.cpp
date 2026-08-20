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
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/PreprocessorOptions.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
  /// Everything clang would have printed to a terminal, escapes and all.
  std::string Text;

  JsonDiagnostics() : TextOS(Text) {
    // Colour on, unconditionally: there is no terminal to ask, and the
    // consumer wants the escapes precisely so it can style the output itself.
    // Both halves are needed — the option makes clang *ask* for colour, and
    // enable_colors makes the string stream agree to emit it.
    TextOpts.ShowColors = true;
    TextOS.enable_colors(true);
    Printer = std::make_unique<TextDiagnosticPrinter>(TextOS, TextOpts);
  }

  // The printer needs the language options and the preprocessor to render a
  // caret line, and CompilerInstance hands them to the diagnostic client at
  // the start of the file. Forward the whole lifecycle or it prints nothing.
  void BeginSourceFile(const LangOptions &LO, const Preprocessor *PP) override {
    DiagnosticConsumer::BeginSourceFile(LO, PP);
    Printer->BeginSourceFile(LO, PP);
  }
  void EndSourceFile() override {
    Printer->EndSourceFile();
    DiagnosticConsumer::EndSourceFile();
  }
  void finish() override {
    Printer->finish();
    DiagnosticConsumer::finish();
  }

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level, Info);
    Printer->HandleDiagnostic(Level, Info);

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

private:
  DiagnosticOptions TextOpts;
  llvm::raw_string_ostream TextOS;
  std::unique_ptr<TextDiagnosticPrinter> Printer;

public:

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
  /// `typedef`s and alias declarations, in source order.
  std::vector<const TypedefNameDecl *> Typedefs;

  /// Without this, `std::string` is never visited: an implicit instantiation is
  /// not part of the written declaration tree, so a field of that type would
  /// reference a record that was never emitted and the id would dangle.
  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitRecordDecl(RecordDecl *RD) {
    if (!isLayoutable(RD))
      return true;
    if (Seen.insert(RD).second)
      Order.push_back(RD);
    return true;
  }

  /// A name for a type is a thing a reader can point at, so it is reported for
  /// the same reason records are: a viewer asked what is under the cursor
  /// should not have to compile a probe to find out.
  bool VisitTypedefNameDecl(TypedefNameDecl *TD) {
    if (TD && !TD->isImplicit())
      Typedefs.push_back(TD);
    return true;
  }

  /// Can clang compute a record layout for this?
  ///
  /// The dependent check is not defensive: a template *pattern* is a complete,
  /// valid definition whose members have dependent types, and asking
  /// ASTContext::getASTRecordLayout for one walks into clang's layout builder
  /// and segfaults there rather than returning an error. Visiting template
  /// instantiations (which the ids require) makes the patterns reachable too,
  /// so this is the guard that keeps them out.
  static bool isLayoutable(const RecordDecl *RD) {
    if (!RD || !RD->isCompleteDefinition() || RD->isInvalidDecl())
      return false;
    // Injected class names repeat the record inside itself.
    if (RD->isImplicit())
      return false;
    if (RD->isDependentContext() || RD->isDependentType())
      return false;
    return true;
  }
};

/// Does this class own a virtual function table pointer of its own?
///
/// The two ABIs answer differently and neither answers with one predicate.
/// Microsoft records it: `hasOwnVFPtr()` is exactly the question, and a class
/// with virtual bases but no virtual functions gets a *vbtable* pointer and no
/// vfptr. Itanium keeps the virtual base offsets in the vtable itself, so such
/// a class does own a vptr — `isDynamicClass()` covers polymorphic-or-has-vbases
/// — unless a primary base already provides one at offset zero.
///
/// Asking `hasOwnVFPtr() || (isDynamicClass() && !getPrimaryBase())` on both
/// gets Microsoft wrong: the second clause fires for a class whose only reason
/// to be dynamic is a virtual base, and drew a phantom vtable pointer on top of
/// the vbtable pointer.
inline bool ownsVFPtr(const ASTContext &Ctx, const CXXRecordDecl *RD,
                      const ASTRecordLayout &L) {
  if (Ctx.getTargetInfo().getCXXABI().isMicrosoft())
    return L.hasOwnVFPtr();
  return RD->isDynamicClass() && !L.getPrimaryBase();
}

// ----------------------------------------------------------- render model --

#include "render_model.inc"

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

    // Emission order: source order for everything the traversal saw, then
    // anything reachability pulled in that it did not. Reachability walks types
    // and can reach a record the visitor never reported; emitting only the
    // visited ones would leave `recordId` pointing at nothing.
    std::vector<const RecordDecl *> Emit;
    for (const RecordDecl *RD : All)
      if (Reachable.count(RD))
        Emit.push_back(RD);
    llvm::SmallPtrSet<const RecordDecl *, 32> Listed(Emit.begin(), Emit.end());
    for (const RecordDecl *RD : ReachedOrder)
      if (!Listed.contains(RD))
        Emit.push_back(RD);

    // Ids are indices into the emitted array, so a consumer resolves a
    // reference with `records[id]` and never searches by name.
    for (const RecordDecl *RD : Emit)
      Ids[RD] = Ids.size();

    llvm::json::Array Out;
    for (const RecordDecl *RD : Emit)
      Out.push_back(emitRecord(RD));
    return Out;
  }

  /// Call after run(): typedef records reference ids run() assigned.
  llvm::json::Array typedefs(const std::vector<const TypedefNameDecl *> &TDs) {
    return emitTypedefs(TDs);
  }

private:
  ASTContext &Ctx;
  const SourceManager &SM;
  bool IncludeSystem;
  llvm::SmallPtrSet<const RecordDecl *, 32> Reachable;
  /// Reachability order, so records the traversal missed still emit predictably.
  std::vector<const RecordDecl *> ReachedOrder;
  llvm::DenseMap<const RecordDecl *, unsigned> Ids;

  bool isUserCode(const RecordDecl *RD) const {
    SourceLocation Loc = RD->getLocation();
    return Loc.isValid() && SM.isWrittenInMainFile(SM.getSpellingLoc(Loc));
  }

  /// A reported record drags in the records it refers to, so every `recordId`
  /// in the output resolves.
  ///
  /// Iterative rather than recursive: with system records included, a libc++
  /// translation unit reaches thousands of instantiations nested deeply enough
  /// that the natural recursion overruns the stack.
  void markReachable(const RecordDecl *Root) {
    // Same rule as the collector: only records clang can actually lay out.
    llvm::SmallVector<const RecordDecl *, 64> Work;
    auto push = [&](const RecordDecl *RD) {
      if (RecordCollector::isLayoutable(RD) && Reachable.insert(RD).second) {
        ReachedOrder.push_back(RD);
        Work.push_back(RD);
      }
    };
    push(Root);
    while (!Work.empty()) {
      const RecordDecl *RD = Work.pop_back_val();
      if (const auto *CXX = dyn_cast<CXXRecordDecl>(RD))
        for (const CXXBaseSpecifier &B : CXX->bases())
          push(B.getType()->getAsRecordDecl());
      for (const FieldDecl *F : RD->fields())
        push(F->getType()->getAsRecordDecl());
    }
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

  /// Size and alignment of a type, in bits.
  ///
  /// An incomplete type has no size and getTypeInfo asserts rather than saying
  /// so, hence the guard. But `char data[]` is not simply unmeasurable: it
  /// occupies nothing *and* is aligned like a `char`, so the element type
  /// answers the alignment. Reporting zero for both would say a flexible array
  /// member has no alignment, which is a different and wrong claim.
  std::pair<int64_t, int64_t> typeInfoBits(QualType T) const {
    if (T->isDependentType())
      return {0, 0};
    if (const auto *IAT = Ctx.getAsIncompleteArrayType(T)) {
      const QualType Elem = IAT->getElementType();
      if (!Elem->isIncompleteType() && !Elem->isDependentType())
        return {0, int64_t(Ctx.getTypeInfo(Elem).Align)};
      return {0, 0};
    }
    if (T->isIncompleteType())
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
    // The enclosing record, when there is one. An anonymous record nested in
    // another is drawn inside its parent rather than listed on its own, and
    // this is how a viewer tells that case from `typedef struct { … } T;`,
    // which is anonymous too but is a record in its own right.
    O["parentRecordId"] =
        idOf(dyn_cast_or_null<RecordDecl>(RD->getDeclContext()));
    O["location"] = JsonDiagnostics::locationToJson(SM, RD->getLocation());
    // The whole declaration's extent, so a caret anywhere inside it can be
    // resolved to this record — including on a blank line or a closing brace,
    // where no member location would match.
    O["range"] = JsonDiagnostics::rangeToJson(SM, RD->getSourceRange())
                     .value_or(llvm::json::Value(nullptr));

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

    // Track which bits are occupied so padding is reported, not inferred. The
    // scan costs one bit per bit of the record, so a type holding a large array
    // is excluded rather than allowed to allocate without bound — no viewer
    // draws a byte map at that size anyway.
    const int64_t SizeBits = bits(L.getSize());
    const int64_t PaddingScanLimit = 1 << 23; // 1 MB of record
    const bool ScanPadding = SizeBits >= 0 && SizeBits <= PaddingScanLimit;
    std::vector<bool> Occupied(ScanPadding ? size_t(SizeBits) : 0, false);
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
    for (size_t B = 0; ScanPadding && B < Occupied.size();) {
      if (Occupied[B]) { ++B; continue; }
      size_t Start = B;
      while (B < Occupied.size() && !Occupied[B]) ++B;
      Runs.push_back(llvm::json::Object{{"startBits", int64_t(Start)},
                                        {"endBits", int64_t(B)}});
      PaddingBits += int64_t(B - Start);
    }
    O["paddingRuns"] = std::move(Runs);
    O["paddingBits"] = PaddingBits;

    // The drawing model: containment, extents, overlap and padding, worked out
    // where the facts are rather than inferred from them by the consumer.
    {
      RenderBuilder RB(Ctx, SM, Ids);
      O["render"] = RB.build(RD, L);
    }
    // Null rather than zero, so a consumer can tell "no padding" from "not
    // computed for a record this large".
    if (!ScanPadding)
      O["paddingBits"] = nullptr;

    return llvm::json::Value(std::move(O));
  }

  /// The names the user gave to types, with what they resolve to.
  llvm::json::Array emitTypedefs(const std::vector<const TypedefNameDecl *> &TDs) {
    llvm::json::Array Out;
    for (const TypedefNameDecl *TD : TDs) {
      SourceLocation Loc = TD->getLocation();
      if (!IncludeSystem &&
          !(Loc.isValid() && SM.isWrittenInMainFile(SM.getSpellingLoc(Loc))))
        continue;
      const QualType T = TD->getUnderlyingType();
      auto [SizeBits, AlignBits] = typeInfoBits(T);
      Out.push_back(llvm::json::Object{
          {"name", TD->getNameAsString()},
          {"qualifiedName", TD->getQualifiedNameAsString()},
          {"location", JsonDiagnostics::locationToJson(SM, Loc)},
          {"typeSpelling", printed(T)},
          {"canonicalTypeSpelling", printed(T.getCanonicalType())},
          {"sizeBits", SizeBits},
          {"alignBits", AlignBits},
          {"recordId", idOf(T->getAsRecordDecl())}});
    }
    return Out;
  }

  template <typename OccupyFn>
  llvm::json::Array emitVTableSlots(const RecordDecl *RD,
                                    const CXXRecordDecl *CXX,
                                    const ASTRecordLayout &L, OccupyFn occupy) {
    llvm::json::Array Slots;
    if (!CXX)
      return Slots;
    const int64_t PtrBits = Ctx.getTargetInfo().getPointerWidth(LangAS::Default);

    const bool OwnsVPtr = ownsVFPtr(Ctx, CXX, L);
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
  LayoutAction(bool IncludeSystem, llvm::json::Array &Records,
               llvm::json::Array &Typedefs)
      : IncludeSystem(IncludeSystem), Records(Records), Typedefs(Typedefs) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    class Consumer : public ASTConsumer {
    public:
      Consumer(bool IncludeSystem, llvm::json::Array &Records,
               llvm::json::Array &Typedefs)
          : IncludeSystem(IncludeSystem), Records(Records), Typedefs(Typedefs) {}
      void HandleTranslationUnit(ASTContext &Ctx) override {
        RecordCollector C;
        C.TraverseDecl(Ctx.getTranslationUnitDecl());
        LayoutWriter W(Ctx, IncludeSystem);
        Records = W.run(C.Order);
        Typedefs = W.typedefs(C.Typedefs);
      }

    private:
      bool IncludeSystem;
      llvm::json::Array &Records;
      llvm::json::Array &Typedefs;
    };
    return std::make_unique<Consumer>(IncludeSystem, Records, Typedefs);
  }

private:
  bool IncludeSystem;
  llvm::json::Array &Records;
  llvm::json::Array &Typedefs;
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

/// musl's per-architecture header tree for a triple.
///
/// The names are musl's own directory names, which mostly but not always match
/// LLVM's architecture spelling. Anything musl has no template for gets
/// "generic": a tree that takes every type from the compiler's own macros, so
/// it is correct for whatever target is being asked about.
///
/// The alternative, and what this used to do, was to serve no C library at all
/// off Linux — which meant `#include <string>` failed outright on Windows,
/// Darwin and every bare-metal target, because libc++ reaches for <wchar.h>
/// and <stdint.h> wherever it runs. Serving *another* architecture's tree
/// instead is worse than either: musl's x86_64 headers say `uint64_t` is
/// `unsigned long`, which on Windows is four bytes, and the struct comes out
/// 32 rather than 40 with nothing to say why.
///
/// musl's own trees are used where they apply, because they are complete:
/// `struct stat` and the rest of the operating-system layer are real there.
/// The generic tree has none of that, so asking about `struct stat` on a
/// Darwin target fails to find a header rather than quietly answering with
/// Linux's.
std::string muslArch(llvm::StringRef Triple) {
  const llvm::Triple T(Triple);
  // musl is a Linux C library and its headers encode Linux's structures, so
  // its own trees serve Linux and nothing else.
  if (!T.isOSLinux())
    return "generic";
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
  default:                     return "generic";
  }
}

/// How libc++ has to be configured for a target, and why.
///
/// A real __config_site records the one configuration a libc++ was built with.
/// This module answers for hundreds of targets, and two of those settings are
/// not properties of a build at all:
///
///   localization  libc++ picks its locale layer from the platform — Darwin's
///                 xlocale, the Microsoft CRT's _locale_t, glibc/musl's. We
///                 ship musl's, so anywhere else there is no locale API to
///                 compile against. Off, rather than stubbed: a stub would
///                 make <locale> parse and then answer about a type nobody
///                 has. Everything that does not need a locale — string,
///                 vector, map, shared_ptr, optional — is unaffected.
///   threads       libc++ requires a threading API and knows how to pick one
///                 for every operating system. A freestanding target has none,
///                 and saying so is what a real bare-metal build would do.
struct LibcxxConfig {
  bool Localization = true;
  bool Threads = true;
  /// The MSVC runtime headers, which we do not ship and cannot.
  bool VCRuntime = false;
};

/// Does libc++ know a threading API for this target?
///
/// Mirrors the list in libc++'s own __config: it picks pthreads for the
/// operating systems it knows and the Win32 API on Windows, and #errors
/// otherwise. Asking "does it have an OS" instead is close but not the same —
/// Solaris has one and libc++ has no branch for it, which is a hard error
/// rather than a fallback.
bool libcxxHasThreadApi(const llvm::Triple &T) {
  if (T.isOSWindows())
    return true;
  if (T.isOSDarwin())
    return true;
  switch (T.getOS()) {
  case llvm::Triple::Linux:
  case llvm::Triple::FreeBSD:
  case llvm::Triple::NetBSD:
  case llvm::Triple::OpenBSD:
  case llvm::Triple::Fuchsia:
  case llvm::Triple::WASI:
  case llvm::Triple::Emscripten:
  case llvm::Triple::AIX:
  case llvm::Triple::ZOS:
  case llvm::Triple::Hurd:
    return true;
  default:
    return false;
  }
}

LibcxxConfig libcxxConfigFor(llvm::StringRef Triple, llvm::StringRef Arch) {
  const llvm::Triple T(Triple);
  LibcxxConfig C;
  // The locale layer needs the platform's own C library. That is exactly the
  // case in which musl has a tree of its own.
  C.Localization = Arch != "generic";
  C.Threads = libcxxHasThreadApi(T);
  C.VCRuntime = T.isKnownWindowsMSVCEnvironment();
  return C;
}

std::string fail(llvm::StringRef Message) {
  llvm::json::Object O{{"ok", false},
                       {"error", Message},
                       {"clangVersion", clang::getClangFullVersion()},
                       {"exitCode", 1},
                       {"target", nullptr},
                       {"headers", nullptr},
                       {"diagnostics", llvm::json::Array{}},
                       {"diagnosticsText", ""},
                       {"typedefs", llvm::json::Array{}},
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

  // Order matters, and it is the reverse of the obvious one. Each layer reaches
  // the next with #include_next, so the *most* derived must come first:
  //
  //   libc++       <cstddef> includes <stddef.h> and expects to find libc++'s
  //                own wrapper, not clang's — put clang's first and libc++
  //                stops the build saying exactly that
  //   clang        its builtin <stddef.h> then include_next's to the C library
  //   musl (arch)  target-varying declarations: musl's own tree where it has
  //                one, otherwise `generic`, whose types come from the
  //                compiler's macros and are right for any target
  //   musl         everything else, unchanged and architecture-independent
  if (!ResourceDir.empty()) {
    Args.push_back("-resource-dir=" + ResourceDir);
    Args.push_back("-nostdinc");
  }
  if (IsCxx && !Sysroot.empty()) {
    Args.push_back("-isystem");
    Args.push_back(Sysroot + "/include/c++/v1");
  }
  if (!ResourceDir.empty()) {
    Args.push_back("-isystem");
    Args.push_back(ResourceDir + "/include");
  }
  const std::string Arch = muslArch(Triple);
  if (!Sysroot.empty()) {
    Args.push_back("-isystem");
    Args.push_back(Sysroot + "/include/musl-arch/" + Arch);
    Args.push_back("-isystem");
    Args.push_back(Sysroot + "/include/musl");
  }
  // libc++, configured for what this target actually has. Every value in
  // __config_site is #ifndef-guarded so these win.
  const LibcxxConfig LC = libcxxConfigFor(Triple, Arch);
  if (IsCxx && !Sysroot.empty()) {
    if (!LC.Localization)
      Args.push_back("-D_LIBCPP_HAS_LOCALIZATION=0");
    if (!LC.Threads)
      Args.push_back("-D_LIBCPP_HAS_THREADS=0");
    if (LC.VCRuntime)
      Args.push_back("-D_LIBCPP_NO_VCRUNTIME");
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

  llvm::json::Array Records, Typedefs;
  LayoutAction Action(IncludeSystem, Records, Typedefs);
  const bool Ok = CI.ExecuteAction(Action);

  llvm::json::Object O{
      {"ok", true},
      {"error", nullptr},
      {"clangVersion", clang::getClangFullVersion()},
      {"exitCode", (Ok && !CI.getDiagnostics().hasErrorOccurred()) ? 0 : 1},
      {"target", CI.hasTarget() ? emitTargetInfo(CI.getTarget(), Triple)
                                : llvm::json::Value(nullptr)},
      // What the answer was computed against. A consumer showing
      // `sizeof(std::string)` for a Windows target should be able to say that
      // the C declarations came from musl with this target's own scalar types,
      // and that <locale> is not available here — rather than leave the
      // difference to be discovered.
      {"headers",
       llvm::json::Object{
           {"cLibrary", Sysroot.empty() ? llvm::json::Value(nullptr)
                                        : llvm::json::Value("musl")},
           {"cLibraryArch", Sysroot.empty() ? llvm::json::Value(nullptr)
                                            : llvm::json::Value(Arch)},
           {"cxxLibrary", (IsCxx && !Sysroot.empty()) ? llvm::json::Value("libc++")
                                                      : llvm::json::Value(nullptr)},
           {"localization", IsCxx && LC.Localization},
           {"threads", IsCxx && LC.Threads}}},
      {"diagnostics", std::move(Diags->Diags)},
      // The same diagnostics as clang would have printed them. Rendering
      // compiler output — carets, source excerpts, colour — is clang's job,
      // and the consumer that used to do it got it subtly wrong.
      {"diagnosticsText", Diags->Text},
      {"typedefs", std::move(Typedefs)},
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
