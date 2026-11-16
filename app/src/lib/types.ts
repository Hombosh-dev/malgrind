// Shapes of what the sidecar returns (host/src/serve.hpp, host/src/store.hpp).
export interface RunMeta {
  id: string;
  name: string;
  notes: string;
  project: string;
  env: string;
  port: string;
  started: string; // ISO-8601 UTC
  arch: string | null;
  ncores: number;
  cpu_mhz: number;
  duration_ms: number;
  has_elf: boolean;
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
