#!/usr/bin/env node
// Measures the C++ share of non-empty, non-comment code lines across the repo.
// Warns (never fails CI) if C++ drops below 60.0%.
//
//   node scripts/cpp-share.mjs
//
// Uses only Node 18 APIs.
import { spawnSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, join, relative, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const SCAN_ROOTS = [
  'core',
  'host',
  'platforms',
  'agent',
  'apps',
  'app/src',
  'app/src-tauri/src',
];

export const EXCLUDED_DIRS = new Set([
  'node_modules',
  'target',
  'dist',
  'build',
  'tests',
  'fixtures',
  '.pio',
]);

const BINARY_EXTS = ['.bin', '.elf', '.png', '.icns', '.ico'];

const EXT_LANGUAGE = new Map([
  ['.cpp', 'C++'],
  ['.hpp', 'C++'],
  ['.h', 'C++'],
  ['.cc', 'C++'],
  ['.hh', 'C++'],
  ['.cxx', 'C++'],
  ['.hxx', 'C++'],
  ['.ts', 'JS/TS'],
  ['.mts', 'JS/TS'],
  ['.js', 'JS/TS'],
  ['.mjs', 'JS/TS'],
  ['.svelte', 'Svelte'],
  ['.rs', 'Rust'],
  ['.py', 'Python'],
  ['.css', 'CSS'],
  ['.html', 'HTML'],
  ['.cmake', 'CMake'],
]);

function toPosix(p) {
  return sep === '/' ? p : p.split(sep).join('/');
}

function getBasename(filename) {
  const posix = filename.replace(/\\/g, '/');
  const idx = posix.lastIndexOf('/');
  return idx >= 0 ? posix.slice(idx + 1) : posix;
}

export function classifyLanguage(filename) {
  const base = getBasename(filename);
  if (base === 'CMakeLists.txt') return 'CMake';
  if (base.toLowerCase().endsWith('.d.ts')) return null;
  const dot = base.lastIndexOf('.');
  if (dot < 0) return null;
  const ext = base.slice(dot).toLowerCase();
  return EXT_LANGUAGE.get(ext) ?? null;
}

export function isExcludedPath(relPath) {
  const posix = relPath.replace(/\\/g, '/');
  const parts = posix.split('/');
  for (let i = 0; i < parts.length - 1; i += 1) {
    if (EXCLUDED_DIRS.has(parts[i])) return true;
  }
  const base = parts[parts.length - 1];
  if (EXCLUDED_DIRS.has(base)) return true;
  const lower = base.toLowerCase();
  if (lower.includes('.test.') || lower.includes('.spec.') || lower.endsWith('.d.ts')) {
    return true;
  }
  if (BINARY_EXTS.some((ext) => lower.endsWith(ext))) {
    return true;
  }
  return false;
}

function countCStyleLines(text, { allowLineComment = true, rustCharLiterals = false, backtickStrings = false } = {}) {
  const lines = text.split(/\r?\n/);
  let inBlock = false;
  let count = 0;

  for (const line of lines) {
    let hasCode = false;
    let inString = null;
    let i = 0;

    while (i < line.length) {
      const ch = line[i];
      const next = line[i + 1];

      if (inBlock) {
        if (ch === '*' && next === '/') {
          inBlock = false;
          i += 2;
        } else {
          i += 1;
        }
        continue;
      }

      if (inString !== null) {
        hasCode = true;
        if (ch === '\\') {
          i += 2;
          continue;
        }
        if (ch === inString) {
          inString = null;
        }
        i += 1;
        continue;
      }

      if (ch === ' ' || ch === '\t' || ch === '\r') {
        i += 1;
        continue;
      }

      if (allowLineComment && ch === '/' && next === '/') {
        break;
      }

      if (ch === '/' && next === '*') {
        inBlock = true;
        i += 2;
        continue;
      }

      if (ch === '"' || (backtickStrings && ch === '`')) {
        inString = ch;
        hasCode = true;
        i += 1;
        continue;
      }

      if (ch === "'") {
        hasCode = true;
        if (rustCharLiterals) {
          // Skip Rust char literals like 'a' or '\'' without misreading lifetimes like 'a.
          if (next === '\\') {
            const close = line.indexOf("'", i + 2);
            if (close > i + 2 && close - i <= 10) {
              i = close + 1;
              continue;
            }
          } else if (next !== undefined && line[i + 2] === "'") {
            i += 3;
            continue;
          }
          i += 1;
          continue;
        }
        inString = "'";
        i += 1;
        continue;
      }

      hasCode = true;
      i += 1;
    }

    if (hasCode) count += 1;
  }

  return count;
}

function countHashLines(text) {
  const lines = text.split(/\r?\n/);
  let count = 0;

  for (const line of lines) {
    let hasCode = false;
    let inString = null;
    let i = 0;

    while (i < line.length) {
      const ch = line[i];

      if (inString !== null) {
        hasCode = true;
        if (ch === '\\') {
          i += 2;
          continue;
        }
        if (ch === inString) {
          inString = null;
        }
        i += 1;
        continue;
      }

      if (ch === ' ' || ch === '\t' || ch === '\r') {
        i += 1;
        continue;
      }

      if (ch === '#') {
        break;
      }

      if (ch === '"' || ch === "'") {
        inString = ch;
      }

      hasCode = true;
      i += 1;
    }

    if (hasCode) count += 1;
  }

  return count;
}

function matchTagOpen(line, i, tagName) {
  const slice = line.slice(i, i + 1 + tagName.length);
  if (slice.toLowerCase() !== `<${tagName}`) return false;
  const after = line[i + 1 + tagName.length];
  return after === '>' || after === ' ' || after === '\t' || after === '/';
}

function matchTagClose(line, i, tagName) {
  const slice = line.slice(i, i + 2 + tagName.length);
  if (slice.toLowerCase() !== `</${tagName}`) return false;
  const after = line[i + 2 + tagName.length];
  return after === '>' || after === ' ' || after === '\t';
}

function countMarkupLines(text) {
  const lines = text.split(/\r?\n/);
  let mode = 'html'; // 'html' | 'script' | 'style'
  let inHtmlComment = false;
  let inBlockComment = false;
  let count = 0;

  for (const line of lines) {
    let hasCode = false;
    let inString = null;
    let i = 0;

    while (i < line.length) {
      const ch = line[i];
      const next = line[i + 1];

      if (inHtmlComment) {
        if (line.startsWith('-->', i)) {
          inHtmlComment = false;
          i += 3;
        } else {
          i += 1;
        }
        continue;
      }

      if (inBlockComment) {
        if (ch === '*' && next === '/') {
          inBlockComment = false;
          i += 2;
        } else {
          i += 1;
        }
        continue;
      }

      if (inString !== null) {
        hasCode = true;
        if (mode !== 'html' && ch === '\\') {
          i += 2;
          continue;
        }
        if (ch === inString) {
          inString = null;
        }
        i += 1;
        continue;
      }

      if (ch === ' ' || ch === '\t' || ch === '\r') {
        i += 1;
        continue;
      }

      if (mode === 'html') {
        if (line.startsWith('<!--', i)) {
          inHtmlComment = true;
          i += 4;
          continue;
        }
        if (ch === '"' || ch === "'") {
          inString = ch;
          hasCode = true;
          i += 1;
          continue;
        }
        if (matchTagOpen(line, i, 'script') || matchTagOpen(line, i, 'style')) {
          const target = matchTagOpen(line, i, 'script') ? 'script' : 'style';
          const gt = line.indexOf('>', i);
          hasCode = true;
          if (gt >= 0) {
            const selfClosing = gt > i && line[gt - 1] === '/';
            if (!selfClosing) mode = target;
            i = gt + 1;
            continue;
          }
        }
        hasCode = true;
        i += 1;
        continue;
      }

      // Inside <script> or <style>
      if (mode === 'script' && matchTagClose(line, i, 'script')) {
        const gt = line.indexOf('>', i);
        hasCode = true;
        mode = 'html';
        i = gt >= 0 ? gt + 1 : line.length;
        continue;
      }
      if (mode === 'style' && matchTagClose(line, i, 'style')) {
        const gt = line.indexOf('>', i);
        hasCode = true;
        mode = 'html';
        i = gt >= 0 ? gt + 1 : line.length;
        continue;
      }

      if (mode === 'script' && ch === '/' && next === '/') {
        break;
      }
      if (ch === '/' && next === '*') {
        inBlockComment = true;
        i += 2;
        continue;
      }
      if (ch === '"' || ch === "'" || (mode === 'script' && ch === '`')) {
        inString = ch;
        hasCode = true;
        i += 1;
        continue;
      }

      hasCode = true;
      i += 1;
    }

    if (hasCode) count += 1;
  }

  return count;
}

export function countCodeLines(text, filename) {
  const lang = classifyLanguage(filename);
  if (!lang || !text) return 0;

  switch (lang) {
    case 'C++':
      return countCStyleLines(text, { allowLineComment: true });
    case 'JS/TS':
      return countCStyleLines(text, { allowLineComment: true, backtickStrings: true });
    case 'Rust':
      return countCStyleLines(text, { allowLineComment: true, rustCharLiterals: true });
    case 'CSS':
      return countCStyleLines(text, { allowLineComment: false });
    case 'Python':
    case 'CMake':
      return countHashLines(text);
    case 'Svelte':
    case 'HTML':
      return countMarkupLines(text);
    default:
      return 0;
  }
}

function buildResult(countsByLang, cppLines, totalLines, thresholdPct, source) {
  const languages = Object.entries(countsByLang)
    .map(([language, stats]) => ({
      language,
      files: stats.files,
      lines: stats.lines,
      sharePct: totalLines > 0 ? (stats.lines / totalLines) * 100 : 0,
    }))
    .sort((a, b) => (b.lines - a.lines) || a.language.localeCompare(b.language));

  const cppSharePct = totalLines > 0 ? (cppLines / totalLines) * 100 : 0;

  return {
    languages,
    byLanguage: countsByLang,
    cppLines,
    totalLines,
    cppSharePct,
    thresholdPct,
    belowThreshold: cppSharePct < thresholdPct,
    source,
  };
}

export function computeShare(files, thresholdPct = 60) {
  const countsByLang = {};
  let cppLines = 0;
  let totalLines = 0;

  for (const file of files) {
    const filePath = file.path ?? file.filename ?? '';
    if (!filePath || isExcludedPath(filePath)) continue;
    const lang = classifyLanguage(filePath);
    if (!lang) continue;

    const lines = typeof file.lines === 'number'
      ? file.lines
      : countCodeLines(file.content ?? '', filePath);

    if (!countsByLang[lang]) {
      countsByLang[lang] = { files: 0, lines: 0 };
    }
    countsByLang[lang].files += 1;
    countsByLang[lang].lines += lines;
    totalLines += lines;
    if (lang === 'C++') cppLines += lines;
  }

  return buildResult(countsByLang, cppLines, totalLines, thresholdPct, 'builtin');
}

// Recursive directory walker compatible with Node 18 (no { recursive: true }).
export function collectFiles(root, roots = SCAN_ROOTS) {
  const collected = [];

  function walk(dir) {
    if (!existsSync(dir)) return;
    const entries = readdirSync(dir, { withFileTypes: true });
    entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    for (const entry of entries) {
      if (entry.isDirectory()) {
        if (EXCLUDED_DIRS.has(entry.name)) continue;
        walk(join(dir, entry.name));
      } else if (entry.isFile()) {
        const rel = toPosix(relative(root, join(dir, entry.name)));
        if (isExcludedPath(rel)) continue;
        if (!classifyLanguage(rel)) continue;
        collected.push(rel);
      }
    }
  }

  for (const r of roots) {
    walk(join(root, r));
  }
  return collected.sort();
}

export function tryCloc(root, relPaths, thresholdPct = 60) {
  if (relPaths.length === 0) return null;
  const useStdin = relPaths.join(' ').length > 16000;
  const args = useStdin
    ? ['--json', '--quiet', '--list-file=-']
    : ['--json', '--quiet', ...relPaths];
  const res = spawnSync('cloc', args, {
    cwd: root,
    encoding: 'utf8',
    input: useStdin ? `${relPaths.join('\n')}\n` : undefined,
    maxBuffer: 10 * 1024 * 1024,
  });

  if (res.error || res.status !== 0 || !res.stdout) return null;

  let parsed;
  try {
    parsed = JSON.parse(res.stdout);
  } catch {
    return null;
  }
  if (!parsed || typeof parsed !== 'object' || !parsed.SUM) return null;

  const countsByLang = {};
  for (const [key, val] of Object.entries(parsed)) {
    if (key === 'header' || key === 'SUM' || !val || typeof val.code !== 'number') continue;
    countsByLang[key] = {
      files: val.nFiles ?? 0,
      lines: val.code,
    };
  }

  const cppLines = (parsed['C++']?.code ?? 0) + (parsed['C/C++ Header']?.code ?? 0);
  const totalLines = parsed.SUM.code ?? 0;
  return buildResult(countsByLang, cppLines, totalLines, thresholdPct, 'cloc');
}

export function measureRepository(root, { useCloc = true, thresholdPct = 60, roots = SCAN_ROOTS } = {}) {
  const relPaths = collectFiles(root, roots);
  if (useCloc) {
    const clocResult = tryCloc(root, relPaths, thresholdPct);
    if (clocResult) return clocResult;
  }
  const files = relPaths.map((rel) => ({
    path: rel,
    content: readFileSync(join(root, rel), 'utf8'),
  }));
  return computeShare(files, thresholdPct);
}

export function formatReport(result) {
  const langWidth = Math.max('Language'.length, ...result.languages.map((l) => l.language.length));
  const lines = [
    `${'Language'.padEnd(langWidth)}  ${'Files'.padStart(5)}  ${'Lines'.padStart(7)}  ${'Share'.padStart(6)}`,
    `${'-'.repeat(langWidth)}  ${'-'.repeat(5)}  ${'-'.repeat(7)}  ${'-'.repeat(6)}`,
  ];
  for (const row of result.languages) {
    lines.push(
      `${row.language.padEnd(langWidth)}  ${String(row.files).padStart(5)}  ${String(row.lines).padStart(7)}  ${`${row.sharePct.toFixed(1)}%`.padStart(6)}`,
    );
  }
  lines.push('');
  lines.push(`C++ share: ${result.cppSharePct.toFixed(1)}% (${result.cppLines} / ${result.totalLines} lines)`);
  return lines.join('\n');
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const root = join(dirname(fileURLToPath(import.meta.url)), '..');
  const result = measureRepository(root);
  console.log(formatReport(result));
  if (result.belowThreshold) {
    console.error(
      `WARNING: C++ share ${result.cppSharePct.toFixed(1)}% is below target ${result.thresholdPct.toFixed(1)}%`,
    );
  }
}
