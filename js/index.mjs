/**
 * clang-abi-wasm — record layout as structured data.
 *
 * The whole JS side is marshalling: a request object becomes a JSON string,
 * crosses into wasm, and comes back as a JSON string that is parsed once. There
 * is deliberately no interpretation here — no regexes over compiler output, no
 * matching of printed type names against printed record names, no reconstructing
 * sizes from neighbouring offsets. If something in the response needs working
 * out, that is a bug in the C++ side, not something to paper over in this file.
 *
 * @typedef {import('./index.d.ts').AbiRequest} AbiRequest
 * @typedef {import('./index.d.ts').AbiResponse} AbiResponse
 * @typedef {import('./index.d.ts').AbiModule} AbiModule
 * @typedef {import('./index.d.ts').LoadOptions} LoadOptions
 */

/** @type {Promise<AbiModule> | null} */
let pending = null;

/**
 * Instantiate the module. Concurrent callers share one instance: the wasm
 * carries a parsed header tree and there is no reason to hold two.
 *
 * @param {LoadOptions} [options]
 * @returns {Promise<AbiModule>}
 */
export function load(options = {}) {
  pending ??= instantiate(options);
  return pending;
}

/** Drop the cached instance. Tests use this; applications should not need it. */
export function reset() {
  pending = null;
}

/**
 * @param {LoadOptions} options
 * @returns {Promise<AbiModule>}
 */
async function instantiate(options) {
  const base = options.baseUrl ? new URL(String(options.baseUrl), baseHref()) : undefined;

  // The Emscripten glue is generated next to the .wasm and .data; letting it
  // resolve its own siblings keeps this working under any base path.
  const factory = (await import(/* @vite-ignore */ resolve('abi_query.mjs', base))).default;

  const module = await factory({
    locateFile: (path) => resolve(path, base),
    // Emscripten reports the .wasm and the preloaded header pack separately;
    // both are worth showing, since the header pack is the larger of the two.
    setStatus: undefined,
    monitorRunDependencies: undefined,
    print: () => {},
    printErr: () => {},
    ...(options.onProgress
      ? {
          onProgress: options.onProgress,
        }
      : {}),
  });

  const rawQuery = module.cwrap('abi_query', 'string', ['string']);
  const rawVersion = module.cwrap('abi_version', 'string', []);

  /** @type {string[] | null} */
  let targetCache = null;

  return {
    /**
     * @param {AbiRequest} request
     * @returns {AbiResponse}
     */
    query(request) {
      if (typeof request?.triple !== 'string' || request.triple === '') {
        throw new TypeError('query() needs a target triple');
      }
      if (typeof request.source !== 'string') {
        throw new TypeError('query() needs source text');
      }
      const text = rawQuery(JSON.stringify(request));
      // A response is always JSON, including for a rejected request — the C++
      // side never throws across the boundary. Anything else is the module
      // having died, and is worth reporting as such rather than as a parse error.
      let response;
      try {
        response = JSON.parse(text);
      } catch (cause) {
        throw new Error('the module returned a malformed response', { cause });
      }
      return response;
    },

    targets() {
      // Derived from the module rather than hand-curated: clang exposes the
      // list to no command-line flag, which is why applications ship a
      // hardcoded dropdown and users hit "unknown target triple" past its edge.
      targetCache ??= JSON.parse(rawQuery(JSON.stringify({ listTargets: true }))).targets ?? [];
      return targetCache;
    },

    version() {
      return rawVersion();
    },
  };
}

/**
 * @param {string} path
 * @param {URL | undefined} base
 * @returns {string}
 */
function resolve(path, base) {
  return base ? new URL(path, base).href : path;
}

function baseHref() {
  if (typeof document !== 'undefined' && document.baseURI) return document.baseURI;
  if (typeof self !== 'undefined' && self.location) return self.location.href;
  return 'file:///';
}
