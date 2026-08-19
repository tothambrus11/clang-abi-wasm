//===- render_model.inc - the drawing model, built where the facts are ----===//
//
// Included by abi_query.cpp into LayoutWriter. Kept in its own file because it
// answers a different question from the rest: not "what is the layout" but
// "what does a viewer draw", and the two are worth reading separately.
//
// Everything here was JavaScript. A viewer given only fields and bases has to
// rebuild the containment itself — which member encloses which, where a base
// subobject's own members sit, which extents overlap, which bytes nobody owns —
// and doing that from flat data means inferring structure that clang never
// stopped knowing. The inference was ~2500 lines and every bug found in this
// project so far lived in it.
//
// The split: clang owns facts and geometry, the viewer owns presentation.
// Offsets, extents, containment, overlap and padding are emitted here. Colour,
// collapse state and hit-testing stay in the viewer, because they are about a
// screen rather than about a record.
//
//===----------------------------------------------------------------------===//

/// A drawable extent: something that occupies bytes and can be pointed at.
struct Leaf {
  enum Kind { Field, BitField, Special } kind;
  std::string name;
  std::string type; // empty for a vtable slot
  int64_t offsetBits = 0;
  int64_t sizeBits = 0;
  int64_t alignBits = 0;
  std::vector<std::string> path; // enclosing labels, outermost first
  const RecordDecl *owner = nullptr;
  const FieldDecl *decl = nullptr; // null for vtable slots
  /// The member's type is empty and it shares an address, so it draws as nothing.
  bool sharesAddress = false;
};

/// A compound member: a base subobject, a record-typed field, or an anonymous
/// aggregate. Owns a contiguous run of leaves.
struct Group {
  std::string kind; // base | primary-base | vbase | primary-vbase | member
  std::string name;
  std::string type;
  int64_t offsetBits = 0;
  /// What it occupies *here*. A base may have its tail padding reused, so this
  /// is its non-virtual size rather than its sizeof.
  int64_t sizeBits = 0;
  int64_t typeSizeBits = 0;
  int64_t alignBits = 0;
  std::vector<std::string> path;
  const RecordDecl *owner = nullptr;
  const RecordDecl *target = nullptr;
  bool isBase = false;
  bool isUnion = false;
  std::optional<llvm::json::Value> location;
  size_t firstLeaf = 0, lastLeaf = 0; // [first, last)
  std::vector<size_t> childGroups;    // groups nested directly in this one
};

/// A zero-size thing worth marking even though it draws no bytes.
struct Marker {
  std::string kind; // empty-base | zero-bitfield
  std::string name;
  std::string type;
  int64_t offsetBits = 0;
  std::vector<std::string> path;
};

/// One record's drawing model, built by walking what clang already knows.
class RenderBuilder {
public:
  RenderBuilder(ASTContext &Ctx, const SourceManager &SM,
                const llvm::DenseMap<const RecordDecl *, unsigned> &Ids)
      : Ctx(Ctx), SM(SM), Ids(Ids) {}

  llvm::json::Value build(const RecordDecl *RD, const ASTRecordLayout &L) {
    Root = RD;
    visit(RD, L, /*base=*/0, {}, VBases::Here, TopLevel);
    return emit(RD, L);
  }

private:
  ASTContext &Ctx;
  const SourceManager &SM;
  const llvm::DenseMap<const RecordDecl *, unsigned> &Ids;
  const RecordDecl *Root = nullptr;

  std::vector<Leaf> Leaves;
  std::vector<Group> Groups;
  std::vector<Marker> Markers;
  std::vector<size_t> TopLevel; // group indices at the record's own level

  int64_t bits(CharUnits CU) const { return Ctx.toBits(CU); }

  std::string printed(QualType T) const {
    PrintingPolicy P = Ctx.getPrintingPolicy();
    P.SuppressTagKeyword = false;
    return T.getAsString(P);
  }

  /// How a base is labelled in the tree. A virtual base says so, because where
  /// it sits is a property of the most-derived object rather than of the base.
  static std::string baseLabel(llvm::StringRef Type, bool Virtual) {
    llvm::StringRef Bare = Type;
    for (llvm::StringRef Kw : {"struct ", "class ", "union ", "__interface "})
      if (Bare.starts_with(Kw)) {
        Bare = Bare.drop_front(Kw.size());
        break;
      }
    return Virtual ? ("virtual " + Bare).str() : Bare.str();
  }

  int64_t pointerBits() const {
    return Ctx.getTargetInfo().getPointerWidth(LangAS::Default);
  }

