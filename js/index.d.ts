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
  paddingBits: number;
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

export interface AbiResponse {
  /** False only when the request itself was malformed (bad triple, bad JSON). */
  ok: boolean;
  /** Message explaining `ok: false`; null otherwise. */
  error: string | null;
  clangVersion: string;
  /** Non-zero if the TU had errors. Records may still be present and valid. */
  exitCode: number;
  target: TargetInfo;
  diagnostics: Diagnostic[];
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
