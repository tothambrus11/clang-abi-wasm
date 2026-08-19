// Conformance: does the module answer the questions the schema promises?
//
// Runs against the wasm module when it is built, and against the native harness
// otherwise — the same entry point either way, so a failure here is a failure in
// abi_query.cpp rather than in the binding.
//
//   node test/conformance.mjs
//
// Each case states an expectation that the *old* text-parsing pipeline either
// got wrong or could not answer at all; that is what makes them worth pinning.

import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const NATIVE = path.join(ROOT, 'build', 'native', 'abi_query_test');
const WASM = path.join(ROOT, 'dist', 'abi_query.mjs');

let query;
if (existsSync(WASM)) {
  const { load } = await import(path.join(ROOT, 'js', 'index.mjs'));
  const abi = await load({ baseUrl: path.join(ROOT, 'dist') });
  query = (req) => abi.query(req);
  console.log('driving: wasm module');
} else if (existsSync(NATIVE)) {
  query = (req) =>
    JSON.parse(execFileSync(NATIVE, { input: JSON.stringify(req), maxBuffer: 1 << 28 }));
  console.log('driving: native harness');
} else {
  console.error('nothing built — run scripts/build.sh native');
  process.exit(2);
}

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log(`  ok   ${name}`);
  } catch (e) {
    failures++;
    console.log(`  FAIL ${name}\n       ${e.message}`);
  }
}
function eq(actual, expected, what) {
  const a = JSON.stringify(actual);
  const b = JSON.stringify(expected);
  if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`);
}
function ok(cond, what) {
  if (!cond) throw new Error(what);
}
const byName = (res, n) => res.records.find((r) => r.name === n);

// ---------------------------------------------------------------- basics --

check('a plain C struct reports offsets, sizes and padding', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    source: 'struct S { char a; int b; char c; double d; };',
  });
  eq(res.ok, true, 'ok');
  eq(res.exitCode, 0, 'exitCode');
  const S = byName(res, 'S');
  ok(S, 'record S present');
  eq(S.sizeBits, 192, 'sizeof S');
  eq(S.alignBits, 64, 'alignof S');
  eq(
    S.fields.map((f) => [f.name, f.offsetBits, f.sizeBits]),
    [['a', 0, 8], ['b', 32, 32], ['c', 64, 8], ['d', 128, 64]],
    'fields',
  );
  // Sizes with no probe translation unit anywhere in sight.
  ok(S.fields.every((f) => f.sizeBits > 0), 'every field measured');
  eq(S.paddingBits, 24 + 56, 'padding total');
});

// ------------------------------------------------- the motivating feature --

check('a base specifier carries its source range', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    source: ['struct Base { virtual ~Base(); int x; };', 'struct D : Base { char c; };'].join('\n'),
  });
  const D = byName(res, 'D');
  ok(D, 'record D present');
  eq(D.bases.length, 1, 'base count');
  const [base] = D.bases;
  // No clang dump format emits this. It is the reason the library exists.
  ok(base.location, 'base has a location');
  eq(base.location.line, 2, 'base line');
  ok(base.location.col > 10, 'base column points into the base clause');
  eq(base.isPrimary, true, 'primary base');
  eq(base.offsetBits, 0, 'base offset');
  // And the reference is an id, not a name to be matched.
  eq(res.records[base.recordId].name, 'Base', 'recordId resolves');
});

check('a virtual base is marked and placed', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    source: ['struct B { virtual ~B(); int x; };', 'struct Diamond : virtual B { double d; };'].join('\n'),
  });
  const D = byName(res, 'Diamond');
  const [vb] = D.bases;
  eq(vb.isVirtual, true, 'isVirtual');
  ok(vb.offsetBits > 0, 'virtual base placed after the derived members');
  ok(vb.location, 'virtual base has a location');
});

// ------------------------------------------------------- old-pipeline bugs --

check('a flexible array member occupies nothing', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    source: 'struct Packet { unsigned len; char data[]; };',
  });
  const P = byName(res, 'Packet');
  const data = P.fields.find((f) => f.name === 'data');
  eq(data.isFlexibleArrayMember, true, 'flagged');
  // The text pipeline could not measure this and guessed one byte, which the
  // byte grid then declined to draw.
  eq(data.sizeBits, 0, 'occupies nothing');
  eq(P.sizeBits, 32, 'sizeof unaffected');
});

check('an empty member sharing an address occupies nothing', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    source: 'struct E {};\nstruct S { [[no_unique_address]] E e; int i; };',
  });
  const S = byName(res, 'S');
  const e = S.fields.find((f) => f.name === 'e');
  eq(e.isNoUniqueAddress, true, 'attribute seen');
  eq(S.sizeBits, 32, 'sizeof S');
});

// The native harness resolves headers through the host toolchain, which may
// have no libc++ at all; the wasm module carries its own. Skip rather than
// fail, so a missing host sysroot does not look like a defect in the code.
const hasLibcxx = (() => {
  const probe = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    std: 'c++20',
    source: '#include <string>\nstruct P { std::string s; };',
  });
  return probe.exitCode === 0;
})();

check('library records stay out unless asked for', () => {
  if (!hasLibcxx) {
    console.log('       (skipped: no libc++ in this toolchain)');
    return;
  }
  const req = {
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    std: 'c++20',
    source: '#include <string>\nstruct Probe { std::string s; };',
  };
  const lean = query(req);
  ok(lean.records.length < 40, `default keeps it small, got ${lean.records.length}`);
  ok(byName(lean, 'Probe'), 'the user record is present');
  // Referenced library records still resolve, or recordId would dangle.
  const probe = byName(lean, 'Probe');
  ok(lean.records[probe.fields[0].recordId], 'the member type resolves');

  const full = query({ ...req, includeSystemRecords: true });
  ok(full.records.length > lean.records.length, 'the flag widens it');
});

// ------------------------------------------------------------- per-target --

check('targets actually differ', () => {
  const src = 'struct S { char a; int b; void *p; long l; };';
  const seen = new Map();
  for (const triple of [
    'x86_64-unknown-linux-gnu',
    'i386-unknown-linux-gnu',
    'avr-unknown-unknown',
    'msp430-none-elf',
    'aarch64-apple-macosx',
  ]) {
    const res = query({ triple, source: src });
    eq(res.ok, true, `${triple} ok`);
    seen.set(triple, byName(res, 'S').sizeBits);
  }
  ok(new Set(seen.values()).size >= 3, `distinct layouts, got ${[...seen.values()].join()}`);
});

check('an exotic triple outside any curated list still works', () => {
  for (const triple of ['xtensa-none-elf', 'arc-unknown-elf', 'lanai-unknown-unknown']) {
    const res = query({ triple, source: 'struct S { char a; int b; };' });
    eq(res.ok, true, `${triple} ok`);
    ok(byName(res, 'S').sizeBits > 0, `${triple} laid out`);
  }
});

// ----------------------------------------------------------- diagnostics --

check('diagnostics are structured, not text to be re-parsed', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    source: 'struct S { int x }\n',
  });
  ok(res.diagnostics.length > 0, 'a diagnostic was reported');
  const d = res.diagnostics[0];
  ok(['error', 'warning', 'fatal'].includes(d.severity), 'severity is a value');
  ok(d.location && d.location.line === 1, 'located');
  ok(!/\[/.test(d.message), 'no ANSI escapes to strip');
  ok(!/^input\.[ch]/.test(d.message), 'no file:line: prefix to strip');
});

check('a bad request is a response, not a crash', () => {
  const res = query({ triple: 'not-a-real-triple-at-all', source: 'struct S{int x;};' });
  ok(typeof res.ok === 'boolean', 'still a well-formed response');
});

console.log(failures ? `\n${failures} failing` : '\nall conformance checks passed');
process.exit(failures ? 1 : 0);
