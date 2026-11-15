#!/usr/bin/env node
// One version for the whole project. VERSION is the source; the places that carry their own copy (the app, the Rust
// shell) are written by `set` and compared by `check`. The C++ engine reads VERSION itself at build time (host/CMakeLists.txt).
//
//   node scripts/version.mjs print
//   node scripts/version.mjs set 0.2.0          (also 0.2.0-rc.1)
//   node scripts/version.mjs check [--tag v0.2.0]   exit 1 and list what disagrees; --tag also requires the tag to be v<VERSION>
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const SEMVER = /^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$/;

// The first `"version": "..."` of a JSON file is the package's own; dependencies sit under other keys.
const json = { get: (text) => JSON.parse(text).version, set: (text, v) => text.replace(/("version"\s*:\s*")[^"]+(")/, `$1${v}$2`) };

const COPIES = {
  'app/package.json': json,
  'app/src-tauri/tauri.conf.json': json,
  'app/src-tauri/Cargo.toml': {
    get: (text) => text.match(/^version\s*=\s*"([^"]+)"/m)?.[1],
    set: (text, v) => text.replace(/^(version\s*=\s*")[^"]+(")/m, `$1${v}$2`),
  },
  // the lock file lists the app's own package too; other packages keep their versions
  'app/src-tauri/Cargo.lock': {
    get: (text) => text.match(/name = "malgrind"\nversion = "([^"]+)"/)?.[1],
    set: (text, v) => text.replace(/(name = "malgrind"\nversion = ")[^"]+(")/, `$1${v}$2`),
  },
};

export const readVersion = (root) => readFileSync(join(root, 'VERSION'), 'utf8').trim();

// What disagrees with VERSION (and with `tag`, if given). An empty list means all is well.
export function check(root, tag) {
  const wanted = readVersion(root);
  const problems = [];
  if (!SEMVER.test(wanted)) problems.push(`VERSION: "${wanted}" is not a version (x.y.z or x.y.z-pre)`);
  for (const [file, copy] of Object.entries(COPIES)) {
    const found = copy.get(readFileSync(join(root, file), 'utf8'));
    if (found !== wanted) problems.push(`${file}: ${found ?? 'no version'}, VERSION says ${wanted}`);
  }
  if (tag !== undefined && tag !== `v${wanted}`) problems.push(`tag ${tag}: a release tag must be v${wanted} (VERSION)`);
  return problems;
}

export function setVersion(root, version) {
  if (!SEMVER.test(version)) throw new Error(`"${version}" is not a version (x.y.z or x.y.z-pre)`);
  writeFileSync(join(root, 'VERSION'), `${version}\n`);
  for (const [file, copy] of Object.entries(COPIES)) {
    const path = join(root, file);
    writeFileSync(path, copy.set(readFileSync(path, 'utf8'), version));
  }
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const root = join(dirname(fileURLToPath(import.meta.url)), '..');
  const [command, ...args] = process.argv.slice(2);
  try {
    if (command === 'print') console.log(readVersion(root));
    else if (command === 'set') {
      setVersion(root, args[0] ?? '');
      console.log(`version ${args[0]}: now commit, then  git tag v${args[0]} && git push --follow-tags`);
    } else if (command === 'check') {
      const tagAt = args.indexOf('--tag');
      const problems = check(root, tagAt >= 0 ? args[tagAt + 1] : undefined);
      problems.forEach((p) => console.error(p));
      if (problems.length) process.exit(1);
      console.log(`version ${readVersion(root)}: all copies agree`);
    } else {
      console.error('usage: version.mjs print | set <x.y.z> | check [--tag v<x.y.z>]');
      process.exit(2);
    }
  } catch (e) {
    console.error(e.message);
    process.exit(1);
  }
}
