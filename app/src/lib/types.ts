// What the user may change before a run (host/src/settings.hpp). Empty or 0 means "what malgrind detected".
export interface RunSettings {
  env: string;
  extra_args: string; // words appended to both pio commands
  build_flags: string; // PLATFORMIO_BUILD_FLAGS
  rate_hz: number; // 0: the agent's default
  elf: string;
  build_command: string; // replaces the detected build command line
  upload_command: string; // replaces the detected upload command line
}

// What the settings panel needs before a run (`run_plan`).
export interface RunPlan {
  detected: RunSettings;
  last: RunSettings | null; // what the project used last time
  envs?: string[];
  elf_hint?: string; // where the build puts its ELF
  detected_commands?: { build: string; upload: string };
  commands: { build: string; upload: string; env: string[] } | null; // for the settings that were sent; null when `error` is set
  error: string; // the field that does not hold up, in words
}

// Shapes of what the sidecar returns (host/src/serve.hpp, host/src/store.hpp).
export interface RunMeta {
  id: string;
  name: string;
  notes: string;
  project: string;
  env: string;
  port: string;
  project_id: string; // the malgrind project this run belongs to; empty for runs made without one
  started: string; // ISO-8601 UTC
  arch: string | null;
  ncores: number;
  cpu_mhz: number;
  duration_ms: number;
  has_elf: boolean;
  settings: RunSettings | null; // what the run was made with; null for runs from before settings existed
  live?: boolean; // only in the `runs` list, for the run being recorded
}

export interface Hello {
  version: string;
  data_dir: string;
  pio: string | null;
}

// One way to profile the connected board; `why` says what is missing when it is not available.
export interface Method {
  id: 'attach' | 'new_run' | 'no_flash';
  available: boolean;
  why: string;
}

export interface Probe {
  busy: boolean;
  agent: boolean;
  denied: boolean; // this user may not open the port (not in its group)
  arch?: string;
  ncores?: number;
  cpu_mhz?: number;
  methods: Method[];
}

export interface ProjectInfo {
  dir: string;
  envs: string[];
  default_env: string;
}

// A project is a container: its source code is optional (a board whose firmware nobody has can still be profiled).
export interface ProjectMeta {
  id: string;
  name: string;
  source: 'folder' | 'git' | 'none';
  dir: string;
  url: string;
  env: string;
  created: string;
  elf: string;
  trusted: boolean; // building runs the project's scripts: a GitHub project must be trusted first
  has_source: boolean;
  envs: string[];
  problem: string; // why the folder cannot be used now (moved, deleted); empty if fine
  last_settings: RunSettings | null;
  run_count?: number; // only in the `projects` list
  last_run?: string | null;
}

export interface BoardPort {
  port: string;
  adapter: string; // the USB adapter (not the chip); empty if unknown
  vid: string;
  pid: string;
  serial: string; // the adapter's serial number, if it has one
  location: string; // where it is plugged in, "3-3"
  access: { ok: boolean; group: string; in_group: boolean }; // whether this user may open the port
}

// What the machine has for the jobs the app needs done (`setup`): paths, or null when missing.
export interface SetupInfo {
  pio: string | null;
  python: string | null;
  git: string | null;
  pkexec: boolean; // the system's graphical password prompt, used to grant port access
  user: string;
}

export interface BoardsInfo {
  ports: BoardPort[];
  probes: { name: string; vid: string; pid: string }[]; // USB debug probes that are plugged in
}
