// What the user may change before a run (host/src/settings.hpp). Empty or 0 means "what malgrind detected".
export interface RunSettings {
  env: string;
  extra_args: string; // words appended to both pio commands
  build_flags: string; // PLATFORMIO_BUILD_FLAGS
  rate_hz: number; // 0: the agent's default
  check_memory?: boolean; // opt-in memory error/leak hooks (slower, uses extra RAM)
  elf: string;
  build_command: string; // replaces the detected build command line
  upload_command: string; // replaces the detected upload command line
  target_cfg?: string; // OpenOCD target/board config for debug probe runs
}

export interface ProbeTargetOption {
  cfg: string;
  label: string;
}

export type ProbeSampleMode = 'auto' | 'dwt_pcsr' | 'halt_resume';

export interface OpenOcdStatus {
  package: string;
  found: boolean;
  exe: string | null;
  scripts: string | null;
}

export interface ProbeConfig {
  package: string;
  interface_desc: string;
  target_cfg: string;
  openocd_args: string[];
  sample_mode: ProbeSampleMode;
  target_arch: string | null;
}

// What the settings panel needs before a run (`run_plan`).
export interface RunPlan {
  detected: RunSettings;
  last: RunSettings | null; // what the project used last time
  caps?: { memcheck?: boolean };
  envs?: string[];
  elf_hint?: string; // where the build puts its ELF
  detected_commands?: { build: string; upload: string };
  commands: {
    build?: string;
    upload?: string;
    openocd?: string;
    package?: string;
    interface?: string;
    target_cfg?: string;
    env: string[];
  } | null; // for the settings that were sent; null when `error` is set
  probe_targets?: ProbeTargetOption[];
  probe_sample_mode?: ProbeSampleMode;
  probe_rate_hint?: string;
  openocd?: OpenOcdStatus;
  probe_config?: ProbeConfig | null;
  summary?: string;
  note?: string;
  error: string; // the field that does not hold up, in words
  warning?: string; // allowed, but probably not meant (a typed upload line that names no device)
}

// Shapes of what the sidecar returns (host/src/serve.hpp, host/src/store.hpp).
export interface RunMeta {
  id: string;
  name: string;
  notes: string;
  project: string;
  env: string;
  port: string;
  device_id?: string;
  project_id: string; // the malgrind project this run belongs to; empty for runs made without one
  started: string; // ISO-8601 UTC
  arch: string | null;
  ncores: number;
  cpu_mhz: number;
  duration_ms: number;
  bytes?: number;
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
  kind?: 'serial' | 'probe';
  busy: boolean;
  agent: boolean;
  denied: boolean; // this user may not open the port (not in its group)
  family: string;
  family_name: string;
  arch?: string;
  ncores?: number;
  cpu_mhz?: number;
  caps?: number;
  state?: number;
  openocd?: OpenOcdStatus;
  probe_config?: ProbeConfig | null;
  probe_targets?: ProbeTargetOption[];
  probe_sample_mode?: ProbeSampleMode;
  probe_rate_hint?: string;
  error?: string;
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

export interface UsbInfo {
  vid: string;
  pid: string;
  serial: string;
  location: string;
  product: string;
  manufacturer: string;
}

export interface Device {
  id: string;
  kind: 'serial' | 'probe';
  path: string;
  name: string;
  title: string;
  usb: UsbInfo | null;
  access: { ok: boolean; group: string; in_group: boolean; fixable: boolean };
  family_hint: string;
  family_hint_name: string;
}

// What the machine has for the jobs the app needs done (`setup`): paths, or null when missing.
export interface SetupInfo {
  pio: string | null;
  python: string | null;
  python_hint: string;
  git: string | null;
  can_grant_access: boolean; // whether the engine can ask the system to grant port access
  os?: string;
  user: string;
}

export interface DevicesInfo {
  devices: Device[];
  can_grant_access: boolean;
}

// Differential flamegraph / comparison result (`diff`, host/src/diff.cpp).
export interface DiffFunction {
  name: string;
  a_self_pct: number;
  b_self_pct: number;
  a_incl_pct: number;
  b_incl_pct: number;
}

export interface DiffNode {
  name: string;
  a: number;
  b: number;
  value: number; // = b
  children: DiffNode[];
}

export interface DiffResult {
  a: RunMeta;
  b: RunMeta;
  total_a: number;
  total_b: number;
  functions: DiffFunction[];
  tree: DiffNode;
}

// Agent v2 capability bits (core/include/malgrind/profiler.hpp).
export const CAP_SAMPLES = 1;
export const CAP_HEAP = 2;
export const CAP_TASKS = 4;
export const CAP_ALLOCS = 8;
export const CAP_ERRORS = 16;

export interface RunStats {
  arch?: string;
  ncores?: number;
  cpu_mhz?: number;
  rate_hz?: number;
  samples_per_s?: number;
  dropped?: number;
  overhead_pct?: number | null;
  isr_max_cycles?: number;
  duration_ms?: number;
  caps?: number;
  agent_state?: number;
}

export interface TargetEvent {
  arch: string;
  ncores: number;
  cpu_mhz: number;
  elf: string | null;
  caps?: number;
  state?: number;
  agent_ver?: number;
}

export interface FunctionsData {
  total: number;
  functions: { name: string; self: number; total: number }[];
  callers: { name: string; count: number }[];
  callees: { name: string; count: number }[];
  series?: [number, number][];
}

// [t_ms, free, min_free, largest, total]
export type HeapSamplePoint = [number, number, number, number, number];

export interface HeapRegionSeries {
  region: number;
  name: string;
  series: HeapSamplePoint[];
}

export interface MemoryData {
  regions: HeapRegionSeries[];
}

export interface TaskRow {
  handle: number;
  name: string;
  prio: number;
  core: number | null;
  state: number;
  cpu_pct: number;
  stack_hwm: number;
  warn: boolean;
}

export interface TasksData {
  tasks: TaskRow[];
}

export type LeakKind = 'definitely' | 'possibly';

export interface LeakGroup {
  kind: LeakKind;
  stack: string[];
  count: number;
  bytes: number;
  oldest_ms: number;
}

export type MemoryErrorKind = 'heap_corrupt' | 'stack_overflow' | 'alloc_failed' | 'unknown';

export interface MemoryErrorRow {
  t_ms: number;
  code: number;
  kind: MemoryErrorKind;
  arg: number;
  name: string;
  stack: string[];
}

export interface MemcheckData {
  leaks: LeakGroup[];
  errors: MemoryErrorRow[];
  dropped: number;
  live_bytes: number;
  live_allocs: number;
}

export interface SeriesLine {
  label: string;
  color: string;
  points: readonly [number, number][];
}
