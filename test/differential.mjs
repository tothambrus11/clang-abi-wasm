// Does the module agree with a real compiler?
//
// Everything else in this repository checks the answer against itself: the
// conformance suite pins cases someone reasoned about, and the web app's
// property tests check that the model is internally consistent. Neither would
// notice a systematic misreading of clang's own API — reporting bit offsets as
// byte offsets, say, or a base at the wrong place — because both sides of the
// comparison come from the same source.
//
// This one asks gcc. It generates record declarations, asks the module for the
// layout, then compiles a program with the system compiler that prints
// `sizeof`, `alignof` and every member's offset, runs it, and compares. The
// numbers have to match exactly.
//
//   node test/differential.mjs [runs]
//
// Skipped when there is no system compiler, or nothing built to ask.

import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import os from 'node:os';
import path from 'node:path';

const ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const NATIVE = path.join(ROOT, 'build', 'native', 'abi_query_test');
const WASM = path.join(ROOT, 'dist', 'abi_query.mjs');

function has(tool) {
  try {
    execFileSync('which', [tool], { stdio: 'ignore' });
    return true;
  } catch {
    return false;
  }
}
if (!has('gcc') || !has('g++')) {
  console.log('no system gcc/g++ to compare against; skipping');
  process.exit(0);
}

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

// The host's own triple. Comparing against gcc only means anything for the
// target gcc is: everything else it cannot answer for.
const HOST = execFileSync('gcc', ['-dumpmachine']).toString().trim();
console.log(`comparing against ${execFileSync('gcc', ['--version']).toString().split('\n')[0]}`);
console.log(`target: ${HOST}\n`);

// --------------------------------------------------------------- generator --

let seed = Number(process.argv[3] ?? 20260820);
/** Deterministic, so a failure can be reproduced from the seed it prints. */
function rnd(n) {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  return seed % n;
}
const pick = (xs) => xs[rnd(xs.length)];

const SCALARS = [
  'char',
  'signed char',
  'unsigned char',
  'short',
  'unsigned short',
  'int',
  'unsigned int',
  'long',
  'unsigned long',
  'long long',
  'float',
  'double',
  'long double',
  'void *',
];

/** One record and the members a program can take the offset of. */
function makeRecord(name, kind, available, cxx) {
  const members = [];
  const lines = [];
  const count = 1 + rnd(5);
  for (let i = 0; i < count; i++) {
    const n = `m${i}`;
    switch (rnd(cxx && available.length ? 7 : 6)) {
      case 0:
        lines.push(`  ${pick(SCALARS)} ${n};`);
        members.push(n);
        break;
      case 1: {
        const t = pick(SCALARS);
        lines.push(`  ${t} ${n}[${1 + rnd(4)}];`);
        members.push(n);
        break;
      }
      case 2: {
        // A bit-field: no offsetof, but it moves everything after it.
        const t = pick(['unsigned int', 'int', 'unsigned char']);
        const cap = t.includes('char') ? 8 : 32;
        lines.push(`  ${t} ${n} : ${1 + rnd(cap)};`);
        break;
      }
      case 3:
        lines.push(`  unsigned int : 0;`);
        break;
      case 4: {
        const t = pick(SCALARS);
        lines.push(`  ${t} ${n} __attribute__((aligned(${pick([2, 4, 8, 16])})));`);
        members.push(n);
        break;
      }
      case 5: {
        if (!available.length) {
          lines.push(`  ${pick(SCALARS)} ${n};`);
          members.push(n);
          break;
        }
        const t = pick(available);
        lines.push(`  ${t.spelling} ${n};`);
        members.push(n);
        break;
      }
      default: {
        const t = pick(available);
        lines.push(`  ${t.spelling} ${n}[${1 + rnd(2)}];`);
        members.push(n);
        break;
      }
    }
  }
  // A union whose members are all at zero is still worth generating; it just
  // tests less.
  const isUnion = kind === 'union';
  const bases = [];
  if (cxx && !isUnion) {
    const classes = available.filter((a) => !a.isUnion);
    for (let i = 0; i < rnd(3) && classes.length; i++) {
      const b = pick(classes);
      if (!bases.some((x) => x.name === b.name)) bases.push(b);
    }
  }
  const inherit = bases.length ? ' : ' + bases.map((b) => `public ${b.name}`).join(', ') : '';
  const pack = rnd(5) === 0 ? pick([1, 2, 4]) : 0;
  const text =
    (pack ? `#pragma pack(${pack})\n` : '') +
    `${kind} ${name}${inherit} {\n${lines.join('\n')}\n};\n` +
    (pack ? '#pragma pack()\n' : '');
  return {
    name,
    spelling: cxx ? name : `${kind} ${name}`,
    isUnion,
    text,
    members,
    bases: bases.map((b) => b.name),
  };
}

function makeSource(cxx) {
  const decls = [];
  const available = [];
  const count = 1 + rnd(4);
  for (let i = 0; i < count; i++) {
    const kind = rnd(5) === 0 ? 'union' : 'struct';
    const d = makeRecord(`R${i}`, kind, available, cxx);
    decls.push(d);
    available.push(d);
  }
  return { source: decls.map((d) => d.text).join('\n'), decls };
}

