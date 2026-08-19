/**
 * The wire contract between the wasm library and JavaScript.
 *
 * Two rules shape every field here, both learned from the text-dump pipeline
 * this replaces:
 *
 *  1. **Sizes and offsets are always bits.** Clang's own interfaces mix bits
 *     (field offsets) with CharUnits (record sizes), and every conversion is a
 *     chance to be wrong by a factor of eight. One unit, named in every field.
 *
 *  2. **Cross-references are ids, never names.** A field whose type is a record
 *     carries that record's `id`. Matching printed type spellings against
 *     printed record names — through elaborated specifiers, anonymous-namespace
 *     qualifiers, template arguments and `(unnamed at f.c:3:9)` forms — was the
 *     single largest source of heuristics, and of bugs, in the previous design.
 *
 * Consumers should need exactly one `JSON.parse` and no interpretation after it.
 */

// ---------------------------------------------------------------- request --

export interface AbiRequest {
  /** Source text of the translation unit. */
  source: string;
  /** Name to compile it as. Defaults to `input.c` / `input.cc` by `lang`. */
  filename?: string;
  /** Defaults to `c++` when `filename` ends in a C++ extension, else `c`. */
  lang?: 'c' | 'c++';
  /** e.g. `gnu17`, `c++23`. Omit for clang's default for the language. */
  std?: string;
  /** Target triple. Any triple clang has a `TargetInfo` for; see `targets()`. */
  triple: string;
  /** Extra clang arguments, appended after the built-in ones. */
  flags?: string[];
  /**
   * Also report records declared outside the submitted file. Off by default:
   * one `#include <string>` contributes over a thousand library records.
   */
  includeSystemRecords?: boolean;
}

// --------------------------------------------------------------- location --

export interface Location {
  /** '' when the location is not in a file (builtin, command line). */
  file: string;
  /** 1-based. */
  line: number;
  /** 1-based, in bytes. */
  col: number;
  /** 1-based, exclusive: one past the last byte of the token. */
  endCol: number;
  /** True when this is the file the caller submitted, not a header. */
  isMainFile: boolean;
}

/** A span that may cross lines — a base specifier, a diagnostic highlight. */
export interface SourceRange {
  line: number;
  col: number;
  endLine: number;
  endCol: number;
}

// ------------------------------------------------------------ diagnostics --

export type Severity = 'note' | 'remark' | 'warning' | 'error' | 'fatal';

export interface Diagnostic {
  severity: Severity;
  /** Rendered text, no ANSI escapes and no `file:line:col:` prefix. */
  message: string;
  location: Location | null;
  /** Highlight ranges clang attached to the diagnostic. */
  ranges: SourceRange[];
  /** The flag that controls it, e.g. `-Wpadded`. Null when unconditional. */
  option: string | null;
}

// ------------------------------------------------------------------ types --

export type RecordKind = 'struct' | 'class' | 'union' | 'interface';
export type Access = 'public' | 'protected' | 'private';

/** A vtable-adjacent pointer or displacement the ABI inserts. */
export interface VTableSlot {
  kind: 'vptr' | 'vbptr' | 'vtordisp';
  /** Display text, e.g. `Base vtable pointer`. */
  label: string;
  offsetBits: number;
  /**
   * Real size from the target, not a constant. The MS ABI's vtordisp is four
   * bytes regardless of pointer width, which the old pipeline hardcoded.
   */
  sizeBits: number;
}

export interface Base {
  /** The record this base is an instance of. Index into `records` by `id`. */
  recordId: number;
  /** The type as written in the base clause. */
  typeSpelling: string;
  offsetBits: number;
  /**
   * Bytes it occupies *here* — its non-virtual size. A derived class may reuse
   * a base's tail padding, so this can be less than `typeSizeBits`.
   */
  sizeBits: number;
  /** `sizeof` the base type on its own. */
  typeSizeBits: number;
  isVirtual: boolean;
  /** The primary base, laid down at offset 0 and sharing the vptr. */
  isPrimary: boolean;
  /** Empty base optimization applied: occupies no storage. */
  isEmpty: boolean;
  access: Access;
  /**
   * Where the base is written. Clang exposes this through no dump format —
   * neither `-ast-dump=json` (which emits `bases` without ranges) nor the text
   * dump — and it is why this library exists rather than another parser.
   */
  location: SourceRange | null;
}

export interface Field {
  id: number;
  /** '' for an anonymous struct/union member. */
  name: string;
  offsetBits: number;
  /** The type as written, e.g. `uint64_t`. */
  typeSpelling: string;
  /** Fully desugared, e.g. `unsigned long`. Equal to `typeSpelling` if same. */
  canonicalTypeSpelling: string;
  /** From the target's type info. No probe translation units involved. */
  sizeBits: number;
  alignBits: number;
  /** `alignas`/`_Alignas` on the declaration, when it raises the alignment. */
  explicitAlignBits: number | null;
  isBitField: boolean;
  /** Declared width for a bit-field, else null. Zero for `unsigned : 0`. */
  bitWidth: number | null;
  isZeroWidthBitField: boolean;
  /**
   * A trailing `T x[]`. It sits at `sizeof` and occupies nothing, which is why
   * `sizeBits` is 0 here rather than an estimated byte.
   */
  isFlexibleArrayMember: boolean;
  /** An anonymous aggregate whose own fields are members of the parent. */
  isAnonymousMember: boolean;
  /** Set when the field's type is a record also present in `records`. */
  recordId: number | null;
  /** `[[no_unique_address]]` was honoured and the member shares an address. */
  isNoUniqueAddress: boolean;
  location: Location | null;
  access: Access;
}

