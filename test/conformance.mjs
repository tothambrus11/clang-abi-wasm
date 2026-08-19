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

// ---------------------------------------------------------- the drawing --

check('the render model says what contains what', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    source: 'struct Inner { int a; char b; };\nstruct Outer { Inner i; double d; };\n',
  });
  const outer = byName(res, 'Outer').render;
  eq(outer.leaves.map((l) => l.name), ['a', 'b', 'd'], 'leaves, in ABI order');
  // A field reached through a member carries the path a viewer labels it with.
  eq(outer.leaves[0].path, ['i'], "a is reached through i");
  eq(outer.leaves[2].path, [], 'd is a member of Outer itself');
  eq(outer.groups.length, 1, 'one compound member');
  eq(outer.groups[0].leafIndexes, [0, 1], 'covering the leaves inside it');
  // The tree is a forest over those two arrays, not something to reconstruct.
  eq(outer.tree.map((n) => [n.kind, n.ref]), [['group', 0], ['leaf', 2]], 'tree');
  eq(outer.tree[0].children.map((n) => n.ref), [0, 1], 'children');
  eq(outer.paddingBits, 24, 'padding between b and d');
});

check('overlap is reported, not inferred from offsets', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    source: 'union U { int i; double d; char c[4]; };',
  });
  const u = byName(res, 'U').render;
  ok(u.tree.length === 3, 'three members');
  ok(u.tree.every((n) => n.overlaps), 'every member of a union overlaps its siblings');
});

check('a member that draws nothing inside it is not a container', () => {
  // The only member of R is a zero-width bit-field: it occupies bytes here and
  // has nothing to expand into, so it is one block rather than an empty box.
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    source: 'struct R { unsigned int : 0; };\nstruct S { R r; int i; };',
  });
  const s = byName(res, 'S').render;
  eq(s.groups.length, 0, 'no group for r');
  ok(s.leaves.some((l) => l.name === 'r' && l.sizeBits > 0), 'r is a leaf with an extent');
});

check('a nested anonymous record says whose it is', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    source: 'typedef struct { int a; } Pair;\nstruct Msg { struct { int lo, hi; }; Pair p; };',
  });
  const msg = byName(res, 'Msg');
  const inner = res.records.find((r) => r.isAnonymous && r.parentRecordId === msg.id);
  ok(inner, 'the anonymous member names Msg as its parent');
  // `typedef struct { … } T;` is anonymous too, and is a record of its own.
  const pair = res.records.find((r) => r.printedName === 'Pair');
  ok(pair && pair.isAnonymous && pair.parentRecordId === null, 'a typedef-ed record has no parent');
});

// ------------------------------------------------- the standard library --

check('the C++ standard library resolves for every kind of target', () => {
  const src = '#include <string>\n#include <vector>\n#include <map>\nstruct S { std::string s; std::vector<int> v; std::map<int,int> m; };';
  // Every family the header layers have to cover: musl's own trees, the ones
  // it has none for, the operating systems libc++ knows and the ones it does
  // not, and the targets whose int is not 32 bits. Before the generic layer
  // this list was "Linux".
  const TRIPLES = [
    'x86_64-unknown-linux-gnu',    // musl's own tree
    'i386-unknown-linux-gnu',      // …and a 32-bit one, which used to get 64-bit types
    'aarch64-unknown-linux-gnu',
    'mips-unknown-linux-gnu',
    'powerpc64le-unknown-linux-gnu',
    's390x-unknown-linux-gnu',
    'x86_64-pc-windows-msvc',      // no MS runtime, no MS locale API
    'aarch64-pc-windows-msvc',
    'i686-pc-windows-gnu',
    'aarch64-apple-macosx',        // no Darwin C library
    'x86_64-apple-darwin',
    'arm64-apple-ios',
    'wasm32-unknown-emscripten',
    'wasm32-wasi',
    'sparcv9-sun-solaris',         // an OS libc++ has no threading branch for
    'armv7-none-eabi',             // no operating system at all
    'riscv32-unknown-elf',
    'riscv64-unknown-elf',
    'xtensa-none-elf',
    'hexagon-unknown-elf',
    'bpfel-unknown-none',
    'avr-unknown-unknown',         // 8-bit, and its int is 16 bits
    'msp430-none-elf',
  ];
  for (const triple of TRIPLES) {
    const res = query({ triple, lang: 'c++', std: 'gnu++20', source: src });
    eq(res.exitCode, 0, `${triple}: ${res.diagnosticsText.slice(0, 400)}`);
    ok(byName(res, 'S').sizeBits > 0, `${triple}: laid out`);
  }
  // A pointer-sized answer on every one of them, which is what says the types
  // came from the target rather than from whichever tree was handy.
  for (const [triple, bytes] of [
    ['x86_64-unknown-linux-gnu', 24],
    ['i386-unknown-linux-gnu', 12],
    ['avr-unknown-unknown', 6],
  ]) {
    const res = query({
      triple,
      lang: 'c++',
      std: 'gnu++20',
      source: '#include <string>\nstruct S { std::string s; };',
    });
    eq(byName(res, 'S').sizeBits / 8, bytes, `sizeof(std::string) on ${triple}`);
  }
});

