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
  const base = normalizeBase(options.baseUrl);
  const named = await manifestPaths(base);
  const at = (name) => named.get(name) ?? resolve(name, base);

  const factory = (await import(/* @vite-ignore */ at('abi_query.mjs'))).default;

  const module = await factory({
    locateFile: (path) => at(path),
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
 * Where the files actually are, according to the manifest.
 *
 * A release names them by content — `wasm-9ec58989c571.wasm` — so that they can
 * be served immutable, and `manifest.json` is the one mutable file that says
 * which is which. Resolving `abi_query.wasm` by name works for a build
 * directory and for anything `fetch-abi-module.mjs` laid out, and fails against
 * a release URL: which is exactly what the README told people to do.
 *
 * No manifest is not an error — a local build directory has plain names, and a
 * filesystem base cannot be fetched at all. Fall back to the plain names.
 *
 * @param {string | undefined} base
 * @returns {Promise<Map<string, string>>}
 */
async function manifestPaths(base) {
  const map = new Map();
  if (typeof fetch !== 'function') return map;
  try {
    const url = resolve('manifest.json', base);
    if (!/^[a-z][a-z0-9+.-]*:/i.test(url) || url.startsWith('file:')) return map;
    const response = await fetch(url);
    if (!response.ok) return map;
    const manifest = await response.json();
    const local = { wasm: 'abi_query.wasm', glue: 'abi_query.mjs', headers: 'abi_query.data' };
    for (const [key, file] of Object.entries(manifest?.files ?? {})) {
      const name = local[key];
      if (name && file?.path) map.set(name, resolve(file.path, base));
    }
  } catch {
    // No manifest, or nothing that can fetch one.
  }
  return map;
}

/**
 * A base the module's siblings can be resolved against, from either a URL or a
 * plain directory path. Node callers pass a directory; browsers pass a URL, and
 * the two need different joining — `new URL('a.wasm', '/tmp/dist')` silently
 * resolves against the page origin rather than the directory.
 *
 * @param {string | URL | undefined} baseUrl
 * @returns {string | undefined} a directory URL ending in '/'
 */
function normalizeBase(baseUrl) {
  if (!baseUrl) return undefined;
  const text = String(baseUrl);
  const withSlash = text.endsWith('/') ? text : text + '/';
  if (/^[a-z][a-z0-9+.-]*:/i.test(withSlash)) return new URL(withSlash, baseHref()).href;
  // A filesystem path. Absolute becomes a file: URL; relative resolves against
  // the current directory, which is what a Node caller means by it.
  const absolute = withSlash.startsWith('/')
    ? withSlash
    : (typeof process !== 'undefined' ? process.cwd() : '') + '/' + withSlash;
  return 'file://' + absolute;
}

/**
 * @param {string} path
 * @param {string | undefined} base
 * @returns {string}
 */
function resolve(path, base) {
  if (!base) return path;
  const url = new URL(path, base).href;
  // Emscripten opens the preloaded data package with the filesystem in Node, so
  // hand it a path there rather than a file: URL it would try to fetch.
  if (url.startsWith('file://') && typeof process !== 'undefined' && process.versions?.node) {
    return decodeURIComponent(url.slice('file://'.length));
  }
  return url;
}

function baseHref() {
  if (typeof document !== 'undefined' && document.baseURI) return document.baseURI;
  if (typeof self !== 'undefined' && self.location) return self.location.href;
  return 'file:///';
}
