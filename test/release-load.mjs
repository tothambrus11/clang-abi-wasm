// Does a *release* load the way the release notes say it does?
//
// A release names its files by content — `wasm-9ec58989c571.wasm` — so they can
// be served immutable, and `manifest.json` is the one mutable file that says
// which is which. `load()` resolved plain names, so the two-line snippet in
// every release's notes did not work, and nothing noticed because every other
// test loads a build directory, where the names are plain.
//
// Needs a packaged release and a browser (Node's import() refuses http: URLs,
// which is the whole point of the case being untested):
//
//   scripts/build.sh wasm && scripts/package-release.sh && node test/release-load.mjs
//
// Skipped when either is missing.

import { createServer } from 'node:http';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const RELEASE = path.join(ROOT, 'release');

if (!existsSync(path.join(RELEASE, 'manifest.json'))) {
  console.log('no release/ — run scripts/package-release.sh; skipping');
  process.exit(0);
}
// Playwright is not a dependency of this repo — the web app next door has it,
// and CI installs it. Resolve it from wherever it is rather than adding a
// browser to a repo that builds a compiler.
let chromium;
for (const spec of [
  'playwright',
  'playwright-core',
  process.env.PLAYWRIGHT_MODULE,
  path.join(ROOT, '..', 'abi-explorer-2', 'node_modules', 'playwright', 'index.mjs'),
]) {
  if (!spec) continue;
  try {
    ({ chromium } = await import(spec.startsWith('/') ? `file://${spec}` : spec));
    if (chromium) break;
  } catch {
    /* try the next */
  }
}
if (!chromium) {
  console.log('no playwright to drive a browser with; skipping');
  process.exit(0);
}

const TYPES = {
  '.mjs': 'text/javascript',
  '.js': 'text/javascript',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.data': 'application/octet-stream',
};

const server = createServer((req, res) => {
  const name = path.basename(decodeURIComponent((req.url ?? '/').split('?')[0]));
  const file = path.join(RELEASE, name);
  if (!name || !existsSync(file) || !statSync(file).isFile()) {
    res.writeHead(name === '' ? 200 : 404, { 'content-type': 'text/html' });
    res.end('<!doctype html><title>release</title>');
    return;
  }
  res.writeHead(200, { 'content-type': TYPES[path.extname(name)] ?? 'application/octet-stream' });
  res.end(readFileSync(file));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}/`;

const browser = await chromium.launch();
let failed = 0;
try {
  const page = await browser.newPage();
  page.on('pageerror', (e) => {
    failed++;
    console.log(`  FAIL page error: ${e.message}`);
  });
  await page.goto(base);
  const result = await page.evaluate(async (origin) => {
    const { load } = await import(`${origin}index.mjs`);
    const abi = await load({ baseUrl: origin });
    const res = abi.query({
      source: '#include <string>\nstruct S { std::string s; int i; };',
      triple: 'aarch64-apple-macosx',
      lang: 'c++',
      std: 'gnu++20',
    });
    return {
      version: abi.version(),
      size: res.records.find((r) => r.name === 'S')?.sizeBits,
      targets: abi.targets().length,
    };
  }, base);

  const check = (ok, what) => {
    if (ok) console.log(`  ok   ${what}`);
    else {
      failed++;
      console.log(`  FAIL ${what}`);
    }
  };
  check(/clang version \d+/.test(result.version), 'the release loads from its own URL');
  check(result.size === 256, `and answers: sizeof(S) = ${result.size / 8} B`);
  check(result.targets > 20, `and knows ${result.targets} targets`);
} finally {
  await browser.close();
  server.close();
}
console.log(failed ? `\n${failed} failing` : '\nrelease loads as documented');
process.exit(failed ? 1 : 0);
