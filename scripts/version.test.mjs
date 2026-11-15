// node --test scripts/
import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { check, setVersion } from './version.mjs';

// A tree with the same five files the real one has, all at `v`.
function tree(v) {
  const root = mkdtempSync(join(tmpdir(), 'malgrind-version-'));
  mkdirSync(join(root, 'app/src-tauri'), { recursive: true });
  writeFileSync(join(root, 'VERSION'), `${v}\n`);
  writeFileSync(join(root, 'app/package.json'), JSON.stringify({ name: 'x', version: v, dependencies: { d3: '7.9.0' } }, null, 2));
  writeFileSync(join(root, 'app/src-tauri/tauri.conf.json'), JSON.stringify({ productName: 'malgrind', version: v }, null, 2));
  writeFileSync(join(root, 'app/src-tauri/Cargo.toml'), `[package]\nname = "malgrind"\nversion = "${v}"\nedition = "2021"\n\n[dependencies]\ntauri = { version = "2" }\n`);
  writeFileSync(join(root, 'app/src-tauri/Cargo.lock'), `[[package]]\nname = "other"\nversion = "9.9.9"\n\n[[package]]\nname = "malgrind"\nversion = "${v}"\n`);
  return root;
}

test('a consistent tree passes', () => {
  assert.deepEqual(check(tree('0.1.0')), []);
});

test('set writes every copy and touches nothing else', () => {
  const root = tree('0.1.0');
  setVersion(root, '1.2.3-rc.1');
  assert.deepEqual(check(root), []);
  assert.equal(readFileSync(join(root, 'VERSION'), 'utf8'), '1.2.3-rc.1\n');
  assert.match(readFileSync(join(root, 'app/src-tauri/Cargo.toml'), 'utf8'), /tauri = \{ version = "2" \}/); // a dependency's version stays
  assert.match(readFileSync(join(root, 'app/src-tauri/Cargo.lock'), 'utf8'), /name = "other"\nversion = "9.9.9"/); // so does another package's
  assert.equal(JSON.parse(readFileSync(join(root, 'app/package.json'), 'utf8')).dependencies.d3, '7.9.0');
});

test('a copy that drifted is named', () => {
  const root = tree('0.1.0');
  writeFileSync(join(root, 'app/src-tauri/Cargo.toml'), readFileSync(join(root, 'app/src-tauri/Cargo.toml'), 'utf8').replace('0.1.0', '0.0.9'));
  const problems = check(root);
  assert.equal(problems.length, 1);
  assert.match(problems[0], /Cargo\.toml.*0\.0\.9.*0\.1\.0/);
});

test('only x.y.z and x.y.z-pre are versions', () => {
  for (const bad of ['1.2', 'v1.2.3', '1.2.3.4', '', '1.2.3-', 'latest']) assert.throws(() => setVersion(tree('0.1.0'), bad), /not a version/, bad);
  assert.doesNotThrow(() => setVersion(tree('0.1.0'), '10.20.30'));
});

test('a release tag must be v plus the version', () => {
  const root = tree('0.2.0');
  assert.deepEqual(check(root, 'v0.2.0'), []);
  assert.match(check(root, 'v0.2.1')[0], /tag v0\.2\.1.*0\.2\.0/);
  assert.match(check(root, '0.2.0')[0], /tag/); // a tag without the v is not a release tag
});
