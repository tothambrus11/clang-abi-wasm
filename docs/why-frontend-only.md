# Why frontend-only, and what it costs

Measurements against `@yowasp/clang` 22.0.0-git20542-10 (clang 22.1.0), the
stock bundle this replaces.

## Targets are not the backends

The stock bundle registers **two** LLVM backends — `wasm32` and `wasm64` — and
still produces correct record layouts for far more than two targets. 41 triples
chosen from outside any curated list were put through `-fsyntax-only` with
`-fdump-record-layouts-complete`: **35 returned a layout**, among them `xtensa`,
`arc`, `lanai`, `tce`, `xcore`, `dxil`, `arm64e-apple-ios` and
`i386-pc-solaris2.11`. Of the six that did not, four were malformed triples and
two (`shave`, `renderscript`) no longer exist in LLVM.

ABI knowledge lives in `clang/lib/Basic/Targets`, which is compiled in
regardless of `LLVM_TARGETS_TO_BUILD`. `-fsyntax-only` never reaches a backend.
Setting the target list empty therefore costs no targets at all.

## What that removes

Every one of these was found linked into the stock 75.5 MB wasm by probing it
for signature strings, and none is reachable from a layout query:

| Component | Found by |
|---|---|
| lld | `wasm-ld`, `--gc-sections` |
| LLVM CodeGen / SelectionDAG | `instcombine`, `loop-vectorize` |
| MC / assembler / AsmPrinter | `MCAsmParser`, `.cfi_startproc` |
| Clang static analyzer | `core.NullDereference` |
| Sanitizers | `AddressSanitizer`, `ThreadSanitizer` |
| OpenMP | `-fopenmp` |
| CUDA / HIP | `__cuda_`, `HIP` |
| OpenCL | `opencl-c.h` |
| LTO / bitcode | `ThinLTO`, `BitcodeWriter` |
| Coverage / PGO | `__llvm_profile` |
| DWARF emission | `DW_TAG_`, `.debug_info` |

The stock build also reports `+assertions`, which costs both size and runtime.

## Where the time goes

Node, warm module, best of three:

| | |
|---|---|
| instantiate the module | 329 ms, once |
| empty C translation unit | 36 ms |
| plain C struct | 33 ms |
| C + `<stdint.h>` | 30 ms |
| C++, no headers | 34 ms |
| C++ + `<string>` | **948 ms** |
| C++ + `<string>` + `<vector>` | **1181 ms** |

Header parsing dominates by more than an order of magnitude, so a smaller
binary improves download and instantiation — not per-query cost. The per-query
win comes from parsing once: this library answers layout, sizes, locations and
base ranges from a single `ASTContext`, where the text pipeline ran a layout
pass, up to four probe rounds, and one AST dump per record, each re-parsing
every header.

## The payload

`llvm-resources.tar` is 26.5 MB of file content:

| Category | Size | Share |
|---|---:|---:|
| libc++ headers | 11.93 MB | 45.0% |
| static libraries | 5.93 MB | 22.4% |
| target intrinsic headers | 5.16 MB | 19.5% |
| OpenCL / sanitizer / CUDA / OpenMP headers | 1.92 MB | 7.3% |
| other builtin headers | 0.98 MB | 3.7% |
| wasi-libc C headers | 0.37 MB | 1.4% |
| libc++ module maps | 0.22 MB | 0.8% |

Static archives cannot be linked by `-fsyntax-only`, and this tool compiles
neither OpenCL nor CUDA — 7.85 MB that can go without losing anything. The
intrinsic headers stay: they are 5 MB against real code that includes them, and
the payload exists so the app works offline.

## The libc bug this fixes

One C library ships in the stock bundle — wasi-libc — and it is used for every
target.

```
$ clang --target=i386-unknown-linux-gnu -fsyntax-only input.c
/usr/include/__stddef_size_t.h:18:23: error: typedef redefinition with
      different types ('unsigned int' vs 'unsigned long')
```

Any 32-bit target plus any libc header is a hard error. And where it does
compile it is quietly wrong: `sizeof(struct stat)` reports 144 for
`x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu` and `wasm32-wasip1`
alike, because it is always wasi's struct. The aarch64 answer should be 128.
For a tool whose premise is per-target accuracy, that is worse than a missing
header — it is a confident wrong answer.

musl is MIT, genuinely multi-arch, and about a megabyte. See
`scripts/package-headers.sh`.