  /**
   * Has this record anything to draw inside it?
   *
   * A member whose type is an empty class is a leaf, not a compound: there is
   * nothing within it to expand, and treating it as a group would put an empty
   * container in the tree and take it out of reach of the address-sharing rule,
   * which is the only interesting thing about such a member.
   */
  static bool hasContents(const RecordDecl *RD) {
    if (const auto *CXX = dyn_cast<CXXRecordDecl>(RD))
      return !CXX->isEmpty();
    return !RD->field_empty();
  }

  /**
   * Where a record's virtual bases belong.
   *
   * `Here` for a complete object — the record being drawn, and any member,
   * which is a complete object of its own type. `Elsewhere` for a base
   * subobject: its virtual bases are placed once by the most derived object,
   * not repeated inside every class that inherits them.
   *
   * This used to be two nearly identical walk functions, and they drifted: the
   * base one recursed into a record-typed *member* with itself, so a member
   * reached through a base lost its own virtual bases and their bytes were
   * reported as padding.
   */
  enum class VBases { Here, Elsewhere };

  /**
   * Walk one record, adding its leaves and groups at absolute offsets.
   *
   * `base` is where this record starts inside the root, so every offset below
   * is absolute and a viewer never adds anything up. `into` collects the groups
   * introduced at this level, which is what makes the tree fall out of the walk
   * rather than have to be recovered from leaf indices afterwards.
   */
  void visit(const RecordDecl *RD, const ASTRecordLayout &L, int64_t base,
             std::vector<std::string> path, VBases Vb, std::vector<size_t> &into) {
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    std::function<void(const CXXBaseSpecifier &, bool)> addBase;

    // The vptr comes first wherever the record owns one.
    if (CXX) {
      const bool OwnsVPtr = ownsVFPtr(Ctx, CXX, L);
      if (OwnsVPtr) {
        Leaves.push_back({Leaf::Special, RD->getNameAsString() + " vtable pointer", "",
                          base, pointerBits(), pointerBits(), path, RD, nullptr, false});
      }
      if (L.hasOwnVBPtr()) {
        Leaves.push_back({Leaf::Special, RD->getNameAsString() + " vbtable pointer", "",
                          base + bits(L.getVBPtrOffset()), pointerBits(), pointerBits(),
                          path, RD, nullptr, false});
      }

      addBase = [&](const CXXBaseSpecifier &B, bool Virtual) {
        const auto *BD = B.getType()->getAsCXXRecordDecl();
        if (!BD || !RecordCollector::isLayoutable(BD))
          return;
        const ASTRecordLayout &BL = Ctx.getASTRecordLayout(BD);
        const int64_t Off =
            base + bits(Virtual ? L.getVBaseClassOffset(BD) : L.getBaseClassOffset(BD));
        const std::string Label = baseLabel(printed(B.getType()), Virtual);

        if (BD->isEmpty()) {
          // No storage at all: sizeof is 1 only because a complete object
          // cannot be zero-sized. It gets a marker, not an extent.
          Markers.push_back({"empty-base", Label, printed(B.getType()), Off, path});
        }

        Group G;
        G.kind = Virtual ? (L.getPrimaryBase() == BD ? "primary-vbase" : "vbase")
                         : (L.getPrimaryBase() == BD ? "primary-base" : "base");
        G.name = Label;
        G.type = printed(B.getType());
        G.offsetBits = Off;
        G.sizeBits = BD->isEmpty() ? 0 : bits(BL.getNonVirtualSize());
        G.typeSizeBits = bits(BL.getSize());
        G.alignBits = bits(BL.getAlignment());
        G.path = path;
        G.owner = RD;
        G.target = BD;
        G.isBase = true;
        G.isUnion = false;
        G.location = JsonDiagnostics::rangeToJson(SM, B.getSourceRange());
        G.firstLeaf = Leaves.size();

        std::vector<std::string> childPath = path;
        childPath.push_back(Label);
        std::vector<size_t> children;
        if (!BD->isEmpty())
          visit(BD, BL, Off, childPath, VBases::Elsewhere, children);
        G.lastLeaf = Leaves.size();
        G.childGroups = std::move(children);
        into.push_back(Groups.size());
        Groups.push_back(std::move(G));
      };

      for (const CXXBaseSpecifier &B : CXX->bases())
        if (!B.isVirtual())
          addBase(B, false);
      // Virtual bases are emitted after this record's own fields, below: the
      // ABI places them last, so doing it here would put a high-offset leaf
      // ahead of lower ones and leave the list out of offset order.
    }

    unsigned Index = 0;
    for (const FieldDecl *F : RD->fields()) {
      const int64_t Off = base + int64_t(L.getFieldOffset(Index));
      ++Index;
      QualType T = F->getType();
      const std::string Type = printed(T);

      if (F->isBitField()) {
        const unsigned Width = F->getBitWidthValue();
        if (F->isZeroLengthBitField()) {
          // A `:0` forces the next field to a storage boundary; it draws
          // nothing but explains the gap that follows.
          Markers.push_back({"zero-bitfield", F->getNameAsString().empty()
                                                  ? std::string(":0")
                                                  : F->getNameAsString(),
                             Type, Off, path});
        } else {
          Leaves.push_back({Leaf::BitField,
                            F->getNameAsString().empty() ? std::string("(pad bits)")
                                                         : F->getNameAsString(),
                            Type, Off, int64_t(Width), 0, path, RD, F, false});
        }
        continue;
      }

      const RecordDecl *Target = T->getAsRecordDecl();
      const bool Compound =
          Target && RecordCollector::isLayoutable(Target) && hasContents(Target);
      if (!Compound) {
        addFieldLeaf(RD, F, T, Type, Off, path);
        continue;
      }

      // A record-typed member. An anonymous one is transparent: its fields are
      // members of this record, and it is labelled so a viewer can say so.
      const ASTRecordLayout &TL = Ctx.getASTRecordLayout(Target);
      const bool Anonymous = F->getNameAsString().empty();
      const std::string Label = Anonymous ? "(anonymous)" : F->getNameAsString();

      Group G;
      G.kind = "member";
      G.name = Label;
      G.type = Type;
      G.offsetBits = Off;
      G.sizeBits = bits(TL.getSize());
      G.typeSizeBits = bits(TL.getSize());
      G.alignBits = bits(TL.getAlignment());
      G.path = path;
      G.owner = RD;
      G.target = Target;
      G.isBase = false;
      G.isUnion = Target->isUnion();
      G.location = locationValue(F);
      G.firstLeaf = Leaves.size();

      const size_t GroupsBefore = Groups.size();
      const size_t MarkersBefore = Markers.size();
      std::vector<std::string> childPath = path;
      childPath.push_back(Label);
      std::vector<size_t> children;
      // Here, not Elsewhere: a member is a complete object of its own type, so
      // its virtual bases are laid out inside it. Only a *base* subobject's are
      // hoisted to the most derived object.
      visit(Target, TL, Off, childPath, VBases::Here, children);

      // Nothing inside it occupies a bit — a record whose only member is a
      // zero-width bit-field, say. It still takes up space here, so it is
      // drawn, but as one block rather than as a container with nothing in it:
      // an expandable row that expands to nothing, and bytes a viewer cannot
      // point at, are both worse than a plain member. What is inside is still
      // reachable by inspecting the member's own record.
      if (Leaves.size() == size_t(G.firstLeaf)) {
        Groups.resize(GroupsBefore);
        Markers.resize(MarkersBefore);
        Leaves.push_back({Leaf::Field, Label, Type, Off, bits(TL.getSize()),
                          bits(TL.getAlignment()), path, RD, F,
                          /*sharesAddress=*/false});
        continue;
      }

      G.lastLeaf = Leaves.size();
      G.childGroups = std::move(children);
      into.push_back(Groups.size());
      Groups.push_back(std::move(G));
    }

    // Last, as the ABI lays them — and only where they belong.
    if (CXX && Vb == VBases::Here)
      for (const CXXBaseSpecifier &B : CXX->vbases())
        addBase(B, true);
  }

