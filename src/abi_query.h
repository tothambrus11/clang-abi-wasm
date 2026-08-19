//===- abi_query.h - C entry point ----------------------------------------===//
//
// The whole surface. One call in, one JSON document out; see js/index.d.ts for
// the shape of both. Kept to C linkage and plain strings so the same object
// works from Emscripten's ccall, a WASI host, or a native test binary.
//
//===----------------------------------------------------------------------===//
#ifndef CLANG_ABI_WASM_ABI_QUERY_H
#define CLANG_ABI_WASM_ABI_QUERY_H

#ifdef __cplusplus
extern "C" {
#endif

/// Analyse one translation unit.
///
/// \param request_json an AbiRequest, as JSON.
/// \returns an AbiResponse, as JSON. Owned by the library and valid until the
///          next call to abi_query; the caller must not free it.
///
/// Never returns null and never throws across the boundary: a malformed
/// request comes back as a response with "ok": false and an "error" string.
const char *abi_query(const char *request_json);

/// e.g. "clang version 22.1.8". Owned by the library.
const char *abi_version(void);

#ifdef __cplusplus
}
#endif
#endif
