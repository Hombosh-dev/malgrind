// Builds the C++ sidecar and puts it where Tauri expects an external binary: binaries/malgrind-cli-<target triple>.
import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const app = join(dirname(fileURLToPath(import.meta.url)), '..');
const repo = join(app, '..');
const run = (cmd, args, cwd = repo) => execFileSync(cmd, args, { cwd, stdio: 'inherit' });

// A separate build tree for the package: no repository path baked into the binary as the agent directory
// (it finds the app's resources next to itself at run time).
run('cmake', ['-B', 'build-app', '-DCMAKE_BUILD_TYPE=Release', '-DMALGRIND_DEFAULT_AGENT_DIR=']);
run('cmake', ['--build', 'build-app', '--target', 'malgrind-cli', '-j']);

const triple = execFileSync('rustc', ['--print', 'host-tuple']).toString().trim();
const dest = join(app, 'src-tauri', 'binaries');
mkdirSync(dest, { recursive: true });
copyFileSync(join(repo, 'build-app', 'host', 'malgrind-cli'), join(dest, `malgrind-cli-${triple}`));
console.log(`sidecar: binaries/malgrind-cli-${triple}`);