// ------------------------------------------------------------- the oracle --

const tmp = mkdtempSync(path.join(os.tmpdir(), 'abi-diff-'));

/** What the system compiler says, as `name.member -> offset` plus sizes. */
function askCompiler(source, decls, cxx) {
  const prints = [];
  for (const d of decls) {
    prints.push(
      `  printf("%s|size|%zu\\n", "${d.name}", sizeof(${d.spelling}));`,
      `  printf("%s|align|%zu\\n", "${d.name}", _Alignof_(${d.spelling}));`,
    );
    for (const m of d.members) {
      prints.push(
        `  printf("%s|off|%s|%zu\\n", "${d.name}", "${m}", (size_t)__builtin_offsetof(${d.spelling}, ${m}));`,
      );
    }
    // A base's offset is where a pointer to it lands, which is the definition
    // and needs no offsetof.
    for (const b of d.bases) {
      prints.push(
        `  { ${d.spelling} o; printf("%s|base|%s|%td\\n", "${d.name}", "${b}",` +
          ` (char *)static_cast<${b} *>(&o) - (char *)&o); }`,
      );
    }
  }
  const alignOf = cxx ? '#define _Alignof_(T) alignof(T)' : '#define _Alignof_(T) _Alignof(T)';
  const program =
    `#include <stdio.h>\n#include <stddef.h>\n${alignOf}\n` +
    source +
    `\nint main(void) {\n${prints.join('\n')}\n  return 0;\n}\n`;

  const src = path.join(tmp, cxx ? 'p.cc' : 'p.c');
  const bin = path.join(tmp, 'p');
  writeFileSync(src, program);
  execFileSync(cxx ? 'g++' : 'gcc', [
    cxx ? '-std=gnu++20' : '-std=gnu17',
    '-w',
    '-o',
    bin,
    src,
  ]);
  const out = execFileSync(bin).toString();
  const facts = new Map();
  for (const line of out.trim().split('\n')) {
    const parts = line.split('|');
    facts.set(parts.slice(0, parts.length - 1).join('|'), Number(parts[parts.length - 1]));
  }
  return facts;
}

/** The same facts, from the module. */
function askModule(source, decls, cxx) {
  const res = query({
    triple: HOST,
    source,
    lang: cxx ? 'c++' : 'c',
    std: cxx ? 'gnu++20' : 'gnu17',
  });
  if (res.exitCode !== 0) {
    throw new Error(`module: ${res.diagnosticsText.replace(/\x1b\[[0-9;]*m/g, '').slice(0, 300)}`);
  }
  const facts = new Map();
  for (const d of decls) {
    const r = res.records.find((x) => x.name === d.name);
    if (!r) throw new Error(`module did not report ${d.name}`);
    facts.set(`${d.name}|size`, r.sizeBits / 8);
    facts.set(`${d.name}|align`, r.alignBits / 8);
    for (const f of r.fields) {
      if (f.isBitField || !f.name) continue;
      facts.set(`${d.name}|off|${f.name}`, f.offsetBits / 8);
    }
    for (const b of r.bases) {
      const name = b.typeSpelling.replace(/^(struct|class)\s+/, '');
      facts.set(`${d.name}|base|${name}`, b.offsetBits / 8);
    }
  }
  return facts;
}

// ------------------------------------------------------------------- run --

const RUNS = Number(process.argv[2] ?? 200);
let checked = 0;
let mismatches = 0;
/** Facts gcc reported that the module did not, by kind. A test that skips most
 * of its comparisons is not doing its job, so this is printed either way. */
const skipped = new Map();

try {
  for (let i = 0; i < RUNS; i++) {
    const cxx = rnd(2) === 1;
    const { source, decls } = makeSource(cxx);
    let theirs, mine;
    try {
      theirs = askCompiler(source, decls, cxx);
    } catch {
      continue; // gcc would not build it: a generator artefact, not a finding
    }
    try {
      mine = askModule(source, decls, cxx);
    } catch (e) {
      mismatches++;
      console.log(`\nFAIL (${cxx ? 'c++' : 'c'}) ${e.message}\n${source}`);
      continue;
    }
    for (const [key, expected] of theirs) {
      if (!mine.has(key)) {
        skipped.set(key.split('|')[1], (skipped.get(key.split('|')[1]) ?? 0) + 1);
        continue;
      }
      checked++;
      if (mine.get(key) !== expected) {
        mismatches++;
        console.log(
          `\nFAIL ${key}: gcc says ${expected}, the module says ${mine.get(key)}\n${source}`,
        );
        break;
      }
    }
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}

if (skipped.size) {
  console.log(`\nnot compared: ${[...skipped].map(([k, n]) => `${n} ${k}`).join(', ')}`);
}
console.log(
  mismatches
    ? `\n${mismatches} mismatch(es) over ${RUNS} programs`
    : `\n${checked} facts over ${RUNS} programs, all matching gcc`,
);
process.exit(mismatches ? 1 : 0);
