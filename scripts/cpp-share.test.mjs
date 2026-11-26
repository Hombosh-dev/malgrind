// node --test scripts/cpp-share.test.mjs
import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import {
  classifyLanguage,
  collectFiles,
  computeShare,
  countCodeLines,
  formatReport,
  measureRepository,
} from './cpp-share.mjs';

function withTempTree(files, fn) {
  const root = mkdtempSync(join(tmpdir(), 'malgrind-cpp-share-'));
  try {
    for (const [relPath, content] of Object.entries(files)) {
      const fullPath = join(root, relPath);
      mkdirSync(join(fullPath, '..'), { recursive: true });
      writeFileSync(fullPath, content);
    }
    return fn(root);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

test('classifyLanguage maps supported extensions and ignores non-code files', () => {
  for (const ext of ['.cpp', '.hpp', '.h', '.cc', '.hh', '.cxx', '.hxx']) {
    assert.equal(classifyLanguage(`src/foo${ext}`), 'C++');
  }
  for (const ext of ['.ts', '.mts', '.js', '.mjs']) {
    assert.equal(classifyLanguage(`app/src/bar${ext}`), 'JS/TS');
  }
  assert.equal(classifyLanguage('app/src/App.svelte'), 'Svelte');
  assert.equal(classifyLanguage('app/src-tauri/src/main.rs'), 'Rust');
  assert.equal(classifyLanguage('agent/esp32/inject.py'), 'Python');
  assert.equal(classifyLanguage('app/src/lib/theme.css'), 'CSS');
  assert.equal(classifyLanguage('host/web/flame.html'), 'HTML');
  assert.equal(classifyLanguage('core/CMakeLists.txt'), 'CMake');
  assert.equal(classifyLanguage('cmake/toolchain.cmake'), 'CMake');

  for (const ignored of ['README.md', 'package.json', 'notes.txt', 'Cargo.lock', 'icon.png', 'app.d.ts']) {
    assert.equal(classifyLanguage(ignored), null);
  }
});

test('countCodeLines strips // and multiline /* */ comments and blank lines in C++ / JS / Rust / CSS', () => {
  const cpp = [
    '// full line comment',
    '',
    '   ',
    '/* single line block */',
    '/*',
    ' * multiline block comment',
    ' */',
    '#include <cstdint> // trailing comment',
    'const char* url = "https://example.com"; /* inline */',
    'int x = /* mid */ 42;',
    'int a = 1; /* start',
    'still comment */ int b = 2;',
    '',
  ].join('\n');
  assert.equal(countCodeLines(cpp, 'host/src/main.cpp'), 5);

  const rust = [
    '// rust comment',
    'pub fn borrow<\'a>(s: &\'a str) -> &\'a str {',
    '    let slash = \'/\'; /* inline */',
    '    s // return',
    '}',
  ].join('\n');
  assert.equal(countCodeLines(rust, 'main.rs'), 4);

  const css = [
    '/* theme',
    '   variables */',
    ':root { --fg: #111; }',
    '',
    'body { /* pad */ margin: 0; }',
  ].join('\n');
  assert.equal(countCodeLines(css, 'theme.css'), 2);
});

test('countCodeLines strips # comments in Python and CMake while keeping quoted #', () => {
  const py = [
    '# header comment',
    '',
    'COLOR = "#ff00ff"  # trailing comment',
    'def run():',
    '    # indented comment',
    '    return COLOR',
  ].join('\n');
  assert.equal(countCodeLines(py, 'inject.py'), 3);

  const cmake = [
    '# CMake setup',
    'cmake_minimum_required(VERSION 3.20)',
    '',
    'add_library(malgrind_core STATIC src/profiler.cpp) # target',
  ].join('\n');
  assert.equal(countCodeLines(cmake, 'core/CMakeLists.txt'), 2);
});

test('countCodeLines strips <!-- --> comments and embedded script/style comments in Svelte and HTML', () => {
  const svelte = [
    '<!-- top comment -->',
    '<!--',
    '  multiline html comment',
    '-->',
    '<script lang="ts">',
    '  // script comment',
    '  /* block',
    '     comment */',
    '  const label = "hi";',
    '</script>',
    '',
    '<main><a href="https://example.com">{label}</a></main> <!-- inline -->',
    '<style>',
    '  /* style comment */',
    '  main { color: red; }',
    '</style>',
  ].join('\n');
  // Lines: <script lang="ts">, const label = "hi";, </script>, <main>..., <style>, main..., </style>
  assert.equal(countCodeLines(svelte, 'App.svelte'), 7);

  const html = [
    '<!doctype html>',
    '<!-- ignore me -->',
    '<html><body>ok</body></html>',
  ].join('\n');
  assert.equal(countCodeLines(html, 'flame.html'), 2);
});

test('computeShare excludes tests, fixtures, generated/binary files, and non-code files', () => {
  const files = [
    { path: 'core/src/a.cpp', content: 'int a = 1;\nint b = 2;\nint c = 3;\n' },
    { path: 'host/tests/test_a.cpp', content: 'int t1 = 1;\nint t2 = 2;\n' },
    { path: 'host/fixtures/sample.cpp', content: 'int f = 1;\n' },
    { path: 'app/src/node_modules/pkg/index.js', content: 'export const x = 1;\n' },
    { path: 'app/src/lib/a.test.ts', content: 'export const t = 1;\n' },
    { path: 'app/src/lib/b.spec.ts', content: 'export const s = 1;\n' },
    { path: 'app/src/env.d.ts', content: 'declare const x: string;\n' },
    { path: 'app/src/icon.png', content: 'binary' },
    { path: 'core/README.md', content: '# docs\n' },
    { path: 'agent/esp32/family.json', content: '{"a":1}\n' },
    { path: 'app/src/lib/a.ts', content: 'export const x = 1;\n' },
  ];

  const res = computeShare(files);
  assert.equal(res.cppLines, 3);
  assert.equal(res.totalLines, 4);
  assert.equal(res.cppSharePct, 75);
  assert.equal(res.belowThreshold, false);
  assert.deepEqual(
    res.languages.map((l) => [l.language, l.files, l.lines]),
    [
      ['C++', 1, 3],
      ['JS/TS', 1, 1],
    ],
  );
});

test('computeShare sets belowThreshold based on >= 60% vs < 60% and formats report', () => {
  const above = computeShare([
    { path: 'core/a.cpp', content: 'a;\nb;\nc;\n' },
    { path: 'app/src/a.ts', content: 'a;\nb;\n' },
  ]);
  assert.equal(above.cppLines, 3);
  assert.equal(above.totalLines, 5);
  assert.equal(above.cppSharePct, 60);
  assert.equal(above.belowThreshold, false);

  const below = computeShare([
    { path: 'core/a.cpp', content: 'a;\nb;\n' },
    { path: 'app/src/a.ts', content: 'a;\nb;\nc;\n' },
  ]);
  assert.equal(below.cppLines, 2);
  assert.equal(below.totalLines, 5);
  assert.equal(below.cppSharePct, 40);
  assert.equal(below.belowThreshold, true);

  const report = formatReport(below);
  assert.match(report, /C\+\+ share: 40\.0% \(2 \/ 5 lines\)/);
  assert.match(report, /JS\/TS\s+1\s+3\s+60\.0%/);
});

test('collectFiles and measureRepository walk only existing scan roots and respect directory exclusions', () => {
  withTempTree(
    {
      'core/src/p.cpp': 'int x = 1;\n// comment\nint y = 2;\n',
      'core/tests/t.cpp': 'int t = 1;\n',
      'host/build/gen.cpp': 'int g = 1;\n',
      'host/.pio/lib.cpp': 'int p = 1;\n',
      'app/src/dist/out.js': 'const z = 1;\n',
      'app/src/main.ts': 'export const ok = true;\n',
      'app/src-tauri/target/debug.rs': 'fn f() {}\n',
      'app/src-tauri/src/main.rs': 'fn main() {}\n',
      'docs/guide.cpp': 'int ignored_outside_roots = 1;\n',
    },
    (root) => {
      const files = collectFiles(root);
      assert.deepEqual(files, [
        'app/src-tauri/src/main.rs',
        'app/src/main.ts',
        'core/src/p.cpp',
      ]);

      const measured = measureRepository(root, { useCloc: false });
      assert.equal(measured.source, 'builtin');
      assert.equal(measured.cppLines, 2);
      assert.equal(measured.totalLines, 4);
      assert.equal(measured.cppSharePct, 50);
      assert.equal(measured.belowThreshold, true);
    },
  );
});