export interface PaddingRun {
  startBits: number;
  endBits: number;
}

export interface RecordLayout {
  /** Stable within one response. Referenced by `Field.recordId`, `Base.recordId`. */
  id: number;
  kind: RecordKind;
  /** Unqualified name; '' when the record has no name of its own. */
  name: string;
  /** Fully qualified, e.g. `ns::Outer::Inner`. */
  qualifiedName: string;
  /** How clang prints it, including `(unnamed struct at f.c:3:9)` forms. */
  printedName: string;
  isAnonymous: boolean;
  isEmpty: boolean;
  isPolymorphic: boolean;
  isStandardLayout: boolean;
  /** Declared in the file the caller submitted. */
  isUserCode: boolean;
  location: Location | null;
  /**
   * The whole declaration's extent. A caret inside it belongs to this record
   * even where no member is declared — a blank line, the closing brace — which
   * is how an editor resolves "which record am I in".
   */
  range: SourceRange | null;

  sizeBits: number;
  alignBits: number;
  /** Size without tail padding — what a derived class may reuse. */
  dataSizeBits: number;
  /** C++ only; equals `sizeBits` when there are no virtual bases. */
  nonVirtualSizeBits: number;
  nonVirtualAlignBits: number;
  /** The alignment the ABI prefers where it exceeds the required one. */
  preferredAlignBits: number;

  bases: Base[];
  fields: Field[];
  vtableSlots: VTableSlot[];

  /** Bits inside `sizeBits` that no member occupies, already merged and sorted. */
  paddingRuns: PaddingRun[];
  /**
   * Bit-exact, and about *this record's own level*: a member counts as its
   * whole `sizeof`, so a hole inside a nested record is that record's padding
   * and not this one's. `Render.paddingBytes` answers the other question —
   * whole bytes, everything nested — and the two are equal exactly when
   * neither difference bites. Null when the record is too large to scan.
   */
  paddingBits: number | null;

  /** The enclosing record, when this one is declared inside another. */
  parentRecordId: number | null;

  /** Everything needed to draw this record. See `Render`. */
  render: Render;
}

// ------------------------------------------------------------ the drawing --

/**
 * The record as something to put on a screen: which extents exist, what
 * contains what, what overlaps what, and where the gaps are.
 *
 * This is the part that is easy to assume a consumer should work out for
 * itself, and it is not. Recovering containment from a flat list of offsets
 * means guessing which field of which base a byte belongs to, and the guess
 * fails exactly where layout is interesting — an empty base at the same address
 * as the first member, a virtual base that moves, a member whose tail padding
 * the derived class reuses. Clang knows all of it while it is laying the record
 * out, so it is reported instead of reconstructed.
 */
export interface Render {
  /** Every extent that occupies bytes, in ABI order. */
  leaves: RenderLeaf[];
  /** Compound members — bases, record-typed fields, anonymous aggregates. */
  groups: RenderGroup[];
  /** Things worth naming that occupy nothing at all. */
  markers: RenderMarker[];
  /** Containment, as a forest over `leaves` and `groups`. */
  tree: RenderNode[];
  /**
   * The gaps, as a byte map draws them: a byte any member touches belongs to
   * that member, so a bit-field's storage unit is not partly padding here.
   */
  paddingRuns: PaddingRun[];
  /**
   * How many bytes those runs cover. Deliberately *not* `RecordLayout`'s
   * `paddingBits`, which differs on two axes:
   *
   *   granularity  bit-exact there, whole bytes here — the byte holding
   *                `unsigned a : 3` is drawn as `a`, not as mostly padding.
   *   depth        this record's own level there, everything nested here —
   *                `struct S { I i; double d; }` where `I` has a three-byte
   *                hole reports 0 there and 3 here, and 3 is what a reader
   *                wants to know.
   *
   * Both are true. They answer different questions, which is why they are not
   * the same name.
   *
   * Null when the record is too large to scan — not zero.
   */
  paddingBytes: number | null;
}

export interface RenderLeaf {
  kind: 'field' | 'bitfield' | 'special';
  /** '' for an unnamed member; `special` leaves carry a printable label. */
  name: string;
  type: string | null;
  offsetBits: number;
  sizeBits: number;
  /** 0 for a bit-field, which has no alignment of its own. */
  alignBits: number;
  /** Labels of the enclosing members, outermost first. */
  path: string[];
  /** The record that declares it. */
  ownerId: number | null;
  ownerName: string;
  /** Empty type at an address something else already covers ([[no_unique_address]]). */
  sharesAddress: boolean;
  location: Location | null;
}

