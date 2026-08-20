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

Measured against the [@yowasp/clang](https://github.com/YoWASP/clang) bundle it
replaces, on the same machine:

| | stock clang | this |
|---|---:|---:|
| wasm | 75.5 MB | **28 MB** |
| download (gzip) | 27 MB | **10.8 MB** |
| instantiate | 329 ms | **107 ms** |
| plain C query | 33 ms | **2 ms** |
| C++ with `<string>` | 948 ms | **459 ms** |

The per-query collapse is the library shape rather than the smaller binary: no
process start, no driver, no argv parsing, and one `ASTContext` answering
everything the driver needed six separate compiles for.

Where the remaining time goes: a query with no `#include` is 1.7 ms, and the
same query with `<string>` is 440 ms. Almost all of that is clang parsing
libc++ — the same source natively takes ~300 ms, so there is no wasm penalty
worth chasing, and building the model from the parsed AST costs about 7 ms of
it. The lever that would move it is a precompiled preamble, the way clangd
avoids re-parsing an unchanged include block while the body below it is edited.
It would want the record walk restricted to the main file at the same time, or
the traversal would simply deserialize back everything the preamble saved.
Not done; measured, and worth doing when someone is editing C++ rather than
looking at an example.

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

The measurements behind the decision are in
[`docs/why-frontend-only.md`](docs/why-frontend-only.md); how a release is cut and
delivered is in [`docs/release-format.md`](docs/release-format.md).

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
| **kept** | clang builtin headers, target intrinsics (avx/neon/altivec), libc++, musl per architecture, a generic architecture layer |
| **dropped** | static archives (`-fsyntax-only` never links), OpenCL, CUDA/HIP, sanitizer headers |

### The standard library, on any target

Answering `sizeof(std::string)` means libc++ has to parse, and libc++ reaches
for `<wchar.h>` and `<stdint.h>` wherever it runs. So a C library is not
optional, and serving the wrong one is worse than serving none: musl's x86_64
headers say `uint64_t` is `unsigned long`, which on Windows is four bytes, and
the struct comes out 32 instead of 40 with nothing to say why.

Three layers, chosen per target:

| target | C declarations | what is available |
|---|---|---|
| Linux, an architecture musl supports | musl's own tree | everything, down to `struct stat` |
| anything else | musl's portable headers over a **generic** `bits/` whose every type comes from the compiler's own macros | the standard C and C++ headers; no operating-system structures |
| — | — | `<sys/stat.h>` on a Darwin target is a missing header, not Linux's answer |

Two libc++ settings follow the target rather than the build, because they are
not properties of a build: **localization** is off where libc++'s locale layer
would need a platform C library we do not ship (so `<string>`, `<vector>` and
`<map>` work everywhere while `<locale>` and `<iostream>` are Linux-only), and
**threads** are off on targets with no operating system, which is what a real
bare-metal build would say. Every response reports which of these applied,
under `headers`.

The result: `#include <string>` resolves on Linux, Windows, Darwin, iOS, WASI,
Emscripten, Solaris, AVR, MSP430, Xtensa, Hexagon and bare-metal Arm and
RISC-V — 23 of 23 targets in the conformance suite, where before it worked
only on Linux.

The MSVC standard library is deliberately absent: it is not redistributable.
Windows targets get libc++ over the portable layer, which is a real answer to
"what would libc++ do here" and not an answer to "what does MSVC's STL do".

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

## Status

Built, tested and driving [ABI Explorer](https://abiexplorer.org) end to end.
The app was rewritten onto this module: about 2500 lines of layout-dump
parsing, probe generation, AST-location matching and containment reconstruction
were deleted, and its browser suite runs in 51 s against the text pipeline's
1.1 min.

`test/conformance.mjs` — 23 checks — covers what the old pipeline got wrong or
could not answer: base specifier source ranges, flexible array members, empty
members sharing an address, exotic triples, structured *and* rendered
diagnostics, the drawing model's containment and overlap, the standard library
on 23 targets, and the two padding figures meaning what they say.

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