check('the C library types are the target\'s, whatever the target', () => {
  // The bug this exists to prevent: serving one architecture's C headers to
  // another. musl's x86_64 tree says `uint64_t` is `unsigned long`, which on
  // Windows is four bytes — the struct came out 32 rather than 40.
  const src = '#include <stdint.h>\nstruct S { uint64_t a; uint32_t b; uint64_t c; uint32_t d; uint64_t e; };';
  for (const [triple, size] of [
    ['x86_64-unknown-linux-gnu', 40],
    ['x86_64-pc-windows-msvc', 40],
    ['i386-unknown-linux-gnu', 32],
    ['avr-unknown-unknown', 32],
  ]) {
    const res = query({ triple, source: src });
    eq(res.exitCode, 0, `${triple}: ${res.diagnosticsText.slice(0, 200)}`);
    eq(byName(res, 'S').sizeBits / 8, size, `sizeof(S) on ${triple}`);
  }
});

check('an operating system we do not ship is a missing header, not a wrong answer', () => {
  // Linux is real here — musl's tree is complete.
  const linux = query({
    triple: 'x86_64-unknown-linux-gnu',
    source: '#include <sys/stat.h>\nstruct S { struct stat s; };',
  });
  eq(linux.exitCode, 0, 'struct stat on Linux');
  ok(byName(linux, 'S').sizeBits > 0, 'and it has a size');
  // Darwin's is not, and the generic layer does not pretend otherwise.
  const mac = query({
    triple: 'aarch64-apple-macosx',
    source: '#include <sys/stat.h>\nstruct S { struct stat s; };',
  });
  ok(mac.exitCode !== 0, 'Darwin does not silently answer with Linux\'s struct');
  ok(/file not found/.test(mac.diagnosticsText), 'and says why');
});

check('the response says what it was answered against', () => {
  const linux = query({ triple: 'x86_64-unknown-linux-gnu', lang: 'c++', source: 'struct S{int x;};' });
  eq(linux.headers.cLibrary, 'musl', 'C library');
  eq(linux.headers.cLibraryArch, 'x86_64', 'its architecture');
  eq(linux.headers.localization, true, 'localization on Linux');
  const bare = query({ triple: 'armv7-none-eabi', lang: 'c++', source: 'struct S{int x;};' });
  eq(bare.headers.cLibraryArch, 'generic', 'the generic layer');
  eq(bare.headers.localization, false, 'no platform locale API');
  eq(bare.headers.threads, false, 'no threads on a freestanding target');
});

// ---------------------------------------------------------------- names --

check('type names are reported with what they resolve to', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    lang: 'c++',
    source: 'using u32 = unsigned int;\nstruct S { int x; };\ntypedef S Alias;\n',
  });
  const u32 = res.typedefs.find((t) => t.name === 'u32');
  ok(u32, 'u32 is reported');
  eq([u32.sizeBits, u32.alignBits, u32.canonicalTypeSpelling], [32, 32, 'unsigned int'], 'u32');
  const alias = res.typedefs.find((t) => t.name === 'Alias');
  eq(alias.recordId, byName(res, 'S').id, 'an alias of a record points at it by id');
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

check('diagnostics also come rendered, the way clang prints them', () => {
  const res = query({
    triple: 'x86_64-unknown-linux-gnu',
    source: 'struct S { int x; };\nint bad = ;\n',
  });
  const t = res.diagnosticsText;
  ok(/input\.c:2:11/.test(t), 'carries the position');
  ok(/\^/.test(t), 'carries the caret');
  ok(/\x1b\[/.test(t), 'carries colour, so a consumer need not invent it');
  // The source line is there with the colour woven through it, which is the
  // point: a consumer styles the escapes rather than re-deriving the excerpt.
  ok(/int bad = ;/.test(t.replace(/\x1b\[[0-9;]*m/g, '')), 'carries the source line');
});

check('a bad request is a response, not a crash', () => {
  const res = query({ triple: 'not-a-real-triple-at-all', source: 'struct S{int x;};' });
  ok(typeof res.ok === 'boolean', 'still a well-formed response');
});

console.log(failures ? `\n${failures} failing` : '\nall conformance checks passed');
process.exit(failures ? 1 : 0);
