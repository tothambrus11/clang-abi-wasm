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
out — so nothing is left out, and offline works. Memory is, so what gets
*mounted* is decided at load, not at packaging.

## Artifacts

A release is these files and nothing else:

```
manifest.json                     mutable, tiny, the only thing fetched by name
abi_query-<hash>.wasm             the frontend-only clang
abi_query-<hash>.mjs              Emscripten glue
core-<hash>.pack                  clang builtins + intrinsics + musl   (~7 MB raw)
cxx-<hash>.pack                   libc++                               (~12 MB raw)
index.mjs, index.d.ts             the JS API and its contract
```

Everything but the manifest is content-addressed, so everything but the manifest
is immutable: `Cache-Control: public, max-age=31536000, immutable`. No
revalidation, no staleness, and two releases share whatever did not change —
bump the LLVM tag without touching headers and clients refetch only the wasm.

`manifest.json` is the single indirection:

```json
{
  "schemaVersion": 1,
  "clang": "22.1.8",
  "built": "2026-08-19T18:50:00Z",
  "inputs": { "llvmTag": "llvmorg-22.1.8", "emsdk": "4.0.3", "cmakeHash": "9f2c…" },
  "files": {
    "wasm":  { "path": "abi_query-4a1c….wasm", "bytes": 0,       "gzip": 0 },
    "glue":  { "path": "abi_query-4a1c….mjs",  "bytes": 0,       "gzip": 0 },
    "core":  { "path": "core-8b3e….pack",      "bytes": 7340032, "gzip": 1100000 },
    "cxx":   { "path": "cxx-2d90….pack",       "bytes": 12582912,"gzip": 1782579 }
  }
}
```

`inputs` is what makes a build reproducible and a cache key honest: the LLVM tag,
the emsdk version, and a hash of the cmake flags are the only things that change
the output, and they are the CI cache key too.

## Why two packs and not one

`core` is everything a C question needs — clang's builtin headers, the target
intrinsics, musl for every architecture. `cxx` is libc++ alone.

The split is not about download size (1.7 MB either way). It is that a C user
should not carry 12 MB of parsed headers in the module's heap for a session they
spend writing structs. Mounting `cxx` is deferred to the first C++ request.

Offline still holds, because deferred is not the same as lazy:

1. `load()` fetches the manifest, wasm and `core`, and resolves.
2. The moment it resolves it starts fetching `cxx` in the background and puts it
   in the Cache API — the user is reading their first layout while it lands.
3. Mounting happens on the first C++ query, from cache, with no network.

By the time anyone selects C++ the bytes are local. The pack is cached even if
this session never mounts it, so the *next* session is offline-ready too. An
app that would rather pay upfront passes `{ eager: ['cxx'] }` and gets both
mounted before `load()` resolves.

## Pack format

A pack is a tar, brotli-compressed, with a header naming its mount point:

```
<u32 magic 'ABIP'><u32 version><u32 jsonLen><json header><brotli tar stream>
```

Deliberately not Emscripten's `--preload-file`. That produces one `.data` blob
tied to the glue, fetched eagerly and in full, with no way to split or to cache
the halves separately — which is exactly the flexibility this design needs. The
tar is decompressed with `DecompressionStream('br')` where available and mounted
into MEMFS; there is no third-party decompressor in the bundle.

Both packs are stored decompressed by the browser only in the module's heap; the
Cache API holds the compressed bytes.

## Distribution

| Channel | For |
|---|---|
| **GitHub Releases** | canonical; assets served immutable, brotli negotiated |
| **npm** (`clang-abi-wasm`) | apps that want to vendor or bundle; ships the same files |
| **self-host** | copy the release directory anywhere and point `baseUrl` at it |

npm serves gzip only, which costs about 20% against brotli on these files — a
reason to prefer the release URL for the packs and npm for the JS, but not a
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

- **Brotli in the pack, or leave it to transport?** Content-encoding is simpler
  and lets a CDN negotiate, but breaks the "same file works from a `file://`
  directory" property. Currently compressed in the pack; revisit if a CDN in
  front makes transport encoding reliable.
- **Is `core` still too big for a first paint?** The intrinsics are the bulk of
  it. If measurement says yes, they split out as a third pack on the same
  background-fetch rule rather than becoming lazy.
- **Per-target header subsetting.** musl's arch trees are mostly disjoint;
  shipping one arch would cut the pack, at the cost of a fetch on every target
  switch. Almost certainly not worth it — the whole of musl is about a megabyte
  — but it is the obvious next lever if `core` needs to shrink.
