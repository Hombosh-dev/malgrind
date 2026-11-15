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

export interface Probe {
  busy: boolean;
  agent: boolean;
  arch?: string;
  ncores?: number;
  cpu_mhz?: number;
}

export interface ProjectInfo {
  dir: string;
  envs: string[];
  default_env: string;
}