  /**
   * One field that draws as a single block, with the extent clang gives it.
   *
   * `isZeroSize` is the whole point. A member whose type is empty occupies a
   * byte of its own unless it is allowed to share an address — and whether it
   * is depends on `[[no_unique_address]]`, on the ABI, and on what else is
   * there. The model used to work it out by scanning for a neighbour covering
   * the same byte, which gets `struct S { [[no_unique_address]] E e; };` wrong:
   * nothing else is there, so the scan concluded `e` owned the byte, while the
   * record's own padding scan — which asks clang — called the same byte
   * padding. Two figures in one response disagreeing is the tell that one of
   * them was inferred.
   */
  void addFieldLeaf(const RecordDecl *RD, const FieldDecl *F, QualType T,
                    const std::string &Type, int64_t Off,
                    const std::vector<std::string> &path) {
    auto [SizeBits, AlignBits] = typeInfoBits(T);
    // A trailing `T x[]` sits at sizeof and occupies nothing.
    const bool Flexible = Ctx.getAsIncompleteArrayType(T) && RD->hasFlexibleArrayMember();
    const bool ZeroSize = F->isZeroSize(Ctx);
    Leaves.push_back({Leaf::Field,
                      F->getNameAsString().empty() ? std::string("(anonymous)")
                                                   : F->getNameAsString(),
                      Type, Off, (Flexible || ZeroSize) ? 0 : SizeBits, AlignBits,
                      path, RD, F, ZeroSize});
  }

