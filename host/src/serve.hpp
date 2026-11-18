#pragma once

// The sidecar protocol of the malgrind app: one JSON object per line on stdin and stdout.
//
//   request   {"id": N, "cmd": "...", ...arguments}   `id` only matches the reply; a run is named by the argument `run`
//   reply     {"id": N, "ok": true, "result": ...}  or  {"id": N, "ok": false, "error": "..."}
//   event     {"event": "...", ...}   (no id) log, target, notice, ended, build
//
// Commands (arguments -> result):
//   hello                                  -> {version, data_dir, pio}
//   ports                                  -> ["<serial port>", ...]
//   probe {port, project?}                 -> {busy, agent, denied, arch?, ncores?, cpu_mhz?, methods}   listens up to 2 s, records nothing;
//                                             methods = [{id: attach|new_run|no_flash, available, why}]; `project` (an id) only
//                                             decides whether new_run is possible with it
//   chip_id {port}                         -> {chip}                                    resets the board
//   boards                                 -> {ports: [{port, adapter, vid, pid, serial, location, access: {ok, group, in_group, fixable}}],
//                                              probes: [{name, vid, pid}]}   what USB and file permissions say, nothing is opened
//   setup                                  -> {pio, python, git (paths or null), can_grant_access (bool), os, user}   what the app needs from the machine
//   install_pio {}                         -> {pio}   python -m venv ~/.platformio/penv + pip install platformio; streams `build` events
//   grant_access {port}                    -> {ok_now, relogin}   prompts to grant device access on Linux (ACL + group)
//   projects                               -> [Project + run_count + last_run]
//   project_add {source, dir?|url?|elf?, name?}   source "folder" (a PlatformIO project of the user's, trusted), "git" (a clone of
//                                          https://github.com/owner/repo, NOT trusted yet, streams `build` events), "none" (only a
//                                          board, optional ELF) -> Project {id, name, source, dir, url, env, elf, trusted, has_source, envs, problem}
//   project_update {project, name?, env?, trusted?} -> Project
//   project_remove {project}               -> {}      the record and a clone; never the user's own folder
//   project {dir}                          -> {dir, envs, default_env}
//   run_plan {project, port, mode?, settings?} -> {detected, last, envs, elf_hint, detected_commands, commands, error}   what the settings
//                                             panel shows before a run: the detected RunSettings, the project's last-used ones, and the
//                                             exact commands (+ environment) for `settings` as they are now. A field that does not hold up
//                                             is `error` (commands null), not a failed request. mode "attach": only `detected` and `last`.
//   new_run {project, port, name, settings?} -> {run}   backs up the board's flash (first time), builds with the agent, uploads,
//                                             then records live; streams `build` events. `settings` = RunSettings {env, extra_args,
//                                             build_flags, rate_hz, elf, build_command, upload_command}, all checked first; stored with
//                                             the run and as the project's last_settings
//   attach {port, name, project?, elf?, settings?} -> {run}   records a board that already runs the agent; `project` is an id
//   stop                                   -> {}      ends the live run, or closes an opened one
//   remove_agent {port, how?, project?}    -> {}      how "restore" (default): writes the board's backup back; "rebuild": builds
//                                             the project (an id) without the agent and uploads it
//   runs                                   -> [RunMeta...], the live one has "live": true
//   open {run}                             -> {run}   replays a stored run (refused while a run is live)
//   update {run, name?, notes?}            -> RunMeta
//   delete {run}                           -> {}      (refused for the run in use)
//   snapshot {core?, window_s, at_ms?}     -> {tree, stats}   {"tree": null} before the first frame
//   functions {core?, window_s, at_ms?, focus?} -> {total, functions, callers, callees}
//
// new_run, remove_agent, project_add, install_pio, grant_access and chip_id are slow and run on a worker thread; only one at a time ("busy: ...").

#include <filesystem>
#include <istream>
#include <ostream>

namespace malgrind::host {

struct ServeOptions {
    std::filesystem::path data_dir;  // where runs are stored
    std::filesystem::path agent_dir; // contains agent/esp32/ (the app's resources, or the repository)
    std::filesystem::path home;      // for finding PlatformIO and esptool
    std::filesystem::path backup_dir; // flash backups, <dir>/<board MAC>/<time>.bin (see backup.hpp)
    std::filesystem::path projects_dir; // projects and clones of GitHub projects (see projects.hpp)
};

// Serves requests from `in` until it ends (the app closed, or end of file), then closes the live run. Returns 0.
int serve(std::istream& in, std::ostream& out, const ServeOptions& options);

} // namespace malgrind::host
