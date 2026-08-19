# Release format

What a release consists of, how it is split, and why. The short version: one
immutable, content-addressed set of files behind a small mutable manifest;
everything the app needs to answer a C question in the first fetch; the C++
headers alongside it but not blocking.

## Two budgets, not one

The instinct is to trade "bundle size" against "works offline", but those pull
on different resources and the numbers are not close:

| | libc++ headers |
|---|---:|
| on disk / in wasm heap | 12.0 MB |
| gzipped over the wire | **1.7 MB** |

Text compresses about 7:1, so header payload is cheap to *ship* and expensive to
*hold*: mounting it costs its full uncompressed size in the module's memory, for
the whole session, whether or not the user ever writes C++.

That splits the decision cleanly. Bandwidth is not the reason to leave anything
out — so nothing is left out, and offline works. Memory would be a reason to
*mount* less than is shipped, which is a separate lever and one this format does
not yet pull; see "Not yet" below.

## Artifacts

A release is these files and nothing else:

```
manifest.json                     mutable, tiny, the only thing fetched by name
wasm-<hash>.wasm                  the frontend-only clang        28 MB  (8.8 MB gzip)
glue-<hash>.mjs                   Emscripten glue               311 kB  (58 kB gzip)
headers-<hash>.data               the whole header payload       20 MB  (2.4 MB gzip)
index.mjs, index.d.ts             the JS API and its contract
```

10.8 MB over the wire, once.

Everything but the manifest is content-addressed, so everything but the manifest
is immutable: `Cache-Control: public, max-age=31536000, immutable`. No
revalidation, no staleness, and two releases share whatever did not change —
bump the LLVM tag without touching headers and clients refetch only the wasm.

`manifest.json` is the single indirection:

```json
{
  "schemaVersion": 1,
  "version": "0.1.0+llvm22.1.8",
  "clang": "22.1.8",
  "built": "2026-08-19T23:14:32Z",
  "inputs": { "llvmTag": "llvmorg-22.1.8", "emsdk": "6.0.7", "cmakeHash": "aa3b58175e47512f" },
  "files": {
    "wasm":    { "path": "wasm-9ec58989c571.wasm",    "sha256": "…", "bytes": 29233966, "gzip": 8837850 },
    "glue":    { "path": "glue-2459f7a19378.mjs",     "sha256": "…", "bytes": 317681,   "gzip": 58385 },
    "headers": { "path": "headers-95122c0f5c56.data", "sha256": "…", "bytes": 20119812, "gzip": 2431096 }
  }
}
```

`inputs` is what makes a build reproducible and a cache key honest: the LLVM tag,
the emsdk version, and a hash of the cmake flags are the only things that change
the output, and they are the CI cache key too. `sha256` is what a consumer
verifies a download against — `tools/fetch-abi-module.mjs` in the web app does,
and skips a file whose hash already matches on disk.

The header payload is Emscripten's `--preload-file` bundle: one `.data` blob,
fetched with the module and mounted whole.

## Not yet: splitting the header payload

The 20 MB of headers is 12 MB of libc++ and 8 MB of everything a C question
needs. Mounting costs the full uncompressed size in the module's heap, for the
session, whether or not the user ever writes C++ — so a C-only session pays 12
MB of heap for nothing.

Splitting it in two would fix that, and the split would not be lazy: `load()`
would fetch and mount the C half, resolve, then fetch the C++ half into the
Cache API in the background, so it is local before anyone selects C++ and the
next visit is offline-ready either way.

It is not done, and this is what it would take: `--preload-file` produces one
blob tied to the glue, with no way to split it or cache the halves separately,
so it would mean a pack format of our own (a brotli tar with a header naming its
mount point, decompressed with `DecompressionStream('br')` and mounted into
MEMFS) and a loader to match. Worth doing when heap pressure is the complaint;
it buys nothing over the wire, and download size is what people notice first.

## Distribution

| Channel | For |
|---|---|
| **GitHub Releases** | canonical; assets served immutable, brotli negotiated |
| **npm** (`clang-abi-wasm`) | apps that want to vendor or bundle; ships the same files |
| **self-host** | copy the release directory anywhere and point `baseUrl` at it |

npm serves gzip only, which costs about 20% against brotli on these files — a
reason to prefer the release URL for the payload and npm for the JS, but not a
reason to complicate the default. `load()` takes `baseUrl` precisely so an app
can decide.

Version is `<semver>+llvm<tag>`, e.g. `0.2.0+llvm22.1.8`: the semver tracks the
schema in `js/index.d.ts`, the build metadata tracks the compiler. A clang bump
that changes no field is a patch release; a schema change is a minor at least,
because consumers read those fields directly.

## Caching, and the 329 ms

Three layers, each earning its place:

- **Cache API**, keyed by the content-addressed URL. Immutable, so a hit never
  revalidates. This is what makes the second visit free.
- **IndexedDB**, holding the compiled `WebAssembly.Module`. It is
  structured-cloneable, so the ~329 ms spent compiling the module can be spent
  once per device rather than once per visit. Keyed by the wasm hash, dropped
  when it changes.
- **Service worker**, precaching `manifest.json` and the core files at install so
  a cold offline start works. It must not precache `cxx` — that is the
  background fetch's job, and doing both downloads it twice.

## What ends up in the payload

Set by [`scripts/package-headers.sh`](../scripts/package-headers.sh), whose rule
is: carry anything cheap enough that fetching it later would be the worse trade.

| Kept | Why |
|---|---|
| clang builtin headers | nothing parses without them |
| target intrinsics (avx, neon, altivec…) | 5 MB raw, ~600 KB shipped; real code includes them |
| musl, every architecture | ~1 MB; fixes 32-bit targets and per-target libc layouts |
| libc++ | the entire C++ story |

| Dropped | Why |
|---|---|
| static archives (`.a`, `.so`) | 5.9 MB that `-fsyntax-only` can never link |
| OpenCL headers | this tool does not compile OpenCL |
| CUDA / HIP wrappers | nor offload languages |
| sanitizer headers | never reached by a layout query |

## Open questions

- **Brotli in the payload, or leave it to transport?** Content-encoding is
  simpler and lets a CDN negotiate, but breaks the "same file works from a
  `file://` directory" property. Currently neither: the `.data` blob is raw and
  the 2.4 MB figure is what gzip transport achieves. Revisit together with the
  split above, which needs a container format anyway.
- **Per-target header subsetting.** musl's arch trees are mostly disjoint;
  shipping one arch would cut the payload, at the cost of a fetch on every
  target switch. Almost certainly not worth it — the whole of musl is about a
  megabyte, and the generic layer means every target needs at most two trees —
  but it is the obvious lever if the payload has to shrink.
- **The intrinsic headers are 5.2 MB of the 20.** avx/neon/altivec appear in
  real code often enough to keep, but they are the largest single thing a
  layout query rarely reaches. The first candidate to move behind the split.