  std::optional<llvm::json::Value> locationValue(const FieldDecl *F) const {
    llvm::json::Value V = JsonDiagnostics::locationToJson(SM, F->getLocation());
    if (V.kind() == llvm::json::Value::Null)
      return std::nullopt;
    return V;
  }

  /// Size and alignment in bits. An incomplete array — a flexible array member
  /// — occupies nothing but is aligned like its element, so the element answers.
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

  llvm::json::Value idOf(const RecordDecl *RD) const {
    if (!RD)
      return nullptr;
    auto It = Ids.find(RD);
    return It == Ids.end() ? llvm::json::Value(nullptr) : llvm::json::Value(int64_t(It->second));
  }

  static llvm::json::Array pathJson(const std::vector<std::string> &P) {
    llvm::json::Array A;
    for (const std::string &S : P)
      A.push_back(S);
    return A;
  }

  /// Do two extents share a byte? Zero-size things never overlap anything.
  static bool overlaps(int64_t aOff, int64_t aSize, int64_t bOff, int64_t bSize) {
    return aSize > 0 && bSize > 0 && aOff < bOff + bSize && bOff < aOff + aSize;
  }

  /// The tree a table renders, straight out of the walk.
  llvm::json::Value treeJson(const std::vector<size_t> &groupsHere, size_t leafLo,
                             size_t leafHi) const {
    // Nodes at this level: the groups introduced here, plus the leaves none of
    // them covers, in offset order.
    struct Node {
      bool isGroup;
      size_t ref;
      int64_t off;
      int64_t size;
      /// Where this node's content starts in the leaf list. Ties at the same
      /// offset are broken by it, so a vtable pointer at offset 0 still
      /// precedes a base that also starts at 0 — which is the order they were
      /// laid down in, and the order the dump prints them.
      size_t seq;
    };
    std::vector<Node> Nodes;
    std::vector<bool> Covered(leafHi > leafLo ? leafHi - leafLo : 0, false);
    for (size_t gi : groupsHere) {
      const Group &G = Groups[gi];
      Nodes.push_back({true, gi, G.offsetBits, G.sizeBits, G.firstLeaf});
      for (size_t li = G.firstLeaf; li < G.lastLeaf; ++li)
        if (li >= leafLo && li < leafHi)
          Covered[li - leafLo] = true;
    }
    for (size_t li = leafLo; li < leafHi; ++li)
      if (!Covered[li - leafLo])
        Nodes.push_back({false, li, Leaves[li].offsetBits, Leaves[li].sizeBits, li});

    std::stable_sort(Nodes.begin(), Nodes.end(), [](const Node &a, const Node &b) {
      if (a.off != b.off)
        return a.off < b.off;
      return a.seq < b.seq;
    });

    llvm::json::Array Out;
    for (size_t i = 0; i < Nodes.size(); ++i) {
      const Node &N = Nodes[i];
      bool Overlap = false;
      for (size_t j = 0; j < Nodes.size() && !Overlap; ++j)
        if (i != j && overlaps(N.off, N.size, Nodes[j].off, Nodes[j].size))
          Overlap = true;

      llvm::json::Object O{{"kind", N.isGroup ? "group" : "leaf"},
                           {"ref", int64_t(N.ref)},
                           {"overlaps", Overlap}};
      if (N.isGroup) {
        const Group &G = Groups[N.ref];
        O["children"] = treeJson(G.childGroups, G.firstLeaf, G.lastLeaf);
      } else {
        O["children"] = llvm::json::Array{};
      }
      Out.push_back(std::move(O));
    }
    return llvm::json::Value(std::move(Out));
  }

