# clang-abi-wasm

Record layout for any target clang knows, as structured data, in the browser.

```js
import { load } from 'clang-abi-wasm';

const abi = await load();
const { records } = abi.query({
  triple: 'aarch64-apple-macosx',
  lang: 'c++',
  source: 'struct Base { virtual ~Base(); int x; };\nstruct D : Base { char c; };',
});

records[1].bases[0].offsetBits;   // 0
records[1].bases[0].location;     // { line: 2, col: 12, endLine: 2, endCol: 16 }
records[1].fields[0].sizeBits;    // 8
records[1].paddingRuns;           // [{ startBits: 104, endBits: 128 }]
```

No text is parsed on the JavaScript side. There is one `JSON.parse` and then
field reads.

## Why this exists

The usual way to get layout information out of clang in a browser is to run the
driver and read what it prints — `-fdump-record-layouts` for offsets,
`-ast-dump=json` for source locations, and a probe translation unit per member
for sizes. That works, and it is how [ABI Explorer](https://abiexplorer.org) was
built. It also has three costs that do not go away with effort:

**Some things are not printed at all.** A base specifier has no source location
in any dump format. `-ast-dump=json` emits `bases` with a type and an access and
no range; the text dump prints `|-public 'Base'` bare. So "highlight the bytes
this base contributes when I hover it" is not implementable against the dumps.
`CXXBaseSpecifier::getSourceRange()` has been there the whole time.

**The join is by printed name.** The layout dump identifies a member's type by
how clang spells it; the AST dump identifies a record by how clang spells
*that*. Matching them means normalising elaborated specifiers, anonymous
namespace qualifiers, template arguments, and `(unnamed struct at f.c:3:9)`
forms — and getting it wrong in the cases where two spellings collide. Here
every reference is an integer id.

**Sizes cost a compile each.** `sizeof` a member is not in the layout dump, so
the old pipeline appended one probe struct per member and compiled the whole
translation unit again, up to four times over as candidate spellings failed.
`ASTContext::getTypeInfo` answers the same question from the AST already in
memory.

The measurements behind the decision are in [`docs/why-frontend-only.md`](docs/why-frontend-only.md).

## The build is frontend-only

`LLVM_TARGETS_TO_BUILD` is empty. That sounds like it would cost targets and
does not: `-fsyntax-only` never reaches a backend, and clang's per-target ABI
knowledge lives in `Basic/Targets`, compiled in regardless. The stock
[@yowasp/clang](https://github.com/YoWASP/clang) bundle registers only the two
wasm backends and still lays records out correctly for 35 of 41 deliberately
exotic triples — the ABI tables are not the backends.

So the build drops LLVM CodeGen, MC, the assembler, lld, the static analyzer,
the sanitizers, OpenMP, CUDA/HIP, OpenCL, LTO and PGO — all verified present in
the stock bundle, none reachable from a layout query — and keeps every target.

`abi.targets()` enumerates what the build supports, which clang exposes to no
command-line flag. (`-print-targets` lists built backends, a different and much
shorter list; this is why applications ship hand-curated dropdowns and users hit
"unknown target triple" past the edge of them.)

## What ships in the payload

Anything small enough that carrying it beats fetching it, because the point is
that the app keeps working offline once loaded. See
[`scripts/package-headers.sh`](scripts/package-headers.sh) for the policy and
its reasoning; in short:

| | |
|---|---|
| **kept** | clang builtin headers, target intrinsics (avx/neon/altivec), libc++, musl per architecture |
| **dropped** | static archives (`-fsyntax-only` never links), OpenCL, CUDA/HIP, sanitizer headers |

musl is the notable addition. The stock bundle ships exactly one C library —
wasi-libc — and uses it for every target, which means any 32-bit target fails
outright on any libc header (`size_t` is hardcoded 64-bit) and
`sizeof(struct stat)` comes back as wasi's number no matter what you asked for.
musl is MIT, genuinely multi-arch, and fixes both.

## Building

```sh
scripts/bootstrap.sh        # once: LLVM source, native LLVM, emsdk, wasm LLVM
scripts/build.sh native     # seconds: compile and run the native harness
scripts/build.sh wasm       # ~a minute: dist/abi_query.{mjs,wasm,data}
```

`bootstrap.sh` is the slow one and is stamped at every step, so re-running it
is cheap and a failed run resumes. Everything it produces lives in
`~/.cache/clang-abi-wasm`, outside the repo — `git clean -xfd` never costs you
the toolchain.

Develop against the **native** harness. It is the same source file and the same
entry point, it runs in milliseconds, and a debugger works:

```sh
echo '{"triple":"x86_64-unknown-linux-gnu","source":"struct S{char a;int b;};"}' \
  | build/native/abi_query_test | jq .
```

The wasm link is mechanical once the native build is clean.

## Using a local build in a web app

```sh
scripts/dev-link.sh ~/abi-explorer-2
```

Symlinks `dist/` into the app's `public/vendor/abi/` and `npm link`s the JS
package, so `scripts/build.sh wasm` plus a reload is the whole iteration loop —
no publish, no version bump, and the app's type checking sees the schema you are
editing.

## The contract

[`js/index.d.ts`](js/index.d.ts) is the source of truth for both the request and
the response. Two rules run through it:

- **Sizes and offsets are always bits**, named as such in every field. Clang's
  own interfaces mix bits with `CharUnits`, and every conversion is a chance to
  be wrong by a factor of eight.
- **Cross-references are ids, never names.** `records[field.recordId]` resolves
  a member's type; `records[base.recordId]` resolves a base.

## License

Apache-2.0 WITH LLVM-exception, matching LLVM. The shipped headers keep their
own licenses: libc++ is Apache-2.0 WITH LLVM-exception, musl is MIT.