export interface RenderGroup {
  kind: 'member' | 'base' | 'primary-base' | 'vbase' | 'primary-vbase';
  name: string;
  type: string;
  offsetBits: number;
  /** What it occupies here — smaller than `typeSizeBits` when tail padding is reused. */
  sizeBits: number;
  /** `sizeof` of its own type. */
  typeSizeBits: number;
  alignBits: number;
  path: string[];
  ownerId: number | null;
  ownerName: string;
  /** The record it is an instance of, for drilling in. */
  recordId: number | null;
  isBase: boolean;
  isUnion: boolean;
  /** Indices into `leaves` this group covers. */
  leafIndexes: number[];
  /** A base carries its specifier's span; a member, its name's position. */
  location: Location | SourceRange | null;
}

export interface RenderMarker {
  kind: 'empty-base' | 'zero-bitfield';
  name: string;
  type: string;
  offsetBits: number;
  path: string[];
}

export interface RenderNode {
  kind: 'leaf' | 'group';
  /** Index into `leaves` or `groups`. */
  ref: number;
  /** Its bytes intersect a sibling's — a union, EBO, reused tail padding. */
  overlaps: boolean;
  children: RenderNode[];
}

// ----------------------------------------------------------------- names --

/** A name the user gave to a type. */
export interface Typedef {
  name: string;
  qualifiedName: string;
  location: Location | null;
  /** What it names, as written. */
  typeSpelling: string;
  canonicalTypeSpelling: string;
  sizeBits: number;
  alignBits: number;
  /** Set when it names a record, so no name matching is needed to find it. */
  recordId: number | null;
}

/** Facts about the target itself, available without naming a record. */
export interface TargetInfo {
  triple: string;
  /** Canonical form clang normalised the request's triple to. */
  normalizedTriple: string;
  pointerSizeBits: number;
  pointerAlignBits: number;
  isLittleEndian: boolean;
  charSizeBits: number;
  shortSizeBits: number;
  intSizeBits: number;
  longSizeBits: number;
  longLongSizeBits: number;
  /** Which C++ ABI decides record layout. */
  cxxAbi: 'itanium' | 'microsoft' | string;
  /** True when `char` is signed on this target. */
  isCharSigned: boolean;
}

/** What a query was answered against, when the answer depends on it. */
export interface Headers {
  /** The C library whose declarations were used; null when none was needed. */
  cLibrary: 'musl' | null;
  /**
   * Which tree of it. An architecture name means musl's own headers, complete
   * down to `struct stat`. `generic` means a target musl has no tree for —
   * Windows, Darwin, anything bare-metal — served with musl's portable headers
   * over types taken from the compiler's own macros, so every scalar is right
   * for this target. That layer has no operating-system structures at all, so
   * `<sys/stat.h>` there is a missing header rather than Linux's answer.
   */
  cLibraryArch: string | null;
  cxxLibrary: 'libc++' | null;
  /**
   * False where libc++'s locale layer needs a platform C library we do not
   * ship. `<string>`, `<vector>`, `<map>` and friends are unaffected;
   * `<locale>` and `<iostream>` are not available.
   */
  localization: boolean;
  /** False on targets with no operating system, as a bare-metal build would be. */
  threads: boolean;
}

export interface AbiResponse {
  /** False only when the request itself was malformed (bad triple, bad JSON). */
  ok: boolean;
  /** Message explaining `ok: false`; null otherwise. */
  error: string | null;
  clangVersion: string;
  /** Non-zero if the TU had errors. Records may still be present and valid. */
  exitCode: number;
  target: TargetInfo;
  /** How the standard headers were configured for this target. */
  headers: Headers;
  diagnostics: Diagnostic[];
  /**
   * The same diagnostics as clang would have printed them: source excerpts,
   * carets, ANSI colour. Formatting compiler output is the compiler's job —
   * a consumer that re-renders from the structured list gets the column
   * arithmetic subtly wrong and the caret lands one character off.
   */
  diagnosticsText: string;
  /** Type names declared in the submitted file. */
  typedefs: Typedef[];
  records: RecordLayout[];
}

// -------------------------------------------------------------------- API --

export interface AbiModule {
  /** Analyse one translation unit. */
  query(request: AbiRequest): AbiResponse;
  /**
   * Every triple this build has a `TargetInfo` for. Clang exposes this to no
   * command-line flag, which is why the app ships a hand-curated list today.
   */
  targets(): string[];
  /** e.g. `clang version 22.1.8`. */
  version(): string;
}

export interface LoadOptions {
  /** Directory or URL the .wasm and header pack are served from. */
  baseUrl?: string | URL;
  /** Progress while the module and headers download. */
  onProgress?: (done: number, total: number, phase: 'wasm' | 'headers') => void;
}

/** Instantiate the module. Resolves once it can answer queries. */
export function load(options?: LoadOptions): Promise<AbiModule>;