  llvm::json::Value emit(const RecordDecl *RD, const ASTRecordLayout &L) {
    llvm::json::Array LeavesJson;
    for (const Leaf &Lf : Leaves) {
      const char *Kind = Lf.kind == Leaf::Field      ? "field"
                         : Lf.kind == Leaf::BitField ? "bitfield"
                                                     : "special";
      llvm::json::Object O{{"kind", Kind},
                           {"name", Lf.name},
                           {"type", Lf.type.empty() ? llvm::json::Value(nullptr)
                                                    : llvm::json::Value(Lf.type)},
                           {"offsetBits", Lf.offsetBits},
                           {"sizeBits", Lf.sizeBits},
                           {"alignBits", Lf.alignBits},
                           {"path", pathJson(Lf.path)},
                           {"ownerId", idOf(Lf.owner)},
                           {"ownerName", Lf.owner ? Lf.owner->getNameAsString() : ""},
                           {"sharesAddress", Lf.sharesAddress}};
      O["location"] = Lf.decl ? JsonDiagnostics::locationToJson(SM, Lf.decl->getLocation())
                              : llvm::json::Value(nullptr);
      LeavesJson.push_back(std::move(O));
    }

    llvm::json::Array GroupsJson;
    for (const Group &G : Groups) {
      llvm::json::Array Indexes;
      for (size_t li = G.firstLeaf; li < G.lastLeaf; ++li)
        Indexes.push_back(int64_t(li));
      GroupsJson.push_back(llvm::json::Object{
          {"kind", G.kind},
          {"name", G.name},
          {"type", G.type},
          {"offsetBits", G.offsetBits},
          {"sizeBits", G.sizeBits},
          {"typeSizeBits", G.typeSizeBits},
          {"alignBits", G.alignBits},
          {"path", pathJson(G.path)},
          {"ownerId", idOf(G.owner)},
          {"ownerName", G.owner ? G.owner->getNameAsString() : ""},
          {"recordId", idOf(G.target)},
          {"isBase", G.isBase},
          {"isUnion", G.isUnion},
          {"leafIndexes", std::move(Indexes)},
          {"location", G.location.value_or(llvm::json::Value(nullptr))}});
    }

    llvm::json::Array MarkersJson;
    for (const Marker &M : Markers)
      MarkersJson.push_back(llvm::json::Object{{"kind", M.kind},
                                               {"name", M.name},
                                               {"type", M.type},
                                               {"offsetBits", M.offsetBits},
                                               {"path", pathJson(M.path)}});

    // Padding: bytes inside sizeof that no leaf covers. Computed here because
    // it is a fact about the layout, and because a viewer computing it has to
    // re-derive coverage that this walk already established.
    //
    // Byte granularity, not bit, and the name says so — the record's own
    // `paddingBits` is the bit-exact figure and these two are different
    // numbers whenever a bit-field is involved. A byte a bit-field touches
    // belongs to that field: the grid draws it as such, and calling the
    // remaining bits of its storage unit "padding" would report gaps nothing
    // can point at.
    const int64_t SizeBits = bits(L.getSize());
    llvm::json::Array Runs;
    int64_t PaddingBytes = 0;
    const int64_t ScanLimit = 1 << 23;
    // Null rather than zero above the limit: a viewer must be able to tell "no
    // padding" from "not measured for a record this large".
    const bool Scanned = SizeBits >= 0 && SizeBits <= ScanLimit;
    if (Scanned) {
      const int64_t SizeBytes = SizeBits / 8;
      std::vector<bool> Occupied(size_t(std::max<int64_t>(SizeBytes, 0)), false);
      for (const Leaf &Lf : Leaves) {
        if (Lf.sizeBits <= 0)
          continue;
        const int64_t From = std::max<int64_t>(Lf.offsetBits / 8, 0);
        const int64_t To = std::min<int64_t>((Lf.offsetBits + Lf.sizeBits + 7) / 8, SizeBytes);
        for (int64_t B = From; B < To; ++B)
          Occupied[size_t(B)] = true;
      }
      for (size_t B = 0; B < Occupied.size();) {
        if (Occupied[B]) { ++B; continue; }
        const size_t Start = B;
        while (B < Occupied.size() && !Occupied[B]) ++B;
        Runs.push_back(
            llvm::json::Object{{"startBits", int64_t(Start) * 8}, {"endBits", int64_t(B) * 8}});
        PaddingBytes += int64_t(B - Start);
      }
    }

    (void)RD;
    return llvm::json::Object{{"leaves", std::move(LeavesJson)},
                              {"groups", std::move(GroupsJson)},
                              {"markers", std::move(MarkersJson)},
                              {"tree", treeJson(TopLevel, 0, Leaves.size())},
                              {"paddingRuns", std::move(Runs)},
                              {"paddingBytes", Scanned ? llvm::json::Value(PaddingBytes)
                                                       : llvm::json::Value(nullptr)}};
  }
};
